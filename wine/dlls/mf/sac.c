/*
 * Copyright 2024 Paul Gofman for CodeWeavers
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

#define COBJMACROS

#include "mfapi.h"
#include "mfidl.h"
#include "mf_private.h"
#include "mmdeviceapi.h"
#include "audioclient.h"
#include "mferror.h"

#include "initguid.h"
#include "devpkey.h"

#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

/* The audio capture source: a live media source with one stream, which reads an endpoint's
 * capture client and hands a sample to each RequestSample. */

enum capture_state
{
    CAPTURE_STOPPED,
    CAPTURE_STARTED,
    CAPTURE_PAUSED,
    CAPTURE_SHUTDOWN,
};

struct captured_sample
{
    struct list entry;
    IMFSample *sample;
};

struct sample_request
{
    struct list entry;
    IUnknown *token;
};

struct audio_capture
{
    IMFMediaSource IMFMediaSource_iface;
    IMFMediaStream IMFMediaStream_iface;
    LONG refcount;

    IMFMediaEventQueue *queue;
    IMFMediaEventQueue *stream_queue;
    IMFPresentationDescriptor *descriptor;
    IMFStreamDescriptor *stream_descriptor;

    IMMDevice *device;
    IAudioClient *client;
    IAudioCaptureClient *capture;
    IMFMediaType *client_type;
    HANDLE ready, quit, thread;
    UINT32 rate, block_align;
    UINT64 frames;
    BOOL discontinuity;

    enum capture_state state;
    BOOL stream_created;
    struct list samples;
    struct list requests;
    CRITICAL_SECTION cs;
};

static struct audio_capture *impl_from_IMFMediaSource(IMFMediaSource *iface)
{
    return CONTAINING_RECORD(iface, struct audio_capture, IMFMediaSource_iface);
}

static struct audio_capture *impl_from_IMFMediaStream(IMFMediaStream *iface)
{
    return CONTAINING_RECORD(iface, struct audio_capture, IMFMediaStream_iface);
}

/* Captured samples go to waiting requests in order. Called with the lock held. */
static void deliver_samples(struct audio_capture *source)
{
    struct captured_sample *captured;
    struct sample_request *request;

    if (source->state != CAPTURE_STARTED) return;
    while (!list_empty(&source->samples) && !list_empty(&source->requests))
    {
        captured = LIST_ENTRY(list_head(&source->samples), struct captured_sample, entry);
        request = LIST_ENTRY(list_head(&source->requests), struct sample_request, entry);
        list_remove(&captured->entry);
        list_remove(&request->entry);

        if (request->token) IMFSample_SetUnknown(captured->sample, &MFSampleExtension_Token, request->token);
        IMFMediaEventQueue_QueueEventParamUnk(source->stream_queue, MEMediaSample, &GUID_NULL, S_OK,
                (IUnknown *)captured->sample);

        IMFSample_Release(captured->sample);
        free(captured);
        if (request->token) IUnknown_Release(request->token);
        free(request);
    }
}

static void flush_capture(struct audio_capture *source)
{
    struct captured_sample *captured, *next_sample;
    struct sample_request *request, *next_request;

    LIST_FOR_EACH_ENTRY_SAFE(captured, next_sample, &source->samples, struct captured_sample, entry)
    {
        list_remove(&captured->entry);
        IMFSample_Release(captured->sample);
        free(captured);
    }
    LIST_FOR_EACH_ENTRY_SAFE(request, next_request, &source->requests, struct sample_request, entry)
    {
        list_remove(&request->entry);
        if (request->token) IUnknown_Release(request->token);
        free(request);
    }
}

