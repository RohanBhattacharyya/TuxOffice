/*
 * Copyright 2020 Nikolay Sivov for CodeWeavers
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

#include <assert.h>

#define COBJMACROS
#include "mfapi.h"
#include "mfidl.h"
#include "mfreadwrite.h"
#include "pathcch.h"
#include "wine/mfinternal.h"
#include "mf_private.h"

#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

enum writer_state
{
    SINK_WRITER_STATE_INITIAL = 0,
    SINK_WRITER_STATE_WRITING,
    SINK_WRITER_STATE_FINALIZING,
};

struct marker_context
{
    IUnknown IUnknown_iface;
    LONG refcount;

    unsigned int marker_type;
    void *user_context;
};

struct pending_item
{
    struct list entry;
    unsigned int marker_type;
    IMFSample *sample;
    LONGLONG timestamp;
    void *user_context;
};

struct stream
{
    IMFStreamSink *stream_sink;
    IMFTransform *converter, *encoder;
    LONGLONG next_time; /* where the last sample out of the transforms ended */
    MF_SINK_WRITER_STATISTICS stats;
    struct list queue; /* struct pending_item. */
};

struct sink_writer
{
    IMFSinkWriterEx IMFSinkWriterEx_iface;
    IMFAsyncCallback events_callback;
    LONG refcount;

    struct
    {
        struct stream *items;
        size_t count;
        size_t capacity;
        DWORD next_id;
    } streams;

    IMFPresentationClock *clock;
    IMFMediaSink *sink;
    enum writer_state state;
    HRESULT status;
    MF_SINK_WRITER_STATISTICS stats;

    IMFAttributes *attributes;
    IMFSinkWriterCallback *callback;
    IMFAsyncCallback finalize_callback;

    CRITICAL_SECTION cs;
    CONDITION_VARIABLE sample_taken;
};

static struct sink_writer *impl_from_IMFSinkWriterEx(IMFSinkWriterEx *iface)
{
    return CONTAINING_RECORD(iface, struct sink_writer, IMFSinkWriterEx_iface);
}

static struct sink_writer *impl_from_events_callback_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct sink_writer, events_callback);
}

static struct sink_writer *impl_from_finalize_callback_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct sink_writer, finalize_callback);
}

static struct marker_context *impl_from_marker_context_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct marker_context, IUnknown_iface);
}

static HRESULT WINAPI marker_context_QueryInterface(IUnknown *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI marker_context_AddRef(IUnknown *iface)
{
    struct marker_context *context = impl_from_marker_context_IUnknown(iface);
    return InterlockedIncrement(&context->refcount);
}

static ULONG WINAPI marker_context_Release(IUnknown *iface)
{
    struct marker_context *context = impl_from_marker_context_IUnknown(iface);
    LONG refcount = InterlockedDecrement(&context->refcount);

    if (!refcount)
        free(context);

    return refcount;
}

static const IUnknownVtbl marker_context_vtbl =
{
    marker_context_QueryInterface,
    marker_context_AddRef,
    marker_context_Release,
};

static struct marker_context *unsafe_impl_from_marker_context_IUnknown(IUnknown *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &marker_context_vtbl);
    return CONTAINING_RECORD(iface, struct marker_context, IUnknown_iface);
}

static HRESULT create_marker_context(unsigned int marker_type, void *user_context,
        IUnknown **ret)
{
    struct marker_context *object;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IUnknown_iface.lpVtbl = &marker_context_vtbl;
    object->refcount = 1;
    object->marker_type = marker_type;
    object->user_context = user_context;

    *ret = &object->IUnknown_iface;

    return S_OK;
}
static void stream_release_transforms(struct stream *stream)
{
    if (stream->converter)
        IMFTransform_Release(stream->converter);
    stream->converter = NULL;

    if (stream->encoder)
        IMFTransform_Release(stream->encoder);
    stream->encoder = NULL;
}

static IMFTransform *stream_get_transform(const struct stream *stream, unsigned int level)
{
    if (stream->converter && !level--) return stream->converter;
    if (stream->encoder && !level) return stream->encoder;
    return NULL;
}

/* Find a transform of the category between the two subtypes that accepts both types. Encoders
 * take their output type first. */
static HRESULT create_transform(const GUID *category, const GUID *major, const GUID *input_subtype,
        const GUID *output_subtype, IMFMediaType *input_type, IMFMediaType *output_type, IMFTransform **ret)
{
    MFT_REGISTER_TYPE_INFO input_info = {*major, *input_subtype}, output_info = {*major, *output_subtype};
    BOOL output_first = IsEqualGUID(category, &MFT_CATEGORY_VIDEO_ENCODER)
            || IsEqualGUID(category, &MFT_CATEGORY_AUDIO_ENCODER);
    IMFActivate **activates;
    IMFTransform *transform;
    UINT32 count = 0, i;
    HRESULT hr;

    *ret = NULL;
    if (FAILED(hr = MFTEnumEx(*category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &input_info, &output_info, &activates, &count)))
        return hr;

    for (i = 0; i < count; ++i)
    {
        if (!*ret && SUCCEEDED(IMFActivate_ActivateObject(activates[i], &IID_IMFTransform, (void **)&transform)))
        {
            if (output_first)
                hr = IMFTransform_SetOutputType(transform, 0, output_type, 0);
            else
                hr = IMFTransform_SetInputType(transform, 0, input_type, 0);
            if (SUCCEEDED(hr))
            {
                if (output_first)
                    hr = IMFTransform_SetInputType(transform, 0, input_type, 0);
                else
                    hr = IMFTransform_SetOutputType(transform, 0, output_type, 0);
            }
            if (SUCCEEDED(hr))
                *ret = transform;
            else
                IMFTransform_Release(transform);
        }
        IMFActivate_Release(activates[i]);
    }
    CoTaskMemFree(activates);

    return *ret ? S_OK : MF_E_TOPO_CODEC_NOT_FOUND;
}

/* A format an encoder takes, given the frame or sample format of what it encodes to: an encoder
 * does not scale or resample, so those come from its output type, or else from the input. */
static HRESULT complete_encoder_input_type(IMFMediaType *available, IMFMediaType *output_type,
        IMFMediaType *input_type, IMFMediaType **ret)
{
    static const GUID *const format[] =
    {
        &MF_MT_FRAME_SIZE, &MF_MT_FRAME_RATE, &MF_MT_PIXEL_ASPECT_RATIO, &MF_MT_INTERLACE_MODE,
        &MF_MT_AUDIO_SAMPLES_PER_SECOND, &MF_MT_AUDIO_NUM_CHANNELS,
    };
    /* Derived from the frame size the available type had. */
    static const GUID *const derived[] =
    {
        &MF_MT_DEFAULT_STRIDE, &MF_MT_SAMPLE_SIZE, &MF_MT_MINIMUM_DISPLAY_APERTURE,
        &MF_MT_GEOMETRIC_APERTURE, &MF_MT_PAN_SCAN_APERTURE,
    };
    PROPVARIANT value;
    unsigned int i;
    HRESULT hr;

    if (FAILED(hr = MFCreateMediaType(ret))) return hr;
    if (FAILED(hr = IMFMediaType_CopyAllItems(available, (IMFAttributes *)*ret)))
    {
        IMFMediaType_Release(*ret);
        return hr;
    }
    for (i = 0; i < ARRAY_SIZE(derived); ++i)
        IMFMediaType_DeleteItem(*ret, derived[i]);
    for (i = 0; i < ARRAY_SIZE(format); ++i)
    {
        PropVariantInit(&value);
        if (SUCCEEDED(IMFMediaType_GetItem(output_type, format[i], &value))
                || (FAILED(IMFMediaType_GetItem(*ret, format[i], NULL))
                && SUCCEEDED(IMFMediaType_GetItem(input_type, format[i], &value))))
            IMFMediaType_SetItem(*ret, format[i], &value);
        PropVariantClear(&value);
    }
    return S_OK;
}

