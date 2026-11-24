/*
 * Camera capture sources for Media Foundation
 *
 * Copyright 2026 The TuxOffice authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "gst_private.h"
#include "mferror.h"
#include "mfapi.h"

#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND ((NTSTATUS)0xc0000225)
#endif

/* mfapi.h has these for C++ only. */
static HRESULT set_attribute_pair(IMFAttributes *attributes, REFGUID key, UINT32 high, UINT32 low)
{
    return IMFAttributes_SetUINT64(attributes, key, ((UINT64)high << 32) | low);
}

static HRESULT get_attribute_pair(IMFAttributes *attributes, REFGUID key, UINT32 *high, UINT32 *low)
{
    UINT64 value;
    HRESULT hr;

    if (SUCCEEDED(hr = IMFAttributes_GetUINT64(attributes, key, &value)))
    {
        *high = value >> 32;
        *low = (UINT32)value;
    }
    return hr;
}

#define MAX_CAMERAS 16
#define MAX_MODES 12
/* A live source keeps the newest frames; older ones a slow reader missed are dropped. */
#define MAX_QUEUED 2

static const struct
{
    const GUID *subtype;
    UINT32 format;
    UINT32 bits;
}
camera_formats[] =
{
    {&MFVideoFormat_NV12, WG_CAMERA_FORMAT_NV12, 12},
    {&MFVideoFormat_YUY2, WG_CAMERA_FORMAT_YUY2, 16},
    {&MFVideoFormat_RGB32, WG_CAMERA_FORMAT_BGRX, 32},
};

static void camera_link(const char *id, WCHAR *link, size_t size)
{
    int len = swprintf(link, size, L"\\\\?\\wine-camera#");
    MultiByteToWideChar(CP_UTF8, 0, id, -1, link + len, size - len);
}

/* Camera media source. */

enum camera_state
{
    CAMERA_STOPPED,
    CAMERA_STARTED,
    CAMERA_PAUSED,
    CAMERA_SHUTDOWN,
};

struct queued_sample
{
    struct list entry;
    IMFSample *sample;
};

struct sample_request
{
    struct list entry;
    IUnknown *token;
};

struct camera_source
{
    IMFMediaSource IMFMediaSource_iface;
    IMFMediaStream IMFMediaStream_iface;
    LONG refcount;

    IMFMediaEventQueue *queue;
    IMFMediaEventQueue *stream_queue;
    IMFPresentationDescriptor *descriptor;
    IMFStreamDescriptor *stream_descriptor;

    char id[128];
    wg_camera_t camera;
    HANDLE thread, quit;
    UINT32 frame_size, width, height, fourcc;
    UINT64 frame_duration;
    LONGLONG last_time;
    BOOL discontinuity;

    enum camera_state state;
    BOOL stream_created;
    struct list samples;
    UINT32 queued;
    struct list requests;
    CRITICAL_SECTION cs;
};

static struct camera_source *impl_from_IMFMediaSource(IMFMediaSource *iface)
{
    return CONTAINING_RECORD(iface, struct camera_source, IMFMediaSource_iface);
}

static struct camera_source *impl_from_IMFMediaStream(IMFMediaStream *iface)
{
    return CONTAINING_RECORD(iface, struct camera_source, IMFMediaStream_iface);
}

/* Captured frames go to waiting requests in order. Called with the lock held. */
static void deliver_samples(struct camera_source *source)
{
    struct queued_sample *queued;
    struct sample_request *request;

    if (source->state != CAMERA_STARTED) return;
    while (!list_empty(&source->samples) && !list_empty(&source->requests))
    {
        queued = LIST_ENTRY(list_head(&source->samples), struct queued_sample, entry);
        request = LIST_ENTRY(list_head(&source->requests), struct sample_request, entry);
        list_remove(&queued->entry);
        list_remove(&request->entry);
        source->queued--;

        if (request->token) IMFSample_SetUnknown(queued->sample, &MFSampleExtension_Token, request->token);
        IMFMediaEventQueue_QueueEventParamUnk(source->stream_queue, MEMediaSample, &GUID_NULL, S_OK,
                (IUnknown *)queued->sample);

        IMFSample_Release(queued->sample);
        free(queued);
        if (request->token) IUnknown_Release(request->token);
        free(request);
    }
}

static void flush_samples(struct camera_source *source)
{
    struct queued_sample *queued, *next_sample;
    struct sample_request *request, *next_request;

    LIST_FOR_EACH_ENTRY_SAFE(queued, next_sample, &source->samples, struct queued_sample, entry)
    {
        list_remove(&queued->entry);
        IMFSample_Release(queued->sample);
        free(queued);
    }
    source->queued = 0;
    LIST_FOR_EACH_ENTRY_SAFE(request, next_request, &source->requests, struct sample_request, entry)
    {
        list_remove(&request->entry);
        if (request->token) IUnknown_Release(request->token);
        free(request);
    }
}