/* Reads every packet the capture client has. Called with the lock held. */
static void read_packets(struct audio_capture *source)
{
    UINT32 packet, frames;
    IMFMediaBuffer *buffer;
    IMFSample *sample;
    BYTE *data, *dst;
    DWORD flags;
    HRESULT hr;

    while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(source->capture, &packet)) && packet)
    {
        struct captured_sample *captured;

        if (FAILED(hr = IAudioCaptureClient_GetBuffer(source->capture, &data, &frames, &flags, NULL, NULL)))
            break;
        if (hr == AUDCLNT_S_BUFFER_EMPTY || !frames)
        {
            IAudioCaptureClient_ReleaseBuffer(source->capture, frames);
            break;
        }

        if (SUCCEEDED(hr = MFCreateMemoryBuffer(frames * source->block_align, &buffer)))
        {
            if (SUCCEEDED(hr = IMFMediaBuffer_Lock(buffer, &dst, NULL, NULL)))
            {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(dst, 0, frames * source->block_align);
                else memcpy(dst, data, frames * source->block_align);
                IMFMediaBuffer_Unlock(buffer);
                IMFMediaBuffer_SetCurrentLength(buffer, frames * source->block_align);
            }
            if (SUCCEEDED(hr) && SUCCEEDED(hr = MFCreateSample(&sample)))
            {
                IMFSample_AddBuffer(sample, buffer);
                IMFSample_SetSampleTime(sample, MFllMulDiv(source->frames, 10000000, source->rate, 0));
                IMFSample_SetSampleDuration(sample, MFllMulDiv(frames, 10000000, source->rate, 0));
                if (source->discontinuity || (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY))
                    IMFSample_SetUINT32(sample, &MFSampleExtension_Discontinuity, TRUE);
                source->discontinuity = FALSE;
                if ((captured = malloc(sizeof(*captured))))
                {
                    captured->sample = sample;
                    list_add_tail(&source->samples, &captured->entry);
                }
                else
                    IMFSample_Release(sample);
            }
            IMFMediaBuffer_Release(buffer);
        }
        source->frames += frames;
        IAudioCaptureClient_ReleaseBuffer(source->capture, frames);
        if (FAILED(hr)) break;
    }
}

static DWORD WINAPI capture_thread(void *arg)
{
    struct audio_capture *source = arg;
    HANDLE events[2] = {source->quit, source->ready};

    SetThreadDescription(GetCurrentThread(), L"wine_mf_audio_capture");

    while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
    {
        EnterCriticalSection(&source->cs);
        if (source->state == CAPTURE_STARTED && source->capture)
        {
            read_packets(source);
            deliver_samples(source);
        }
        LeaveCriticalSection(&source->cs);
    }
    return 0;
}

static void release_client(struct audio_capture *source)
{
    if (source->thread)
    {
        SetEvent(source->quit);
        LeaveCriticalSection(&source->cs);
        WaitForSingleObject(source->thread, INFINITE);
        EnterCriticalSection(&source->cs);
        CloseHandle(source->thread);
        source->thread = NULL;
        ResetEvent(source->quit);
    }
    if (source->client) IAudioClient_Stop(source->client);
    if (source->capture) IAudioCaptureClient_Release(source->capture);
    if (source->client) IAudioClient_Release(source->client);
    if (source->client_type) IMFMediaType_Release(source->client_type);
    source->capture = NULL;
    source->client = NULL;
    source->client_type = NULL;
}