/* Insert a converter, an encoder, or both, so that samples of the input type become samples of the
 * stream's type. */
static HRESULT stream_create_transforms(struct stream *stream, IMFMediaType *input_type, IMFMediaType *output_type)
{
    MFT_REGISTER_TYPE_INFO output_info;
    const GUID *encoder_category, *converter_category;
    GUID major, input_subtype, output_subtype, subtype;
    IMFTransform *encoder, *converter = NULL;
    IMFMediaType *available, *encoder_input;
    IMFActivate **activates;
    UINT32 count = 0, i, j, pass;
    HRESULT hr;

    if (FAILED(hr = IMFMediaType_GetMajorType(input_type, &major))
            || FAILED(hr = IMFMediaType_GetGUID(input_type, &MF_MT_SUBTYPE, &input_subtype))
            || FAILED(hr = IMFMediaType_GetGUID(output_type, &MF_MT_SUBTYPE, &output_subtype)))
        return hr;

    if (IsEqualGUID(&major, &MFMediaType_Video))
    {
        encoder_category = &MFT_CATEGORY_VIDEO_ENCODER;
        converter_category = &MFT_CATEGORY_VIDEO_PROCESSOR;
    }
    else if (IsEqualGUID(&major, &MFMediaType_Audio))
    {
        encoder_category = &MFT_CATEGORY_AUDIO_ENCODER;
        converter_category = &MFT_CATEGORY_AUDIO_EFFECT;
    }
    else
        return MF_E_INVALIDMEDIATYPE;

    /* An uncompressed output only needs a converter. */
    if (SUCCEEDED(create_transform(converter_category, &major, &input_subtype, &output_subtype,
            input_type, output_type, &stream->converter)))
        return S_OK;

    /* An encoder that takes the input as it is. */
    if (SUCCEEDED(create_transform(encoder_category, &major, &input_subtype, &output_subtype,
            input_type, output_type, &stream->encoder)))
        return S_OK;

    /* An encoder behind a converter to one of the formats it takes. */
    output_info.guidMajorType = major;
    output_info.guidSubtype = output_subtype;
    if (FAILED(hr = MFTEnumEx(*encoder_category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT
            | MFT_ENUM_FLAG_SORTANDFILTER, NULL, &output_info, &activates, &count)))
        return hr;

    for (i = 0; i < count; ++i)
    {
        if (!converter && SUCCEEDED(IMFActivate_ActivateObject(activates[i], &IID_IMFTransform, (void **)&encoder)))
        {
            /* NV12 first: it is what encoders take natively, and its layout is the same in Media
             * Foundation and GStreamer at any even width, which is not so for the planar formats. */
            for (pass = 0; pass < 2 && !converter; ++pass)
            {
                if (FAILED(IMFTransform_SetOutputType(encoder, 0, output_type, 0))) break;
                for (j = 0; !converter && SUCCEEDED(IMFTransform_GetInputAvailableType(encoder, 0, j, &available)); ++j)
                {
                    if (SUCCEEDED(IMFMediaType_GetGUID(available, &MF_MT_SUBTYPE, &subtype))
                            && IsEqualGUID(&subtype, &MFVideoFormat_NV12) == !pass
                            && SUCCEEDED(complete_encoder_input_type(available, output_type, input_type, &encoder_input)))
                    {
                        if (SUCCEEDED(IMFTransform_SetInputType(encoder, 0, encoder_input, 0)))
                            create_transform(converter_category, &major, &input_subtype, &subtype,
                                    input_type, encoder_input, &converter);
                        IMFMediaType_Release(encoder_input);
                    }
                    IMFMediaType_Release(available);
                }
            }
            if (converter)
            {
                stream->converter = converter;
                stream->encoder = encoder;
            }
            else
                IMFTransform_Release(encoder);
        }
        IMFActivate_Release(activates[i]);
    }
    CoTaskMemFree(activates);

    if (!converter)
    {
        WARN("No transform from %s to %s.\n", debugstr_guid(&input_subtype), debugstr_guid(&output_subtype));
        return MF_E_TOPO_CODEC_NOT_FOUND;
    }
    return S_OK;
}

/* Get an output sample of a transform, allocating it when the transform does not. */
static HRESULT transform_get_sample(IMFTransform *transform, IMFSample **ret)
{
    MFT_OUTPUT_DATA_BUFFER data = {0};
    MFT_OUTPUT_STREAM_INFO info;
    IMFMediaBuffer *buffer;
    IMFMediaType *type;
    DWORD status;
    HRESULT hr;

    *ret = NULL;
    if (FAILED(hr = IMFTransform_GetOutputStreamInfo(transform, 0, &info))) return hr;
    if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)))
    {
        if (FAILED(hr = MFCreateSample(&data.pSample))) return hr;
        if (FAILED(hr = MFCreateAlignedMemoryBuffer(info.cbSize ? info.cbSize : 0x100000,
                info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer)))
        {
            IMFSample_Release(data.pSample);
            return hr;
        }
        IMFSample_AddBuffer(data.pSample, buffer);
        IMFMediaBuffer_Release(buffer);
    }

    hr = IMFTransform_ProcessOutput(transform, 0, 1, &data, &status);
    if (data.pEvents)
        IMFCollection_Release(data.pEvents);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE && SUCCEEDED(IMFTransform_GetOutputAvailableType(transform, 0, 0, &type)))
    {
        /* Take the format the transform settled on, and ask again. */
        hr = IMFTransform_SetOutputType(transform, 0, type, 0);
        IMFMediaType_Release(type);
        if (data.pSample)
            IMFSample_Release(data.pSample);
        return SUCCEEDED(hr) ? transform_get_sample(transform, ret) : hr;
    }
    if (FAILED(hr))
    {
        if (data.pSample)
            IMFSample_Release(data.pSample);
        return hr;
    }

    *ret = data.pSample;
    return S_OK;
}

static HRESULT stream_get_type(struct stream *stream, IMFMediaType **out_type)
{
    IMFMediaTypeHandler *type_handler;
    HRESULT hr;

    if (FAILED(hr = IMFStreamSink_GetMediaTypeHandler(stream->stream_sink, &type_handler)))
        return hr;
    hr = IMFMediaTypeHandler_GetCurrentMediaType(type_handler, out_type);

    IMFMediaTypeHandler_Release(type_handler);
    return hr;
}

static struct stream *sink_writer_get_stream(const struct sink_writer *writer, DWORD index)
{
    if (index >= writer->streams.count)
        return NULL;
    return &writer->streams.items[index];
}

static void sink_writer_release_pending_item(struct pending_item *item)
{
    list_remove(&item->entry);
    if (item->sample)
        IMFSample_Release(item->sample);
    free(item);
}