static void queue_frame(struct camera_source *source, IMFSample *sample)
{
    struct queued_sample *queued;

    if (source->state == CAMERA_PAUSED) return;
    if (source->queued == MAX_QUEUED)
    {
        queued = LIST_ENTRY(list_head(&source->samples), struct queued_sample, entry);
        list_remove(&queued->entry);
        IMFSample_Release(queued->sample);
        free(queued);
        source->queued--;
        source->discontinuity = TRUE;
    }
    if (!(queued = malloc(sizeof(*queued)))) return;
    queued->sample = sample;
    IMFSample_AddRef(sample);
    if (source->discontinuity) IMFSample_SetUINT32(sample, &MFSampleExtension_Discontinuity, TRUE);
    source->discontinuity = FALSE;
    list_add_tail(&source->samples, &queued->entry);
    source->queued++;
    deliver_samples(source);
}

static DWORD WINAPI capture_thread(void *arg)
{
    struct camera_source *source = arg;
    struct wg_camera_read_params params = {.camera = source->camera, .timeout_ms = 200};
    IMFMediaBuffer *buffer = NULL;
    IMFSample *sample;
    NTSTATUS status;
    BYTE *data;

    SetThreadDescription(GetCurrentThread(), L"wine_mf_camera");

    while (WaitForSingleObject(source->quit, 0) == WAIT_TIMEOUT)
    {
        /* Cameras deliver 2D buffers on Windows, and video consumers lock them as such. */
        if (!buffer && FAILED(MFCreate2DMediaBuffer(source->width, source->height, source->fourcc, FALSE, &buffer)))
            break;
        if (FAILED(IMFMediaBuffer_Lock(buffer, &data, NULL, NULL))) break;
        params.data = (UINT_PTR)data;
        params.size = source->frame_size;
        status = WINE_UNIX_CALL(unix_wg_camera_read, &params);
        IMFMediaBuffer_Unlock(buffer);
        if (status == STATUS_TIMEOUT) continue;
        if (status)
        {
            ERR("Camera %s failed, status %#lx.\n", debugstr_a(source->id), status);
            EnterCriticalSection(&source->cs);
            if (source->state != CAMERA_SHUTDOWN)
                IMFMediaEventQueue_QueueEventParamVar(source->queue, MEError, &GUID_NULL,
                        MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED, NULL);
            LeaveCriticalSection(&source->cs);
            break;
        }

        IMFMediaBuffer_SetCurrentLength(buffer, params.written);
        if (FAILED(MFCreateSample(&sample))) continue;
        IMFSample_AddBuffer(sample, buffer);
        IMFMediaBuffer_Release(buffer);
        buffer = NULL;
        EnterCriticalSection(&source->cs);
        if (params.time < 0) params.time = source->last_time + source->frame_duration;
        source->last_time = params.time;
        IMFSample_SetSampleTime(sample, params.time);
        IMFSample_SetSampleDuration(sample, source->frame_duration);
        IMFSample_SetUINT32(sample, &MFSampleExtension_CleanPoint, TRUE);
        queue_frame(source, sample);
        LeaveCriticalSection(&source->cs);
        IMFSample_Release(sample);
    }
    if (buffer) IMFMediaBuffer_Release(buffer);
    return 0;
}

static void stop_camera(struct camera_source *source)
{
    if (source->thread)
    {
        SetEvent(source->quit);
        WaitForSingleObject(source->thread, INFINITE);
        CloseHandle(source->thread);
        source->thread = NULL;
        ResetEvent(source->quit);
    }
    if (source->camera)
    {
        WINE_UNIX_CALL(unix_wg_camera_destroy, &source->camera);
        source->camera = 0;
    }
}

/* Opens the camera in the stream's current media type. */
static HRESULT start_camera(struct camera_source *source)
{
    struct wg_camera_create_params params = {0};
    IMFMediaTypeHandler *handler;
    IMFMediaType *type;
    UINT32 width, height, n, d, i;
    GUID subtype;
    NTSTATUS status;
    HRESULT hr;

    if (FAILED(hr = IMFStreamDescriptor_GetMediaTypeHandler(source->stream_descriptor, &handler))) return hr;
    hr = IMFMediaTypeHandler_GetCurrentMediaType(handler, &type);
    IMFMediaTypeHandler_Release(handler);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)) ||
            FAILED(hr = get_attribute_pair((IMFAttributes *)type, &MF_MT_FRAME_SIZE, &width, &height)) ||
            FAILED(hr = get_attribute_pair((IMFAttributes *)type, &MF_MT_FRAME_RATE, &n, &d)))
    {
        IMFMediaType_Release(type);
        return hr;
    }
    IMFMediaType_Release(type);

    for (i = 0; i < ARRAY_SIZE(camera_formats); i++)
        if (IsEqualGUID(&subtype, camera_formats[i].subtype)) break;
    if (i == ARRAY_SIZE(camera_formats) || !d || !n) return MF_E_INVALIDMEDIATYPE;

    strcpy(params.id, source->id);
    params.format = camera_formats[i].format;
    params.mode.width = width;
    params.mode.height = height;
    params.mode.fps_n = n;
    params.mode.fps_d = d;
    if ((status = WINE_UNIX_CALL(unix_wg_camera_create, &params)))
    {
        ERR("Failed to start camera %s, status %#lx.\n", debugstr_a(source->id), status);
        return status == STATUS_NOT_FOUND ? MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED : MF_E_HW_MFT_FAILED_START_STREAMING;
    }
    source->camera = params.camera;
    source->frame_size = (UINT64)width * height * camera_formats[i].bits / 8;
    source->width = width;
    source->height = height;
    source->fourcc = subtype.Data1;
    source->frame_duration = MFllMulDiv(10000000, d, n, 0);
    source->last_time = -(LONGLONG)source->frame_duration;
    source->discontinuity = TRUE;
    if (!(source->thread = CreateThread(NULL, 0, capture_thread, source, 0, NULL)))
    {
        stop_camera(source);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    TRACE("Camera %s started, %ux%u at %u/%u.\n", debugstr_a(source->id), width, height, n, d);
    return S_OK;
}