/* Sets the endpoint up for the stream's current media type, again if that has changed. */
static HRESULT init_client(struct audio_capture *source)
{
    IMFMediaTypeHandler *handler;
    IMFMediaType *type;
    WAVEFORMATEX *format, *mix;
    UINT32 size;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    BOOL same;
    HRESULT hr;

    if (FAILED(hr = IMFStreamDescriptor_GetMediaTypeHandler(source->stream_descriptor, &handler))) return hr;
    hr = IMFMediaTypeHandler_GetCurrentMediaType(handler, &type);
    IMFMediaTypeHandler_Release(handler);
    if (FAILED(hr)) return hr;

    if (source->client && source->client_type &&
            SUCCEEDED(IMFMediaType_Compare(type, (IMFAttributes *)source->client_type, MF_ATTRIBUTES_MATCH_INTERSECTION, &same)) && same)
    {
        IMFMediaType_Release(type);
        return S_OK;
    }
    release_client(source);

    if (FAILED(hr = MFCreateWaveFormatExFromMFMediaType(type, &format, &size, 0)))
    {
        IMFMediaType_Release(type);
        return hr;
    }
    if (SUCCEEDED(hr = IMMDevice_Activate(source->device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL,
            (void **)&source->client)) && SUCCEEDED(hr = IAudioClient_GetMixFormat(source->client, &mix)))
    {
        if (mix->nSamplesPerSec != format->nSamplesPerSec || mix->nChannels != format->nChannels ||
                mix->wBitsPerSample != format->wBitsPerSample)
            flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        CoTaskMemFree(mix);
        /* A 100 ms buffer, as Windows' capture source uses. */
        hr = IAudioClient_Initialize(source->client, AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, format, NULL);
    }
    if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(source->client, source->ready);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(source->client, &IID_IAudioCaptureClient, (void **)&source->capture);
    if (SUCCEEDED(hr) && !(source->thread = CreateThread(NULL, 0, capture_thread, source, 0, NULL)))
        hr = HRESULT_FROM_WIN32(GetLastError());

    if (SUCCEEDED(hr))
    {
        source->rate = format->nSamplesPerSec;
        source->block_align = format->nBlockAlign;
        source->client_type = type;
        IMFMediaType_AddRef(type);
    }
    else
    {
        WARN("Failed to set up the capture client, hr %#lx.\n", hr);
        release_client(source);
    }
    CoTaskMemFree(format);
    IMFMediaType_Release(type);
    return hr;
}

static HRESULT WINAPI capture_source_QueryInterface(IMFMediaSource *iface, REFIID riid, void **out)
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

