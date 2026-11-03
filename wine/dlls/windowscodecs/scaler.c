/*
 * Copyright 2010 Vincent Povirk for CodeWeavers
 * Copyright 2016 Dmitry Timoshkov
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

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "objbase.h"

#include "wincodecs_private.h"

#include <math.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wincodecs);

/* Not yet in wincodec.idl. */
#define WICBitmapInterpolationModeHighQualityCubic ((WICBitmapInterpolationMode)4)

typedef struct BitmapScaler {
    IWICBitmapScaler IWICBitmapScaler_iface;
    LONG ref;
    IMILBitmapScaler IMILBitmapScaler_iface;
    IWICBitmapSource *source;
    UINT width, height;
    UINT src_width, src_height;
    WICBitmapInterpolationMode mode;
    UINT bpp;
    void (*fn_get_required_source_rect)(struct BitmapScaler*,UINT,UINT,WICRect*);
    void (*fn_copy_scanline)(struct BitmapScaler*,UINT,UINT,UINT,BYTE**,UINT,UINT,BYTE*);
    BOOL filtered;
    enum { ALPHA_NONE, ALPHA_STRAIGHT, ALPHA_PREMULTIPLIED } alpha;
    BYTE *scaled; /* the whole of the filtered result, made when first asked for */
    CRITICAL_SECTION lock; /* must be held when initialized */
} BitmapScaler;

static inline BitmapScaler *impl_from_IWICBitmapScaler(IWICBitmapScaler *iface)
{
    return CONTAINING_RECORD(iface, BitmapScaler, IWICBitmapScaler_iface);
}

static inline BitmapScaler *impl_from_IMILBitmapScaler(IMILBitmapScaler *iface)
{
    return CONTAINING_RECORD(iface, BitmapScaler, IMILBitmapScaler_iface);
}