static HRESULT WINAPI camera_source_QueryInterface(IMFMediaSource *iface, REFIID riid, void **out)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFMediaSource) || IsEqualIID(riid, &IID_IMFMediaEventGenerator) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFMediaSource_AddRef(iface);
        return S_OK;
    }
    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI camera_source_AddRef(IMFMediaSource *iface)
{
    struct camera_source *source = impl_from_IMFMediaSource(iface);
    ULONG refcount = InterlockedIncrement(&source->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG WINAPI camera_source_Release(IMFMediaSource *iface)
{
    struct camera_source *source = impl_from_IMFMediaSource(iface);
    ULONG refcount = InterlockedDecrement(&source->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        IMFMediaSource_Shutdown(iface);
        if (source->queue) IMFMediaEventQueue_Release(source->queue);
        if (source->stream_queue) IMFMediaEventQueue_Release(source->stream_queue);
        if (source->descriptor) IMFPresentationDescriptor_Release(source->descriptor);
        if (source->stream_descriptor) IMFStreamDescriptor_Release(source->stream_descriptor);
        CloseHandle(source->quit);
        source->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&source->cs);
        free(source);
    }
    return refcount;
}

static HRESULT WINAPI camera_source_GetEvent(IMFMediaSource *iface, DWORD flags, IMFMediaEvent **event)
{
    struct camera_source *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %#lx, %p.\n", iface, flags, event);
    return IMFMediaEventQueue_GetEvent(source->queue, flags, event);
}

static HRESULT WINAPI camera_source_BeginGetEvent(IMFMediaSource *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    struct camera_source *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %p, %p.\n", iface, callback, state);
    return IMFMediaEventQueue_BeginGetEvent(source->queue, callback, state);
}

static HRESULT WINAPI camera_source_EndGetEvent(IMFMediaSource *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    struct camera_source *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %p, %p.\n", iface, result, event);
    return IMFMediaEventQueue_EndGetEvent(source->queue, result, event);
}

static HRESULT WINAPI camera_source_QueueEvent(IMFMediaSource *iface, MediaEventType type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    struct camera_source *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %lu, %s, %#lx, %p.\n", iface, type, debugstr_guid(ext_type), hr, value);
    return IMFMediaEventQueue_QueueEventParamVar(source->queue, type, ext_type, hr, value);
}

static HRESULT WINAPI camera_source_GetCharacteristics(IMFMediaSource *iface, DWORD *characteristics)
{
    struct camera_source *source = impl_from_IMFMediaSource(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, characteristics);

    if (!characteristics) return E_POINTER;
    EnterCriticalSection(&source->cs);
    if (source->state == CAMERA_SHUTDOWN) hr = MF_E_SHUTDOWN;
    else *characteristics = MFMEDIASOURCE_IS_LIVE;
    LeaveCriticalSection(&source->cs);
    return hr;
}

static HRESULT WINAPI camera_source_CreatePresentationDescriptor(IMFMediaSource *iface, IMFPresentationDescriptor **descriptor)
{
    struct camera_source *source = impl_from_IMFMediaSource(iface);
    HRESULT hr;

    TRACE("%p, %p.\n", iface, descriptor);

    if (!descriptor) return E_POINTER;
    EnterCriticalSection(&source->cs);
    if (source->state == CAMERA_SHUTDOWN) hr = MF_E_SHUTDOWN;
    else hr = IMFPresentationDescriptor_Clone(source->descriptor, descriptor);
    LeaveCriticalSection(&source->cs);
    return hr;
}

static HRESULT WINAPI camera_source_Start(IMFMediaSource *iface, IMFPresentationDescriptor *descriptor,
        const GUID *time_format, const PROPVARIANT *start_position)
{
    struct camera_source *source = impl_from_IMFMediaSource(iface);
    IMFStreamDescriptor *stream_descriptor;
    PROPVARIANT time;
    BOOL selected;
    HRESULT hr;

    TRACE("%p, %p, %s, %p.\n", iface, descriptor, debugstr_guid(time_format), start_position);

    if (!descriptor) return E_POINTER;
    if (time_format && !IsEqualGUID(time_format, &GUID_NULL)) return MF_E_UNSUPPORTED_TIME_FORMAT;

    EnterCriticalSection(&source->cs);
    if (source->state == CAMERA_SHUTDOWN)
    {
        LeaveCriticalSection(&source->cs);
        return MF_E_SHUTDOWN;
    }
    if (FAILED(hr = IMFPresentationDescriptor_GetStreamDescriptorByIndex(descriptor, 0, &selected, &stream_descriptor)))
    {
        LeaveCriticalSection(&source->cs);
        return hr;
    }
    IMFStreamDescriptor_Release(stream_descriptor);

    if (selected && source->state == CAMERA_STOPPED && FAILED(hr = start_camera(source)))
    {
        LeaveCriticalSection(&source->cs);
        return hr;
    }

    /* A live source starts at the presentation's current time, whatever position is asked for. */
    time.vt = VT_I8;
    time.hVal.QuadPart = max(0, source->last_time + (LONGLONG)source->frame_duration);
    if (selected)
    {
        IMFMediaEventQueue_QueueEventParamUnk(source->queue, source->stream_created ? MEUpdatedStream : MENewStream,
                &GUID_NULL, S_OK, (IUnknown *)&source->IMFMediaStream_iface);
        source->stream_created = TRUE;
    }
    source->state = CAMERA_STARTED;
    IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourceStarted, &GUID_NULL, S_OK, &time);
    if (selected)
        IMFMediaEventQueue_QueueEventParamVar(source->stream_queue, MEStreamStarted, &GUID_NULL, S_OK, &time);
    deliver_samples(source);
    LeaveCriticalSection(&source->cs);
    return S_OK;
}

