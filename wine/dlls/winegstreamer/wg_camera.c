/*
 * Cameras, for Media Foundation's video capture sources
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include <gst/gst.h>
#include <gst/video/video.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"

#include "unix_private.h"

/* A camera's pipeline ends in our own pad, which keeps the newest frame: a live
 * source drops frames a slow reader misses instead of falling behind. */
struct wg_camera
{
    GstElement *pipeline;
    GstPad *sink;
    GstVideoInfo info;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    GstBuffer *frame;
    GstClockTime first;
    gboolean failed;
};

static struct wg_camera *get_camera(wg_camera_t camera)
{
    return (struct wg_camera *)(ULONG_PTR)camera;
}

static GList *camera_devices(void)
{
    GstDeviceMonitor *monitor;
    GList *devices;

    if (!(monitor = gst_device_monitor_new())) return NULL;
    gst_device_monitor_add_filter(monitor, "Video/Source", NULL);
    /* Without a start, this probes each provider once. */
    devices = gst_device_monitor_get_devices(monitor);
    gst_object_unref(monitor);
    return devices;
}

/* A stable id: PipeWire's node, a V4L2 device path, or the name. */
static void camera_id(GstDevice *device, char *id, size_t size)
{
    static const char *keys[] = {"object.serial", "node.name", "device.path", "api.v4l2.path"};
    GstStructure *props = gst_device_get_properties(device);
    const char *api = NULL, *value = NULL;
    gchar *name, *owned = NULL;
    unsigned int i;

    if (props)
    {
        api = gst_structure_get_string(props, "device.api");
        for (i = 0; i < G_N_ELEMENTS(keys) && !value; i++)
        {
            const GValue *v = gst_structure_get_value(props, keys[i]);
            if (!v) continue;
            if (G_VALUE_HOLDS_STRING(v)) value = g_value_get_string(v);
            else if ((owned = gst_value_serialize(v))) value = owned;
        }
    }
    name = gst_device_get_display_name(device);
    snprintf(id, size, "%s:%s", api ? api : "camera", value ? value : name);
    g_free(name);
    g_free(owned);
    if (props) gst_structure_free(props);
}

static GstDevice *find_camera(const char *id)
{
    GstDevice *found = NULL;
    GList *devices, *l;
    char other[128];

    devices = camera_devices();
    for (l = devices; l; l = l->next)
    {
        camera_id(l->data, other, sizeof(other));
        if (!found && !strcmp(id, other)) found = gst_object_ref(l->data);
    }
    g_list_free_full(devices, gst_object_unref);
    return found;
}

NTSTATUS wg_camera_list(void *args)
{
    struct wg_camera_list_params *params = args;
    struct wg_camera_info *infos = (struct wg_camera_info *)(ULONG_PTR)params->infos;
    GList *devices, *l;
    UINT32 count = 0;

    devices = camera_devices();
    for (l = devices; l; l = l->next)
    {
        if (count < params->capacity)
        {
            gchar *name = gst_device_get_display_name(l->data);
            snprintf(infos[count].name, sizeof(infos[count].name), "%s", name ? name : "Camera");
            camera_id(l->data, infos[count].id, sizeof(infos[count].id));
            g_free(name);
        }
        count++;
    }
    g_list_free_full(devices, gst_object_unref);
    params->count = count;
    GST_INFO("%u cameras.", count);
    return STATUS_SUCCESS;
}

/* The best rate a structure offers, up to 30 fps. */
static gboolean structure_rate(const GstStructure *structure, int *n, int *d)
{
    const GValue *rates = gst_structure_get_value(structure, "framerate"), *v;
    int best_n = 0, best_d = 1, rn, rd;
    unsigned int i, count;

    if (!rates) return FALSE;
    if (GST_VALUE_HOLDS_FRACTION_RANGE(rates))
    {
        const GValue *max = gst_value_get_fraction_range_max(rates);
        best_n = gst_value_get_fraction_numerator(max);
        best_d = gst_value_get_fraction_denominator(max);
        if ((gint64)best_n > (gint64)30 * best_d) best_n = 30, best_d = 1;
    }
    else
    {
        count = GST_VALUE_HOLDS_LIST(rates) ? gst_value_list_get_size(rates) : 1;
        for (i = 0; i < count; i++)
        {
            v = GST_VALUE_HOLDS_LIST(rates) ? gst_value_list_get_value(rates, i) : rates;
            if (!GST_VALUE_HOLDS_FRACTION(v)) continue;
            rn = gst_value_get_fraction_numerator(v);
            rd = gst_value_get_fraction_denominator(v);
            if (!rd || (gint64)rn > (gint64)30 * rd) continue;
            if ((gint64)rn * best_d > (gint64)best_n * rd) best_n = rn, best_d = rd;
        }
    }
    if (!best_n) return FALSE;
    *n = best_n;
    *d = best_d;
    return TRUE;
}