static HRESULT WINAPI sink_writer_QueryInterface(IMFSinkWriterEx *iface, REFIID riid, void **out)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFSinkWriterEx)
            || IsEqualIID(riid, &IID_IMFSinkWriter)
            || IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFSinkWriterEx_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI sink_writer_AddRef(IMFSinkWriterEx *iface)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    ULONG refcount = InterlockedIncrement(&writer->refcount);

    TRACE("%p, %lu.\n", iface, refcount);

    return refcount;
}

static void sink_writer_drop_pending_items(struct stream *stream)
{
    struct pending_item *item, *next;

    LIST_FOR_EACH_ENTRY_SAFE(item, next, &stream->queue, struct pending_item, entry)
    {
        sink_writer_release_pending_item(item);
    }
}

static ULONG WINAPI sink_writer_Release(IMFSinkWriterEx *iface)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    ULONG refcount = InterlockedDecrement(&writer->refcount);
    unsigned int i;

    TRACE("%p, %lu.\n", iface, refcount);

    if (!refcount)
    {
        if (writer->clock)
            IMFPresentationClock_Release(writer->clock);
        if (writer->sink)
            IMFMediaSink_Release(writer->sink);
        if (writer->attributes)
            IMFAttributes_Release(writer->attributes);
        if (writer->callback)
            IMFSinkWriterCallback_Release(writer->callback);
        for (i = 0; i < writer->streams.count; ++i)
        {
            struct stream *stream = &writer->streams.items[i];

            if (stream->stream_sink)
                IMFStreamSink_Release(stream->stream_sink);
            stream_release_transforms(stream);
            sink_writer_drop_pending_items(stream);
        }
        DeleteCriticalSection(&writer->cs);
        free(writer);
    }

    return refcount;
}

static HRESULT sink_writer_add_stream(struct sink_writer *writer, IMFStreamSink *stream_sink, DWORD *index)
{
    struct stream *stream;
    DWORD id = 0;
    HRESULT hr;

    if (!mf_array_reserve((void **)&writer->streams.items, &writer->streams.capacity, writer->streams.count + 1,
            sizeof(*writer->streams.items)))
    {
        return E_OUTOFMEMORY;
    }

    if (FAILED(hr = IMFStreamSink_GetIdentifier(stream_sink, &id))) return hr;

    *index = writer->streams.count++;

    stream = &writer->streams.items[*index];
    memset(stream, 0, sizeof(*stream));
    stream->stream_sink = stream_sink;
    IMFStreamSink_AddRef(stream_sink);
    stream->stats.cb = sizeof(stream->stats);
    list_init(&stream->queue);

    writer->streams.next_id = max(writer->streams.next_id, id);

    return hr;
}

static HRESULT WINAPI sink_writer_AddStream(IMFSinkWriterEx *iface, IMFMediaType *media_type, DWORD *index)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    HRESULT hr = MF_E_INVALIDREQUEST;
    IMFStreamSink *stream_sink;
    DWORD id;

    TRACE("%p, %p, %p.\n", iface, media_type, index);

    if (!media_type)
        return E_INVALIDARG;

    if (!index)
        return E_POINTER;

    EnterCriticalSection(&writer->cs);

    if (writer->state == SINK_WRITER_STATE_INITIAL)
    {
        id = writer->streams.next_id + 1;
        if (SUCCEEDED(hr = IMFMediaSink_AddStreamSink(writer->sink, id, media_type, &stream_sink)))
        {
            /* The stream keeps its own reference. */
            if (FAILED(hr = sink_writer_add_stream(writer, stream_sink, index)))
                IMFMediaSink_RemoveStreamSink(writer->sink, id);
            IMFStreamSink_Release(stream_sink);
        }
    }

    LeaveCriticalSection(&writer->cs);

    return hr;
}

static HRESULT WINAPI sink_writer_SetInputMediaType(IMFSinkWriterEx *iface, DWORD index, IMFMediaType *type,
        IMFAttributes *parameters)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    IMFMediaType *stream_type = NULL;
    struct stream *stream;
    DWORD flags;
    HRESULT hr;

    TRACE("%p, %lu, %p, %p.\n", iface, index, type, parameters);

    if (!type)
        return E_INVALIDARG;

    EnterCriticalSection(&writer->cs);

    if (!(stream = sink_writer_get_stream(writer, index)))
    {
        hr = MF_E_INVALIDSTREAMNUMBER;
        goto done;
    }

    if (FAILED(hr = stream_get_type(stream, &stream_type))
            || FAILED(hr = IMFMediaType_IsEqual(type, stream_type, &flags)))
        goto done;

    if (!(flags & MF_MEDIATYPE_EQUAL_MAJOR_TYPES))
    {
        hr = MF_E_INVALIDMEDIATYPE;
        goto done;
    }

    stream_release_transforms(stream);
    hr = S_OK;

    /* Types are not compatible, create transforms: a converter, an encoder or both. */
    if (!(flags & MF_MEDIATYPE_EQUAL_FORMAT_DATA))
        hr = stream_create_transforms(stream, type, stream_type);

done:
    if (stream_type)
        IMFMediaType_Release(stream_type);
    LeaveCriticalSection(&writer->cs);
    return hr;
}

static HRESULT sink_writer_set_presentation_clock(struct sink_writer *writer)
{
    IMFPresentationTimeSource *time_source = NULL;
    HRESULT hr;

    if (FAILED(hr = MFCreatePresentationClock(&writer->clock))) return hr;

    if (FAILED(IMFMediaSink_QueryInterface(writer->sink, &IID_IMFPresentationTimeSource, (void **)&time_source)))
        hr = MFCreateSystemTimeSource(&time_source);

    if (SUCCEEDED(hr = IMFPresentationClock_SetTimeSource(writer->clock, time_source)))
        hr = IMFMediaSink_SetPresentationClock(writer->sink, writer->clock);

    if (time_source)
        IMFPresentationTimeSource_Release(time_source);

    return hr;
}

static HRESULT WINAPI sink_writer_BeginWriting(IMFSinkWriterEx *iface)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    HRESULT hr = S_OK;
    unsigned int i;

    TRACE("%p.\n", iface);

    EnterCriticalSection(&writer->cs);

    if (!writer->streams.count)
        hr = MF_E_INVALIDREQUEST;
    else if (writer->state != SINK_WRITER_STATE_INITIAL)
        hr = MF_E_INVALIDREQUEST;
    else if (SUCCEEDED(hr = sink_writer_set_presentation_clock(writer)))
    {
        for (i = 0; i < writer->streams.count; ++i)
        {
            struct stream *stream = &writer->streams.items[i];

            if (FAILED(hr = IMFStreamSink_BeginGetEvent(stream->stream_sink, &writer->events_callback,
                    (IUnknown *)stream->stream_sink)))
            {
                WARN("Failed to subscribe to events for steam %u, hr %#lx.\n", i, hr);
            }

            if (stream->converter)
                IMFTransform_ProcessMessage(stream->converter, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            if (stream->encoder)
                IMFTransform_ProcessMessage(stream->encoder, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        }

        if (SUCCEEDED(hr))
            hr = IMFPresentationClock_Start(writer->clock, 0);

        writer->state = SINK_WRITER_STATE_WRITING;
    }

    LeaveCriticalSection(&writer->cs);

    return hr;
}

static HRESULT sink_writer_get_buffer_length(IMFSample *sample, LONGLONG *timestamp, DWORD *length)
{
    IMFMediaBuffer *buffer;
    HRESULT hr;

    *timestamp = 0;
    *length = 0;

    if (FAILED(hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer))) return hr;
    hr = IMFMediaBuffer_GetCurrentLength(buffer, length);
    IMFMediaBuffer_Release(buffer);
    if (FAILED(hr)) return hr;
    if (!*length) return E_INVALIDARG;
    IMFSample_GetSampleTime(sample, timestamp);

    return hr;
}