static HRESULT WINAPI camera_source_Stop(IMFMediaSource *iface)
{
    struct camera_source *source = impl_from_IMFMediaSource(iface);

    TRACE("%p.\n", iface);

    EnterCriticalSection(&source->cs);
    if (source->state == CAMERA_SHUTDOWN)
    {
        LeaveCriticalSection(&source->cs);
        return MF_E_SHUTDOWN;
    }
    source->state = CAMERA_STOPPED;
    LeaveCriticalSection(&source->cs);

    /* The capture thread takes the lock to queue frames: stop it without holding it. */
    stop_camera(source);

    EnterCriticalSection(&source->cs);
    flush_samples(source);
    IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourceStopped, &GUID_NULL, S_OK, NULL);
    if (source->stream_created)
        IMFMediaEventQueue_QueueEventParamVar(source->stream_queue, MEStreamStopped, &GUID_NULL, S_OK, NULL);
    LeaveCriticalSection(&source->cs);
    return S_OK;
}

static HRESULT WINAPI camera_source_Pause(IMFMediaSource *iface)
{
    struct camera_source *source = impl_from_IMFMediaSource(iface);
    HRESULT hr = S_OK;

    TRACE("%p.\n", iface);

    EnterCriticalSection(&source->cs);
    if (source->state == CAMERA_SHUTDOWN) hr = MF_E_SHUTDOWN;
    else if (source->state != CAMERA_STARTED) hr = MF_E_INVALID_STATE_TRANSITION;
    else
    {
        source->state = CAMERA_PAUSED;
        source->discontinuity = TRUE;
        IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourcePaused, &GUID_NULL, S_OK, NULL);
        IMFMediaEventQueue_QueueEventParamVar(source->stream_queue, MEStreamPaused, &GUID_NULL, S_OK, NULL);
    }
    LeaveCriticalSection(&source->cs);
    return hr;
}

static HRESULT WINAPI camera_source_Shutdown(IMFMediaSource *iface)
{
    struct camera_source *source = impl_from_IMFMediaSource(iface);

    TRACE("%p.\n", iface);

    EnterCriticalSection(&source->cs);
    if (source->state == CAMERA_SHUTDOWN)
    {
        LeaveCriticalSection(&source->cs);
        return MF_E_SHUTDOWN;
    }
    source->state = CAMERA_SHUTDOWN;
    LeaveCriticalSection(&source->cs);

    stop_camera(source);

    EnterCriticalSection(&source->cs);
    flush_samples(source);
    IMFMediaEventQueue_Shutdown(source->queue);
    IMFMediaEventQueue_Shutdown(source->stream_queue);
    LeaveCriticalSection(&source->cs);
    return S_OK;
}

static const IMFMediaSourceVtbl camera_source_vtbl =
{
    camera_source_QueryInterface,
    camera_source_AddRef,
    camera_source_Release,
    camera_source_GetEvent,
    camera_source_BeginGetEvent,
    camera_source_EndGetEvent,
    camera_source_QueueEvent,
    camera_source_GetCharacteristics,
    camera_source_CreatePresentationDescriptor,
    camera_source_Start,
    camera_source_Stop,
    camera_source_Pause,
    camera_source_Shutdown,
};

static HRESULT WINAPI camera_stream_QueryInterface(IMFMediaStream *iface, REFIID riid, void **out)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFMediaStream) || IsEqualIID(riid, &IID_IMFMediaEventGenerator) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFMediaStream_AddRef(iface);
        return S_OK;
    }
    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    *out = NULL;
    return E_NOINTERFACE;
}