static HRESULT WINAPI BitmapScaler_QueryInterface(IWICBitmapScaler *iface, REFIID iid,
    void **ppv)
{
    BitmapScaler *This = impl_from_IWICBitmapScaler(iface);
    TRACE("(%p,%s,%p)\n", iface, debugstr_guid(iid), ppv);

    if (!ppv) return E_INVALIDARG;

    if (IsEqualIID(&IID_IUnknown, iid) ||
        IsEqualIID(&IID_IWICBitmapSource, iid) ||
        IsEqualIID(&IID_IWICBitmapScaler, iid))
    {
        *ppv = &This->IWICBitmapScaler_iface;
    }
    else if (IsEqualIID(&IID_IMILBitmapScaler, iid))
    {
        *ppv = &This->IMILBitmapScaler_iface;
    }
    else
    {
        FIXME("unknown interface %s\n", debugstr_guid(iid));
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI BitmapScaler_AddRef(IWICBitmapScaler *iface)
{
    BitmapScaler *This = impl_from_IWICBitmapScaler(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) refcount=%lu\n", iface, ref);

    return ref;
}

static ULONG WINAPI BitmapScaler_Release(IWICBitmapScaler *iface)
{
    BitmapScaler *This = impl_from_IWICBitmapScaler(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) refcount=%lu\n", iface, ref);

    if (ref == 0)
    {
        This->lock.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&This->lock);
        if (This->source) IWICBitmapSource_Release(This->source);
        free(This->scaled);
        free(This);
    }

    return ref;
}

static HRESULT WINAPI BitmapScaler_GetSize(IWICBitmapScaler *iface,
    UINT *puiWidth, UINT *puiHeight)
{
    BitmapScaler *This = impl_from_IWICBitmapScaler(iface);
    TRACE("(%p,%p,%p)\n", iface, puiWidth, puiHeight);

    if (!This->source)
        return WINCODEC_ERR_NOTINITIALIZED;

    if (!puiWidth || !puiHeight)
        return E_INVALIDARG;

    *puiWidth = This->width;
    *puiHeight = This->height;

    return S_OK;
}

static HRESULT WINAPI BitmapScaler_GetPixelFormat(IWICBitmapScaler *iface,
    WICPixelFormatGUID *pPixelFormat)
{
    BitmapScaler *This = impl_from_IWICBitmapScaler(iface);
    TRACE("(%p,%p)\n", iface, pPixelFormat);

    if (!pPixelFormat)
        return E_INVALIDARG;

    if (!This->source)
    {
        memcpy(pPixelFormat, &GUID_WICPixelFormatDontCare, sizeof(*pPixelFormat));
        return S_OK;
    }

    return IWICBitmapSource_GetPixelFormat(This->source, pPixelFormat);
}

static HRESULT WINAPI BitmapScaler_GetResolution(IWICBitmapScaler *iface,
    double *pDpiX, double *pDpiY)
{
    BitmapScaler *This = impl_from_IWICBitmapScaler(iface);
    TRACE("(%p,%p,%p)\n", iface, pDpiX, pDpiY);

    if (!This->source)
        return WINCODEC_ERR_NOTINITIALIZED;

    if (!pDpiX || !pDpiY)
        return E_INVALIDARG;

    return IWICBitmapSource_GetResolution(This->source, pDpiX, pDpiY);
}

static HRESULT WINAPI BitmapScaler_CopyPalette(IWICBitmapScaler *iface,
    IWICPalette *pIPalette)
{
    BitmapScaler *This = impl_from_IWICBitmapScaler(iface);
    TRACE("(%p,%p)\n", iface, pIPalette);

    if (!pIPalette)
        return E_INVALIDARG;

    if (!This->source)
        return WINCODEC_ERR_PALETTEUNAVAILABLE;

    return IWICBitmapSource_CopyPalette(This->source, pIPalette);
}

static void NearestNeighbor_GetRequiredSourceRect(BitmapScaler *This,
    UINT x, UINT y, WICRect *src_rect)
{
    src_rect->X = x * This->src_width / This->width;
    src_rect->Y = y * This->src_height / This->height;
    src_rect->Width = src_rect->Height = 1;
}

static void NearestNeighbor_CopyScanline(BitmapScaler *This,
    UINT dst_x, UINT dst_y, UINT dst_width,
    BYTE **src_data, UINT src_data_x, UINT src_data_y, BYTE *pbBuffer)
{
    UINT i;
    UINT bytesperpixel = This->bpp/8;
    UINT src_x, src_y;

    src_y = dst_y * This->src_height / This->height - src_data_y;

    for (i=0; i<dst_width; i++)
    {
        src_x = (dst_x + i) * This->src_width / This->width - src_data_x;
        memcpy(pbBuffer + bytesperpixel * i, src_data[src_y] + bytesperpixel * src_x, bytesperpixel);
    }
}

/* Filtered modes. Each destination pixel is a weighted sum of the source pixels under
 * it, computed one dimension at a time. All of them widen their filter when shrinking,
 * which is what keeps fine detail from turning into false patterns. */
struct scaler_weights
{
    UINT *start, *count, *offset;
    float *weights;
};

static void scaler_weights_free(struct scaler_weights *w)
{
    free(w->start);
    free(w->count);
    free(w->offset);
    free(w->weights);
}

static double scaler_kernel(WICBitmapInterpolationMode mode, double x)
{
    x = fabs(x);
    if (mode == WICBitmapInterpolationModeLinear)
        return x < 1.0 ? 1.0 - x : 0.0;
    /* Catmull-Rom */
    if (x < 1.0)
        return 1.5 * x * x * x - 2.5 * x * x + 1.0;
    if (x < 2.0)
        return -0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0;
    return 0.0;
}

static HRESULT scaler_weights_init(struct scaler_weights *w, WICBitmapInterpolationMode mode, UINT src, UINT dst)
{
    double ratio = (double)src / dst, scale = max(ratio, 1.0);
    double radius = mode == WICBitmapInterpolationModeLinear ? 1.0 : 2.0;
    size_t total = 0, capacity;
    UINT i;

    memset(w, 0, sizeof(*w));
    capacity = (size_t)dst * (size_t)(ceil(2.0 * radius * scale) + 2);
    w->start = calloc(dst, sizeof(*w->start));
    w->count = calloc(dst, sizeof(*w->count));
    w->offset = calloc(dst, sizeof(*w->offset));
    w->weights = malloc(capacity * sizeof(*w->weights));
    if (!w->start || !w->count || !w->offset || !w->weights)
    {
        scaler_weights_free(w);
        return E_OUTOFMEMORY;
    }

    for (i = 0; i < dst; ++i)
    {
        double center = (i + 0.5) * ratio - 0.5, sum = 0.0;
        int first, last, j;
        UINT k;

        if (mode == WICBitmapInterpolationModeFant)
        {
            /* The share of each source pixel in the area this pixel covers. */
            double from = i * ratio, to = (i + 1) * ratio;

            first = (int)floor(from);
            last = (int)ceil(to) - 1;
        }
        else
        {
            first = (int)ceil(center - radius * scale);
            last = (int)floor(center + radius * scale);
        }
        first = max(first, 0);
        last = min(last, (int)src - 1);
        if (last < first)
            first = last = min(max((int)floor(center + 0.5), 0), (int)src - 1);

        w->start[i] = first;
        w->count[i] = last - first + 1;
        w->offset[i] = total;
        for (j = first; j <= last; ++j)
        {
            double weight;

            if (mode == WICBitmapInterpolationModeFant)
                weight = min((double)j + 1.0, (i + 1) * ratio) - max((double)j, i * ratio);
            else
                weight = scaler_kernel(mode, (j - center) / scale);
            w->weights[total + (j - first)] = weight;
            sum += weight;
        }
        if (sum == 0.0)
        {
            w->weights[total] = 1.0f;
            for (k = 1; k < w->count[i]; ++k)
                w->weights[total + k] = 0.0f;
            sum = 1.0;
        }
        for (k = 0; k < w->count[i]; ++k)
            w->weights[total + k] /= sum;
        total += w->count[i];
    }
    return S_OK;
}

static HRESULT scaler_filter(BitmapScaler *This)
{
    UINT channels = This->bpp / 8, row_bytes = This->src_width * channels;
    UINT out_row = This->width * channels, ring_rows = 0, x, y, c, i;
    struct scaler_weights horizontal, vertical;
    BYTE *source_row = NULL, *scaled = NULL;
    float *float_row = NULL, *ring = NULL, *sum = NULL;
    int *ring_source = NULL;
    HRESULT hr;

    if ((UINT64)out_row * This->height > 0x7fffffff)
        return E_OUTOFMEMORY;

    if (FAILED(hr = scaler_weights_init(&horizontal, This->mode, This->src_width, This->width)))
        return hr;
    if (FAILED(hr = scaler_weights_init(&vertical, This->mode, This->src_height, This->height)))
    {
        scaler_weights_free(&horizontal);
        return hr;
    }

    for (y = 0; y < This->height; ++y)
        ring_rows = max(ring_rows, vertical.count[y]);

    source_row = malloc(row_bytes);
    float_row = malloc(row_bytes * sizeof(*float_row));
    ring = malloc((size_t)ring_rows * out_row * sizeof(*ring));
    sum = malloc(out_row * sizeof(*sum));
    ring_source = malloc(ring_rows * sizeof(*ring_source));
    scaled = malloc((size_t)out_row * This->height);
    if (!source_row || !float_row || !ring || !sum || !ring_source || !scaled)
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    for (i = 0; i < ring_rows; ++i)
        ring_source[i] = -1;

    for (y = 0; y < This->height && SUCCEEDED(hr); ++y)
    {
        const float *weights = &vertical.weights[vertical.offset[y]];
        BYTE *out = scaled + (size_t)y * out_row;

        memset(sum, 0, out_row * sizeof(*sum));
        for (i = 0; i < vertical.count[y]; ++i)
        {
            UINT j = vertical.start[y] + i, slot = j % ring_rows;
            float *filtered = ring + (size_t)slot * out_row;

            if (ring_source[slot] != (int)j)
            {
                WICRect rect = {0, j, This->src_width, 1};

                if (FAILED(hr = IWICBitmapSource_CopyPixels(This->source, &rect, row_bytes, row_bytes, source_row)))
                    break;
                ring_source[slot] = j;

                for (x = 0; x < This->src_width; ++x)
                {
                    const BYTE *p = source_row + x * channels;
                    float *f = float_row + x * channels;

                    /* Colours under transparent pixels must not bleed into the edges. */
                    if (This->alpha == ALPHA_STRAIGHT)
                    {
                        f[0] = p[0] * p[3] / 255.0f;
                        f[1] = p[1] * p[3] / 255.0f;
                        f[2] = p[2] * p[3] / 255.0f;
                        f[3] = p[3];
                    }
                    else
                    {
                        for (c = 0; c < channels; ++c)
                            f[c] = p[c];
                    }
                }

                for (x = 0; x < This->width; ++x)
                {
                    const float *w = &horizontal.weights[horizontal.offset[x]];
                    const float *f = float_row + (size_t)horizontal.start[x] * channels;
                    float *o = filtered + (size_t)x * channels;
                    UINT k;

                    for (c = 0; c < channels; ++c)
                        o[c] = 0.0f;
                    for (k = 0; k < horizontal.count[x]; ++k, f += channels)
                    {
                        for (c = 0; c < channels; ++c)
                            o[c] += w[k] * f[c];
                    }
                }
            }

            for (x = 0; x < out_row; ++x)
                sum[x] += weights[i] * filtered[x];
        }
        if (FAILED(hr))
            break;

        for (x = 0; x < This->width; ++x)
        {
            const float *f = sum + (size_t)x * channels;
            BYTE *o = out + (size_t)x * channels;
            float limit = 255.0f, alpha = 255.0f;

            if (This->alpha != ALPHA_NONE)
            {
                alpha = min(max(f[3], 0.0f), 255.0f);
                if (This->alpha == ALPHA_PREMULTIPLIED)
                    limit = alpha;
            }
            for (c = 0; c < channels; ++c)
            {
                float value = f[c];

                if (c < 3 || This->alpha == ALPHA_NONE)
                {
                    if (This->alpha == ALPHA_STRAIGHT)
                        value = alpha > 0.0f ? value * 255.0f / alpha : 0.0f;
                }
                else
                {
                    value = alpha;
                }
                o[c] = (BYTE)(min(max(value, 0.0f), c < 3 ? limit : 255.0f) + 0.5f);
            }
        }
    }

    if (SUCCEEDED(hr))
    {
        This->scaled = scaled;
        scaled = NULL;
    }

done:
    free(source_row);
    free(float_row);
    free(ring);
    free(sum);
    free(ring_source);
    free(scaled);
    scaler_weights_free(&horizontal);
    scaler_weights_free(&vertical);
    return hr;
}

static HRESULT WINAPI BitmapScaler_CopyPixels(IWICBitmapScaler *iface,
    const WICRect *prc, UINT cbStride, UINT cbBufferSize, BYTE *pbBuffer)
{
    BitmapScaler *This = impl_from_IWICBitmapScaler(iface);
    HRESULT hr;
    WICRect dest_rect;
    WICRect src_rect_ul, src_rect_br, src_rect;
    BYTE **src_rows;
    BYTE *src_bits;
    ULONG bytesperrow;
    ULONG src_bytesperrow;
    ULONG buffer_size;
    UINT y;

    TRACE("(%p,%s,%u,%u,%p)\n", iface, debug_wic_rect(prc), cbStride, cbBufferSize, pbBuffer);

    EnterCriticalSection(&This->lock);

    if (!This->source)
    {
        hr = WINCODEC_ERR_NOTINITIALIZED;
        goto end;
    }

    if (prc)
        dest_rect = *prc;
    else
    {
        dest_rect.X = dest_rect.Y = 0;
        dest_rect.Width = This->width;
        dest_rect.Height = This->height;
    }

    if (dest_rect.X < 0 || dest_rect.Y < 0 ||
        dest_rect.X+dest_rect.Width > This->width|| dest_rect.Y+dest_rect.Height > This->height)
    {
        hr = E_INVALIDARG;
        goto end;
    }

    bytesperrow = ((This->bpp * dest_rect.Width)+7)/8;

    if (cbStride < bytesperrow)
    {
        hr = E_INVALIDARG;
        goto end;
    }

    if (cbStride * (dest_rect.Height - 1) + bytesperrow > cbBufferSize)
    {
        hr = E_INVALIDARG;
        goto end;
    }

    if (This->filtered)
    {
        UINT row = This->width * (This->bpp / 8);

        if (!This->scaled && FAILED(hr = scaler_filter(This)))
            goto end;
        for (y = 0; y < dest_rect.Height; y++)
            memcpy(pbBuffer + (size_t)y * cbStride,
                    This->scaled + (size_t)(dest_rect.Y + y) * row + (size_t)dest_rect.X * (This->bpp / 8),
                    bytesperrow);
        hr = S_OK;
        goto end;
    }

    /* MSDN recommends calling CopyPixels once for each scanline from top to
     * bottom, and claims codecs optimize for this. Ideally, when called in this
     * way, we should avoid requesting a scanline from the source more than
     * once, by saving the data that will be useful for the next scanline after
     * the call returns. The GetRequiredSourceRect/CopyScanline functions are
     * designed to make it possible to do this in a generic way, but for now we
     * just grab all the data we need in each call. */

    This->fn_get_required_source_rect(This, dest_rect.X, dest_rect.Y, &src_rect_ul);
    This->fn_get_required_source_rect(This, dest_rect.X+dest_rect.Width-1,
        dest_rect.Y+dest_rect.Height-1, &src_rect_br);

    src_rect.X = src_rect_ul.X;
    src_rect.Y = src_rect_ul.Y;
    src_rect.Width = src_rect_br.Width + src_rect_br.X - src_rect_ul.X;
    src_rect.Height = src_rect_br.Height + src_rect_br.Y - src_rect_ul.Y;

    src_bytesperrow = (src_rect.Width * This->bpp + 7)/8;
    buffer_size = src_bytesperrow * src_rect.Height;

    src_rows = malloc(sizeof(BYTE*) * src_rect.Height);
    src_bits = malloc(buffer_size);

    if (!src_rows || !src_bits)
    {
        free(src_rows);
        free(src_bits);
        hr = E_OUTOFMEMORY;
        goto end;
    }

    for (y=0; y<src_rect.Height; y++)
        src_rows[y] = src_bits + y * src_bytesperrow;

    hr = IWICBitmapSource_CopyPixels(This->source, &src_rect, src_bytesperrow,
        buffer_size, src_bits);

    if (SUCCEEDED(hr))
    {
        for (y=0; y < dest_rect.Height; y++)
        {
            This->fn_copy_scanline(This, dest_rect.X, dest_rect.Y+y, dest_rect.Width,
                src_rows, src_rect.X, src_rect.Y, pbBuffer + cbStride * y);
        }
    }

    free(src_rows);
    free(src_bits);

end:
    LeaveCriticalSection(&This->lock);

    return hr;
}

static HRESULT WINAPI BitmapScaler_Initialize(IWICBitmapScaler *iface,
    IWICBitmapSource *pISource, UINT uiWidth, UINT uiHeight,
    WICBitmapInterpolationMode mode)
{
    BitmapScaler *This = impl_from_IWICBitmapScaler(iface);
    HRESULT hr;
    GUID src_pixelformat;

    TRACE("(%p,%p,%u,%u,%u)\n", iface, pISource, uiWidth, uiHeight, mode);

    if (!pISource || !uiWidth || !uiHeight)
        return E_INVALIDARG;

    EnterCriticalSection(&This->lock);

    if (This->source)
    {
        hr = WINCODEC_ERR_WRONGSTATE;
        goto end;
    }

    This->width = uiWidth;
    This->height = uiHeight;
    This->mode = mode;

    hr = IWICBitmapSource_GetSize(pISource, &This->src_width, &This->src_height);

    if (SUCCEEDED(hr))
        hr = IWICBitmapSource_GetPixelFormat(pISource, &src_pixelformat);

    if (SUCCEEDED(hr))
    {
        hr = get_pixelformat_bpp(&src_pixelformat, &This->bpp);
    }

    if (SUCCEEDED(hr))
    {
        BOOL filter = FALSE;

        if (mode == WICBitmapInterpolationModeLinear || mode == WICBitmapInterpolationModeCubic
                || mode == WICBitmapInterpolationModeFant || mode == WICBitmapInterpolationModeHighQualityCubic)
        {
            filter = TRUE;
            if (IsEqualGUID(&src_pixelformat, &GUID_WICPixelFormat32bppBGRA)
                    || IsEqualGUID(&src_pixelformat, &GUID_WICPixelFormat32bppRGBA))
                This->alpha = ALPHA_STRAIGHT;
            else if (IsEqualGUID(&src_pixelformat, &GUID_WICPixelFormat32bppPBGRA)
                    || IsEqualGUID(&src_pixelformat, &GUID_WICPixelFormat32bppPRGBA))
                This->alpha = ALPHA_PREMULTIPLIED;
            else if (IsEqualGUID(&src_pixelformat, &GUID_WICPixelFormat32bppBGR)
                    || IsEqualGUID(&src_pixelformat, &GUID_WICPixelFormat24bppBGR)
                    || IsEqualGUID(&src_pixelformat, &GUID_WICPixelFormat24bppRGB)
                    || IsEqualGUID(&src_pixelformat, &GUID_WICPixelFormat8bppGray))
                This->alpha = ALPHA_NONE;
            else
                filter = FALSE;
        }

        if (filter)
        {
            IWICBitmapSource_AddRef(pISource);
            This->source = pISource;
            This->filtered = TRUE;
        }
        else switch (mode)
        {
        default:
            FIXME("Mode %i is unsupported for this format.\n", mode);
            /* fall-through */
        case WICBitmapInterpolationModeNearestNeighbor:
            if ((This->bpp % 8) == 0)
            {
                IWICBitmapSource_AddRef(pISource);
                This->source = pISource;
            }
            else
            {
                hr = WICConvertBitmapSource(&GUID_WICPixelFormat32bppBGRA,
                    pISource, &This->source);
                This->bpp = 32;
            }
            This->fn_get_required_source_rect = NearestNeighbor_GetRequiredSourceRect;
            This->fn_copy_scanline = NearestNeighbor_CopyScanline;
            break;
        }
    }

end:
    LeaveCriticalSection(&This->lock);

    return hr;
}

static const IWICBitmapScalerVtbl BitmapScaler_Vtbl = {
    BitmapScaler_QueryInterface,
    BitmapScaler_AddRef,
    BitmapScaler_Release,
    BitmapScaler_GetSize,
    BitmapScaler_GetPixelFormat,
    BitmapScaler_GetResolution,
    BitmapScaler_CopyPalette,
    BitmapScaler_CopyPixels,
    BitmapScaler_Initialize
};

static HRESULT WINAPI IMILBitmapScaler_QueryInterface(IMILBitmapScaler *iface, REFIID iid,
    void **ppv)
{
    BitmapScaler *This = impl_from_IMILBitmapScaler(iface);
    TRACE("(%p,%s,%p)\n", iface, debugstr_guid(iid), ppv);
    return IWICBitmapScaler_QueryInterface(&This->IWICBitmapScaler_iface, iid, ppv);
}

static ULONG WINAPI IMILBitmapScaler_AddRef(IMILBitmapScaler *iface)
{
    BitmapScaler *This = impl_from_IMILBitmapScaler(iface);
    return IWICBitmapScaler_AddRef(&This->IWICBitmapScaler_iface);
}

static ULONG WINAPI IMILBitmapScaler_Release(IMILBitmapScaler *iface)
{
    BitmapScaler *This = impl_from_IMILBitmapScaler(iface);
    return IWICBitmapScaler_Release(&This->IWICBitmapScaler_iface);
}

static HRESULT WINAPI IMILBitmapScaler_GetSize(IMILBitmapScaler *iface,
    UINT *width, UINT *height)
{
    BitmapScaler *This = impl_from_IMILBitmapScaler(iface);
    TRACE("(%p,%p,%p)\n", iface, width, height);
    return IWICBitmapScaler_GetSize(&This->IWICBitmapScaler_iface, width, height);
}

static HRESULT WINAPI IMILBitmapScaler_GetPixelFormat(IMILBitmapScaler *iface,
    int *format)
{
    BitmapScaler *This = impl_from_IMILBitmapScaler(iface);
    IMILBitmapSource *source;
    HRESULT hr;

    TRACE("(%p,%p)\n", iface, format);

    if (!format) return E_INVALIDARG;

    if (!This->source)
        return WINCODEC_ERR_NOTINITIALIZED;

    hr = IWICBitmapSource_QueryInterface(This->source, &IID_IMILBitmapSource, (void **)&source);
    if (hr == S_OK)
    {
        hr = source->lpVtbl->GetPixelFormat(source, format);
        source->lpVtbl->Release(source);
    }
    return hr;
}

static HRESULT WINAPI IMILBitmapScaler_GetResolution(IMILBitmapScaler *iface,
    double *dpix, double *dpiy)
{
    BitmapScaler *This = impl_from_IMILBitmapScaler(iface);
    TRACE("(%p,%p,%p)\n", iface, dpix, dpiy);
    return IWICBitmapScaler_GetResolution(&This->IWICBitmapScaler_iface, dpix, dpiy);
}

static HRESULT WINAPI IMILBitmapScaler_CopyPalette(IMILBitmapScaler *iface,
    IWICPalette *palette)
{
    BitmapScaler *This = impl_from_IMILBitmapScaler(iface);

    TRACE("(%p,%p)\n", iface, palette);

    if (!This->source)
        return WINCODEC_ERR_NOTINITIALIZED;

    return IWICBitmapScaler_CopyPalette(&This->IWICBitmapScaler_iface, palette);
}

static HRESULT WINAPI IMILBitmapScaler_CopyPixels(IMILBitmapScaler *iface,
    const WICRect *rc, UINT stride, UINT size, BYTE *buffer)
{
    BitmapScaler *This = impl_from_IMILBitmapScaler(iface);
    TRACE("(%p,%p,%u,%u,%p)\n", iface, rc, stride, size, buffer);
    return IWICBitmapScaler_CopyPixels(&This->IWICBitmapScaler_iface, rc, stride, size, buffer);
}

static HRESULT WINAPI IMILBitmapScaler_unknown1(IMILBitmapScaler *iface, void **ppv)
{
    TRACE("(%p,%p)\n", iface, ppv);
    return E_NOINTERFACE;
}

static HRESULT WINAPI IMILBitmapScaler_Initialize(IMILBitmapScaler *iface,
    IMILBitmapSource *mil_source, UINT width, UINT height,
    WICBitmapInterpolationMode mode)
{
    BitmapScaler *This = impl_from_IMILBitmapScaler(iface);
    IWICBitmapSource *wic_source;
    HRESULT hr;

    TRACE("(%p,%p,%u,%u,%u)\n", iface, mil_source, width, height, mode);

    if (!mil_source) return E_INVALIDARG;

    hr = mil_source->lpVtbl->QueryInterface(mil_source, &IID_IWICBitmapSource, (void **)&wic_source);
    if (hr == S_OK)
    {
        hr = IWICBitmapScaler_Initialize(&This->IWICBitmapScaler_iface, wic_source, width, height, mode);
        IWICBitmapSource_Release(wic_source);
    }
    return hr;
}

static const IMILBitmapScalerVtbl IMILBitmapScaler_Vtbl = {
    IMILBitmapScaler_QueryInterface,
    IMILBitmapScaler_AddRef,
    IMILBitmapScaler_Release,
    IMILBitmapScaler_GetSize,
    IMILBitmapScaler_GetPixelFormat,
    IMILBitmapScaler_GetResolution,
    IMILBitmapScaler_CopyPalette,
    IMILBitmapScaler_CopyPixels,
    IMILBitmapScaler_unknown1,
    IMILBitmapScaler_Initialize
};

HRESULT BitmapScaler_Create(IWICBitmapScaler **scaler)
{
    BitmapScaler *This;

    This = malloc(sizeof(BitmapScaler));
    if (!This) return E_OUTOFMEMORY;

    This->IWICBitmapScaler_iface.lpVtbl = &BitmapScaler_Vtbl;
    This->IMILBitmapScaler_iface.lpVtbl = &IMILBitmapScaler_Vtbl;
    This->ref = 1;
    This->source = NULL;
    This->filtered = FALSE;
    This->alpha = ALPHA_NONE;
    This->scaled = NULL;
    This->width = 0;
    This->height = 0;
    This->src_width = 0;
    This->src_height = 0;
    This->mode = 0;
    This->bpp = 0;
    InitializeCriticalSectionEx(&This->lock, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    This->lock.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": BitmapScaler.lock");

    *scaler = &This->IWICBitmapScaler_iface;

    return S_OK;
}