static ULONG WINAPI capture_source_AddRef(IMFMediaSource *iface)
{
    struct audio_capture *source = impl_from_IMFMediaSource(iface);
    ULONG refcount = InterlockedIncrement(&source->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG WINAPI capture_source_Release(IMFMediaSource *iface)
{
    struct audio_capture *source = impl_from_IMFMediaSource(iface);
    ULONG refcount = InterlockedDecrement(&source->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        IMFMediaSource_Shutdown(iface);
        IMFMediaEventQueue_Release(source->queue);
        IMFMediaEventQueue_Release(source->stream_queue);
        IMFPresentationDescriptor_Release(source->descriptor);
        IMFStreamDescriptor_Release(source->stream_descriptor);
        CloseHandle(source->ready);
        CloseHandle(source->quit);
        source->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&source->cs);
        free(source);
    }
    return refcount;
}

static HRESULT WINAPI capture_source_GetEvent(IMFMediaSource *iface, DWORD flags, IMFMediaEvent **event)
{
    struct audio_capture *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %#lx, %p.\n", iface, flags, event);
    return IMFMediaEventQueue_GetEvent(source->queue, flags, event);
}

static HRESULT WINAPI capture_source_BeginGetEvent(IMFMediaSource *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    struct audio_capture *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %p, %p.\n", iface, callback, state);
    return IMFMediaEventQueue_BeginGetEvent(source->queue, callback, state);
}

static HRESULT WINAPI capture_source_EndGetEvent(IMFMediaSource *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    struct audio_capture *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %p, %p.\n", iface, result, event);
    return IMFMediaEventQueue_EndGetEvent(source->queue, result, event);
}

static HRESULT WINAPI capture_source_QueueEvent(IMFMediaSource *iface, MediaEventType type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    struct audio_capture *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %lu, %s, %#lx, %p.\n", iface, type, debugstr_guid(ext_type), hr, value);
    return IMFMediaEventQueue_QueueEventParamVar(source->queue, type, ext_type, hr, value);
}

static HRESULT WINAPI capture_source_GetCharacteristics(IMFMediaSource *iface, DWORD *characteristics)
{
    struct audio_capture *source = impl_from_IMFMediaSource(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, characteristics);

    if (!characteristics) return E_POINTER;
    EnterCriticalSection(&source->cs);
    if (source->state == CAPTURE_SHUTDOWN) hr = MF_E_SHUTDOWN;
    else *characteristics = MFMEDIASOURCE_IS_LIVE;
    LeaveCriticalSection(&source->cs);
    return hr;
}

static HRESULT WINAPI capture_source_CreatePresentationDescriptor(IMFMediaSource *iface, IMFPresentationDescriptor **descriptor)
{
    struct audio_capture *source = impl_from_IMFMediaSource(iface);
    HRESULT hr;

    TRACE("%p, %p.\n", iface, descriptor);

    if (!descriptor) return E_POINTER;
    EnterCriticalSection(&source->cs);
    if (source->state == CAPTURE_SHUTDOWN) hr = MF_E_SHUTDOWN;
    else hr = IMFPresentationDescriptor_Clone(source->descriptor, descriptor);
    LeaveCriticalSection(&source->cs);
    return hr;
}

static HRESULT WINAPI capture_source_Start(IMFMediaSource *iface, IMFPresentationDescriptor *descriptor,
        const GUID *time_format, const PROPVARIANT *start_position)
{
    struct audio_capture *source = impl_from_IMFMediaSource(iface);
    IMFStreamDescriptor *stream_descriptor;
    PROPVARIANT time;
    BOOL selected;
    HRESULT hr;

    TRACE("%p, %p, %s, %p.\n", iface, descriptor, debugstr_guid(time_format), start_position);

    if (!descriptor) return E_POINTER;
    if (time_format && !IsEqualGUID(time_format, &GUID_NULL)) return MF_E_UNSUPPORTED_TIME_FORMAT;

    EnterCriticalSection(&source->cs);
    if (source->state == CAPTURE_SHUTDOWN)
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

    if (selected && source->state != CAPTURE_PAUSED && FAILED(hr = init_client(source)))
    {
        LeaveCriticalSection(&source->cs);
        return hr;
    }
    if (selected && source->state == CAPTURE_STOPPED)
    {
        source->frames = 0;
        source->discontinuity = TRUE;
    }
    if (selected && source->state != CAPTURE_STARTED && FAILED(hr = IAudioClient_Start(source->client)))
    {
        LeaveCriticalSection(&source->cs);
        return hr;
    }

    /* A live source starts at the presentation's current time, whatever position is asked for. */
    time.vt = VT_I8;
    time.hVal.QuadPart = MFllMulDiv(source->frames, 10000000, source->rate ? source->rate : 1, 0);
    if (selected)
    {
        IMFMediaEventQueue_QueueEventParamUnk(source->queue, source->stream_created ? MEUpdatedStream : MENewStream,
                &GUID_NULL, S_OK, (IUnknown *)&source->IMFMediaStream_iface);
        source->stream_created = TRUE;
    }
    source->state = CAPTURE_STARTED;
    IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourceStarted, &GUID_NULL, S_OK, &time);
    if (selected)
        IMFMediaEventQueue_QueueEventParamVar(source->stream_queue, MEStreamStarted, &GUID_NULL, S_OK, &time);
    deliver_samples(source);
    LeaveCriticalSection(&source->cs);
    return S_OK;
}

static HRESULT WINAPI capture_source_Stop(IMFMediaSource *iface)
{
    struct audio_capture *source = impl_from_IMFMediaSource(iface);

    TRACE("%p.\n", iface);

    EnterCriticalSection(&source->cs);
    if (source->state == CAPTURE_SHUTDOWN)
    {
        LeaveCriticalSection(&source->cs);
        return MF_E_SHUTDOWN;
    }
    if (source->client)
    {
        IAudioClient_Stop(source->client);
        IAudioClient_Reset(source->client);
    }
    flush_capture(source);
    source->state = CAPTURE_STOPPED;
    IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourceStopped, &GUID_NULL, S_OK, NULL);
    if (source->stream_created)
        IMFMediaEventQueue_QueueEventParamVar(source->stream_queue, MEStreamStopped, &GUID_NULL, S_OK, NULL);
    LeaveCriticalSection(&source->cs);
    return S_OK;
}