/* The stream lives as long as its source, and holds it. */
static ULONG WINAPI camera_stream_AddRef(IMFMediaStream *iface)
{
    return IMFMediaSource_AddRef(&impl_from_IMFMediaStream(iface)->IMFMediaSource_iface);
}

static ULONG WINAPI camera_stream_Release(IMFMediaStream *iface)
{
    return IMFMediaSource_Release(&impl_from_IMFMediaStream(iface)->IMFMediaSource_iface);
}

static HRESULT WINAPI camera_stream_GetEvent(IMFMediaStream *iface, DWORD flags, IMFMediaEvent **event)
{
    struct camera_source *source = impl_from_IMFMediaStream(iface);

    TRACE("%p, %#lx, %p.\n", iface, flags, event);
    return IMFMediaEventQueue_GetEvent(source->stream_queue, flags, event);
}

static HRESULT WINAPI camera_stream_BeginGetEvent(IMFMediaStream *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    struct camera_source *source = impl_from_IMFMediaStream(iface);

    TRACE("%p, %p, %p.\n", iface, callback, state);
    return IMFMediaEventQueue_BeginGetEvent(source->stream_queue, callback, state);
}

static HRESULT WINAPI camera_stream_EndGetEvent(IMFMediaStream *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    struct camera_source *source = impl_from_IMFMediaStream(iface);

    TRACE("%p, %p, %p.\n", iface, result, event);
    return IMFMediaEventQueue_EndGetEvent(source->stream_queue, result, event);
}

static HRESULT WINAPI camera_stream_QueueEvent(IMFMediaStream *iface, MediaEventType type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    struct camera_source *source = impl_from_IMFMediaStream(iface);

    TRACE("%p, %lu, %s, %#lx, %p.\n", iface, type, debugstr_guid(ext_type), hr, value);
    return IMFMediaEventQueue_QueueEventParamVar(source->stream_queue, type, ext_type, hr, value);
}

static HRESULT WINAPI camera_stream_GetMediaSource(IMFMediaStream *iface, IMFMediaSource **out)
{
    struct camera_source *source = impl_from_IMFMediaStream(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, out);

    EnterCriticalSection(&source->cs);
    if (source->state == CAMERA_SHUTDOWN) hr = MF_E_SHUTDOWN;
    else
    {
        *out = &source->IMFMediaSource_iface;
        IMFMediaSource_AddRef(*out);
    }
    LeaveCriticalSection(&source->cs);
    return hr;
}

static HRESULT WINAPI camera_stream_GetStreamDescriptor(IMFMediaStream *iface, IMFStreamDescriptor **descriptor)
{
    struct camera_source *source = impl_from_IMFMediaStream(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, descriptor);

    EnterCriticalSection(&source->cs);
    if (source->state == CAMERA_SHUTDOWN) hr = MF_E_SHUTDOWN;
    else
    {
        *descriptor = source->stream_descriptor;
        IMFStreamDescriptor_AddRef(*descriptor);
    }
    LeaveCriticalSection(&source->cs);
    return hr;
}

static HRESULT WINAPI camera_stream_RequestSample(IMFMediaStream *iface, IUnknown *token)
{
    struct camera_source *source = impl_from_IMFMediaStream(iface);
    struct sample_request *request;
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, token);

    EnterCriticalSection(&source->cs);
    if (source->state == CAMERA_SHUTDOWN) hr = MF_E_SHUTDOWN;
    else if (source->state == CAMERA_STOPPED) hr = MF_E_MEDIA_SOURCE_WRONGSTATE;
    else if (!(request = malloc(sizeof(*request)))) hr = E_OUTOFMEMORY;
    else
    {
        if ((request->token = token)) IUnknown_AddRef(token);
        list_add_tail(&source->requests, &request->entry);
        deliver_samples(source);
    }
    LeaveCriticalSection(&source->cs);
    return hr;
}

static const IMFMediaStreamVtbl camera_stream_vtbl =
{
    camera_stream_QueryInterface,
    camera_stream_AddRef,
    camera_stream_Release,
    camera_stream_GetEvent,
    camera_stream_BeginGetEvent,
    camera_stream_EndGetEvent,
    camera_stream_QueueEvent,
    camera_stream_GetMediaSource,
    camera_stream_GetStreamDescriptor,
    camera_stream_RequestSample,
};