static HRESULT sink_writer_place_marker(struct sink_writer *writer, struct stream *stream, unsigned int marker_type,
        LONGLONG timestamp, void *user_context)
{
    PROPVARIANT value, context;
    IUnknown *context_obj;
    HRESULT hr;

    if (FAILED(hr = create_marker_context(marker_type, user_context, &context_obj))) return hr;

    context.vt = VT_UNKNOWN;
    context.punkVal = context_obj;
    value.vt = VT_I8;
    value.hVal.QuadPart = timestamp;
    hr = IMFStreamSink_PlaceMarker(stream->stream_sink, marker_type, marker_type == MFSTREAMSINK_MARKER_TICK ? &value : NULL,
            &context);
    IUnknown_Release(context_obj);
    return hr;
}

static HRESULT sink_writer_queue_marker(struct sink_writer *writer, struct stream *stream, unsigned int marker_type,
        LONGLONG timestamp, void *user_context)
{
    struct pending_item *item;

    if (list_empty(&stream->queue))
        return sink_writer_place_marker(writer, stream, marker_type, timestamp, user_context);

    if (!(item = calloc(1, sizeof(*item))))
        return E_OUTOFMEMORY;

    item->marker_type = marker_type;
    item->timestamp = timestamp;
    list_add_tail(&stream->queue, &item->entry);

    return S_OK;
}

static HRESULT sink_writer_send_stream_tick(struct sink_writer *writer, unsigned int index, LONGLONG timestamp)
{
    struct stream *stream;

    if (!(stream = sink_writer_get_stream(writer, index))) return MF_E_INVALIDSTREAMNUMBER;

    writer->stats.llLastStreamTickReceived = timestamp;
    writer->stats.qwNumStreamTicksReceived++;
    stream->stats.llLastStreamTickReceived = timestamp;
    stream->stats.qwNumStreamTicksReceived++;

    return sink_writer_queue_marker(writer, stream, MFSTREAMSINK_MARKER_TICK, timestamp, NULL);
}

static HRESULT sink_writer_notify_end_of_segment(struct sink_writer *writer, unsigned int index)
{
    struct stream *stream;

    if (!(stream = sink_writer_get_stream(writer, index))) return MF_E_INVALIDSTREAMNUMBER;

    return sink_writer_queue_marker(writer, stream, MFSTREAMSINK_MARKER_ENDOFSEGMENT, 0, NULL);
}

static HRESULT sink_writer_flush(struct sink_writer *writer, unsigned int index)
{
    struct stream *stream;

    if (!(stream = sink_writer_get_stream(writer, index))) return MF_E_INVALIDSTREAMNUMBER;

    sink_writer_drop_pending_items(stream);

    IMFStreamSink_Flush(stream->stream_sink);

    if (stream->converter)
        IMFTransform_ProcessMessage(stream->converter, MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (stream->encoder)
        IMFTransform_ProcessMessage(stream->encoder, MFT_MESSAGE_COMMAND_FLUSH, 0);

    return sink_writer_place_marker(writer, stream, MFSTREAMSINK_MARKER_ENDOFSEGMENT, 0, NULL);
}

static BOOL stream_has_samples(const struct stream *stream)
{
    struct pending_item *item;

    LIST_FOR_EACH_ENTRY(item, &stream->queue, struct pending_item, entry)
        if (item->sample) return TRUE;
    return FALSE;
}

static HRESULT sink_writer_process_sample(struct sink_writer *writer, struct stream *stream)
{
    struct pending_item *item, *next;
    LONGLONG timestamp;
    IMFSample *sample;
    HRESULT hr;

    if (list_empty(&stream->queue)) return S_OK;
    item = LIST_ENTRY(list_head(&stream->queue), struct pending_item, entry);
    if (!item->sample) return S_OK;

    IMFSample_AddRef((sample = item->sample));
    sink_writer_release_pending_item(item);

    writer->stats.dwNumOutstandingSinkSampleRequests--;
    stream->stats.dwNumOutstandingSinkSampleRequests--;

    if (FAILED(hr = IMFSample_GetSampleTime(sample, &timestamp)))
    {
        IMFSample_Release(sample);
        writer->status = hr;
        return S_OK;
    }

    writer->stats.llLastTimestampProcessed = timestamp;
    stream->stats.llLastTimestampProcessed = timestamp;

    hr = IMFStreamSink_ProcessSample(stream->stream_sink, sample);
    IMFSample_Release(sample);
    if (FAILED(hr)) return hr;

    LIST_FOR_EACH_ENTRY_SAFE(item, next, &stream->queue, struct pending_item, entry)
    {
        if (item->sample) break;
        sink_writer_place_marker(writer, stream, item->marker_type, item->timestamp, item->user_context);
        sink_writer_release_pending_item(item);
    }

    return hr;
}

static HRESULT stream_queue_sample(struct stream *stream, IMFSample *sample)
{
    struct pending_item *item;

    if (!(item = calloc(1, sizeof(*item))))
        return E_OUTOFMEMORY;

    item->sample = sample;
    IMFSample_AddRef(item->sample);
    list_add_tail(&stream->queue, &item->entry);

    return S_OK;
}

static HRESULT stream_push_sample(struct stream *stream, unsigned int level, IMFSample *sample);

/* Pass everything a transform has ready on to the next one, or to the queue. */
static HRESULT stream_pull_samples(struct stream *stream, unsigned int level)
{
    IMFTransform *transform = stream_get_transform(stream, level);
    IMFSample *sample;
    HRESULT hr;

    while (SUCCEEDED(hr = transform_get_sample(transform, &sample)))
    {
        LONGLONG time, duration;

        /* Muxers need a valid time on every sample; some encoders leave one out, or give the last
         * one drained a negative time. */
        if (FAILED(IMFSample_GetSampleTime(sample, &time)) || time < 0)
        {
            TRACE("Sample %p from transform %p has no time, using %s.\n", sample, transform,
                    wine_dbgstr_longlong(stream->next_time));
            IMFSample_SetSampleTime(sample, (time = stream->next_time));
        }
        if (SUCCEEDED(IMFSample_GetSampleDuration(sample, &duration)))
            stream->next_time = time + duration;
        else
            stream->next_time = time;

        hr = stream_push_sample(stream, level + 1, sample);
        IMFSample_Release(sample);
        if (FAILED(hr)) return hr;
    }

    return hr == MF_E_TRANSFORM_NEED_MORE_INPUT ? S_OK : hr;
}

static HRESULT stream_push_sample(struct stream *stream, unsigned int level, IMFSample *sample)
{
    IMFTransform *transform;
    HRESULT hr;

    if (!(transform = stream_get_transform(stream, level)))
        return stream_queue_sample(stream, sample);

    if ((hr = IMFTransform_ProcessInput(transform, 0, sample, 0)) == MF_E_NOTACCEPTING)
    {
        if (FAILED(hr = stream_pull_samples(stream, level))) return hr;
        hr = IMFTransform_ProcessInput(transform, 0, sample, 0);
    }
    if (FAILED(hr))
    {
        WARN("Transform %p did not take the sample, hr %#lx.\n", transform, hr);
        return hr;
    }

    return stream_pull_samples(stream, level);
}

/* Let each transform give up what it holds, in order, so that the last samples reach the queue. */
static HRESULT stream_drain_transforms(struct stream *stream)
{
    IMFTransform *transform;
    unsigned int level;
    HRESULT hr;

    for (level = 0; (transform = stream_get_transform(stream, level)); ++level)
    {
        IMFTransform_ProcessMessage(transform, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (FAILED(hr = IMFTransform_ProcessMessage(transform, MFT_MESSAGE_COMMAND_DRAIN, 0)))
            return hr;
        if (FAILED(hr = stream_pull_samples(stream, level)))
            return hr;
    }

    return S_OK;
}

static HRESULT sink_writer_encode_sample(struct sink_writer *writer, struct stream *stream, IMFSample *sample)
{
    return stream_push_sample(stream, 0, sample);
}

static HRESULT sink_writer_write_sample(struct sink_writer *writer, struct stream *stream, IMFSample *sample)
{
    LONGLONG timestamp;
    DWORD length;
    HRESULT hr;

    if (FAILED(hr = sink_writer_get_buffer_length(sample, &timestamp, &length))) return hr;

    stream->stats.llLastTimestampReceived = timestamp;
    stream->stats.qwNumSamplesReceived++;
    stream->stats.dwByteCountQueued += length;

    writer->stats.llLastTimestampReceived = timestamp;
    writer->stats.qwNumSamplesReceived++;
    writer->stats.dwByteCountQueued += length;

    if (FAILED(hr = sink_writer_encode_sample(writer, stream, sample))) return hr;

    /* An encoder may give no sample for this one, or several. */
    while (SUCCEEDED(hr) && stream->stats.dwNumOutstandingSinkSampleRequests && stream_has_samples(stream))
        hr = sink_writer_process_sample(writer, stream);

    return hr;
}

static HRESULT WINAPI sink_writer_WriteSample(IMFSinkWriterEx *iface, DWORD index, IMFSample *sample)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    struct stream *stream;
    LONGLONG timestamp;
    DWORD length;
    HRESULT hr;

    TRACE("%p, %lu, %p.\n", iface, index, sample);

    if (!sample)
        return E_INVALIDARG;

    EnterCriticalSection(&writer->cs);

    if (writer->state != SINK_WRITER_STATE_WRITING)
        hr = MF_E_INVALIDREQUEST;
    else if (!(stream = sink_writer_get_stream(writer, index)))
    {
        hr = MF_E_INVALIDSTREAMNUMBER;
    }
    else if (SUCCEEDED(hr = sink_writer_get_buffer_length(sample, &timestamp, &length)))
    {
        stream->stats.llLastTimestampReceived = timestamp;
        stream->stats.qwNumSamplesReceived++;
        stream->stats.dwByteCountQueued += length;

        writer->stats.llLastTimestampReceived = timestamp;
        writer->stats.qwNumSamplesReceived++;
        writer->stats.dwByteCountQueued += length;

        hr = sink_writer_write_sample(writer, stream, sample);
    }

    LeaveCriticalSection(&writer->cs);

    return hr;
}

static HRESULT WINAPI sink_writer_SendStreamTick(IMFSinkWriterEx *iface, DWORD index, LONGLONG timestamp)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    HRESULT hr;

    TRACE("%p, %lu, %s.\n", iface, index, wine_dbgstr_longlong(timestamp));

    EnterCriticalSection(&writer->cs);

    if (writer->state != SINK_WRITER_STATE_WRITING)
        hr = MF_E_INVALIDREQUEST;
    else
        hr = sink_writer_send_stream_tick(writer, index, timestamp);

    LeaveCriticalSection(&writer->cs);

    return hr;
}