static HRESULT WINAPI capture_source_Pause(IMFMediaSource *iface)
{
    struct audio_capture *source = impl_from_IMFMediaSource(iface);
    HRESULT hr = S_OK;

    TRACE("%p.\n", iface);

    EnterCriticalSection(&source->cs);
    if (source->state == CAPTURE_SHUTDOWN) hr = MF_E_SHUTDOWN;
    else if (source->state != CAPTURE_STARTED) hr = MF_E_INVALID_STATE_TRANSITION;
    else
    {
        IAudioClient_Stop(source->client);
        source->state = CAPTURE_PAUSED;
        IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourcePaused, &GUID_NULL, S_OK, NULL);
        IMFMediaEventQueue_QueueEventParamVar(source->stream_queue, MEStreamPaused, &GUID_NULL, S_OK, NULL);
    }
    LeaveCriticalSection(&source->cs);
    return hr;
}

static HRESULT WINAPI capture_source_Shutdown(IMFMediaSource *iface)
{
    struct audio_capture *source = impl_from_IMFMediaSource(iface);

    TRACE("%p.\n", iface);

    EnterCriticalSection(&source->cs);
    if (source->state == CAPTURE_SHUTDOWN)
    {
        LeaveCriticalSection(&source->cs);
        return MF_E_SHUTDOWN;
    }
    source->state = CAPTURE_SHUTDOWN;
    release_client(source);
    flush_capture(source);
    IMFMediaEventQueue_Shutdown(source->queue);
    IMFMediaEventQueue_Shutdown(source->stream_queue);
    if (source->device) IMMDevice_Release(source->device);
    source->device = NULL;
    LeaveCriticalSection(&source->cs);
    return S_OK;
}

static const IMFMediaSourceVtbl capture_source_vtbl =
{
    capture_source_QueryInterface,
    capture_source_AddRef,
    capture_source_Release,
    capture_source_GetEvent,
    capture_source_BeginGetEvent,
    capture_source_EndGetEvent,
    capture_source_QueueEvent,
    capture_source_GetCharacteristics,
    capture_source_CreatePresentationDescriptor,
    capture_source_Start,
    capture_source_Stop,
    capture_source_Pause,
    capture_source_Shutdown,
};

static HRESULT WINAPI capture_stream_QueryInterface(IMFMediaStream *iface, REFIID riid, void **out)
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
static ULONG WINAPI capture_stream_AddRef(IMFMediaStream *iface)
{
    return IMFMediaSource_AddRef(&impl_from_IMFMediaStream(iface)->IMFMediaSource_iface);
}

static ULONG WINAPI capture_stream_Release(IMFMediaStream *iface)
{
    return IMFMediaSource_Release(&impl_from_IMFMediaStream(iface)->IMFMediaSource_iface);
}

static HRESULT WINAPI capture_stream_GetEvent(IMFMediaStream *iface, DWORD flags, IMFMediaEvent **event)
{
    TRACE("%p, %#lx, %p.\n", iface, flags, event);
    return IMFMediaEventQueue_GetEvent(impl_from_IMFMediaStream(iface)->stream_queue, flags, event);
}

static HRESULT WINAPI capture_stream_BeginGetEvent(IMFMediaStream *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    TRACE("%p, %p, %p.\n", iface, callback, state);
    return IMFMediaEventQueue_BeginGetEvent(impl_from_IMFMediaStream(iface)->stream_queue, callback, state);
}

static HRESULT WINAPI capture_stream_EndGetEvent(IMFMediaStream *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    TRACE("%p, %p, %p.\n", iface, result, event);
    return IMFMediaEventQueue_EndGetEvent(impl_from_IMFMediaStream(iface)->stream_queue, result, event);
}

static HRESULT WINAPI capture_stream_QueueEvent(IMFMediaStream *iface, MediaEventType type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    TRACE("%p, %lu, %s, %#lx, %p.\n", iface, type, debugstr_guid(ext_type), hr, value);
    return IMFMediaEventQueue_QueueEventParamVar(impl_from_IMFMediaStream(iface)->stream_queue, type, ext_type, hr, value);
}