static HRESULT create_media_type(const struct wg_camera_mode *mode, unsigned int format, IMFMediaType **out)
{
    UINT32 stride = mode->width * camera_formats[format].bits / 8;
    IMFMediaType *type;
    HRESULT hr;

    if (camera_formats[format].format == WG_CAMERA_FORMAT_NV12) stride = mode->width;
    if (FAILED(hr = MFCreateMediaType(&type))) return hr;
    if (FAILED(hr = IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video)) ||
            FAILED(hr = IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, camera_formats[format].subtype)) ||
            FAILED(hr = set_attribute_pair((IMFAttributes *)type, &MF_MT_FRAME_SIZE, mode->width, mode->height)) ||
            FAILED(hr = set_attribute_pair((IMFAttributes *)type, &MF_MT_FRAME_RATE, mode->fps_n, mode->fps_d)) ||
            FAILED(hr = set_attribute_pair((IMFAttributes *)type, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
            FAILED(hr = IMFMediaType_SetUINT32(type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
            FAILED(hr = IMFMediaType_SetUINT32(type, &MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE)) ||
            FAILED(hr = IMFMediaType_SetUINT32(type, &MF_MT_FIXED_SIZE_SAMPLES, TRUE)) ||
            FAILED(hr = IMFMediaType_SetUINT32(type, &MF_MT_DEFAULT_STRIDE, stride)) ||
            FAILED(hr = IMFMediaType_SetUINT32(type, &MF_MT_SAMPLE_SIZE,
                    (UINT64)mode->width * mode->height * camera_formats[format].bits / 8)))
    {
        IMFMediaType_Release(type);
        return hr;
    }
    *out = type;
    return S_OK;
}

/* Each of the camera's sizes, largest first, in each format videoconvert can give. */
static HRESULT create_stream_descriptor(const char *id, IMFStreamDescriptor **descriptor)
{
    struct wg_camera_mode modes[MAX_MODES];
    struct wg_camera_get_modes_params params = {.modes = (UINT_PTR)modes, .capacity = ARRAY_SIZE(modes)};
    IMFMediaType *types[MAX_MODES * ARRAY_SIZE(camera_formats)];
    IMFMediaTypeHandler *handler;
    UINT32 count = 0, i, j;
    HRESULT hr = S_OK;

    strcpy(params.id, id);
    if (WINE_UNIX_CALL(unix_wg_camera_get_modes, &params) || !params.count)
        return MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED;
    for (i = 0; i < min(params.count, ARRAY_SIZE(modes)) && SUCCEEDED(hr); i++)
    {
        /* NV12 subsamples by two in both directions. */
        if ((modes[i].width | modes[i].height) & 1) continue;
        for (j = 0; j < ARRAY_SIZE(camera_formats) && SUCCEEDED(hr); j++)
            if (SUCCEEDED(hr = create_media_type(&modes[i], j, &types[count]))) count++;
    }
    if (SUCCEEDED(hr) && !count) hr = MF_E_INVALIDMEDIATYPE;
    if (SUCCEEDED(hr)) hr = MFCreateStreamDescriptor(0, count, types, descriptor);
    if (SUCCEEDED(hr) && SUCCEEDED(hr = IMFStreamDescriptor_GetMediaTypeHandler(*descriptor, &handler)))
    {
        hr = IMFMediaTypeHandler_SetCurrentMediaType(handler, types[0]);
        IMFMediaTypeHandler_Release(handler);
    }
    for (i = 0; i < count; i++) IMFMediaType_Release(types[i]);
    return hr;
}

static HRESULT create_camera_source(const char *id, IMFMediaSource **out)
{
    struct camera_source *source;
    HRESULT hr;

    if (!(source = calloc(1, sizeof(*source)))) return E_OUTOFMEMORY;
    source->IMFMediaSource_iface.lpVtbl = &camera_source_vtbl;
    source->IMFMediaStream_iface.lpVtbl = &camera_stream_vtbl;
    source->refcount = 1;
    lstrcpynA(source->id, id, sizeof(source->id));
    list_init(&source->samples);
    list_init(&source->requests);
    InitializeCriticalSectionEx(&source->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    source->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": camera_source.cs");
    source->quit = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (FAILED(hr = MFCreateEventQueue(&source->queue)) ||
            FAILED(hr = MFCreateEventQueue(&source->stream_queue)) ||
            FAILED(hr = create_stream_descriptor(id, &source->stream_descriptor)) ||
            FAILED(hr = MFCreatePresentationDescriptor(1, &source->stream_descriptor, &source->descriptor)) ||
            FAILED(hr = IMFPresentationDescriptor_SelectStream(source->descriptor, 0)))
    {
        IMFMediaSource_Release(&source->IMFMediaSource_iface);
        return hr;
    }
    *out = &source->IMFMediaSource_iface;
    return S_OK;
}

/* Activation objects: the camera's attributes, and its source once activated. */

struct camera_activate
{
    IMFActivate IMFActivate_iface;
    LONG refcount;
    IMFAttributes *attributes;
    char id[128];
    IMFMediaSource *source;
    CRITICAL_SECTION cs;
};

static struct camera_activate *impl_from_IMFActivate(IMFActivate *iface)
{
    return CONTAINING_RECORD(iface, struct camera_activate, IMFActivate_iface);
}

static HRESULT WINAPI camera_activate_QueryInterface(IMFActivate *iface, REFIID riid, void **out)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFActivate) || IsEqualIID(riid, &IID_IMFAttributes) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFActivate_AddRef(iface);
        return S_OK;
    }
    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI camera_activate_AddRef(IMFActivate *iface)
{
    struct camera_activate *activate = impl_from_IMFActivate(iface);
    return InterlockedIncrement(&activate->refcount);
}