static HRESULT WINAPI sink_writer_PlaceMarker(IMFSinkWriterEx *iface, DWORD index, void *user_context)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    struct stream *stream;
    HRESULT hr;

    TRACE("%p, %lu, %p.\n", iface, index, user_context);

    EnterCriticalSection(&writer->cs);

    if (!writer->callback)
        hr = MF_E_INVALIDREQUEST;
    else if (writer->state != SINK_WRITER_STATE_WRITING)
        hr = MF_E_INVALIDREQUEST;
    else if (!(stream = sink_writer_get_stream(writer, index)))
        hr = MF_E_INVALIDSTREAMNUMBER;
    else
        hr = sink_writer_queue_marker(writer, stream, MFSTREAMSINK_MARKER_DEFAULT, 0, user_context);

    LeaveCriticalSection(&writer->cs);

    return hr;
}

static HRESULT WINAPI sink_writer_NotifyEndOfSegment(IMFSinkWriterEx *iface, DWORD index)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    HRESULT hr = S_OK;
    unsigned int i;

    TRACE("%p, %lu.\n", iface, index);

    EnterCriticalSection(&writer->cs);

    if (writer->state != SINK_WRITER_STATE_WRITING)
        hr = MF_E_INVALIDREQUEST;
    else if (index == MF_SINK_WRITER_ALL_STREAMS)
    {
        for (i = 0; i < writer->streams.count; ++i)
        {
            if (FAILED(hr = sink_writer_notify_end_of_segment(writer, index)))
            {
                WARN("Failed to place a marker for stream %u.\n", i);
                break;
            }
        }
    }
    else
        hr = sink_writer_notify_end_of_segment(writer, index);

    LeaveCriticalSection(&writer->cs);

    return hr;
}

static HRESULT WINAPI sink_writer_Flush(IMFSinkWriterEx *iface, DWORD index)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    HRESULT hr = S_OK;
    unsigned int i;

    TRACE("%p, %lu.\n", iface, index);

    EnterCriticalSection(&writer->cs);

    if (writer->state != SINK_WRITER_STATE_WRITING)
        hr = MF_E_INVALIDREQUEST;
    else if (index == MF_SINK_WRITER_ALL_STREAMS)
    {
        for (i = 0; i < writer->streams.count; ++i)
        {
            if (FAILED(hr = sink_writer_flush(writer, i)))
            {
                WARN("Failed to flush stream %u.\n", i);
                break;
            }
        }
    }
    else
        hr = sink_writer_flush(writer, index);

    LeaveCriticalSection(&writer->cs);

    return hr;
}

struct sink_finalize
{
    IMFAsyncCallback IMFAsyncCallback_iface;
    LONG refcount;
    IMFFinalizableMediaSink *sink;
    HANDLE done;
    HRESULT hr;
};

static struct sink_finalize *impl_from_sink_finalize_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct sink_finalize, IMFAsyncCallback_iface);
}

static HRESULT WINAPI sink_finalize_QueryInterface(IMFAsyncCallback *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFAsyncCallback) || IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFAsyncCallback_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI sink_finalize_AddRef(IMFAsyncCallback *iface)
{
    return InterlockedIncrement(&impl_from_sink_finalize_IMFAsyncCallback(iface)->refcount);
}