NTSTATUS wg_camera_get_modes(void *args)
{
    struct wg_camera_get_modes_params *params = args;
    struct wg_camera_mode *modes = (struct wg_camera_mode *)(ULONG_PTR)params->modes, found[32];
    UINT32 count = 0, i, j;
    GstDevice *device;
    GstCaps *caps;

    if (!(device = find_camera(params->id))) return STATUS_NOT_FOUND;
    caps = gst_device_get_caps(device);
    gst_object_unref(device);
    if (!caps) return STATUS_NOT_FOUND;

    for (i = 0; i < gst_caps_get_size(caps); i++)
    {
        static const int standard[][2] = {{1920, 1080}, {1280, 720}, {640, 480}};
        const GstStructure *structure = gst_caps_get_structure(caps, i);
        const GValue *width_value = gst_structure_get_value(structure, "width");
        const GValue *height_value = gst_structure_get_value(structure, "height");
        int sizes[ARRAY_SIZE(standard)][2], size_count = 0, width, height, n, d, k;

        if (!width_value || !height_value || !structure_rate(structure, &n, &d)) continue;
        if (G_VALUE_HOLDS_INT(width_value) && G_VALUE_HOLDS_INT(height_value))
        {
            sizes[0][0] = g_value_get_int(width_value);
            sizes[0][1] = g_value_get_int(height_value);
            size_count = 1;
        }
        else if (GST_VALUE_HOLDS_INT_RANGE(width_value) && GST_VALUE_HOLDS_INT_RANGE(height_value))
        {
            /* Some PipeWire nodes give ranges: offer the usual camera sizes within them. */
            for (k = 0; k < ARRAY_SIZE(standard); k++)
            {
                if (standard[k][0] < gst_value_get_int_range_min(width_value) ||
                        standard[k][0] > gst_value_get_int_range_max(width_value) ||
                        standard[k][1] < gst_value_get_int_range_min(height_value) ||
                        standard[k][1] > gst_value_get_int_range_max(height_value))
                    continue;
                sizes[size_count][0] = standard[k][0];
                sizes[size_count][1] = standard[k][1];
                size_count++;
            }
        }

        for (k = 0; k < size_count; k++)
        {
            width = sizes[k][0];
            height = sizes[k][1];
            if (width < 160 || height < 120) continue;
            for (j = 0; j < count; j++)
                if (found[j].width == width && found[j].height == height) break;
            if (j == count)
            {
                if (count == ARRAY_SIZE(found)) continue;
                found[count].width = width;
                found[count].height = height;
                found[count].fps_n = 0;
                found[count].fps_d = 1;
                count++;
            }
            if ((gint64)n * found[j].fps_d > (gint64)found[j].fps_n * d)
            {
                found[j].fps_n = n;
                found[j].fps_d = d;
            }
        }
    }
    gst_caps_unref(caps);

    /* Largest first, as cameras list them on Windows. */
    for (i = 1; i < count; i++)
        for (j = i; j > 0 && (UINT64)found[j].width * found[j].height >
                (UINT64)found[j - 1].width * found[j - 1].height; j--)
        {
            struct wg_camera_mode tmp = found[j];
            found[j] = found[j - 1];
            found[j - 1] = tmp;
        }
    for (i = 0; i < count && i < params->capacity; i++) modes[i] = found[i];
    params->count = count;
    return STATUS_SUCCESS;
}