static ULONG WINAPI camera_activate_Release(IMFActivate *iface)
{
    struct camera_activate *activate = impl_from_IMFActivate(iface);
    ULONG refcount = InterlockedDecrement(&activate->refcount);

    if (!refcount)
    {
        if (activate->source) IMFMediaSource_Release(activate->source);
        IMFAttributes_Release(activate->attributes);
        activate->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&activate->cs);
        free(activate);
    }
    return refcount;
}

#define FORWARD(name, args, call) \
static HRESULT WINAPI camera_activate_##name args \
{ \
    return IMFAttributes_##name call; \
}
#define ATTRS impl_from_IMFActivate(iface)->attributes

FORWARD(GetItem, (IMFActivate *iface, REFGUID key, PROPVARIANT *value), (ATTRS, key, value))
FORWARD(GetItemType, (IMFActivate *iface, REFGUID key, MF_ATTRIBUTE_TYPE *type), (ATTRS, key, type))
FORWARD(CompareItem, (IMFActivate *iface, REFGUID key, REFPROPVARIANT value, BOOL *result), (ATTRS, key, value, result))
FORWARD(Compare, (IMFActivate *iface, IMFAttributes *theirs, MF_ATTRIBUTES_MATCH_TYPE type, BOOL *result), (ATTRS, theirs, type, result))
FORWARD(GetUINT32, (IMFActivate *iface, REFGUID key, UINT32 *value), (ATTRS, key, value))
FORWARD(GetUINT64, (IMFActivate *iface, REFGUID key, UINT64 *value), (ATTRS, key, value))
FORWARD(GetDouble, (IMFActivate *iface, REFGUID key, double *value), (ATTRS, key, value))
FORWARD(GetGUID, (IMFActivate *iface, REFGUID key, GUID *value), (ATTRS, key, value))
FORWARD(GetStringLength, (IMFActivate *iface, REFGUID key, UINT32 *length), (ATTRS, key, length))
FORWARD(GetString, (IMFActivate *iface, REFGUID key, WCHAR *value, UINT32 size, UINT32 *length), (ATTRS, key, value, size, length))
FORWARD(GetAllocatedString, (IMFActivate *iface, REFGUID key, WCHAR **value, UINT32 *length), (ATTRS, key, value, length))
FORWARD(GetBlobSize, (IMFActivate *iface, REFGUID key, UINT32 *size), (ATTRS, key, size))
FORWARD(GetBlob, (IMFActivate *iface, REFGUID key, UINT8 *buf, UINT32 bufsize, UINT32 *blobsize), (ATTRS, key, buf, bufsize, blobsize))
FORWARD(GetAllocatedBlob, (IMFActivate *iface, REFGUID key, UINT8 **buf, UINT32 *size), (ATTRS, key, buf, size))
FORWARD(GetUnknown, (IMFActivate *iface, REFGUID key, REFIID riid, void **out), (ATTRS, key, riid, out))
FORWARD(SetItem, (IMFActivate *iface, REFGUID key, REFPROPVARIANT value), (ATTRS, key, value))
FORWARD(DeleteItem, (IMFActivate *iface, REFGUID key), (ATTRS, key))
FORWARD(DeleteAllItems, (IMFActivate *iface), (ATTRS))
FORWARD(SetUINT32, (IMFActivate *iface, REFGUID key, UINT32 value), (ATTRS, key, value))
FORWARD(SetUINT64, (IMFActivate *iface, REFGUID key, UINT64 value), (ATTRS, key, value))
FORWARD(SetDouble, (IMFActivate *iface, REFGUID key, double value), (ATTRS, key, value))
FORWARD(SetGUID, (IMFActivate *iface, REFGUID key, REFGUID value), (ATTRS, key, value))
FORWARD(SetString, (IMFActivate *iface, REFGUID key, const WCHAR *value), (ATTRS, key, value))
FORWARD(SetBlob, (IMFActivate *iface, REFGUID key, const UINT8 *buf, UINT32 size), (ATTRS, key, buf, size))
FORWARD(SetUnknown, (IMFActivate *iface, REFGUID key, IUnknown *unknown), (ATTRS, key, unknown))
FORWARD(LockStore, (IMFActivate *iface), (ATTRS))
FORWARD(UnlockStore, (IMFActivate *iface), (ATTRS))
FORWARD(GetCount, (IMFActivate *iface, UINT32 *count), (ATTRS, count))
FORWARD(GetItemByIndex, (IMFActivate *iface, UINT32 index, GUID *key, PROPVARIANT *value), (ATTRS, index, key, value))
FORWARD(CopyAllItems, (IMFActivate *iface, IMFAttributes *dest), (ATTRS, dest))

#undef ATTRS
#undef FORWARD

static HRESULT WINAPI camera_activate_ActivateObject(IMFActivate *iface, REFIID riid, void **out)
{
    struct camera_activate *activate = impl_from_IMFActivate(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), out);

    EnterCriticalSection(&activate->cs);
    if (!activate->source) hr = create_camera_source(activate->id, &activate->source);
    if (SUCCEEDED(hr)) hr = IMFMediaSource_QueryInterface(activate->source, riid, out);
    LeaveCriticalSection(&activate->cs);
    return hr;
}