static ULONG WINAPI sink_finalize_Release(IMFAsyncCallback *iface)
{
    struct sink_finalize *finalize = impl_from_sink_finalize_IMFAsyncCallback(iface);
    LONG refcount = InterlockedDecrement(&finalize->refcount);

    if (!refcount)
    {
        IMFFinalizableMediaSink_Release(finalize->sink);
        CloseHandle(finalize->done);
        free(finalize);
    }
    return refcount;
}

static HRESULT WINAPI sink_finalize_GetParameters(IMFAsyncCallback *iface, DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI sink_finalize_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct sink_finalize *finalize = impl_from_sink_finalize_IMFAsyncCallback(iface);

    finalize->hr = IMFFinalizableMediaSink_EndFinalize(finalize->sink, result);
    SetEvent(finalize->done);
    return S_OK;
}

static const IMFAsyncCallbackVtbl sink_finalize_vtbl =
{
    sink_finalize_QueryInterface,
    sink_finalize_AddRef,
    sink_finalize_Release,
    sink_finalize_GetParameters,
    sink_finalize_Invoke,
};

/* Finalize a sink that can be, and wait for it. */
static HRESULT sink_writer_finalize_sink(struct sink_writer *writer)
{
    struct sink_finalize *finalize;
    IMFFinalizableMediaSink *sink;
    HRESULT hr;

    if (FAILED(IMFMediaSink_QueryInterface(writer->sink, &IID_IMFFinalizableMediaSink, (void **)&sink)))
        return S_OK;

    if (!(finalize = calloc(1, sizeof(*finalize))))
    {
        IMFFinalizableMediaSink_Release(sink);
        return E_OUTOFMEMORY;
    }
    finalize->IMFAsyncCallback_iface.lpVtbl = &sink_finalize_vtbl;
    finalize->refcount = 1;
    finalize->sink = sink;
    finalize->done = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (SUCCEEDED(hr = IMFFinalizableMediaSink_BeginFinalize(sink, &finalize->IMFAsyncCallback_iface, NULL)))
    {
        WaitForSingleObject(finalize->done, INFINITE);
        hr = finalize->hr;
    }
    IMFAsyncCallback_Release(&finalize->IMFAsyncCallback_iface);
    return hr;
}

/* Run the remaining samples through the transforms, wait for the sink to take them all, and
 * finalize the sink. Called without the lock held. */
static HRESULT sink_writer_finalize(struct sink_writer *writer)
{
    ULONGLONG idle_since = GetTickCount64();
    struct stream *stream;
    HRESULT hr = S_OK;
    unsigned int i;
    BOOL pending;

    EnterCriticalSection(&writer->cs);

    for (i = 0; i < writer->streams.count && SUCCEEDED(hr); ++i)
    {
        stream = &writer->streams.items[i];
        if (SUCCEEDED(hr = stream_drain_transforms(stream)))
        {
            while (SUCCEEDED(hr) && stream->stats.dwNumOutstandingSinkSampleRequests && stream_has_samples(stream))
                hr = sink_writer_process_sample(writer, stream);
        }
    }

    /* The sink asks for samples from its own thread; give up if it stops asking. */
    for (;;)
    {
        for (i = 0, pending = FALSE; i < writer->streams.count; ++i)
            pending |= stream_has_samples(&writer->streams.items[i]);
        if (!pending || FAILED(hr) || FAILED(writer->status)) break;
        if (SleepConditionVariableCS(&writer->sample_taken, &writer->cs, 1000))
            idle_since = GetTickCount64();
        else if (GetTickCount64() - idle_since > 30000)
        {
            WARN("The sink stopped taking samples.\n");
            hr = MF_E_SINK_NO_SAMPLES_PROCESSED;
            break;
        }
    }
    if (SUCCEEDED(hr) && FAILED(writer->status))
        hr = writer->status;

    LeaveCriticalSection(&writer->cs);

    if (SUCCEEDED(hr))
        hr = sink_writer_finalize_sink(writer);
    if (writer->clock)
        IMFPresentationClock_Stop(writer->clock);
    IMFMediaSink_Shutdown(writer->sink);

    return hr;
}

static HRESULT WINAPI sink_writer_finalize_callback_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct sink_writer *writer = impl_from_finalize_callback_IMFAsyncCallback(iface);
    HRESULT hr = sink_writer_finalize(writer);

    IMFSinkWriterCallback_OnFinalize(writer->callback, hr);
    return S_OK;
}

static HRESULT WINAPI sink_writer_Finalize(IMFSinkWriterEx *iface)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    HRESULT hr = S_OK;

    TRACE("%p.\n", iface);

    EnterCriticalSection(&writer->cs);
    if (writer->state != SINK_WRITER_STATE_WRITING)
        hr = MF_E_INVALIDREQUEST;
    else
        writer->state = SINK_WRITER_STATE_FINALIZING;
    LeaveCriticalSection(&writer->cs);
    if (FAILED(hr)) return hr;

    /* With a callback the writer finalizes in the background and reports through it. */
    if (writer->callback)
        return MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, &writer->finalize_callback, NULL);

    return sink_writer_finalize(writer);
}

static HRESULT sink_writer_get_service(void *object, REFGUID service, REFIID riid, void **ret)
{
    IUnknown *iface = object;
    IMFGetService *gs;
    HRESULT hr;

    if (!iface) return MF_E_UNSUPPORTED_SERVICE;

    if (IsEqualGUID(service, &GUID_NULL))
        return IUnknown_QueryInterface(iface, riid, ret);

    if (FAILED(hr = IUnknown_QueryInterface(iface, &IID_IMFGetService, (void **)&gs)))
        return hr;

    hr = IMFGetService_GetService(gs, service, riid, ret);
    IMFGetService_Release(gs);
    return hr;
}

static HRESULT WINAPI sink_writer_GetServiceForStream(IMFSinkWriterEx *iface, DWORD index, REFGUID service,
        REFIID riid, void **object)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    HRESULT hr = E_UNEXPECTED;
    struct stream *stream;

    TRACE("%p, %lu, %s, %s, %p.\n", iface, index, debugstr_guid(service), debugstr_guid(riid), object);

    EnterCriticalSection(&writer->cs);

    if (index == MF_SINK_WRITER_MEDIASINK)
        hr = sink_writer_get_service(writer->sink, service, riid, object);
    else if ((stream = sink_writer_get_stream(writer, index)))
    {
        if (stream->encoder)
            hr = sink_writer_get_service(stream->encoder, service, riid, object);
        else if (stream->converter)
            hr = sink_writer_get_service(stream->converter, service, riid, object);

        if (FAILED(hr))
            hr = sink_writer_get_service(stream->stream_sink, service, riid, object);
    }
    else
        hr = MF_E_INVALIDSTREAMNUMBER;

    LeaveCriticalSection(&writer->cs);

    return hr;
}

static HRESULT WINAPI sink_writer_GetStatistics(IMFSinkWriterEx *iface, DWORD index, MF_SINK_WRITER_STATISTICS *stats)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    struct stream *stream;
    HRESULT hr = S_OK;

    TRACE("%p, %lu, %p.\n", iface, index, stats);

    if (!stats)
        return E_POINTER;

    if (stats->cb != sizeof(*stats))
        return E_INVALIDARG;

    EnterCriticalSection(&writer->cs);

    if (FAILED(writer->status))
        hr = writer->status;
    else if (index == MF_SINK_WRITER_ALL_STREAMS)
        *stats = writer->stats;
    else if ((stream = sink_writer_get_stream(writer, index)))
        *stats = stream->stats;
    else
        hr = MF_E_INVALIDSTREAMNUMBER;

    LeaveCriticalSection(&writer->cs);

    return hr;
}