static GstFlowReturn camera_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
    struct wg_camera *camera = gst_pad_get_element_private(pad);

    pthread_mutex_lock(&camera->mutex);
    if (camera->frame) gst_buffer_unref(camera->frame);
    camera->frame = buffer;
    pthread_cond_signal(&camera->cond);
    pthread_mutex_unlock(&camera->mutex);
    return GST_FLOW_OK;
}

static gboolean camera_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
    struct wg_camera *camera = gst_pad_get_element_private(pad);
    GstCaps *caps;

    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS)
    {
        gst_event_parse_caps(event, &caps);
        pthread_mutex_lock(&camera->mutex);
        gst_video_info_from_caps(&camera->info, caps);
        pthread_mutex_unlock(&camera->mutex);
    }
    gst_event_unref(event);
    return TRUE;
}

static const char *format_name(UINT32 format)
{
    switch (format)
    {
        case WG_CAMERA_FORMAT_NV12: return "NV12";
        case WG_CAMERA_FORMAT_YUY2: return "YUY2";
        case WG_CAMERA_FORMAT_BGRX: return "BGRx";
    }
    return NULL;
}

static void camera_free(struct wg_camera *camera)
{
    if (camera->pipeline)
    {
        gst_element_set_state(camera->pipeline, GST_STATE_NULL);
        gst_object_unref(camera->pipeline);
    }
    if (camera->sink) gst_object_unref(camera->sink);
    if (camera->frame) gst_buffer_unref(camera->frame);
    pthread_mutex_destroy(&camera->mutex);
    pthread_cond_destroy(&camera->cond);
    free(camera);
}

NTSTATUS wg_camera_create(void *args)
{
    struct wg_camera_create_params *params = args;
    GstElement *source = NULL, *convert, *scale, *rate, *filter;
    const char *format = format_name(params->format);
    struct wg_camera *camera;
    GstDevice *device;
    GstCaps *caps;
    GstPad *pad;

    if (!format || !params->mode.width || !params->mode.height || !params->mode.fps_d)
        return STATUS_INVALID_PARAMETER;
    if (!(device = find_camera(params->id))) return STATUS_NOT_FOUND;
    if (!(camera = calloc(1, sizeof(*camera))))
    {
        gst_object_unref(device);
        return STATUS_NO_MEMORY;
    }
    pthread_mutex_init(&camera->mutex, NULL);
    pthread_cond_init(&camera->cond, NULL);
    camera->first = GST_CLOCK_TIME_NONE;

    source = gst_device_create_element(device, NULL);
    gst_object_unref(device);
    camera->pipeline = gst_pipeline_new("wine-camera");
    convert = create_element("videoconvert", "base");
    scale = create_element("videoscale", "base");
    rate = create_element("videorate", "base");
    filter = create_element("capsfilter", "core");
    if (!source || !camera->pipeline || !convert || !scale || !rate || !filter)
    {
        GST_ERROR("Failed to create the camera pipeline.");
        if (source) gst_object_unref(source);
        if (convert) gst_object_unref(convert);
        if (scale) gst_object_unref(scale);
        if (rate) gst_object_unref(rate);
        if (filter) gst_object_unref(filter);
        camera_free(camera);
        return STATUS_UNSUCCESSFUL;
    }
    caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, format,
            "width", G_TYPE_INT, (int)params->mode.width, "height", G_TYPE_INT, (int)params->mode.height,
            "framerate", GST_TYPE_FRACTION, (int)params->mode.fps_n, (int)params->mode.fps_d, NULL);
    g_object_set(filter, "caps", caps, NULL);
    gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(camera->pipeline), source, convert, scale, rate, filter, NULL);

    camera->sink = gst_pad_new("wine-camera-sink", GST_PAD_SINK);
    gst_pad_set_element_private(camera->sink, camera);
    gst_pad_set_chain_function(camera->sink, camera_chain);
    gst_pad_set_event_function(camera->sink, camera_event);

    pad = gst_element_get_static_pad(filter, "src");
    if (!gst_element_link_many(source, convert, scale, rate, filter, NULL) ||
            gst_pad_link(pad, camera->sink) != GST_PAD_LINK_OK)
    {
        GST_ERROR("Failed to link the camera pipeline.");
        gst_object_unref(pad);
        camera_free(camera);
        return STATUS_UNSUCCESSFUL;
    }
    gst_object_unref(pad);
    gst_pad_set_active(camera->sink, TRUE);

    if (gst_element_set_state(camera->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        GST_ERROR("Failed to start camera %s.", params->id);
        camera_free(camera);
        return STATUS_DEVICE_NOT_READY;
    }
    GST_INFO("Camera %s started as %s %ux%u at %u/%u.", params->id, format,
            params->mode.width, params->mode.height, params->mode.fps_n, params->mode.fps_d);
    params->camera = (wg_camera_t)(ULONG_PTR)camera;
    return STATUS_SUCCESS;
}