static HRESULT WINAPI camera_activate_ShutdownObject(IMFActivate *iface)
{
    struct camera_activate *activate = impl_from_IMFActivate(iface);

    TRACE("%p.\n", iface);

    EnterCriticalSection(&activate->cs);
    if (activate->source)
    {
        IMFMediaSource_Shutdown(activate->source);
        IMFMediaSource_Release(activate->source);
        activate->source = NULL;
    }
    LeaveCriticalSection(&activate->cs);
    return S_OK;
}

static HRESULT WINAPI camera_activate_DetachObject(IMFActivate *iface)
{
    struct camera_activate *activate = impl_from_IMFActivate(iface);

    TRACE("%p.\n", iface);

    EnterCriticalSection(&activate->cs);
    if (activate->source) IMFMediaSource_Release(activate->source);
    activate->source = NULL;
    LeaveCriticalSection(&activate->cs);
    return S_OK;
}

static const IMFActivateVtbl camera_activate_vtbl =
{
    camera_activate_QueryInterface,
    camera_activate_AddRef,
    camera_activate_Release,
    camera_activate_GetItem,
    camera_activate_GetItemType,
    camera_activate_CompareItem,
    camera_activate_Compare,
    camera_activate_GetUINT32,
    camera_activate_GetUINT64,
    camera_activate_GetDouble,
    camera_activate_GetGUID,
    camera_activate_GetStringLength,
    camera_activate_GetString,
    camera_activate_GetAllocatedString,
    camera_activate_GetBlobSize,
    camera_activate_GetBlob,
    camera_activate_GetAllocatedBlob,
    camera_activate_GetUnknown,
    camera_activate_SetItem,
    camera_activate_DeleteItem,
    camera_activate_DeleteAllItems,
    camera_activate_SetUINT32,
    camera_activate_SetUINT64,
    camera_activate_SetDouble,
    camera_activate_SetGUID,
    camera_activate_SetString,
    camera_activate_SetBlob,
    camera_activate_SetUnknown,
    camera_activate_LockStore,
    camera_activate_UnlockStore,
    camera_activate_GetCount,
    camera_activate_GetItemByIndex,
    camera_activate_CopyAllItems,
    camera_activate_ActivateObject,
    camera_activate_ShutdownObject,
    camera_activate_DetachObject,
};

static HRESULT create_camera_activate(const struct wg_camera_info *info, IMFActivate **out)
{
    struct camera_activate *activate;
    WCHAR name[ARRAY_SIZE(info->name)], link[ARRAY_SIZE(info->id) + 32];
    HRESULT hr;

    if (!(activate = calloc(1, sizeof(*activate)))) return E_OUTOFMEMORY;
    activate->IMFActivate_iface.lpVtbl = &camera_activate_vtbl;
    activate->refcount = 1;
    lstrcpynA(activate->id, info->id, sizeof(activate->id));
    InitializeCriticalSectionEx(&activate->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    activate->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": camera_activate.cs");

    MultiByteToWideChar(CP_UTF8, 0, info->name, -1, name, ARRAY_SIZE(name));
    camera_link(info->id, link, ARRAY_SIZE(link));
    if (FAILED(hr = MFCreateAttributes(&activate->attributes, 4)) ||
            FAILED(hr = IMFAttributes_SetGUID(activate->attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                    &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) ||
            FAILED(hr = IMFAttributes_SetString(activate->attributes, &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, name)) ||
            FAILED(hr = IMFAttributes_SetString(activate->attributes,
                    &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link)))
    {
        if (activate->attributes) IMFAttributes_Release(activate->attributes);
        activate->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&activate->cs);
        free(activate);
        return hr;
    }
    *out = &activate->IMFActivate_iface;
    return S_OK;
}

/* CLSID_WineVideoCaptureDevices: an IMFCollection with an activation object per camera. */
HRESULT video_capture_devices_create(IUnknown *outer, IUnknown **out)
{
    struct wg_camera_info infos[MAX_CAMERAS];
    struct wg_camera_list_params params = {.infos = (UINT_PTR)infos, .capacity = ARRAY_SIZE(infos)};
    IMFCollection *collection;
    IMFActivate *activate;
    UINT32 i;
    HRESULT hr;

    TRACE("outer %p, out %p.\n", outer, out);

    if (outer) return CLASS_E_NOAGGREGATION;
    if (WINE_UNIX_CALL(unix_wg_camera_list, &params)) return E_FAIL;
    if (FAILED(hr = MFCreateCollection(&collection))) return hr;
    for (i = 0; i < min(params.count, ARRAY_SIZE(infos)); i++)
    {
        TRACE("Camera %s, %s.\n", debugstr_a(infos[i].name), debugstr_a(infos[i].id));
        if (FAILED(create_camera_activate(&infos[i], &activate))) continue;
        IMFCollection_AddElement(collection, (IUnknown *)activate);
        IMFActivate_Release(activate);
    }
    *out = (IUnknown *)collection;
    return S_OK;
}