static HRESULT WINAPI sink_writer_GetTransformForStream(IMFSinkWriterEx *iface,
        DWORD stream_index, DWORD transform_index, GUID *out_category, IMFTransform **out_transform)
{
    struct sink_writer *writer = impl_from_IMFSinkWriterEx(iface);
    IMFTransform *transform = NULL;
    struct stream *stream;
    GUID category = {};
    HRESULT hr;

    TRACE("%p, %#lx, %#lx, %p, %p.\n", iface, stream_index, transform_index, out_category, out_transform);

    if (!out_transform)
        return E_POINTER;
    *out_transform = NULL;

    EnterCriticalSection(&writer->cs);

    if (!(stream = sink_writer_get_stream(writer, stream_index)))
    {
        LeaveCriticalSection(&writer->cs);
        return MF_E_INVALIDSTREAMNUMBER;
    }

    if (stream->converter && transform_index == 0)
    {
        transform = stream->converter;
        category = MFT_CATEGORY_VIDEO_PROCESSOR;
    }
    else if ((stream->converter && transform_index == 1)
            || (!stream->converter && transform_index == 0))
    {
        transform = stream->encoder;
        category = MFT_CATEGORY_VIDEO_ENCODER;
    }

    hr = MF_E_INVALIDINDEX;
    if (transform)
    {
        IMFTransform_AddRef((*out_transform = transform));
        if (out_category)
            *out_category = category;
        hr = S_OK;
    }

    LeaveCriticalSection(&writer->cs);
    return hr;
}

static const IMFSinkWriterExVtbl sink_writer_vtbl =
{
    sink_writer_QueryInterface,
    sink_writer_AddRef,
    sink_writer_Release,
    sink_writer_AddStream,
    sink_writer_SetInputMediaType,
    sink_writer_BeginWriting,
    sink_writer_WriteSample,
    sink_writer_SendStreamTick,
    sink_writer_PlaceMarker,
    sink_writer_NotifyEndOfSegment,
    sink_writer_Flush,
    sink_writer_Finalize,
    sink_writer_GetServiceForStream,
    sink_writer_GetStatistics,
    sink_writer_GetTransformForStream,
};

static HRESULT WINAPI sink_writer_callback_QueryInterface(IMFAsyncCallback *iface,
        REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFAsyncCallback) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFAsyncCallback_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI sink_writer_events_callback_AddRef(IMFAsyncCallback *iface)
{
    struct sink_writer *writer = impl_from_events_callback_IMFAsyncCallback(iface);
    return IMFSinkWriterEx_AddRef(&writer->IMFSinkWriterEx_iface);
}

static ULONG WINAPI sink_writer_events_callback_Release(IMFAsyncCallback *iface)
{
    struct sink_writer *writer = impl_from_events_callback_IMFAsyncCallback(iface);
    return IMFSinkWriterEx_Release(&writer->IMFSinkWriterEx_iface);
}

static HRESULT WINAPI sink_writer_callback_GetParameters(IMFAsyncCallback *iface,
        DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static struct stream *sink_writer_get_stream_for_stream_sink(struct sink_writer *writer,
        IMFStreamSink *stream_sink, DWORD *index)
{
    unsigned int i;

    for (i = 0; i < writer->streams.count; ++i)
    {
        if (writer->streams.items[i].stream_sink == stream_sink)
        {
            *index = i;
            return &writer->streams.items[i];
        }
    }

    return NULL;
}

static HRESULT WINAPI sink_writer_events_callback_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct sink_writer *writer = impl_from_events_callback_IMFAsyncCallback(iface);
    struct marker_context *context;
    IMFStreamSink *stream_sink;
    MediaEventType event_type;
    struct stream *stream;
    IMFMediaEvent *event;
    LONGLONG timestamp;
    HRESULT status, hr;
    PROPVARIANT value;
    DWORD index;

    TRACE("%p, %p.\n", iface, result);

    stream_sink = (IMFStreamSink *)IMFAsyncResult_GetStateNoAddRef(result);

    if (FAILED(hr = IMFStreamSink_EndGetEvent(stream_sink, result, &event)))
        return hr;

    IMFMediaEvent_GetType(event, &event_type);
    IMFMediaEvent_GetStatus(event, &status);

    TRACE("Got event %lu.\n", event_type);

    PropVariantInit(&value);

    EnterCriticalSection(&writer->cs);

    if (writer->status == S_OK && FAILED(status))
        writer->status = status;

    if (writer->status == S_OK && (stream = sink_writer_get_stream_for_stream_sink(writer, stream_sink, &index)))
    {
        switch (event_type)
        {
            case MEStreamSinkRequestSample:

                timestamp = MFGetSystemTime();

                writer->stats.llLastSinkSampleRequest = timestamp;
                writer->stats.dwNumOutstandingSinkSampleRequests++;
                stream->stats.llLastSinkSampleRequest = timestamp;
                stream->stats.dwNumOutstandingSinkSampleRequests++;

                sink_writer_process_sample(writer, stream);
                WakeAllConditionVariable(&writer->sample_taken);

                break;

            case MEStreamSinkMarker:
                if (FAILED(hr = IMFMediaEvent_GetValue(event, &value))) break;
                if (value.vt != VT_UNKNOWN || !(context = unsafe_impl_from_marker_context_IUnknown(value.punkVal))) break;

                /* This relies on the fact that default marker type is only used for PlaceMarker(). */
                if (context->marker_type == MFSTREAMSINK_MARKER_DEFAULT)
                    IMFSinkWriterCallback_OnMarker(writer->callback, index, context->user_context);

                break;

            default:
                ;
        }
    }

    LeaveCriticalSection(&writer->cs);

    PropVariantClear(&value);

    IMFMediaEvent_Release(event);

    IMFStreamSink_BeginGetEvent(stream_sink, iface, (IUnknown *)stream_sink);

    return S_OK;
}

static const IMFAsyncCallbackVtbl sink_writer_events_callback_vtbl =
{
    sink_writer_callback_QueryInterface,
    sink_writer_events_callback_AddRef,
    sink_writer_events_callback_Release,
    sink_writer_callback_GetParameters,
    sink_writer_events_callback_Invoke,
};

static ULONG WINAPI sink_writer_finalize_callback_AddRef(IMFAsyncCallback *iface)
{
    struct sink_writer *writer = impl_from_finalize_callback_IMFAsyncCallback(iface);
    return IMFSinkWriterEx_AddRef(&writer->IMFSinkWriterEx_iface);
}

static ULONG WINAPI sink_writer_finalize_callback_Release(IMFAsyncCallback *iface)
{
    struct sink_writer *writer = impl_from_finalize_callback_IMFAsyncCallback(iface);
    return IMFSinkWriterEx_Release(&writer->IMFSinkWriterEx_iface);
}

static const IMFAsyncCallbackVtbl sink_writer_finalize_callback_vtbl =
{
    sink_writer_callback_QueryInterface,
    sink_writer_finalize_callback_AddRef,
    sink_writer_finalize_callback_Release,
    sink_writer_callback_GetParameters,
    sink_writer_finalize_callback_Invoke,
};