static HRESULT WINAPI capture_stream_GetMediaSource(IMFMediaStream *iface, IMFMediaSource **out)
{
    struct audio_capture *source = impl_from_IMFMediaStream(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, out);

    EnterCriticalSection(&source->cs);
    if (source->state == CAPTURE_SHUTDOWN) hr = MF_E_SHUTDOWN;
    else
    {
        *out = &source->IMFMediaSource_iface;
        IMFMediaSource_AddRef(*out);
    }
    LeaveCriticalSection(&source->cs);
    return hr;
}

static HRESULT WINAPI capture_stream_GetStreamDescriptor(IMFMediaStream *iface, IMFStreamDescriptor **descriptor)
{
    struct audio_capture *source = impl_from_IMFMediaStream(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, descriptor);

    EnterCriticalSection(&source->cs);
    if (source->state == CAPTURE_SHUTDOWN) hr = MF_E_SHUTDOWN;
    else
    {
        *descriptor = source->stream_descriptor;
        IMFStreamDescriptor_AddRef(*descriptor);
    }
    LeaveCriticalSection(&source->cs);
    return hr;
}

static HRESULT WINAPI capture_stream_RequestSample(IMFMediaStream *iface, IUnknown *token)
{
    struct audio_capture *source = impl_from_IMFMediaStream(iface);
    struct sample_request *request;
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, token);

    EnterCriticalSection(&source->cs);
    if (source->state == CAPTURE_SHUTDOWN) hr = MF_E_SHUTDOWN;
    else if (source->state == CAPTURE_STOPPED) hr = MF_E_MEDIA_SOURCE_WRONGSTATE;
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

static const IMFMediaStreamVtbl capture_stream_vtbl =
{
    capture_stream_QueryInterface,
    capture_stream_AddRef,
    capture_stream_Release,
    capture_stream_GetEvent,
    capture_stream_BeginGetEvent,
    capture_stream_EndGetEvent,
    capture_stream_QueueEvent,
    capture_stream_GetMediaSource,
    capture_stream_GetStreamDescriptor,
    capture_stream_RequestSample,
};

/* The endpoint's mix format, and 16-bit PCM at its rate and channel count, which the capture
 * client converts to. */
static HRESULT create_stream_descriptor(IMMDevice *device, IMFStreamDescriptor **descriptor)
{
    IMFMediaTypeHandler *handler;
    IMFMediaType *types[2] = {0};
    WAVEFORMATEX *mix, pcm;
    IAudioClient *client;
    unsigned int count = 0, i;
    HRESULT hr;

    if (FAILED(hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&client)))
        return hr;
    hr = IAudioClient_GetMixFormat(client, &mix);
    IAudioClient_Release(client);
    if (FAILED(hr)) return hr;

    if (SUCCEEDED(hr = MFCreateMediaType(&types[0])) &&
            SUCCEEDED(hr = MFInitMediaTypeFromWaveFormatEx(types[0], mix, sizeof(*mix) + mix->cbSize)))
        count = 1;
    if (SUCCEEDED(hr) && mix->wBitsPerSample != 16)
    {
        pcm.wFormatTag = WAVE_FORMAT_PCM;
        pcm.nChannels = mix->nChannels;
        pcm.nSamplesPerSec = mix->nSamplesPerSec;
        pcm.wBitsPerSample = 16;
        pcm.nBlockAlign = pcm.nChannels * 2;
        pcm.nAvgBytesPerSec = pcm.nSamplesPerSec * pcm.nBlockAlign;
        pcm.cbSize = 0;
        if (SUCCEEDED(MFCreateMediaType(&types[1])) && SUCCEEDED(MFInitMediaTypeFromWaveFormatEx(types[1], &pcm, sizeof(pcm))))
            count = 2;
    }
    CoTaskMemFree(mix);

    if (SUCCEEDED(hr)) hr = MFCreateStreamDescriptor(0, count, types, descriptor);
    if (SUCCEEDED(hr) && SUCCEEDED(hr = IMFStreamDescriptor_GetMediaTypeHandler(*descriptor, &handler)))
    {
        hr = IMFMediaTypeHandler_SetCurrentMediaType(handler, types[0]);
        IMFMediaTypeHandler_Release(handler);
    }
    for (i = 0; i < ARRAY_SIZE(types); i++)
        if (types[i]) IMFMediaType_Release(types[i]);
    return hr;
}