/* A pipeline error, such as a camera another program holds, ends the capture. */
static gboolean camera_failed(struct wg_camera *camera)
{
    GstBus *bus = gst_element_get_bus(camera->pipeline);
    GstMessage *message;
    GError *error;
    gchar *debug;

    while ((message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)))
    {
        gst_message_parse_error(message, &error, &debug);
        GST_ERROR("Camera error: %s (%s)", error->message, debug ? debug : "");
        g_error_free(error);
        g_free(debug);
        gst_message_unref(message);
        camera->failed = TRUE;
    }
    gst_object_unref(bus);
    return camera->failed;
}

/* Copies the frame's planes without padding, as Media Foundation lays them out. */
static UINT32 copy_frame(const GstVideoInfo *info, GstBuffer *buffer, BYTE *data, UINT32 size)
{
    UINT32 written = 0, plane, row, width, rows;
    GstVideoFrame frame;

    if (!gst_video_frame_map(&frame, info, buffer, GST_MAP_READ)) return 0;
    for (plane = 0; plane < GST_VIDEO_FRAME_N_PLANES(&frame); plane++)
    {
        const BYTE *src = GST_VIDEO_FRAME_PLANE_DATA(&frame, plane);
        int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, plane);

        width = GST_VIDEO_FRAME_COMP_WIDTH(&frame, plane) * GST_VIDEO_FRAME_COMP_PSTRIDE(&frame, plane);
        rows = GST_VIDEO_FRAME_COMP_HEIGHT(&frame, plane);
        /* NV12's second plane interleaves U and V: one row of it spans the full width. */
        if (GST_VIDEO_FRAME_FORMAT(&frame) == GST_VIDEO_FORMAT_NV12 && plane == 1)
            width = GST_VIDEO_FRAME_WIDTH(&frame);
        for (row = 0; row < rows; row++)
        {
            if (written + width > size)
            {
                gst_video_frame_unmap(&frame);
                return 0;
            }
            memcpy(data + written, src + (gsize)row * stride, width);
            written += width;
        }
    }
    gst_video_frame_unmap(&frame);
    return written;
}

NTSTATUS wg_camera_read(void *args)
{
    struct wg_camera_read_params *params = args;
    struct wg_camera *camera = get_camera(params->camera);
    GstBuffer *buffer = NULL;
    GstVideoInfo info;
    struct timespec deadline;
    GstClockTime pts;

    params->written = 0;
    if (camera_failed(camera)) return STATUS_DEVICE_NOT_READY;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += params->timeout_ms / 1000;
    deadline.tv_nsec += (params->timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&camera->mutex);
    while (!camera->frame && pthread_cond_timedwait(&camera->cond, &camera->mutex, &deadline) == 0)
        ;
    if ((buffer = camera->frame)) camera->frame = NULL;
    info = camera->info;
    pthread_mutex_unlock(&camera->mutex);
    if (!buffer) return STATUS_TIMEOUT;

    params->written = copy_frame(&info, buffer, (BYTE *)(ULONG_PTR)params->data, params->size);
    pts = GST_BUFFER_PTS(buffer);
    if (GST_CLOCK_TIME_IS_VALID(pts))
    {
        if (!GST_CLOCK_TIME_IS_VALID(camera->first)) camera->first = pts;
        params->time = (pts - camera->first) / 100;
    }
    else params->time = -1;
    gst_buffer_unref(buffer);
    return params->written ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}

NTSTATUS wg_camera_destroy(void *args)
{
    camera_free(get_camera(*(wg_camera_t *)args));
    return STATUS_SUCCESS;
}