static HRESULT sink_writer_initialize_existing_streams(struct sink_writer *writer, IMFMediaSink *sink)
{
    IMFStreamSink *stream_sink;
    DWORD count = 0, i, index;
    HRESULT hr;

    if (FAILED(hr = IMFMediaSink_GetStreamSinkCount(sink, &count))) return hr;
    if (!count) return S_OK;

    for (i = 0; i < count; ++i)
    {
        if (FAILED(hr = IMFMediaSink_GetStreamSinkByIndex(sink, i, &stream_sink))) break;
        hr = sink_writer_add_stream(writer, stream_sink, &index);
        IMFStreamSink_Release(stream_sink);
        if (FAILED(hr)) break;
    }

    return hr;
}

HRESULT create_sink_writer_from_sink(IMFMediaSink *sink, IMFAttributes *attributes,
        REFIID riid, void **out)
{
    struct sink_writer *object;
    HRESULT hr;

    *out = NULL;

    if (!sink)
        return E_INVALIDARG;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IMFSinkWriterEx_iface.lpVtbl = &sink_writer_vtbl;
    object->events_callback.lpVtbl = &sink_writer_events_callback_vtbl;
    object->finalize_callback.lpVtbl = &sink_writer_finalize_callback_vtbl;
    InitializeConditionVariable(&object->sample_taken);
    object->refcount = 1;
    object->sink = sink;
    IMFMediaSink_AddRef(sink);
    object->stats.cb = sizeof(object->stats);
    InitializeCriticalSection(&object->cs);

    if (attributes)
    {
        IMFAttributes_AddRef((object->attributes = attributes));
        IMFAttributes_GetUnknown(attributes, &MF_SINK_WRITER_ASYNC_CALLBACK,
                &IID_IMFSinkWriterCallback, (void **)&object->callback);
    }

    if (FAILED(hr = sink_writer_initialize_existing_streams(object, sink)))
    {
        IMFSinkWriterEx_Release(&object->IMFSinkWriterEx_iface);
        return hr;
    }

    hr = IMFSinkWriterEx_QueryInterface(&object->IMFSinkWriterEx_iface, riid, out);
    IMFSinkWriterEx_Release(&object->IMFSinkWriterEx_iface);
    return hr;
}

/***********************************************************************
 *      MFCreateSinkWriterFromMediaSink (mfreadwrite.@)
 */
HRESULT WINAPI MFCreateSinkWriterFromMediaSink(IMFMediaSink *sink, IMFAttributes *attributes, IMFSinkWriter **writer)
{
    TRACE("%p, %p, %p.\n", sink, attributes, writer);

    if (!writer)
        return E_INVALIDARG;

    return create_sink_writer_from_sink(sink, attributes, &IID_IMFSinkWriter, (void **)writer);
}

static HRESULT sink_writer_get_sink_factory_class(const WCHAR *url, IMFAttributes *attributes, CLSID *clsid)
{
    static const struct extension_map
    {
        const WCHAR *ext;
        const GUID *guid;
    } ext_map[] =
    {
        { L".mp4", &MFTranscodeContainerType_MPEG4 },
        { L".mp3", &MFTranscodeContainerType_MP3 },
        { L".wav", &MFTranscodeContainerType_WAVE },
        { L".avi", &MFTranscodeContainerType_AVI },
    };
    static const struct
    {
        const GUID *container;
        const CLSID *clsid;
    } class_map[] =
    {
        { &MFTranscodeContainerType_MPEG4, &CLSID_MFMPEG4SinkClassFactory },
        { &MFTranscodeContainerType_MP3, &CLSID_MFMP3SinkClassFactory },
        { &MFTranscodeContainerType_WAVE, &CLSID_MFWAVESinkClassFactory },
        { &MFTranscodeContainerType_AVI, &CLSID_MFAVISinkClassFactory },
    };
    const WCHAR *extension;
    GUID container;
    unsigned int i;

    if (url)
    {
        if (!attributes || FAILED(IMFAttributes_GetGUID(attributes, &MF_TRANSCODE_CONTAINERTYPE, &container)))
        {
            const struct extension_map *map = NULL;

            if (FAILED(PathCchFindExtension(url, PATHCCH_MAX_CCH, &extension))) return E_INVALIDARG;
            if (!extension || !*extension) return E_INVALIDARG;

            for (i = 0; i < ARRAY_SIZE(ext_map); ++i)
            {
                map = &ext_map[i];

                if (!wcsicmp(map->ext, extension))
                    break;
            }

            if (!map)
            {
                WARN("Couldn't find container type for extension %s.\n", debugstr_w(extension));
                return E_INVALIDARG;
            }

            container = *map->guid;
        }
    }
    else
    {
        if (!attributes) return E_INVALIDARG;
        if (FAILED(IMFAttributes_GetGUID(attributes, &MF_TRANSCODE_CONTAINERTYPE, &container))) return E_INVALIDARG;
    }

    for (i = 0; i < ARRAY_SIZE(class_map); ++i)
    {
        if (IsEqualGUID(&container, class_map[i].container))
        {
            *clsid = *class_map[i].clsid;
            return S_OK;
        }
    }

    WARN("Couldn't find factory class for container %s.\n", debugstr_guid(&container));
    return E_INVALIDARG;
}

HRESULT create_sink_writer_from_url(const WCHAR *url, IMFByteStream *bytestream, IMFAttributes *attributes,
        REFIID riid, void **out)
{
    IMFSinkClassFactory *factory;
    IMFMediaSink *sink;
    CLSID clsid;
    HRESULT hr;

    *out = NULL;

    if (!url && !bytestream)
        return E_INVALIDARG;

    if (FAILED(hr = sink_writer_get_sink_factory_class(url, attributes, &clsid))) return hr;

    if (FAILED(hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IMFSinkClassFactory, (void **)&factory)))
    {
        WARN("Failed to create a sink factory, hr %#lx.\n", hr);
        return hr;
    }

    if (bytestream)
        IMFByteStream_AddRef(bytestream);
    else if (FAILED(hr = MFCreateFile(MF_ACCESSMODE_WRITE, MF_OPENMODE_DELETE_IF_EXIST, 0, url, &bytestream)))
    {
        WARN("Failed to create output file stream, hr %#lx.\n", hr);
        IMFSinkClassFactory_Release(factory);
        return hr;
    }

    hr = IMFSinkClassFactory_CreateMediaSink(factory, bytestream, NULL, NULL, &sink);
    IMFSinkClassFactory_Release(factory);
    IMFByteStream_Release(bytestream);
    if (FAILED(hr))
    {
        WARN("Failed to create a sink, hr %#lx.\n", hr);
        return hr;
    }

    hr = create_sink_writer_from_sink(sink, attributes, riid, out);
    IMFMediaSink_Release(sink);

    return hr;
}

/***********************************************************************
 *      MFCreateSinkWriterFromURL (mfreadwrite.@)
 */
HRESULT WINAPI MFCreateSinkWriterFromURL(const WCHAR *url, IMFByteStream *bytestream, IMFAttributes *attributes,
        IMFSinkWriter **writer)
{
    TRACE("%s, %p, %p, %p.\n", debugstr_w(url), bytestream, attributes, writer);

    if (!writer)
        return E_INVALIDARG;

    return create_sink_writer_from_url(url, bytestream, attributes, &IID_IMFSinkWriter, (void **)writer);
}