static HRESULT get_capture_device(IMFAttributes *attributes, IMMDevice **device)
{
    IMMDeviceEnumerator *devenum;
    WCHAR *id = NULL;
    UINT32 size;
    ERole role;
    HRESULT hr;

    if (FAILED(hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator,
            (void **)&devenum)))
        return hr;
    if (SUCCEEDED(IMFAttributes_GetAllocatedString(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID, &id, &size)))
    {
        hr = IMMDeviceEnumerator_GetDevice(devenum, id, device);
        CoTaskMemFree(id);
    }
    else if (SUCCEEDED(IMFAttributes_GetUINT32(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ROLE, (UINT32 *)&role)))
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(devenum, eCapture, role, device);
    else
        hr = MF_E_ATTRIBUTENOTFOUND;
    IMMDeviceEnumerator_Release(devenum);
    return hr;
}

HRESULT create_audio_capture_source(IMFAttributes *attributes, IMFMediaSource **out)
{
    struct audio_capture *source;
    IMMDevice *device;
    HRESULT hr;

    if (FAILED(hr = get_capture_device(attributes, &device))) return hr;
    if (!(source = calloc(1, sizeof(*source))))
    {
        IMMDevice_Release(device);
        return E_OUTOFMEMORY;
    }
    source->IMFMediaSource_iface.lpVtbl = &capture_source_vtbl;
    source->IMFMediaStream_iface.lpVtbl = &capture_stream_vtbl;
    source->refcount = 1;
    source->device = device;
    list_init(&source->samples);
    list_init(&source->requests);
    InitializeCriticalSectionEx(&source->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    source->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": audio_capture.cs");
    source->ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    source->quit = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (FAILED(hr = MFCreateEventQueue(&source->queue)) ||
            FAILED(hr = MFCreateEventQueue(&source->stream_queue)) ||
            FAILED(hr = create_stream_descriptor(device, &source->stream_descriptor)) ||
            FAILED(hr = MFCreatePresentationDescriptor(1, &source->stream_descriptor, &source->descriptor)) ||
            FAILED(hr = IMFPresentationDescriptor_SelectStream(source->descriptor, 0)))
    {
        IMFMediaSource_Release(&source->IMFMediaSource_iface);
        return hr;
    }
    *out = &source->IMFMediaSource_iface;
    return S_OK;
}

static HRESULT audio_capture_create_object(IMFAttributes *attributes, void *user_context, IUnknown **obj)
{
    TRACE("%p, %p, %p.\n", attributes, user_context, obj);
    return MFCreateDeviceSource(attributes, (IMFMediaSource **)obj);
}

static void audio_capture_shutdown_object(void *user_context, IUnknown *obj)
{
    IMFMediaSource *source;

    TRACE("%p %p.\n", user_context, obj);

    if (SUCCEEDED(IUnknown_QueryInterface(obj, &IID_IMFMediaSource, (void **)&source)))
    {
        IMFMediaSource_Shutdown(source);
        IMFMediaSource_Release(source);
    }
}

static void audio_capture_free_private(void *user_context)
{
    TRACE("%p.\n", user_context);
}

static const struct activate_funcs audio_capture_activate_funcs =
{
    audio_capture_create_object,
    audio_capture_shutdown_object,
    audio_capture_free_private,
};

/***********************************************************************
 *      MFCreateDeviceSource (mf.@)
 */
HRESULT WINAPI MFCreateDeviceSource(IMFAttributes *attributes, IMFMediaSource **source)
{
    GUID type;
    HRESULT hr;

    TRACE("%p, %p.\n", attributes, source);

    if (!attributes || !source) return E_POINTER;
    *source = NULL;
    if (FAILED(hr = IMFAttributes_GetGUID(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &type))) return hr;
    if (IsEqualGUID(&type, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID))
        return create_audio_capture_source(attributes, source);
    if (IsEqualGUID(&type, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))
        return create_video_capture_source(attributes, source);
    return E_INVALIDARG;
}

/***********************************************************************
 *      MFCreateDeviceSourceActivate (mf.@)
 */
HRESULT WINAPI MFCreateDeviceSourceActivate(IMFAttributes *attributes, IMFActivate **activate)
{
    GUID type;
    HRESULT hr;

    TRACE("%p, %p.\n", attributes, activate);

    if (!attributes || !activate) return E_POINTER;
    *activate = NULL;
    if (FAILED(hr = IMFAttributes_GetGUID(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &type))) return hr;
    if (!IsEqualGUID(&type, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID) &&
            !IsEqualGUID(&type, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))
        return E_INVALIDARG;
    if (FAILED(hr = create_activation_object(NULL, &audio_capture_activate_funcs, activate))) return hr;
    if (FAILED(hr = IMFAttributes_CopyAllItems(attributes, (IMFAttributes *)*activate)))
    {
        IMFActivate_Release(*activate);
        *activate = NULL;
    }
    return hr;
}

HRESULT enum_audio_capture_sources(IMFAttributes *attributes, IMFActivate ***ret_sources, UINT32 *ret_count)
{
    IMMDeviceEnumerator *devenum = NULL;
    IMMDeviceCollection *devices = NULL;
    UINT32 i, count, created_count = 0;
    IMFActivate **sources = NULL;
    ERole role;
    HRESULT hr;

    if (FAILED(hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator,
            (void **)&devenum)))
        goto done;
    if (SUCCEEDED(IMFAttributes_GetUINT32(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ROLE, (UINT32 *)&role)))
        FIXME("Specifying role (%d) is not supported.\n", role);
    if (FAILED(hr = IMMDeviceEnumerator_EnumAudioEndpoints(devenum, eCapture, DEVICE_STATE_ACTIVE, &devices)))
        goto done;
    if (FAILED(hr = IMMDeviceCollection_GetCount(devices, &count)))
        goto done;
    sources = CoTaskMemAlloc(count * sizeof(*sources));
    for (i = 0; i < count; ++i)
    {
        IMMDevice *device = NULL;
        IPropertyStore *ps;
        WCHAR *str = NULL;
        PROPVARIANT pv;

        if (FAILED(hr = create_activation_object(NULL, &audio_capture_activate_funcs, &sources[i])))
            goto done;
        ++created_count;

        hr = IMFActivate_SetGUID(sources[i], &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID);
        if (SUCCEEDED(hr))
            hr = IMMDeviceCollection_Item(devices, i, &device);
        if (SUCCEEDED(hr))
            hr = IMMDevice_GetId(device, &str);
        if (SUCCEEDED(hr))
            hr = IMFActivate_SetString(sources[i], &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID, str);
        CoTaskMemFree(str);

        PropVariantInit(&pv);
        if (SUCCEEDED(hr) && SUCCEEDED((hr = IMMDevice_OpenPropertyStore(device, STGM_READ, &ps))))
        {
            hr = IPropertyStore_GetValue(ps, (const PROPERTYKEY*)&DEVPKEY_Device_FriendlyName, &pv);
            IPropertyStore_Release(ps);
        }
        if (device)
            IMMDevice_Release(device);

        if (SUCCEEDED(hr) && pv.vt != VT_LPWSTR)
            hr = E_FAIL;

        if (SUCCEEDED(hr))
            hr = IMFActivate_SetString(sources[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, pv.pwszVal);
        PropVariantClear(&pv);
        if (FAILED(hr))
            goto done;
    }
    *ret_count = count;
    *ret_sources = sources;

done:
    if (FAILED(hr))
    {
        for (i = 0; i < created_count; ++i)
            IMFActivate_Release(sources[i]);
        CoTaskMemFree(sources);
    }
    if (devices)
        IMMDeviceCollection_Release(devices);
    if (devenum)
        IMMDeviceEnumerator_Release(devenum);
    TRACE("ret %#lx, *ret_count %u.\n", hr, *ret_count);
    return hr;
}
