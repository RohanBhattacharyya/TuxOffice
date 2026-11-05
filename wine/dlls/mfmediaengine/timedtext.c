/*
 * Media engine timed text (captions, subtitles and metadata cues)
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

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"

#include "mfapi.h"
#include "mfmediaengine.h"
#include "mferror.h"
#include "objbase.h"
#include "initguid.h"
#include "xmllite.h"

#include "mediaengine_private.h"

#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

/* How often active cues are compared with the playback position. */
#define CUE_POLL_MS 40

static WCHAR *heap_wcsdup(const WCHAR *str)
{
    WCHAR *ret;
    size_t len;

    if (!str) str = L"";
    len = (wcslen(str) + 1) * sizeof(WCHAR);
    if ((ret = malloc(len))) memcpy(ret, str, len);
    return ret;
}

static WCHAR *heap_wcsndup(const WCHAR *str, size_t count)
{
    WCHAR *ret;

    if ((ret = malloc((count + 1) * sizeof(WCHAR))))
    {
        memcpy(ret, str, count * sizeof(WCHAR));
        ret[count] = 0;
    }
    return ret;
}

/* Strings handed to callers are theirs to free with CoTaskMemFree. */
static HRESULT return_string(const WCHAR *str, WCHAR **out)
{
    size_t len;

    if (!out) return E_POINTER;
    if (!str) str = L"";
    len = (wcslen(str) + 1) * sizeof(WCHAR);
    if (!(*out = CoTaskMemAlloc(len))) return E_OUTOFMEMORY;
    memcpy(*out, str, len);
    return S_OK;
}

static MFARGB argb(BYTE alpha, BYTE red, BYTE green, BYTE blue)
{
    MFARGB color = { .rgbBlue = blue, .rgbGreen = green, .rgbRed = red, .rgbAlpha = alpha };
    return color;
}

/* Styles */

struct tt_style
{
    IMFTimedTextStyle IMFTimedTextStyle_iface;
    LONG refcount;

    WCHAR *name;
    WCHAR *font_family;
    double font_size;
    MF_TIMED_TEXT_UNIT_TYPE font_size_unit;
    MFARGB color;
    MFARGB background;
    BOOL show_background_always;
    MF_TIMED_TEXT_FONT_STYLE font_style;
    BOOL bold;
    BOOL right_to_left;
    MF_TIMED_TEXT_ALIGNMENT alignment;
    DWORD decoration;
    MFARGB outline_color;
    double outline_thickness;
    double outline_blur;
    MF_TIMED_TEXT_UNIT_TYPE outline_unit;
};

static struct tt_style *impl_from_IMFTimedTextStyle(IMFTimedTextStyle *iface)
{
    return CONTAINING_RECORD(iface, struct tt_style, IMFTimedTextStyle_iface);
}

static HRESULT WINAPI tt_style_QueryInterface(IMFTimedTextStyle *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFTimedTextStyle) || IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFTimedTextStyle_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI tt_style_AddRef(IMFTimedTextStyle *iface)
{
    return InterlockedIncrement(&impl_from_IMFTimedTextStyle(iface)->refcount);
}

static ULONG WINAPI tt_style_Release(IMFTimedTextStyle *iface)
{
    struct tt_style *style = impl_from_IMFTimedTextStyle(iface);
    ULONG refcount = InterlockedDecrement(&style->refcount);

    if (!refcount)
    {
        free(style->name);
        free(style->font_family);
        free(style);
    }
    return refcount;
}

static HRESULT WINAPI tt_style_GetName(IMFTimedTextStyle *iface, WCHAR **name)
{
    return return_string(impl_from_IMFTimedTextStyle(iface)->name, name);
}

static BOOL WINAPI tt_style_IsExternal(IMFTimedTextStyle *iface)
{
    return FALSE;
}

static HRESULT WINAPI tt_style_GetFontFamily(IMFTimedTextStyle *iface, WCHAR **family)
{
    return return_string(impl_from_IMFTimedTextStyle(iface)->font_family, family);
}

static HRESULT WINAPI tt_style_GetFontSize(IMFTimedTextStyle *iface, double *size, MF_TIMED_TEXT_UNIT_TYPE *unit)
{
    struct tt_style *style = impl_from_IMFTimedTextStyle(iface);

    if (!size || !unit) return E_POINTER;
    *size = style->font_size;
    *unit = style->font_size_unit;
    return S_OK;
}

static HRESULT WINAPI tt_style_GetColor(IMFTimedTextStyle *iface, MFARGB *color)
{
    if (!color) return E_POINTER;
    *color = impl_from_IMFTimedTextStyle(iface)->color;
    return S_OK;
}

static HRESULT WINAPI tt_style_GetBackgroundColor(IMFTimedTextStyle *iface, MFARGB *color)
{
    if (!color) return E_POINTER;
    *color = impl_from_IMFTimedTextStyle(iface)->background;
    return S_OK;
}

static HRESULT WINAPI tt_style_GetShowBackgroundAlways(IMFTimedTextStyle *iface, BOOL *show)
{
    if (!show) return E_POINTER;
    *show = impl_from_IMFTimedTextStyle(iface)->show_background_always;
    return S_OK;
}

static HRESULT WINAPI tt_style_GetFontStyle(IMFTimedTextStyle *iface, MF_TIMED_TEXT_FONT_STYLE *font_style)
{
    if (!font_style) return E_POINTER;
    *font_style = impl_from_IMFTimedTextStyle(iface)->font_style;
    return S_OK;
}

static HRESULT WINAPI tt_style_GetBold(IMFTimedTextStyle *iface, BOOL *bold)
{
    if (!bold) return E_POINTER;
    *bold = impl_from_IMFTimedTextStyle(iface)->bold;
    return S_OK;
}

static HRESULT WINAPI tt_style_GetRightToLeft(IMFTimedTextStyle *iface, BOOL *rtl)
{
    if (!rtl) return E_POINTER;
    *rtl = impl_from_IMFTimedTextStyle(iface)->right_to_left;
    return S_OK;
}

static HRESULT WINAPI tt_style_GetTextAlignment(IMFTimedTextStyle *iface, MF_TIMED_TEXT_ALIGNMENT *alignment)
{
    if (!alignment) return E_POINTER;
    *alignment = impl_from_IMFTimedTextStyle(iface)->alignment;
    return S_OK;
}

static HRESULT WINAPI tt_style_GetTextDecoration(IMFTimedTextStyle *iface, DWORD *decoration)
{
    if (!decoration) return E_POINTER;
    *decoration = impl_from_IMFTimedTextStyle(iface)->decoration;
    return S_OK;
}

static HRESULT WINAPI tt_style_GetTextOutline(IMFTimedTextStyle *iface, MFARGB *color, double *thickness,
        double *blur, MF_TIMED_TEXT_UNIT_TYPE *unit)
{
    struct tt_style *style = impl_from_IMFTimedTextStyle(iface);

    if (!color || !thickness || !blur || !unit) return E_POINTER;
    *color = style->outline_color;
    *thickness = style->outline_thickness;
    *blur = style->outline_blur;
    *unit = style->outline_unit;
    return S_OK;
}

static const IMFTimedTextStyleVtbl tt_style_vtbl =
{
    tt_style_QueryInterface,
    tt_style_AddRef,
    tt_style_Release,
    tt_style_GetName,
    tt_style_IsExternal,
    tt_style_GetFontFamily,
    tt_style_GetFontSize,
    tt_style_GetColor,
    tt_style_GetBackgroundColor,
    tt_style_GetShowBackgroundAlways,
    tt_style_GetFontStyle,
    tt_style_GetBold,
    tt_style_GetRightToLeft,
    tt_style_GetTextAlignment,
    tt_style_GetTextDecoration,
    tt_style_GetTextOutline,
};

/* What a style starts from: WebVTT's defaults, white text on a translucent black
 * background, 5% of the video height, centred. */
static struct tt_style *tt_style_create(const struct tt_style *base, const WCHAR *name)
{
    struct tt_style *style;

    if (!(style = calloc(1, sizeof(*style)))) return NULL;
    style->IMFTimedTextStyle_iface.lpVtbl = &tt_style_vtbl;
    style->refcount = 1;
    if (base)
    {
        style->font_size = base->font_size;
        style->font_size_unit = base->font_size_unit;
        style->color = base->color;
        style->background = base->background;
        style->show_background_always = base->show_background_always;
        style->font_style = base->font_style;
        style->bold = base->bold;
        style->right_to_left = base->right_to_left;
        style->alignment = base->alignment;
        style->decoration = base->decoration;
        style->outline_color = base->outline_color;
        style->outline_thickness = base->outline_thickness;
        style->outline_blur = base->outline_blur;
        style->outline_unit = base->outline_unit;
        style->font_family = heap_wcsdup(base->font_family);
    }
    else
    {
        style->font_size = 5.0;
        style->font_size_unit = MF_TIMED_TEXT_UNIT_TYPE_PERCENTAGE;
        style->color = argb(0xff, 0xff, 0xff, 0xff);
        style->background = argb(0xcc, 0, 0, 0);
        style->alignment = MF_TIMED_TEXT_ALIGNMENT_CENTER;
        style->outline_color = argb(0, 0, 0, 0);
        style->outline_unit = MF_TIMED_TEXT_UNIT_TYPE_PIXELS;
        style->font_family = heap_wcsdup(L"default");
    }
    style->name = heap_wcsdup(name);
    if (!style->name || !style->font_family)
    {
        IMFTimedTextStyle_Release(&style->IMFTimedTextStyle_iface);
        return NULL;
    }
    return style;
}

/* Regions */

struct tt_region
{
    IMFTimedTextRegion IMFTimedTextRegion_iface;
    LONG refcount;

    WCHAR *name;
    double x, y, width, height;
    MF_TIMED_TEXT_UNIT_TYPE unit;
    MFARGB background;
    MF_TIMED_TEXT_WRITING_MODE writing_mode;
    MF_TIMED_TEXT_DISPLAY_ALIGNMENT display_alignment;
    double line_height;
    MF_TIMED_TEXT_UNIT_TYPE line_height_unit;
    BOOL clip_overflow;
    double padding[4];
    BOOL wrap;
    INT32 zindex;
    MF_TIMED_TEXT_SCROLL_MODE scroll_mode;
};

static struct tt_region *impl_from_IMFTimedTextRegion(IMFTimedTextRegion *iface)
{
    return CONTAINING_RECORD(iface, struct tt_region, IMFTimedTextRegion_iface);
}

static HRESULT WINAPI tt_region_QueryInterface(IMFTimedTextRegion *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFTimedTextRegion) || IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFTimedTextRegion_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI tt_region_AddRef(IMFTimedTextRegion *iface)
{
    return InterlockedIncrement(&impl_from_IMFTimedTextRegion(iface)->refcount);
}

static ULONG WINAPI tt_region_Release(IMFTimedTextRegion *iface)
{
    struct tt_region *region = impl_from_IMFTimedTextRegion(iface);
    ULONG refcount = InterlockedDecrement(&region->refcount);

    if (!refcount)
    {
        free(region->name);
        free(region);
    }
    return refcount;
}

static HRESULT WINAPI tt_region_GetName(IMFTimedTextRegion *iface, WCHAR **name)
{
    return return_string(impl_from_IMFTimedTextRegion(iface)->name, name);
}

static HRESULT WINAPI tt_region_GetPosition(IMFTimedTextRegion *iface, double *x, double *y,
        MF_TIMED_TEXT_UNIT_TYPE *unit)
{
    struct tt_region *region = impl_from_IMFTimedTextRegion(iface);

    if (!x || !y || !unit) return E_POINTER;
    *x = region->x;
    *y = region->y;
    *unit = region->unit;
    return S_OK;
}

static HRESULT WINAPI tt_region_GetExtent(IMFTimedTextRegion *iface, double *width, double *height,
        MF_TIMED_TEXT_UNIT_TYPE *unit)
{
    struct tt_region *region = impl_from_IMFTimedTextRegion(iface);

    if (!width || !height || !unit) return E_POINTER;
    *width = region->width;
    *height = region->height;
    *unit = region->unit;
    return S_OK;
}

static HRESULT WINAPI tt_region_GetBackgroundColor(IMFTimedTextRegion *iface, MFARGB *color)
{
    if (!color) return E_POINTER;
    *color = impl_from_IMFTimedTextRegion(iface)->background;
    return S_OK;
}

static HRESULT WINAPI tt_region_GetWritingMode(IMFTimedTextRegion *iface, MF_TIMED_TEXT_WRITING_MODE *mode)
{
    if (!mode) return E_POINTER;
    *mode = impl_from_IMFTimedTextRegion(iface)->writing_mode;
    return S_OK;
}

static HRESULT WINAPI tt_region_GetDisplayAlignment(IMFTimedTextRegion *iface,
        MF_TIMED_TEXT_DISPLAY_ALIGNMENT *alignment)
{
    if (!alignment) return E_POINTER;
    *alignment = impl_from_IMFTimedTextRegion(iface)->display_alignment;
    return S_OK;
}

static HRESULT WINAPI tt_region_GetLineHeight(IMFTimedTextRegion *iface, double *height,
        MF_TIMED_TEXT_UNIT_TYPE *unit)
{
    struct tt_region *region = impl_from_IMFTimedTextRegion(iface);

    if (!height || !unit) return E_POINTER;
    *height = region->line_height;
    *unit = region->line_height_unit;
    return S_OK;
}

static HRESULT WINAPI tt_region_GetClipOverflow(IMFTimedTextRegion *iface, BOOL *clip)
{
    if (!clip) return E_POINTER;
    *clip = impl_from_IMFTimedTextRegion(iface)->clip_overflow;
    return S_OK;
}

static HRESULT WINAPI tt_region_GetPadding(IMFTimedTextRegion *iface, double *before, double *start,
        double *after, double *end, MF_TIMED_TEXT_UNIT_TYPE *unit)
{
    struct tt_region *region = impl_from_IMFTimedTextRegion(iface);

    if (!before || !start || !after || !end || !unit) return E_POINTER;
    *before = region->padding[0];
    *start = region->padding[1];
    *after = region->padding[2];
    *end = region->padding[3];
    *unit = region->unit;
    return S_OK;
}

static HRESULT WINAPI tt_region_GetWrap(IMFTimedTextRegion *iface, BOOL *wrap)
{
    if (!wrap) return E_POINTER;
    *wrap = impl_from_IMFTimedTextRegion(iface)->wrap;
    return S_OK;
}

static HRESULT WINAPI tt_region_GetZIndex(IMFTimedTextRegion *iface, INT32 *zindex)
{
    if (!zindex) return E_POINTER;
    *zindex = impl_from_IMFTimedTextRegion(iface)->zindex;
    return S_OK;
}

static HRESULT WINAPI tt_region_GetScrollMode(IMFTimedTextRegion *iface, MF_TIMED_TEXT_SCROLL_MODE *mode)
{
    if (!mode) return E_POINTER;
    *mode = impl_from_IMFTimedTextRegion(iface)->scroll_mode;
    return S_OK;
}

static const IMFTimedTextRegionVtbl tt_region_vtbl =
{
    tt_region_QueryInterface,
    tt_region_AddRef,
    tt_region_Release,
    tt_region_GetName,
    tt_region_GetPosition,
    tt_region_GetExtent,
    tt_region_GetBackgroundColor,
    tt_region_GetWritingMode,
    tt_region_GetDisplayAlignment,
    tt_region_GetLineHeight,
    tt_region_GetClipOverflow,
    tt_region_GetPadding,
    tt_region_GetWrap,
    tt_region_GetZIndex,
    tt_region_GetScrollMode,
};

/* The whole video, text at the bottom: where cues go unless they say otherwise. */
static struct tt_region *tt_region_create(const struct tt_region *base, const WCHAR *name)
{
    struct tt_region *region;

    if (!(region = calloc(1, sizeof(*region)))) return NULL;
    region->IMFTimedTextRegion_iface.lpVtbl = &tt_region_vtbl;
    region->refcount = 1;
    if (base)
    {
        *region = *base;
        region->IMFTimedTextRegion_iface.lpVtbl = &tt_region_vtbl;
        region->refcount = 1;
    }
    else
    {
        region->width = 100.0;
        region->height = 100.0;
        region->unit = MF_TIMED_TEXT_UNIT_TYPE_PERCENTAGE;
        region->background = argb(0, 0, 0, 0);
        region->writing_mode = MF_TIMED_TEXT_WRITING_MODE_LRTB;
        region->display_alignment = MF_TIMED_TEXT_DISPLAY_ALIGNMENT_AFTER;
        region->line_height = -1.0; /* normal */
        region->line_height_unit = MF_TIMED_TEXT_UNIT_TYPE_PIXELS;
        region->clip_overflow = TRUE;
        region->wrap = TRUE;
        region->scroll_mode = MF_TIMED_TEXT_SCROLL_MODE_POP_ON;
    }
    if (!(region->name = heap_wcsdup(name)))
    {
        free(region);
        return NULL;
    }
    return region;
}

/* Formatted text: one line of a cue, with the runs that differ from the cue's style. */

struct tt_run
{
    DWORD first;
    DWORD length;
    struct tt_style *style;
};

struct tt_text
{
    IMFTimedTextFormattedText IMFTimedTextFormattedText_iface;
    LONG refcount;

    WCHAR *text;
    struct tt_run *runs;
    size_t run_count;
};

static struct tt_text *impl_from_IMFTimedTextFormattedText(IMFTimedTextFormattedText *iface)
{
    return CONTAINING_RECORD(iface, struct tt_text, IMFTimedTextFormattedText_iface);
}

static HRESULT WINAPI tt_text_QueryInterface(IMFTimedTextFormattedText *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFTimedTextFormattedText) || IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFTimedTextFormattedText_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI tt_text_AddRef(IMFTimedTextFormattedText *iface)
{
    return InterlockedIncrement(&impl_from_IMFTimedTextFormattedText(iface)->refcount);
}

static ULONG WINAPI tt_text_Release(IMFTimedTextFormattedText *iface)
{
    struct tt_text *text = impl_from_IMFTimedTextFormattedText(iface);
    ULONG refcount = InterlockedDecrement(&text->refcount);
    size_t i;

    if (!refcount)
    {
        for (i = 0; i < text->run_count; ++i)
            IMFTimedTextStyle_Release(&text->runs[i].style->IMFTimedTextStyle_iface);
        free(text->runs);
        free(text->text);
        free(text);
    }
    return refcount;
}

static HRESULT WINAPI tt_text_GetText(IMFTimedTextFormattedText *iface, WCHAR **text)
{
    return return_string(impl_from_IMFTimedTextFormattedText(iface)->text, text);
}

static DWORD WINAPI tt_text_GetSubformattingCount(IMFTimedTextFormattedText *iface)
{
    return impl_from_IMFTimedTextFormattedText(iface)->run_count;
}

static HRESULT WINAPI tt_text_GetSubformatting(IMFTimedTextFormattedText *iface, DWORD index, DWORD *first,
        DWORD *length, IMFTimedTextStyle **style)
{
    struct tt_text *text = impl_from_IMFTimedTextFormattedText(iface);

    if (!first || !length || !style) return E_POINTER;
    if (index >= text->run_count)
    {
        *style = NULL;
        return E_INVALIDARG;
    }
    *first = text->runs[index].first;
    *length = text->runs[index].length;
    IMFTimedTextStyle_AddRef((*style = &text->runs[index].style->IMFTimedTextStyle_iface));
    return S_OK;
}

static const IMFTimedTextFormattedTextVtbl tt_text_vtbl =
{
    tt_text_QueryInterface,
    tt_text_AddRef,
    tt_text_Release,
    tt_text_GetText,
    tt_text_GetSubformattingCount,
    tt_text_GetSubformatting,
};

/* A line under construction, with a stack of the inline styles open at its end. */
struct text_builder
{
    WCHAR *text;
    size_t length, capacity;
    struct tt_run *runs;
    size_t run_count, run_capacity;
    const struct tt_style *base;
    struct tt_style *current; /* style of the text being added, NULL for the cue's */
    DWORD run_start;
};

static BOOL text_builder_reserve(void **elements, size_t *capacity, size_t count, size_t size)
{
    size_t new_capacity;
    void *new_elements;

    if (count <= *capacity) return TRUE;
    new_capacity = max(*capacity * 2, max(count, 16));
    if (!(new_elements = realloc(*elements, new_capacity * size))) return FALSE;
    *elements = new_elements;
    *capacity = new_capacity;
    return TRUE;
}

static BOOL text_builder_append(struct text_builder *builder, const WCHAR *str, size_t count)
{
    if (!text_builder_reserve((void **)&builder->text, &builder->capacity, builder->length + count + 1,
            sizeof(WCHAR)))
        return FALSE;
    memcpy(builder->text + builder->length, str, count * sizeof(WCHAR));
    builder->length += count;
    builder->text[builder->length] = 0;
    return TRUE;
}

/* End the run of the current style at the end of the text so far. */
static void text_builder_close_run(struct text_builder *builder)
{
    struct tt_run *run;

    if (builder->current && builder->length > builder->run_start
            && text_builder_reserve((void **)&builder->runs, &builder->run_capacity, builder->run_count + 1,
            sizeof(*builder->runs)))
    {
        run = &builder->runs[builder->run_count++];
        run->first = builder->run_start;
        run->length = builder->length - builder->run_start;
        IMFTimedTextStyle_AddRef(&builder->current->IMFTimedTextStyle_iface);
        run->style = builder->current;
    }
    builder->run_start = builder->length;
}

/* Switch to the style the open tags give, which is NULL when it is the cue's own. */
static void text_builder_set_style(struct text_builder *builder, struct tt_style *style)
{
    if (style == builder->current) return;
    text_builder_close_run(builder);
    if (builder->current) IMFTimedTextStyle_Release(&builder->current->IMFTimedTextStyle_iface);
    if ((builder->current = style)) IMFTimedTextStyle_AddRef(&style->IMFTimedTextStyle_iface);
}

static struct tt_text *text_builder_finish(struct text_builder *builder)
{
    struct tt_text *text;

    text_builder_close_run(builder);
    text_builder_set_style(builder, NULL);
    if (!(text = calloc(1, sizeof(*text)))) return NULL;
    text->IMFTimedTextFormattedText_iface.lpVtbl = &tt_text_vtbl;
    text->refcount = 1;
    text->text = builder->text ? builder->text : heap_wcsdup(L"");
    text->runs = builder->runs;
    text->run_count = builder->run_count;
    memset(builder, 0, sizeof(*builder));
    return text;
}

static void text_builder_cleanup(struct text_builder *builder)
{
    size_t i;

    text_builder_set_style(builder, NULL);
    for (i = 0; i < builder->run_count; ++i)
        IMFTimedTextStyle_Release(&builder->runs[i].style->IMFTimedTextStyle_iface);
    free(builder->runs);
    free(builder->text);
    memset(builder, 0, sizeof(*builder));
}

/* Binary data of metadata cues */

struct tt_binary
{
    IMFTimedTextBinary IMFTimedTextBinary_iface;
    LONG refcount;
    BYTE *data;
    DWORD size;
};

static struct tt_binary *impl_from_IMFTimedTextBinary(IMFTimedTextBinary *iface)
{
    return CONTAINING_RECORD(iface, struct tt_binary, IMFTimedTextBinary_iface);
}

static HRESULT WINAPI tt_binary_QueryInterface(IMFTimedTextBinary *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFTimedTextBinary) || IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFTimedTextBinary_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI tt_binary_AddRef(IMFTimedTextBinary *iface)
{
    return InterlockedIncrement(&impl_from_IMFTimedTextBinary(iface)->refcount);
}

static ULONG WINAPI tt_binary_Release(IMFTimedTextBinary *iface)
{
    struct tt_binary *binary = impl_from_IMFTimedTextBinary(iface);
    ULONG refcount = InterlockedDecrement(&binary->refcount);

    if (!refcount)
    {
        free(binary->data);
        free(binary);
    }
    return refcount;
}

static HRESULT WINAPI tt_binary_GetData(IMFTimedTextBinary *iface, const BYTE **data, DWORD *length)
{
    struct tt_binary *binary = impl_from_IMFTimedTextBinary(iface);

    if (!data || !length) return E_POINTER;
    *data = binary->data;
    *length = binary->size;
    return S_OK;
}

static const IMFTimedTextBinaryVtbl tt_binary_vtbl =
{
    tt_binary_QueryInterface,
    tt_binary_AddRef,
    tt_binary_Release,
    tt_binary_GetData,
};

/* Cues */

struct tt_cue
{
    IMFTimedTextCue IMFTimedTextCue_iface;
    LONG refcount;

    DWORD id;
    WCHAR *original_id;
    MF_TIMED_TEXT_TRACK_KIND kind;
    double start;
    double duration;
    DWORD track_id;
    struct tt_binary *data;
    struct tt_region *region;
    struct tt_style *style;
    struct tt_text **lines;
    size_t line_count;
};

static struct tt_cue *impl_from_IMFTimedTextCue(IMFTimedTextCue *iface)
{
    return CONTAINING_RECORD(iface, struct tt_cue, IMFTimedTextCue_iface);
}

static HRESULT WINAPI tt_cue_QueryInterface(IMFTimedTextCue *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFTimedTextCue) || IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFTimedTextCue_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI tt_cue_AddRef(IMFTimedTextCue *iface)
{
    return InterlockedIncrement(&impl_from_IMFTimedTextCue(iface)->refcount);
}

static ULONG WINAPI tt_cue_Release(IMFTimedTextCue *iface)
{
    struct tt_cue *cue = impl_from_IMFTimedTextCue(iface);
    ULONG refcount = InterlockedDecrement(&cue->refcount);
    size_t i;

    if (!refcount)
    {
        for (i = 0; i < cue->line_count; ++i)
            IMFTimedTextFormattedText_Release(&cue->lines[i]->IMFTimedTextFormattedText_iface);
        free(cue->lines);
        if (cue->data) IMFTimedTextBinary_Release(&cue->data->IMFTimedTextBinary_iface);
        if (cue->region) IMFTimedTextRegion_Release(&cue->region->IMFTimedTextRegion_iface);
        if (cue->style) IMFTimedTextStyle_Release(&cue->style->IMFTimedTextStyle_iface);
        free(cue->original_id);
        free(cue);
    }
    return refcount;
}

static DWORD WINAPI tt_cue_GetId(IMFTimedTextCue *iface)
{
    return impl_from_IMFTimedTextCue(iface)->id;
}

static HRESULT WINAPI tt_cue_GetOriginalId(IMFTimedTextCue *iface, WCHAR **original_id)
{
    return return_string(impl_from_IMFTimedTextCue(iface)->original_id, original_id);
}

static MF_TIMED_TEXT_TRACK_KIND WINAPI tt_cue_GetCueKind(IMFTimedTextCue *iface)
{
    return impl_from_IMFTimedTextCue(iface)->kind;
}

static double WINAPI tt_cue_GetStartTime(IMFTimedTextCue *iface)
{
    return impl_from_IMFTimedTextCue(iface)->start;
}

static double WINAPI tt_cue_GetDuration(IMFTimedTextCue *iface)
{
    return impl_from_IMFTimedTextCue(iface)->duration;
}

static DWORD WINAPI tt_cue_GetTrackId(IMFTimedTextCue *iface)
{
    return impl_from_IMFTimedTextCue(iface)->track_id;
}

static HRESULT WINAPI tt_cue_GetData(IMFTimedTextCue *iface, IMFTimedTextBinary **data)
{
    struct tt_cue *cue = impl_from_IMFTimedTextCue(iface);

    if (!data) return E_POINTER;
    if (!(*data = cue->data ? &cue->data->IMFTimedTextBinary_iface : NULL))
        return MF_E_NOT_FOUND;
    IMFTimedTextBinary_AddRef(*data);
    return S_OK;
}

static HRESULT WINAPI tt_cue_GetRegion(IMFTimedTextCue *iface, IMFTimedTextRegion **region)
{
    struct tt_cue *cue = impl_from_IMFTimedTextCue(iface);

    if (!region) return E_POINTER;
    if (!(*region = cue->region ? &cue->region->IMFTimedTextRegion_iface : NULL))
        return MF_E_NOT_FOUND;
    IMFTimedTextRegion_AddRef(*region);
    return S_OK;
}

static HRESULT WINAPI tt_cue_GetStyle(IMFTimedTextCue *iface, IMFTimedTextStyle **style)
{
    struct tt_cue *cue = impl_from_IMFTimedTextCue(iface);

    if (!style) return E_POINTER;
    if (!(*style = cue->style ? &cue->style->IMFTimedTextStyle_iface : NULL))
        return MF_E_NOT_FOUND;
    IMFTimedTextStyle_AddRef(*style);
    return S_OK;
}

static DWORD WINAPI tt_cue_GetLineCount(IMFTimedTextCue *iface)
{
    return impl_from_IMFTimedTextCue(iface)->line_count;
}

static HRESULT WINAPI tt_cue_GetLine(IMFTimedTextCue *iface, DWORD index, IMFTimedTextFormattedText **line)
{
    struct tt_cue *cue = impl_from_IMFTimedTextCue(iface);

    if (!line) return E_POINTER;
    if (index >= cue->line_count)
    {
        *line = NULL;
        return E_INVALIDARG;
    }
    IMFTimedTextFormattedText_AddRef((*line = &cue->lines[index]->IMFTimedTextFormattedText_iface));
    return S_OK;
}

static const IMFTimedTextCueVtbl tt_cue_vtbl =
{
    tt_cue_QueryInterface,
    tt_cue_AddRef,
    tt_cue_Release,
    tt_cue_GetId,
    tt_cue_GetOriginalId,
    tt_cue_GetCueKind,
    tt_cue_GetStartTime,
    tt_cue_GetDuration,
    tt_cue_GetTrackId,
    tt_cue_GetData,
    tt_cue_GetRegion,
    tt_cue_GetStyle,
    tt_cue_GetLineCount,
    tt_cue_GetLine,
};

static struct tt_cue *tt_cue_create(double start, double duration, const WCHAR *original_id)
{
    struct tt_cue *cue;

    if (!(cue = calloc(1, sizeof(*cue)))) return NULL;
    cue->IMFTimedTextCue_iface.lpVtbl = &tt_cue_vtbl;
    cue->refcount = 1;
    cue->start = start;
    cue->duration = max(duration, 0.0);
    if (!(cue->original_id = heap_wcsdup(original_id)))
    {
        free(cue);
        return NULL;
    }
    return cue;
}

static BOOL tt_cue_add_line(struct tt_cue *cue, struct tt_text *line)
{
    struct tt_text **lines;

    if (!(lines = realloc(cue->lines, (cue->line_count + 1) * sizeof(*lines)))) return FALSE;
    cue->lines = lines;
    cue->lines[cue->line_count++] = line;
    return TRUE;
}

/* Inline markup shared by WebVTT and SRT: <b>, <i>, <u>, SRT's <font color>, and
 * WebVTT's classes, voices, languages and timestamps, which carry no style here. */

enum inline_flag
{
    INLINE_BOLD = 0x1,
    INLINE_ITALIC = 0x2,
    INLINE_UNDERLINE = 0x4,
    INLINE_COLOR = 0x8,
};

struct inline_state
{
    unsigned int flags;
    MFARGB color;
};

struct inline_styles
{
    const struct tt_style *base;
    struct
    {
        struct inline_state state;
        struct tt_style *style;
    } cache[16];
    size_t count;
};

/* The style for a combination of open tags, made once per cue. */
static struct tt_style *inline_styles_get(struct inline_styles *styles, const struct inline_state *state)
{
    struct tt_style *style;
    size_t i;

    if (!state->flags) return NULL;
    for (i = 0; i < styles->count; ++i)
    {
        if (styles->cache[i].state.flags == state->flags
                && (!(state->flags & INLINE_COLOR) || !memcmp(&styles->cache[i].state.color, &state->color,
                sizeof(state->color))))
            return styles->cache[i].style;
    }
    if (!(style = tt_style_create(styles->base, L""))) return NULL;
    if (state->flags & INLINE_BOLD) style->bold = TRUE;
    if (state->flags & INLINE_ITALIC) style->font_style = MF_TIMED_TEXT_FONT_STYLE_ITALIC;
    if (state->flags & INLINE_UNDERLINE) style->decoration |= MF_TIMED_TEXT_DECORATION_UNDERLINE;
    if (state->flags & INLINE_COLOR) style->color = state->color;
    if (styles->count < ARRAY_SIZE(styles->cache))
    {
        styles->cache[styles->count].state = *state;
        styles->cache[styles->count++].style = style;
    }
    else
    {
        /* Out of slots: let the builder keep the only reference. */
        IMFTimedTextStyle_Release(&style->IMFTimedTextStyle_iface);
        return NULL;
    }
    return style;
}

static void inline_styles_cleanup(struct inline_styles *styles)
{
    size_t i;

    for (i = 0; i < styles->count; ++i)
        IMFTimedTextStyle_Release(&styles->cache[i].style->IMFTimedTextStyle_iface);
    styles->count = 0;
}

static const struct
{
    const WCHAR *name;
    MFARGB color;
}
named_colors[] =
{
    { L"white",   { 0xff, 0xff, 0xff, 0xff } },
    { L"black",   { 0x00, 0x00, 0x00, 0xff } },
    { L"red",     { 0x00, 0x00, 0xff, 0xff } },
    { L"lime",    { 0x00, 0xff, 0x00, 0xff } },
    { L"green",   { 0x00, 0x80, 0x00, 0xff } },
    { L"blue",    { 0xff, 0x00, 0x00, 0xff } },
    { L"yellow",  { 0x00, 0xff, 0xff, 0xff } },
    { L"cyan",    { 0xff, 0xff, 0x00, 0xff } },
    { L"aqua",    { 0xff, 0xff, 0x00, 0xff } },
    { L"magenta", { 0xff, 0x00, 0xff, 0xff } },
    { L"fuchsia", { 0xff, 0x00, 0xff, 0xff } },
    { L"gray",    { 0x80, 0x80, 0x80, 0xff } },
    { L"grey",    { 0x80, 0x80, 0x80, 0xff } },
    { L"silver",  { 0xc0, 0xc0, 0xc0, 0xff } },
    { L"maroon",  { 0x00, 0x00, 0x80, 0xff } },
    { L"olive",   { 0x00, 0x80, 0x80, 0xff } },
    { L"navy",    { 0x80, 0x00, 0x00, 0xff } },
    { L"purple",  { 0x80, 0x00, 0x80, 0xff } },
    { L"teal",    { 0x80, 0x80, 0x00, 0xff } },
    { L"transparent", { 0x00, 0x00, 0x00, 0x00 } },
};

static int hex_digit(WCHAR c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "#rgb", "#rrggbb", "#rrggbbaa", "rgb(r,g,b)", "rgba(r,g,b,a)" or a colour name. */
static BOOL parse_color(const WCHAR *str, size_t len, MFARGB *color)
{
    WCHAR buffer[64];
    unsigned int values[8], count = 0, i;
    int digit;

    if (len >= ARRAY_SIZE(buffer)) return FALSE;
    while (len && (*str == ' ' || *str == '"' || *str == '\'')) { ++str; --len; }
    while (len && (str[len - 1] == ' ' || str[len - 1] == '"' || str[len - 1] == '\'')) --len;
    memcpy(buffer, str, len * sizeof(WCHAR));
    buffer[len] = 0;

    if (buffer[0] == '#')
    {
        for (i = 1; i < len; ++i)
        {
            if ((digit = hex_digit(buffer[i])) < 0 || count >= ARRAY_SIZE(values)) return FALSE;
            values[count++] = digit;
        }
        if (count == 3)
            *color = argb(0xff, values[0] * 17, values[1] * 17, values[2] * 17);
        else if (count == 6 || count == 8)
            *color = argb(count == 8 ? values[6] * 16 + values[7] : 0xff, values[0] * 16 + values[1],
                    values[2] * 16 + values[3], values[4] * 16 + values[5]);
        else
            return FALSE;
        return TRUE;
    }
    if (!wcsnicmp(buffer, L"rgb", 3))
    {
        double v[4] = {0, 0, 0, 1};
        WCHAR *p = wcschr(buffer, '('), *end;

        if (!p) return FALSE;
        for (count = 0, ++p; count < 4 && *p && *p != ')'; ++count)
        {
            v[count] = wcstod(p, &end);
            if (end == p) return FALSE;
            p = end;
            while (*p == ',' || *p == ' ') ++p;
        }
        if (count < 3) return FALSE;
        *color = argb((BYTE)(min(max(v[3], 0.0), 1.0) * 255 + 0.5), (BYTE)min(max(v[0], 0.0), 255.0),
                (BYTE)min(max(v[1], 0.0), 255.0), (BYTE)min(max(v[2], 0.0), 255.0));
        return TRUE;
    }
    for (i = 0; i < ARRAY_SIZE(named_colors); ++i)
    {
        if (!wcsicmp(buffer, named_colors[i].name))
        {
            *color = named_colors[i].color;
            return TRUE;
        }
    }
    return FALSE;
}

/* Decode an entity at str (after '&'); returns the characters consumed, 0 if none. */
static size_t parse_entity(const WCHAR *str, WCHAR *out)
{
    static const struct { const WCHAR *name; WCHAR ch; } entities[] =
    {
        { L"amp;", '&' }, { L"lt;", '<' }, { L"gt;", '>' }, { L"nbsp;", 0xa0 },
        { L"lrm;", 0x200e }, { L"rlm;", 0x200f }, { L"quot;", '"' }, { L"apos;", '\'' },
    };
    unsigned int i, value = 0;
    const WCHAR *p;

    for (i = 0; i < ARRAY_SIZE(entities); ++i)
    {
        size_t len = wcslen(entities[i].name);
        if (!wcsncmp(str, entities[i].name, len))
        {
            *out = entities[i].ch;
            return len;
        }
    }
    if (str[0] == '#')
    {
        p = str + 1;
        if (*p == 'x' || *p == 'X')
            for (++p; hex_digit(*p) >= 0; ++p) value = value * 16 + hex_digit(*p);
        else
            for (; *p >= '0' && *p <= '9'; ++p) value = value * 10 + (*p - '0');
        if (*p == ';' && p > str + 1 && value && value < 0x10000)
        {
            *out = value;
            return p + 1 - str;
        }
    }
    return 0;
}

static const WCHAR *find_char(const WCHAR *p, const WCHAR *end, WCHAR ch)
{
    for (; p < end; ++p) if (*p == ch) return p;
    return NULL;
}

/* Parse one payload line into formatted text. state carries tags left open on
 * earlier lines of the same cue, as WebVTT allows. */
static struct tt_text *parse_inline_line(const WCHAR *line, size_t len, struct inline_styles *styles,
        struct inline_state *stack, size_t *depth, size_t max_depth)
{
    struct text_builder builder = { .base = styles->base };
    struct inline_state state = *depth ? stack[*depth - 1] : (struct inline_state){0};
    const WCHAR *p = line, *end = line + len, *tag_end;
    WCHAR ch;
    size_t used;

    text_builder_set_style(&builder, inline_styles_get(styles, &state));
    while (p < end)
    {
        if (*p == '<' && (tag_end = find_char(p, end, '>')))
        {
            const WCHAR *name = p + 1;
            BOOL closing = *name == '/';
            size_t name_len;

            if (closing) ++name;
            for (name_len = 0; name + name_len < tag_end && name[name_len] != ' ' && name[name_len] != '.'
                    && name[name_len] != '\t'; ++name_len) ;

            if (closing)
            {
                if (*depth) --*depth;
                state = *depth ? stack[*depth - 1] : (struct inline_state){0};
            }
            else if (name_len && (name[0] < '0' || name[0] > '9'))
            {
                /* Every open tag nests, styled or not, so closing tags pair up. */
                if (name_len == 1 && (name[0] == 'b' || name[0] == 'B')) state.flags |= INLINE_BOLD;
                else if (name_len == 1 && (name[0] == 'i' || name[0] == 'I')) state.flags |= INLINE_ITALIC;
                else if (name_len == 1 && (name[0] == 'u' || name[0] == 'U')) state.flags |= INLINE_UNDERLINE;
                else if (name_len == 4 && !wcsnicmp(name, L"font", 4))
                {
                    const WCHAR *attr = name + 4, *value;
                    for (; attr < tag_end; ++attr)
                    {
                        if (!wcsnicmp(attr, L"color=", 6))
                        {
                            const WCHAR *value_end;
                            value = attr + 6;
                            for (value_end = value + 1; value_end < tag_end && *value_end != ' '; ++value_end) ;
                            if (parse_color(value, value_end - value, &state.color)) state.flags |= INLINE_COLOR;
                            break;
                        }
                    }
                }
                if (*depth < max_depth) stack[(*depth)++] = state;
            }
            /* A timestamp tag (<00:01.000>) or anything else carries no style. */
            text_builder_set_style(&builder, inline_styles_get(styles, &state));
            p = tag_end + 1;
            continue;
        }
        if (*p == '&' && (used = parse_entity(p + 1, &ch)))
        {
            if (!text_builder_append(&builder, &ch, 1)) goto failed;
            p += used + 1;
            continue;
        }
        if (!text_builder_append(&builder, p, 1)) goto failed;
        ++p;
    }
    return text_builder_finish(&builder);

failed:
    text_builder_cleanup(&builder);
    return NULL;
}

/* Parsed sources */

struct cue_array
{
    struct tt_cue **cues;
    size_t count, capacity;
};

static BOOL cue_array_add(struct cue_array *array, struct tt_cue *cue)
{
    if (!text_builder_reserve((void **)&array->cues, &array->capacity, array->count + 1, sizeof(*array->cues)))
        return FALSE;
    array->cues[array->count++] = cue;
    return TRUE;
}

static void cue_array_cleanup(struct cue_array *array)
{
    size_t i;

    for (i = 0; i < array->count; ++i)
        IMFTimedTextCue_Release(&array->cues[i]->IMFTimedTextCue_iface);
    free(array->cues);
    memset(array, 0, sizeof(*array));
}

/* Lines of a document; split in place, each without its line break. */
struct lines
{
    WCHAR **line;
    size_t count;
};

static BOOL split_lines(WCHAR *text, struct lines *lines)
{
    size_t capacity = 0;
    WCHAR *p = text;

    lines->line = NULL;
    lines->count = 0;
    for (;;)
    {
        if (!text_builder_reserve((void **)&lines->line, &capacity, lines->count + 1, sizeof(*lines->line)))
            return FALSE;
        lines->line[lines->count++] = p;
        while (*p && *p != '\r' && *p != '\n') ++p;
        if (!*p) break;
        if (p[0] == '\r' && p[1] == '\n') *p++ = 0;
        *p++ = 0;
    }
    return TRUE;
}

static BOOL is_blank(const WCHAR *line)
{
    for (; *line; ++line) if (*line != ' ' && *line != '\t') return FALSE;
    return TRUE;
}

/* "[hh:]mm:ss.ttt" (WebVTT) or "hh:mm:ss,ttt" (SRT); returns the end, NULL if malformed. */
static const WCHAR *parse_cue_timestamp(const WCHAR *p, double *seconds)
{
    unsigned int fields[3], count = 0, digits;
    double fraction = 0.0, scale = 0.1;

    while (*p == ' ' || *p == '\t') ++p;
    for (;;)
    {
        if (*p < '0' || *p > '9') return NULL;
        fields[count] = 0;
        for (digits = 0; *p >= '0' && *p <= '9'; ++p, ++digits) fields[count] = fields[count] * 10 + (*p - '0');
        ++count;
        if (*p == ':' && count < 3) { ++p; continue; }
        break;
    }
    if (count < 2) return NULL;
    if (*p == '.' || *p == ',')
    {
        for (++p; *p >= '0' && *p <= '9'; ++p, scale /= 10) fraction += (*p - '0') * scale;
    }
    if (count == 3)
        *seconds = fields[0] * 3600.0 + fields[1] * 60.0 + fields[2] + fraction;
    else
        *seconds = fields[0] * 60.0 + fields[1] + fraction;
    return p;
}

/* A timing line, "start --> end [settings]"; returns the settings, NULL if it is not one. */
static const WCHAR *parse_timing_line(const WCHAR *line, double *start, double *end)
{
    const WCHAR *p;

    if (!wcsstr(line, L"-->")) return NULL;
    if (!(p = parse_cue_timestamp(line, start))) return NULL;
    while (*p == ' ' || *p == '\t') ++p;
    if (wcsncmp(p, L"-->", 3)) return NULL;
    if (!(p = parse_cue_timestamp(p + 3, end))) return NULL;
    return p;
}

struct vtt_region
{
    struct tt_region *region;
    WCHAR *id;
};

struct parsed_source
{
    GUID format;
    struct cue_array cues;
    struct tt_style *style;     /* the document's style, for cues that set none */
    struct tt_region *region;   /* the document's region */
    struct vtt_region *regions; /* named regions, WebVTT REGION blocks or TTML <region> */
    size_t region_count;
};

static void parsed_source_cleanup(struct parsed_source *source)
{
    size_t i;

    cue_array_cleanup(&source->cues);
    for (i = 0; i < source->region_count; ++i)
    {
        IMFTimedTextRegion_Release(&source->regions[i].region->IMFTimedTextRegion_iface);
        free(source->regions[i].id);
    }
    free(source->regions);
    if (source->style) IMFTimedTextStyle_Release(&source->style->IMFTimedTextStyle_iface);
    if (source->region) IMFTimedTextRegion_Release(&source->region->IMFTimedTextRegion_iface);
    memset(source, 0, sizeof(*source));
}

static struct tt_region *parsed_source_find_region(struct parsed_source *source, const WCHAR *id, size_t len)
{
    size_t i;

    for (i = 0; i < source->region_count; ++i)
        if (wcslen(source->regions[i].id) == len && !wcsncmp(source->regions[i].id, id, len))
            return source->regions[i].region;
    return NULL;
}

static BOOL parsed_source_add_region(struct parsed_source *source, const WCHAR *id, struct tt_region *region)
{
    struct vtt_region *regions;

    if (!(regions = realloc(source->regions, (source->region_count + 1) * sizeof(*regions)))) return FALSE;
    source->regions = regions;
    if (!(regions[source->region_count].id = heap_wcsdup(id))) return FALSE;
    regions[source->region_count++].region = region;
    return TRUE;
}

/* Lines of a cue's payload, with their markup. */
static BOOL add_payload_lines(struct tt_cue *cue, WCHAR **lines, size_t count)
{
    struct inline_styles styles = { .base = cue->style };
    struct inline_state stack[32];
    size_t i, depth = 0;
    struct tt_text *text;
    BOOL ret = TRUE;

    for (i = 0; i < count && ret; ++i)
    {
        if ((text = parse_inline_line(lines[i], wcslen(lines[i]), &styles, stack, &depth, ARRAY_SIZE(stack))))
            ret = tt_cue_add_line(cue, text);
        else
            ret = FALSE;
        if (!ret && text) IMFTimedTextFormattedText_Release(&text->IMFTimedTextFormattedText_iface);
    }
    inline_styles_cleanup(&styles);
    return ret;
}

static double parse_percentage(const WCHAR *value, const WCHAR **end, BOOL *percent)
{
    WCHAR *stop;
    double ret = wcstod(value, &stop);

    *percent = *stop == '%';
    if (*percent) ++stop;
    *end = stop;
    return ret;
}

/* A setting's value: from after "name:" to the next space or tab. */
static const WCHAR *get_setting(const WCHAR *settings, const WCHAR *name, size_t *len)
{
    size_t name_len = wcslen(name);
    const WCHAR *p = settings;

    while (*p)
    {
        while (*p == ' ' || *p == '\t') ++p;
        if (!wcsncmp(p, name, name_len) && p[name_len] == ':')
        {
            const WCHAR *value = p + name_len + 1, *end = value;
            while (*end && *end != ' ' && *end != '\t') ++end;
            *len = end - value;
            return value;
        }
        while (*p && *p != ' ' && *p != '\t') ++p;
    }
    return NULL;
}

/* WebVTT cue settings: vertical, line, position, size, align, region. */
static HRESULT apply_vtt_settings(struct parsed_source *source, struct tt_cue *cue, const WCHAR *settings)
{
    struct tt_region *named, *region = NULL;
    const WCHAR *value, *end;
    BOOL percent, has_line = FALSE, has_position = FALSE, has_size = FALSE;
    double line = 0.0, position = 50.0, size = 100.0;
    MF_TIMED_TEXT_ALIGNMENT alignment = MF_TIMED_TEXT_ALIGNMENT_CENTER;
    size_t len;

    if ((value = get_setting(settings, L"region", &len)) && (named = parsed_source_find_region(source, value, len)))
    {
        IMFTimedTextRegion_Release(&cue->region->IMFTimedTextRegion_iface);
        IMFTimedTextRegion_AddRef(&(cue->region = named)->IMFTimedTextRegion_iface);
    }

    if ((value = get_setting(settings, L"align", &len)))
    {
        if (!wcsncmp(value, L"start", len) || !wcsncmp(value, L"left", len))
            alignment = MF_TIMED_TEXT_ALIGNMENT_START;
        else if (!wcsncmp(value, L"end", len) || !wcsncmp(value, L"right", len))
            alignment = MF_TIMED_TEXT_ALIGNMENT_END;
    }
    if (alignment != cue->style->alignment)
    {
        struct tt_style *style;
        if (!(style = tt_style_create(cue->style, cue->style->name))) return E_OUTOFMEMORY;
        style->alignment = alignment;
        IMFTimedTextStyle_Release(&cue->style->IMFTimedTextStyle_iface);
        cue->style = style;
    }

    if ((value = get_setting(settings, L"line", &len)))
    {
        line = parse_percentage(value, &end, &percent);
        if (end > value)
        {
            has_line = TRUE;
            /* A line number counts lines of about the default size from the top,
             * or from the bottom when negative. */
            if (!percent) line = line >= 0 ? line * 5.33 : 100.0 + line * 5.33;
            line = min(max(line, 0.0), 100.0);
        }
    }
    if ((value = get_setting(settings, L"position", &len)))
    {
        position = parse_percentage(value, &end, &percent);
        has_position = end > value;
    }
    if ((value = get_setting(settings, L"size", &len)))
    {
        size = parse_percentage(value, &end, &percent);
        has_size = end > value;
        size = min(max(size, 0.0), 100.0);
    }

    if (has_line || has_position || has_size || get_setting(settings, L"vertical", &len))
    {
        if (!(region = tt_region_create(cue->region, L""))) return E_OUTOFMEMORY;
        if ((value = get_setting(settings, L"vertical", &len)))
        {
            if (!wcsncmp(value, L"rl", len)) region->writing_mode = MF_TIMED_TEXT_WRITING_MODE_TBRL;
            else if (!wcsncmp(value, L"lr", len)) region->writing_mode = MF_TIMED_TEXT_WRITING_MODE_TBLR;
        }
        if (has_size || has_position)
        {
            region->width = size;
            switch (alignment)
            {
                case MF_TIMED_TEXT_ALIGNMENT_START: region->x = position; break;
                case MF_TIMED_TEXT_ALIGNMENT_END: region->x = position - size; break;
                default: region->x = position - size / 2; break;
            }
            region->x = min(max(region->x, 0.0), 100.0 - region->width);
        }
        if (has_line)
        {
            /* The line is where the cue's box starts, so it grows downwards from there. */
            region->y = line;
            region->height = 100.0 - line;
            region->display_alignment = MF_TIMED_TEXT_DISPLAY_ALIGNMENT_BEFORE;
        }
        IMFTimedTextRegion_Release(&cue->region->IMFTimedTextRegion_iface);
        cue->region = region;
    }
    return S_OK;
}

/* A WebVTT REGION block: id, width, lines, regionanchor, viewportanchor, scroll. */
static HRESULT parse_vtt_region(struct parsed_source *source, WCHAR **lines, size_t count)
{
    double width = 100.0, line_count = 3, anchor_x = 0, anchor_y = 100, viewport_x = 0, viewport_y = 100;
    struct tt_region *region;
    const WCHAR *value, *end;
    WCHAR *id = NULL;
    BOOL percent, roll_up = FALSE;
    size_t i, len;
    HRESULT hr = S_OK;

    for (i = 0; i < count; ++i)
    {
        if ((value = get_setting(lines[i], L"id", &len))) { free(id); id = heap_wcsndup(value, len); }
        if ((value = get_setting(lines[i], L"width", &len))) width = parse_percentage(value, &end, &percent);
        if ((value = get_setting(lines[i], L"lines", &len))) line_count = wcstod(value, NULL);
        if ((value = get_setting(lines[i], L"regionanchor", &len)))
        {
            anchor_x = parse_percentage(value, &end, &percent);
            if (*end == ',') anchor_y = parse_percentage(end + 1, &end, &percent);
        }
        if ((value = get_setting(lines[i], L"viewportanchor", &len)))
        {
            viewport_x = parse_percentage(value, &end, &percent);
            if (*end == ',') viewport_y = parse_percentage(end + 1, &end, &percent);
        }
        if ((value = get_setting(lines[i], L"scroll", &len)) && !wcsncmp(value, L"up", len)) roll_up = TRUE;
    }
    if (!id) return S_OK;
    if (!(region = tt_region_create(source->region, id)))
        hr = E_OUTOFMEMORY;
    else
    {
        region->width = width;
        region->height = line_count * 5.33;
        /* The anchor point of the region sits on the anchor point of the video. */
        region->x = viewport_x - anchor_x * width / 100.0;
        region->y = viewport_y - anchor_y * region->height / 100.0;
        region->scroll_mode = roll_up ? MF_TIMED_TEXT_SCROLL_MODE_ROLL_UP : MF_TIMED_TEXT_SCROLL_MODE_POP_ON;
        region->display_alignment = MF_TIMED_TEXT_DISPLAY_ALIGNMENT_AFTER;
        if (!parsed_source_add_region(source, id, region))
        {
            IMFTimedTextRegion_Release(&region->IMFTimedTextRegion_iface);
            hr = E_OUTOFMEMORY;
        }
    }
    free(id);
    return hr;
}

/* Cues of WebVTT or SRT: blocks separated by blank lines, each an optional
 * identifier, a timing line and the payload. */
static HRESULT parse_cue_blocks(struct parsed_source *source, WCHAR *text, BOOL vtt)
{
    struct lines lines;
    size_t i = 0, first, payload;
    const WCHAR *settings, *id;
    double start, end;
    struct tt_cue *cue;
    HRESULT hr = S_OK;

    if (!split_lines(text, &lines)) return E_OUTOFMEMORY;

    if (vtt)
    {
        /* The header: "WEBVTT", optionally followed by text, up to a blank line. */
        if (wcsncmp(lines.line[0], L"WEBVTT", 6) || (lines.line[0][6] && lines.line[0][6] != ' '
                && lines.line[0][6] != '\t'))
            hr = MF_E_INVALID_FILE_FORMAT;
        while (i < lines.count && !is_blank(lines.line[i])) ++i;
    }

    while (SUCCEEDED(hr) && i < lines.count)
    {
        while (i < lines.count && is_blank(lines.line[i])) ++i;
        if (i == lines.count) break;
        first = i;
        while (i < lines.count && !is_blank(lines.line[i])) ++i;

        if (vtt && (!wcsncmp(lines.line[first], L"NOTE", 4) || !wcsncmp(lines.line[first], L"STYLE", 5)))
            continue;
        if (vtt && !wcsncmp(lines.line[first], L"REGION", 6) && !wcsstr(lines.line[first], L"-->"))
        {
            hr = parse_vtt_region(source, lines.line + first + 1, i - first - 1);
            continue;
        }

        id = NULL;
        if (!(settings = parse_timing_line(lines.line[first], &start, &end)))
        {
            /* An identifier first: WebVTT's free text, SRT's sequence number. */
            id = lines.line[first];
            if (first + 1 >= i || !(settings = parse_timing_line(lines.line[first + 1], &start, &end)))
            {
                WARN("Skipping a block without timing at line %Iu.\n", first + 1);
                continue;
            }
            payload = first + 2;
        }
        else
            payload = first + 1;

        if (!(cue = tt_cue_create(start, end - start, id)))
        {
            hr = E_OUTOFMEMORY;
            break;
        }
        IMFTimedTextStyle_AddRef(&(cue->style = source->style)->IMFTimedTextStyle_iface);
        IMFTimedTextRegion_AddRef(&(cue->region = source->region)->IMFTimedTextRegion_iface);
        if (vtt) hr = apply_vtt_settings(source, cue, settings);
        if (SUCCEEDED(hr) && !add_payload_lines(cue, lines.line + payload, i - payload))
            hr = E_OUTOFMEMORY;
        if (SUCCEEDED(hr) && !cue_array_add(&source->cues, cue))
            hr = E_OUTOFMEMORY;
        if (FAILED(hr))
            IMFTimedTextCue_Release(&cue->IMFTimedTextCue_iface);
    }

    free(lines.line);
    return hr;
}

/* TTML */

struct ttml_context
{
    struct parsed_source *source;
    IXmlReader *reader;
    double frame_rate;
    double tick_rate;
    struct
    {
        WCHAR *id;
        struct tt_style *style;
    } *styles;
    size_t style_count;
};

static void ttml_context_cleanup(struct ttml_context *ctx)
{
    size_t i;

    for (i = 0; i < ctx->style_count; ++i)
    {
        free(ctx->styles[i].id);
        IMFTimedTextStyle_Release(&ctx->styles[i].style->IMFTimedTextStyle_iface);
    }
    free(ctx->styles);
}

static struct tt_style *ttml_find_style(struct ttml_context *ctx, const WCHAR *id)
{
    size_t i;

    for (i = 0; i < ctx->style_count; ++i)
        if (!wcscmp(ctx->styles[i].id, id)) return ctx->styles[i].style;
    return NULL;
}

/* TTML time expressions: clock time "hh:mm:ss[.fff]" or "hh:mm:ss:ff[.sub]",
 * or an offset such as "1.5s", "200ms", "2h", "3m", "25f" or "1000t". */
static BOOL parse_ttml_time(struct ttml_context *ctx, const WCHAR *value, double *seconds)
{
    unsigned int h, m, s, frames;
    WCHAR *end;
    double number;

    while (*value == ' ') ++value;
    if (swscanf(value, L"%u:%u:%u:%u", &h, &m, &s, &frames) == 4)
    {
        *seconds = h * 3600.0 + m * 60.0 + s + frames / ctx->frame_rate;
        return TRUE;
    }
    if (wcschr(value, ':'))
        return !!parse_cue_timestamp(value, seconds);

    number = wcstod(value, &end);
    if (end == value) return FALSE;
    if (!wcscmp(end, L"h")) *seconds = number * 3600.0;
    else if (!wcscmp(end, L"m")) *seconds = number * 60.0;
    else if (!wcscmp(end, L"s") || !*end) *seconds = number;
    else if (!wcscmp(end, L"ms")) *seconds = number / 1000.0;
    else if (!wcscmp(end, L"f")) *seconds = number / ctx->frame_rate;
    else if (!wcscmp(end, L"t")) *seconds = number / ctx->tick_rate;
    else return FALSE;
    return TRUE;
}

/* A length such as "10%", "40px" or "1.5c" (cells, taken as 1/15 of the height). */
static BOOL parse_ttml_length(const WCHAR *value, double *length, MF_TIMED_TEXT_UNIT_TYPE *unit, const WCHAR **rest)
{
    WCHAR *end;

    *length = wcstod(value, &end);
    if (end == value) return FALSE;
    if (*end == '%') { *unit = MF_TIMED_TEXT_UNIT_TYPE_PERCENTAGE; ++end; }
    else if (!wcsncmp(end, L"px", 2)) { *unit = MF_TIMED_TEXT_UNIT_TYPE_PIXELS; end += 2; }
    else if (*end == 'c') { *length = *length * 100.0 / 15.0; *unit = MF_TIMED_TEXT_UNIT_TYPE_PERCENTAGE; ++end; }
    else if (!wcsncmp(end, L"em", 2)) { *length *= 5.0; *unit = MF_TIMED_TEXT_UNIT_TYPE_PERCENTAGE; end += 2; }
    else *unit = MF_TIMED_TEXT_UNIT_TYPE_PIXELS;
    while (*end == ' ') ++end;
    if (rest) *rest = end;
    return TRUE;
}

/* Apply the tts: attribute the reader is on to a style or a region. */
static void ttml_apply_attribute(const WCHAR *name, const WCHAR *value, struct tt_style *style,
        struct tt_region *region)
{
    MF_TIMED_TEXT_UNIT_TYPE unit;
    const WCHAR *rest;
    double a, b;
    MFARGB color;

    if (!wcscmp(name, L"color") && style && parse_color(value, wcslen(value), &color)) style->color = color;
    else if (!wcscmp(name, L"backgroundColor") && parse_color(value, wcslen(value), &color))
    {
        if (style) style->background = color;
        if (region) region->background = color;
    }
    else if (!wcscmp(name, L"fontFamily") && style)
    {
        WCHAR *family = heap_wcsdup(value);
        if (family) { free(style->font_family); style->font_family = family; }
    }
    else if (!wcscmp(name, L"fontSize") && style && parse_ttml_length(value, &a, &unit, NULL))
    {
        style->font_size = a;
        style->font_size_unit = unit;
    }
    else if (!wcscmp(name, L"fontStyle") && style)
        style->font_style = !wcscmp(value, L"italic") ? MF_TIMED_TEXT_FONT_STYLE_ITALIC
                : !wcscmp(value, L"oblique") ? MF_TIMED_TEXT_FONT_STYLE_OBLIQUE : MF_TIMED_TEXT_FONT_STYLE_NORMAL;
    else if (!wcscmp(name, L"fontWeight") && style) style->bold = !wcscmp(value, L"bold");
    else if (!wcscmp(name, L"textDecoration") && style)
    {
        if (wcsstr(value, L"noUnderline")) style->decoration &= ~MF_TIMED_TEXT_DECORATION_UNDERLINE;
        else if (wcsstr(value, L"underline")) style->decoration |= MF_TIMED_TEXT_DECORATION_UNDERLINE;
        if (wcsstr(value, L"noLineThrough")) style->decoration &= ~MF_TIMED_TEXT_DECORATION_LINE_THROUGH;
        else if (wcsstr(value, L"lineThrough")) style->decoration |= MF_TIMED_TEXT_DECORATION_LINE_THROUGH;
        if (wcsstr(value, L"noOverline")) style->decoration &= ~MF_TIMED_TEXT_DECORATION_OVERLINE;
        else if (wcsstr(value, L"overline")) style->decoration |= MF_TIMED_TEXT_DECORATION_OVERLINE;
    }
    else if (!wcscmp(name, L"textAlign") && style)
        style->alignment = !wcscmp(value, L"left") || !wcscmp(value, L"start") ? MF_TIMED_TEXT_ALIGNMENT_START
                : !wcscmp(value, L"right") || !wcscmp(value, L"end") ? MF_TIMED_TEXT_ALIGNMENT_END
                : MF_TIMED_TEXT_ALIGNMENT_CENTER;
    else if (!wcscmp(name, L"direction") && style) style->right_to_left = !wcscmp(value, L"rtl");
    else if (!wcscmp(name, L"showBackground") && style) style->show_background_always = !wcscmp(value, L"always");
    else if (!wcscmp(name, L"textOutline") && style)
    {
        /* "[color] thickness [blur]" */
        const WCHAR *p = value;
        WCHAR *space = wcschr(value, ' ');
        if (space && parse_color(value, space - value, &color)) { style->outline_color = color; p = space + 1; }
        if (parse_ttml_length(p, &a, &unit, &rest))
        {
            style->outline_thickness = a;
            style->outline_unit = unit;
            if (parse_ttml_length(rest, &b, &unit, NULL)) style->outline_blur = b;
        }
    }
    else if (!wcscmp(name, L"origin") && region && parse_ttml_length(value, &a, &unit, &rest)
            && parse_ttml_length(rest, &b, &unit, NULL))
    {
        region->x = a;
        region->y = b;
        region->unit = unit;
    }
    else if (!wcscmp(name, L"extent") && region && parse_ttml_length(value, &a, &unit, &rest)
            && parse_ttml_length(rest, &b, &unit, NULL))
    {
        region->width = a;
        region->height = b;
        region->unit = unit;
    }
    else if (!wcscmp(name, L"displayAlign") && region)
        region->display_alignment = !wcscmp(value, L"before") ? MF_TIMED_TEXT_DISPLAY_ALIGNMENT_BEFORE
                : !wcscmp(value, L"center") ? MF_TIMED_TEXT_DISPLAY_ALIGNMENT_CENTER
                : MF_TIMED_TEXT_DISPLAY_ALIGNMENT_AFTER;
    else if (!wcscmp(name, L"writingMode") && region)
    {
        static const struct { const WCHAR *name; MF_TIMED_TEXT_WRITING_MODE mode; } modes[] =
        {
            { L"lrtb", MF_TIMED_TEXT_WRITING_MODE_LRTB }, { L"rltb", MF_TIMED_TEXT_WRITING_MODE_RLTB },
            { L"tbrl", MF_TIMED_TEXT_WRITING_MODE_TBRL }, { L"tblr", MF_TIMED_TEXT_WRITING_MODE_TBLR },
            { L"lr", MF_TIMED_TEXT_WRITING_MODE_LR }, { L"rl", MF_TIMED_TEXT_WRITING_MODE_RL },
            { L"tb", MF_TIMED_TEXT_WRITING_MODE_TB },
        };
        unsigned int i;
        for (i = 0; i < ARRAY_SIZE(modes); ++i)
            if (!wcscmp(value, modes[i].name)) region->writing_mode = modes[i].mode;
    }
    else if (!wcscmp(name, L"lineHeight") && region)
    {
        if (!wcscmp(value, L"normal")) region->line_height = -1.0;
        else if (parse_ttml_length(value, &a, &unit, NULL)) { region->line_height = a; region->line_height_unit = unit; }
    }
    else if (!wcscmp(name, L"padding") && region && parse_ttml_length(value, &a, &unit, &rest))
    {
        /* One to four lengths: before, end, after, start, as in CSS. */
        double values[4] = { a, a, a, a };
        unsigned int count = 1;
        while (count < 4 && parse_ttml_length(rest, &values[count], &unit, &rest)) ++count;
        if (count == 2) { values[2] = values[0]; values[3] = values[1]; }
        else if (count == 3) values[3] = values[1];
        region->padding[0] = values[0];
        region->padding[3] = values[1];
        region->padding[2] = values[2];
        region->padding[1] = values[3];
    }
    else if (!wcscmp(name, L"overflow") && region) region->clip_overflow = !wcscmp(value, L"hidden");
    else if (!wcscmp(name, L"wrapOption") && region) region->wrap = wcscmp(value, L"noWrap");
    else if (!wcscmp(name, L"zIndex") && region) region->zindex = wcstol(value, NULL, 10);
}

/* Walk the attributes of the current element: tts: styling goes to style and
 * region; the rest is handed back through the callback. */
static void ttml_read_attributes(struct ttml_context *ctx, struct tt_style *style, struct tt_region *region,
        void (*other)(struct ttml_context *ctx, const WCHAR *prefix, const WCHAR *name, const WCHAR *value,
        void *param), void *param)
{
    const WCHAR *prefix, *name, *value;

    if (IXmlReader_MoveToFirstAttribute(ctx->reader) != S_OK) return;
    do
    {
        IXmlReader_GetPrefix(ctx->reader, &prefix, NULL);
        IXmlReader_GetLocalName(ctx->reader, &name, NULL);
        IXmlReader_GetValue(ctx->reader, &value, NULL);
        if (!wcscmp(prefix, L"tts")) ttml_apply_attribute(name, value, style, region);
        else if (other) other(ctx, prefix, name, value, param);
    } while (IXmlReader_MoveToNextAttribute(ctx->reader) == S_OK);
    IXmlReader_MoveToElement(ctx->reader);
}

struct ttml_timing
{
    double begin, end, dur;
    BOOL has_begin, has_end, has_dur;
    const WCHAR *style_ref;
    WCHAR region_ref[128];
    WCHAR style_refs[256];
};

static void ttml_timing_attribute(struct ttml_context *ctx, const WCHAR *prefix, const WCHAR *name,
        const WCHAR *value, void *param)
{
    struct ttml_timing *timing = param;

    if (*prefix) return;
    if (!wcscmp(name, L"begin")) timing->has_begin = parse_ttml_time(ctx, value, &timing->begin);
    else if (!wcscmp(name, L"end")) timing->has_end = parse_ttml_time(ctx, value, &timing->end);
    else if (!wcscmp(name, L"dur")) timing->has_dur = parse_ttml_time(ctx, value, &timing->dur);
    else if (!wcscmp(name, L"region")) lstrcpynW(timing->region_ref, value, ARRAY_SIZE(timing->region_ref));
    else if (!wcscmp(name, L"style")) lstrcpynW(timing->style_refs, value, ARRAY_SIZE(timing->style_refs));
}

/* A style made from a parent style, the styles an element refers to, and its own attributes. */
static struct tt_style *ttml_element_style(struct ttml_context *ctx, const struct tt_style *parent,
        struct ttml_timing *timing)
{
    struct tt_style *style, *named;
    WCHAR *ref, *next, refs[256];

    if (!(style = tt_style_create(parent, L""))) return NULL;
    /* Read the element's attributes once, for its timing and references. */
    ttml_read_attributes(ctx, NULL, NULL, ttml_timing_attribute, timing);
    lstrcpynW(refs, timing->style_refs, ARRAY_SIZE(refs));
    for (ref = refs; ref && *ref; ref = next)
    {
        if ((next = wcschr(ref, ' '))) *next++ = 0;
        if ((named = ttml_find_style(ctx, ref)))
        {
            struct tt_style *copy = tt_style_create(named, named->name);
            if (copy)
            {
                IMFTimedTextStyle_Release(&style->IMFTimedTextStyle_iface);
                style = copy;
            }
        }
    }
    ttml_read_attributes(ctx, style, NULL, NULL, NULL);
    return style;
}

struct ttml_paragraph
{
    struct text_builder line;
    struct tt_cue *cue;
};

static HRESULT ttml_finish_line(struct ttml_paragraph *paragraph)
{
    struct tt_text *text;

    if (!(text = text_builder_finish(&paragraph->line))) return E_OUTOFMEMORY;
    if (!tt_cue_add_line(paragraph->cue, text))
    {
        IMFTimedTextFormattedText_Release(&text->IMFTimedTextFormattedText_iface);
        return E_OUTOFMEMORY;
    }
    paragraph->line.base = paragraph->cue->style;
    return S_OK;
}

/* Content of a <p>: text, <span> with styles, and <br/> for new lines. */
static HRESULT ttml_parse_paragraph(struct ttml_context *ctx, struct ttml_paragraph *paragraph,
        struct tt_style *span_style, unsigned int depth)
{
    const WCHAR *name, *value;
    XmlNodeType type;
    HRESULT hr;

    while ((hr = IXmlReader_Read(ctx->reader, &type)) == S_OK)
    {
        if (type == XmlNodeType_EndElement) return S_OK;
        if (type == XmlNodeType_Text || type == XmlNodeType_Whitespace || type == XmlNodeType_CDATA)
        {
            WCHAR *text, *p, *out;
            UINT len;

            IXmlReader_GetValue(ctx->reader, &value, &len);
            /* Whitespace collapses to single spaces, as TTML's default xml:space does. */
            if (!(text = heap_wcsndup(value, len))) return E_OUTOFMEMORY;
            for (p = out = text; *p; ++p)
            {
                BOOL space = *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n';
                if (space && (out == text ? !paragraph->line.length
                        || paragraph->line.text[paragraph->line.length - 1] == ' ' : out[-1] == ' '))
                    continue;
                *out++ = space ? ' ' : *p;
            }
            *out = 0;
            text_builder_set_style(&paragraph->line, span_style);
            hr = text_builder_append(&paragraph->line, text, out - text) ? S_OK : E_OUTOFMEMORY;
            free(text);
            if (FAILED(hr)) return hr;
        }
        else if (type == XmlNodeType_Element)
        {
            BOOL empty = IXmlReader_IsEmptyElement(ctx->reader);

            IXmlReader_GetLocalName(ctx->reader, &name, NULL);
            if (!wcscmp(name, L"br"))
            {
                /* Trailing spaces belong to no line. */
                while (paragraph->line.length && paragraph->line.text[paragraph->line.length - 1] == ' ')
                    paragraph->line.text[--paragraph->line.length] = 0;
                if (FAILED(hr = ttml_finish_line(paragraph))) return hr;
                if (!empty && FAILED(hr = ttml_parse_paragraph(ctx, paragraph, span_style, depth + 1))) return hr;
            }
            else if (!wcscmp(name, L"span"))
            {
                struct ttml_timing timing = {0};
                struct tt_style *style = ttml_element_style(ctx, span_style ? span_style : paragraph->cue->style,
                        &timing);

                if (!style) return E_OUTOFMEMORY;
                if (!empty && depth < 32) hr = ttml_parse_paragraph(ctx, paragraph, style, depth + 1);
                IMFTimedTextStyle_Release(&style->IMFTimedTextStyle_iface);
                text_builder_set_style(&paragraph->line, span_style);
                if (FAILED(hr)) return hr;
            }
            else if (!empty && FAILED(hr = ttml_parse_paragraph(ctx, paragraph, span_style, depth + 1)))
                return hr;
        }
    }
    return FAILED(hr) ? hr : S_OK;
}

struct ttml_scope
{
    double begin;              /* absolute begin of the enclosing element */
    double end;                /* absolute end, or INFINITY */
    const struct tt_style *style;
    struct tt_region *region;
};

static struct tt_region *ttml_region(struct ttml_context *ctx, const WCHAR *ref, struct tt_region *inherited)
{
    struct tt_region *region;

    if (*ref && (region = parsed_source_find_region(ctx->source, ref, wcslen(ref)))) return region;
    return inherited;
}

static void ttml_resolve_times(const struct ttml_scope *scope, const struct ttml_timing *timing, double *begin,
        double *end)
{
    *begin = scope->begin + (timing->has_begin ? timing->begin : 0.0);
    if (timing->has_end) *end = scope->begin + timing->end;
    else if (timing->has_dur) *end = *begin + timing->dur;
    else *end = scope->end;
    if (timing->has_dur && timing->has_end) *end = min(*end, *begin + timing->dur);
    *end = min(*end, scope->end);
}

static HRESULT ttml_parse_block(struct ttml_context *ctx, const struct ttml_scope *scope, unsigned int depth);

static HRESULT ttml_parse_p(struct ttml_context *ctx, const struct ttml_scope *scope)
{
    struct ttml_timing timing = {0};
    struct ttml_paragraph paragraph = {{0}};
    struct tt_style *style;
    struct tt_region *region;
    double begin, end;
    BOOL empty = IXmlReader_IsEmptyElement(ctx->reader);
    HRESULT hr = S_OK;

    if (!(style = ttml_element_style(ctx, scope->style, &timing))) return E_OUTOFMEMORY;
    ttml_resolve_times(scope, &timing, &begin, &end);
    region = ttml_region(ctx, timing.region_ref, scope->region);

    if (!(paragraph.cue = tt_cue_create(begin, isinf(end) ? INFINITY : end - begin, L"")))
    {
        IMFTimedTextStyle_Release(&style->IMFTimedTextStyle_iface);
        return E_OUTOFMEMORY;
    }
    paragraph.cue->style = style;
    IMFTimedTextRegion_AddRef(&(paragraph.cue->region = region)->IMFTimedTextRegion_iface);
    paragraph.line.base = style;

    if (!empty) hr = ttml_parse_paragraph(ctx, &paragraph, NULL, 0);
    while (SUCCEEDED(hr) && paragraph.line.length && paragraph.line.text[paragraph.line.length - 1] == ' ')
        paragraph.line.text[--paragraph.line.length] = 0;
    if (SUCCEEDED(hr) && (paragraph.line.length || !paragraph.cue->line_count))
        hr = ttml_finish_line(&paragraph);
    text_builder_cleanup(&paragraph.line);
    if (SUCCEEDED(hr) && !cue_array_add(&ctx->source->cues, paragraph.cue)) hr = E_OUTOFMEMORY;
    if (FAILED(hr)) IMFTimedTextCue_Release(&paragraph.cue->IMFTimedTextCue_iface);
    return hr;
}

/* <body> and <div>: timing containers for paragraphs. */
static HRESULT ttml_parse_block(struct ttml_context *ctx, const struct ttml_scope *scope, unsigned int depth)
{
    const WCHAR *name;
    XmlNodeType type;
    HRESULT hr;

    while ((hr = IXmlReader_Read(ctx->reader, &type)) == S_OK)
    {
        if (type == XmlNodeType_EndElement) return S_OK;
        if (type != XmlNodeType_Element) continue;
        IXmlReader_GetLocalName(ctx->reader, &name, NULL);
        if (!wcscmp(name, L"p"))
        {
            if (FAILED(hr = ttml_parse_p(ctx, scope))) return hr;
        }
        else if (!wcscmp(name, L"div") && depth < 32)
        {
            struct ttml_timing timing = {0};
            struct ttml_scope inner;
            struct tt_style *style;
            BOOL empty = IXmlReader_IsEmptyElement(ctx->reader);

            if (!(style = ttml_element_style(ctx, scope->style, &timing))) return E_OUTOFMEMORY;
            ttml_resolve_times(scope, &timing, &inner.begin, &inner.end);
            inner.style = style;
            inner.region = ttml_region(ctx, timing.region_ref, scope->region);
            hr = empty ? S_OK : ttml_parse_block(ctx, &inner, depth + 1);
            IMFTimedTextStyle_Release(&style->IMFTimedTextStyle_iface);
            if (FAILED(hr)) return hr;
        }
        else if (!IXmlReader_IsEmptyElement(ctx->reader))
        {
            /* Skip what is not timed text. */
            unsigned int open = 0;
            while ((hr = IXmlReader_Read(ctx->reader, &type)) == S_OK)
            {
                if (type == XmlNodeType_Element && !IXmlReader_IsEmptyElement(ctx->reader)) ++open;
                else if (type == XmlNodeType_EndElement && !open--) break;
            }
            if (FAILED(hr)) return hr;
        }
    }
    return FAILED(hr) ? hr : S_OK;
}

static void ttml_style_id_attribute(struct ttml_context *ctx, const WCHAR *prefix, const WCHAR *name,
        const WCHAR *value, void *param)
{
    WCHAR **id = param;

    if (!wcscmp(prefix, L"xml") && !wcscmp(name, L"id")) { free(*id); *id = heap_wcsdup(value); }
}

/* <head>: <styling> with <style xml:id>, and <layout> with <region xml:id>. */
static HRESULT ttml_parse_head(struct ttml_context *ctx)
{
    const WCHAR *name;
    XmlNodeType type;
    unsigned int open = 0;
    HRESULT hr;

    if (IXmlReader_IsEmptyElement(ctx->reader)) return S_OK;
    /* Nesting is counted here: empty elements have no end node. */
    while ((hr = IXmlReader_Read(ctx->reader, &type)) == S_OK)
    {
        if (type == XmlNodeType_EndElement)
        {
            if (!open) return S_OK;
            --open;
            continue;
        }
        if (type != XmlNodeType_Element) continue;
        if (!IXmlReader_IsEmptyElement(ctx->reader)) ++open;
        IXmlReader_GetLocalName(ctx->reader, &name, NULL);
        if (!wcscmp(name, L"style"))
        {
            struct tt_style *style;
            WCHAR *id = NULL;
            void *styles;

            ttml_read_attributes(ctx, NULL, NULL, ttml_style_id_attribute, &id);
            if (!id) continue;
            if (!(style = tt_style_create(ctx->source->style, id))
                    || !(styles = realloc(ctx->styles, (ctx->style_count + 1) * sizeof(*ctx->styles))))
            {
                if (style) IMFTimedTextStyle_Release(&style->IMFTimedTextStyle_iface);
                free(id);
                return E_OUTOFMEMORY;
            }
            ttml_read_attributes(ctx, style, NULL, NULL, NULL);
            ctx->styles = styles;
            ctx->styles[ctx->style_count].id = id;
            ctx->styles[ctx->style_count++].style = style;
        }
        else if (!wcscmp(name, L"region"))
        {
            struct tt_region *region;
            WCHAR *id = NULL;

            ttml_read_attributes(ctx, NULL, NULL, ttml_style_id_attribute, &id);
            if (!id) continue;
            if (!(region = tt_region_create(ctx->source->region, id)))
            {
                free(id);
                return E_OUTOFMEMORY;
            }
            ttml_read_attributes(ctx, NULL, region, NULL, NULL);
            if (!parsed_source_add_region(ctx->source, id, region))
            {
                IMFTimedTextRegion_Release(&region->IMFTimedTextRegion_iface);
                free(id);
                return E_OUTOFMEMORY;
            }
            free(id);
        }
    }
    return FAILED(hr) ? hr : S_OK;
}

static void ttml_root_attribute(struct ttml_context *ctx, const WCHAR *prefix, const WCHAR *name,
        const WCHAR *value, void *param)
{
    if (!wcscmp(prefix, L"ttp") && !wcscmp(name, L"frameRate")) ctx->frame_rate = max(wcstod(value, NULL), 1.0);
    else if (!wcscmp(prefix, L"ttp") && !wcscmp(name, L"tickRate")) ctx->tick_rate = max(wcstod(value, NULL), 1.0);
}

static HRESULT parse_ttml(struct parsed_source *source, IStream *stream)
{
    struct ttml_context ctx = { .source = source, .frame_rate = 30.0, .tick_rate = 1.0 };
    struct ttml_scope scope = { .begin = 0.0, .end = INFINITY };
    const WCHAR *name;
    XmlNodeType type;
    BOOL in_tt = FALSE;
    HRESULT hr;

    if (FAILED(hr = CreateXmlReader(&IID_IXmlReader, (void **)&ctx.reader, NULL))) return hr;
    IXmlReader_SetProperty(ctx.reader, XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    if (FAILED(hr = IXmlReader_SetInput(ctx.reader, (IUnknown *)stream)))
    {
        IXmlReader_Release(ctx.reader);
        return hr;
    }
    scope.style = source->style;
    scope.region = source->region;

    while ((hr = IXmlReader_Read(ctx.reader, &type)) == S_OK)
    {
        if (type != XmlNodeType_Element) continue;
        IXmlReader_GetLocalName(ctx.reader, &name, NULL);
        if (!in_tt)
        {
            if (wcscmp(name, L"tt"))
            {
                hr = MF_E_INVALID_FILE_FORMAT;
                break;
            }
            in_tt = TRUE;
            ttml_read_attributes(&ctx, NULL, NULL, ttml_root_attribute, NULL);
            continue;
        }
        if (!wcscmp(name, L"head"))
        {
            if (FAILED(hr = ttml_parse_head(&ctx))) break;
        }
        else if (!wcscmp(name, L"body"))
        {
            struct ttml_timing timing = {0};
            struct ttml_scope body;
            struct tt_style *style;

            if (!(style = ttml_element_style(&ctx, scope.style, &timing)))
            {
                hr = E_OUTOFMEMORY;
                break;
            }
            ttml_resolve_times(&scope, &timing, &body.begin, &body.end);
            body.style = style;
            body.region = ttml_region(&ctx, timing.region_ref, scope.region);
            hr = IXmlReader_IsEmptyElement(ctx.reader) ? S_OK : ttml_parse_block(&ctx, &body, 0);
            IMFTimedTextStyle_Release(&style->IMFTimedTextStyle_iface);
            if (FAILED(hr)) break;
        }
    }
    if (hr == S_FALSE) hr = S_OK;
    if (SUCCEEDED(hr) && !in_tt) hr = MF_E_INVALID_FILE_FORMAT;
    ttml_context_cleanup(&ctx);
    IXmlReader_Release(ctx.reader);
    return hr;
}

/* Read a whole byte stream. */
static HRESULT read_byte_stream(IMFByteStream *stream, BYTE **data, DWORD *size)
{
    QWORD length = 0;
    DWORD capacity, read;
    BYTE *buffer, *new_buffer;
    HRESULT hr;

    *data = NULL;
    *size = 0;
    if (FAILED(IMFByteStream_GetLength(stream, &length)) || length == ~(QWORD)0) length = 0;
    if (length > 64 * 1024 * 1024) return MF_E_INVALID_FILE_FORMAT;
    capacity = length ? (DWORD)length + 1 : 65536;
    if (!(buffer = malloc(capacity))) return E_OUTOFMEMORY;
    IMFByteStream_SetCurrentPosition(stream, 0);
    for (;;)
    {
        if (*size == capacity)
        {
            if (capacity >= 64 * 1024 * 1024 || !(new_buffer = realloc(buffer, capacity * 2)))
            {
                free(buffer);
                return E_OUTOFMEMORY;
            }
            buffer = new_buffer;
            capacity *= 2;
        }
        if (FAILED(hr = IMFByteStream_Read(stream, buffer + *size, capacity - *size, &read)))
        {
            free(buffer);
            return hr;
        }
        if (!read) break;
        *size += read;
    }
    *data = buffer;
    return S_OK;
}

/* Text of a document: UTF-16 with a byte order mark, UTF-8, or the ANSI code
 * page for SRT files that are neither. */
static WCHAR *decode_text(const BYTE *data, DWORD size)
{
    WCHAR *text;
    DWORD i;
    int len;

    if (size >= 2 && ((data[0] == 0xff && data[1] == 0xfe) || (data[0] == 0xfe && data[1] == 0xff)))
    {
        BOOL big_endian = data[0] == 0xfe;
        len = (size - 2) / 2;
        if (!(text = malloc((len + 1) * sizeof(WCHAR)))) return NULL;
        for (i = 0; i < (DWORD)len; ++i)
            text[i] = big_endian ? data[2 + 2 * i] << 8 | data[3 + 2 * i] : data[2 + 2 * i] | data[3 + 2 * i] << 8;
        text[len] = 0;
        return text;
    }
    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
    {
        data += 3;
        size -= 3;
    }
    if ((len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)data, size, NULL, 0)) > 0
            || !size)
    {
        if (!(text = malloc((len + 1) * sizeof(WCHAR)))) return NULL;
        MultiByteToWideChar(CP_UTF8, 0, (const char *)data, size, text, len);
    }
    else
    {
        len = MultiByteToWideChar(CP_ACP, 0, (const char *)data, size, NULL, 0);
        if (!(text = malloc((len + 1) * sizeof(WCHAR)))) return NULL;
        MultiByteToWideChar(CP_ACP, 0, (const char *)data, size, text, len);
    }
    text[len] = 0;
    return text;
}

/* Parse a timed text document: WebVTT, SRT or TTML, told apart by their first characters. */
static HRESULT parse_source(const BYTE *data, DWORD size, struct parsed_source *source)
{
    const WCHAR *start;
    WCHAR *text;
    HRESULT hr;

    memset(source, 0, sizeof(*source));
    if (!(source->style = tt_style_create(NULL, L"")) || !(source->region = tt_region_create(NULL, L"")))
    {
        parsed_source_cleanup(source);
        return E_OUTOFMEMORY;
    }
    if (!(text = decode_text(data, size)))
    {
        parsed_source_cleanup(source);
        return E_OUTOFMEMORY;
    }
    for (start = text; *start == ' ' || *start == '\t' || *start == '\r' || *start == '\n' || *start == 0xfeff;
            ++start) ;

    if (!wcsncmp(start, L"WEBVTT", 6))
    {
        source->format = MFSubtitleFormat_WebVTT;
        hr = parse_cue_blocks(source, (WCHAR *)start, TRUE);
    }
    else if (*start == '<')
    {
        IStream *stream;
        HGLOBAL memory;

        source->format = MFSubtitleFormat_TTML;
        /* The reader decodes the document itself, from its declaration. */
        if (!(memory = GlobalAlloc(GMEM_MOVEABLE, max(size, 1))))
            hr = E_OUTOFMEMORY;
        else
        {
            memcpy(GlobalLock(memory), data, size);
            GlobalUnlock(memory);
            if (SUCCEEDED(hr = CreateStreamOnHGlobal(memory, TRUE, &stream)))
            {
                IStream_SetSize(stream, (ULARGE_INTEGER){ .QuadPart = size });
                hr = parse_ttml(source, stream);
                IStream_Release(stream);
            }
            else GlobalFree(memory);
        }
    }
    else if (wcsstr(start, L"-->"))
    {
        source->format = MFSubtitleFormat_SRT;
        hr = parse_cue_blocks(source, (WCHAR *)start, FALSE);
    }
    else
        hr = MF_E_INVALID_FILE_FORMAT;

    free(text);
    if (FAILED(hr)) parsed_source_cleanup(source);
    return hr;
}

/* The timed text object, its tracks and lists */

struct tt_track;

struct timed_text
{
    IMFTimedText IMFTimedText_iface;
    LONG refcount;
    CRITICAL_SECTION cs;

    IMFMediaEngineEx *engine; /* not a reference; NULL once the engine is gone */
    IMFTimedTextNotify *notify;
    struct tt_track **tracks;
    size_t track_count;
    DWORD next_track_id;
    DWORD next_cue_id;
    double cue_offset;
    BOOL in_band;

    PTP_TIMER timer;
    BOOL timer_running;
    struct tt_cue **active;    /* cues reported active, each with a reference */
    size_t active_count;

    struct list events;        /* struct tt_event, delivered in order by the event worker */
    BOOL delivering;
};

enum tt_event_type
{
    TT_EVENT_TRACK_ADDED,
    TT_EVENT_TRACK_REMOVED,
    TT_EVENT_TRACK_SELECTED,
    TT_EVENT_TRACK_READY_STATE,
    TT_EVENT_ERROR,
    TT_EVENT_CUE,
    TT_EVENT_RESET,
};

struct tt_event
{
    struct list entry;
    enum tt_event_type type;
    DWORD id;
    BOOL selected;
    MF_TIMED_TEXT_ERROR_CODE code;
    HRESULT extended;
    MF_TIMED_TEXT_CUE_EVENT cue_event;
    double time;
    struct tt_cue *cue;
};

struct tt_track
{
    IMFTimedTextTrack IMFTimedTextTrack_iface;
    IMFTimedTextCueList IMFTimedTextCueList_iface;
    LONG refcount;

    struct timed_text *owner; /* guarded by its lock; NULL when removed */
    DWORD id;
    WCHAR *label;
    WCHAR *language;
    MF_TIMED_TEXT_TRACK_KIND kind;
    BOOL active;
    MF_TIMED_TEXT_ERROR_CODE error;
    HRESULT extended_error;
    GUID format;
    MF_TIMED_TEXT_TRACK_READY_STATE ready_state;
    struct cue_array cues;
    struct parsed_source source; /* styles and regions the cues share */
};

static struct timed_text *impl_from_IMFTimedText(IMFTimedText *iface)
{
    return CONTAINING_RECORD(iface, struct timed_text, IMFTimedText_iface);
}

static struct tt_track *impl_from_IMFTimedTextTrack(IMFTimedTextTrack *iface)
{
    return CONTAINING_RECORD(iface, struct tt_track, IMFTimedTextTrack_iface);
}

static struct tt_track *impl_from_IMFTimedTextCueList(IMFTimedTextCueList *iface)
{
    return CONTAINING_RECORD(iface, struct tt_track, IMFTimedTextCueList_iface);
}

static void timed_text_queue_event(struct timed_text *tt, const struct tt_event *event);

static HRESULT WINAPI tt_track_QueryInterface(IMFTimedTextTrack *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFTimedTextTrack) || IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFTimedTextTrack_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI tt_track_AddRef(IMFTimedTextTrack *iface)
{
    return InterlockedIncrement(&impl_from_IMFTimedTextTrack(iface)->refcount);
}

static ULONG WINAPI tt_track_Release(IMFTimedTextTrack *iface)
{
    struct tt_track *track = impl_from_IMFTimedTextTrack(iface);
    ULONG refcount = InterlockedDecrement(&track->refcount);

    if (!refcount)
    {
        cue_array_cleanup(&track->cues);
        parsed_source_cleanup(&track->source);
        free(track->label);
        free(track->language);
        free(track);
    }
    return refcount;
}

static DWORD WINAPI tt_track_GetId(IMFTimedTextTrack *iface)
{
    return impl_from_IMFTimedTextTrack(iface)->id;
}

static HRESULT WINAPI tt_track_GetLabel(IMFTimedTextTrack *iface, WCHAR **label)
{
    struct tt_track *track = impl_from_IMFTimedTextTrack(iface);
    struct timed_text *tt = track->owner;
    HRESULT hr;

    if (tt) EnterCriticalSection(&tt->cs);
    hr = return_string(track->label, label);
    if (tt) LeaveCriticalSection(&tt->cs);
    return hr;
}

static HRESULT WINAPI tt_track_SetLabel(IMFTimedTextTrack *iface, const WCHAR *label)
{
    struct tt_track *track = impl_from_IMFTimedTextTrack(iface);
    struct timed_text *tt = track->owner;
    WCHAR *copy;

    if (!(copy = heap_wcsdup(label))) return E_OUTOFMEMORY;
    if (tt) EnterCriticalSection(&tt->cs);
    free(track->label);
    track->label = copy;
    if (tt) LeaveCriticalSection(&tt->cs);
    return S_OK;
}

static HRESULT WINAPI tt_track_GetLanguage(IMFTimedTextTrack *iface, WCHAR **language)
{
    return return_string(impl_from_IMFTimedTextTrack(iface)->language, language);
}

static MF_TIMED_TEXT_TRACK_KIND WINAPI tt_track_GetTrackKind(IMFTimedTextTrack *iface)
{
    return impl_from_IMFTimedTextTrack(iface)->kind;
}

static BOOL WINAPI tt_track_IsInBand(IMFTimedTextTrack *iface)
{
    return FALSE;
}

static HRESULT WINAPI tt_track_GetInBandMetadataTrackDispatchType(IMFTimedTextTrack *iface, WCHAR **type)
{
    /* Only in-band metadata tracks have one. */
    return return_string(L"", type);
}

static BOOL WINAPI tt_track_IsActive(IMFTimedTextTrack *iface)
{
    return impl_from_IMFTimedTextTrack(iface)->active;
}

static MF_TIMED_TEXT_ERROR_CODE WINAPI tt_track_GetErrorCode(IMFTimedTextTrack *iface)
{
    return impl_from_IMFTimedTextTrack(iface)->error;
}

static HRESULT WINAPI tt_track_GetExtendedErrorCode(IMFTimedTextTrack *iface)
{
    return impl_from_IMFTimedTextTrack(iface)->extended_error;
}

static HRESULT WINAPI tt_track_GetDataFormat(IMFTimedTextTrack *iface, GUID *format)
{
    if (!format) return E_POINTER;
    *format = impl_from_IMFTimedTextTrack(iface)->format;
    return S_OK;
}

static MF_TIMED_TEXT_TRACK_READY_STATE WINAPI tt_track_GetReadyState(IMFTimedTextTrack *iface)
{
    return impl_from_IMFTimedTextTrack(iface)->ready_state;
}

static HRESULT WINAPI tt_track_GetCueList(IMFTimedTextTrack *iface, IMFTimedTextCueList **cues)
{
    struct tt_track *track = impl_from_IMFTimedTextTrack(iface);

    if (!cues) return E_POINTER;
    IMFTimedTextCueList_AddRef((*cues = &track->IMFTimedTextCueList_iface));
    return S_OK;
}

static const IMFTimedTextTrackVtbl tt_track_vtbl =
{
    tt_track_QueryInterface,
    tt_track_AddRef,
    tt_track_Release,
    tt_track_GetId,
    tt_track_GetLabel,
    tt_track_SetLabel,
    tt_track_GetLanguage,
    tt_track_GetTrackKind,
    tt_track_IsInBand,
    tt_track_GetInBandMetadataTrackDispatchType,
    tt_track_IsActive,
    tt_track_GetErrorCode,
    tt_track_GetExtendedErrorCode,
    tt_track_GetDataFormat,
    tt_track_GetReadyState,
    tt_track_GetCueList,
};

static HRESULT WINAPI tt_cue_list_QueryInterface(IMFTimedTextCueList *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFTimedTextCueList) || IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFTimedTextCueList_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI tt_cue_list_AddRef(IMFTimedTextCueList *iface)
{
    return IMFTimedTextTrack_AddRef(&impl_from_IMFTimedTextCueList(iface)->IMFTimedTextTrack_iface);
}

static ULONG WINAPI tt_cue_list_Release(IMFTimedTextCueList *iface)
{
    return IMFTimedTextTrack_Release(&impl_from_IMFTimedTextCueList(iface)->IMFTimedTextTrack_iface);
}

static void track_lock(struct tt_track *track)
{
    if (track->owner) EnterCriticalSection(&track->owner->cs);
}

static void track_unlock(struct tt_track *track, struct timed_text *owner)
{
    if (owner) LeaveCriticalSection(&owner->cs);
}

static DWORD WINAPI tt_cue_list_GetLength(IMFTimedTextCueList *iface)
{
    struct tt_track *track = impl_from_IMFTimedTextCueList(iface);
    struct timed_text *owner = track->owner;
    DWORD ret;

    track_lock(track);
    ret = track->cues.count;
    track_unlock(track, owner);
    return ret;
}

static HRESULT find_cue(struct tt_track *track, IMFTimedTextCue **cue, BOOL (*match)(struct tt_cue *cue,
        const void *key, DWORD index), const void *key)
{
    struct timed_text *owner = track->owner;
    HRESULT hr = MF_E_NOT_FOUND;
    size_t i;

    if (!cue) return E_POINTER;
    *cue = NULL;
    track_lock(track);
    for (i = 0; i < track->cues.count; ++i)
    {
        if (match(track->cues.cues[i], key, i))
        {
            IMFTimedTextCue_AddRef((*cue = &track->cues.cues[i]->IMFTimedTextCue_iface));
            hr = S_OK;
            break;
        }
    }
    track_unlock(track, owner);
    return hr;
}

static BOOL match_index(struct tt_cue *cue, const void *key, DWORD index) { return index == *(const DWORD *)key; }
static BOOL match_id(struct tt_cue *cue, const void *key, DWORD index) { return cue->id == *(const DWORD *)key; }
static BOOL match_original_id(struct tt_cue *cue, const void *key, DWORD index)
{
    return !wcscmp(cue->original_id, key);
}

static HRESULT WINAPI tt_cue_list_GetCueByIndex(IMFTimedTextCueList *iface, DWORD index, IMFTimedTextCue **cue)
{
    HRESULT hr = find_cue(impl_from_IMFTimedTextCueList(iface), cue, match_index, &index);
    return hr == MF_E_NOT_FOUND ? E_INVALIDARG : hr;
}

static HRESULT WINAPI tt_cue_list_GetCueById(IMFTimedTextCueList *iface, DWORD id, IMFTimedTextCue **cue)
{
    return find_cue(impl_from_IMFTimedTextCueList(iface), cue, match_id, &id);
}

static HRESULT WINAPI tt_cue_list_GetCueByOriginalId(IMFTimedTextCueList *iface, const WCHAR *original_id,
        IMFTimedTextCue **cue)
{
    if (!original_id) return E_POINTER;
    return find_cue(impl_from_IMFTimedTextCueList(iface), cue, match_original_id, original_id);
}

/* Give a cue its place in a track: its id, the track's, and the track's kind. */
static HRESULT track_add_cue(struct tt_track *track, struct tt_cue *cue)
{
    struct timed_text *owner = track->owner;
    HRESULT hr = S_OK;

    track_lock(track);
    cue->id = owner ? ++owner->next_cue_id : track->cues.count + 1;
    cue->track_id = track->id;
    cue->kind = track->kind;
    if (!cue_array_add(&track->cues, cue)) hr = E_OUTOFMEMORY;
    track_unlock(track, owner);
    return hr;
}

static HRESULT WINAPI tt_cue_list_AddTextCue(IMFTimedTextCueList *iface, double start, double duration,
        const WCHAR *text, IMFTimedTextCue **out)
{
    struct tt_track *track = impl_from_IMFTimedTextCueList(iface);
    WCHAR *copy;
    struct lines lines;
    struct tt_cue *cue;
    HRESULT hr;

    TRACE("%p, %.8e, %.8e, %s, %p.\n", iface, start, duration, debugstr_w(text), out);

    if (!text) return E_POINTER;
    if (out) *out = NULL;
    if (!(cue = tt_cue_create(start, duration, L""))) return E_OUTOFMEMORY;
    IMFTimedTextStyle_AddRef(&(cue->style = track->source.style)->IMFTimedTextStyle_iface);
    IMFTimedTextRegion_AddRef(&(cue->region = track->source.region)->IMFTimedTextRegion_iface);

    /* The text is a payload as in WebVTT: lines, with markup. */
    if (!(copy = heap_wcsdup(text)) || !split_lines(copy, &lines))
        hr = E_OUTOFMEMORY;
    else
    {
        hr = add_payload_lines(cue, lines.line, lines.count) ? S_OK : E_OUTOFMEMORY;
        free(lines.line);
    }
    free(copy);
    if (SUCCEEDED(hr)) hr = track_add_cue(track, cue);
    if (SUCCEEDED(hr) && out) IMFTimedTextCue_AddRef((*out = &cue->IMFTimedTextCue_iface));
    if (FAILED(hr)) IMFTimedTextCue_Release(&cue->IMFTimedTextCue_iface);
    return hr;
}

static HRESULT WINAPI tt_cue_list_AddDataCue(IMFTimedTextCueList *iface, double start, double duration,
        const BYTE *data, DWORD size, IMFTimedTextCue **out)
{
    struct tt_track *track = impl_from_IMFTimedTextCueList(iface);
    struct tt_binary *binary;
    struct tt_cue *cue;
    HRESULT hr;

    TRACE("%p, %.8e, %.8e, %p, %lu, %p.\n", iface, start, duration, data, size, out);

    if (!data && size) return E_POINTER;
    if (out) *out = NULL;
    if (!(binary = calloc(1, sizeof(*binary)))) return E_OUTOFMEMORY;
    binary->IMFTimedTextBinary_iface.lpVtbl = &tt_binary_vtbl;
    binary->refcount = 1;
    if (size && !(binary->data = malloc(size)))
    {
        free(binary);
        return E_OUTOFMEMORY;
    }
    if (size) memcpy(binary->data, data, size);
    binary->size = size;
    if (!(cue = tt_cue_create(start, duration, L"")))
    {
        IMFTimedTextBinary_Release(&binary->IMFTimedTextBinary_iface);
        return E_OUTOFMEMORY;
    }
    cue->data = binary;
    if (SUCCEEDED(hr = track_add_cue(track, cue)) && out)
        IMFTimedTextCue_AddRef((*out = &cue->IMFTimedTextCue_iface));
    if (FAILED(hr)) IMFTimedTextCue_Release(&cue->IMFTimedTextCue_iface);
    return hr;
}

static HRESULT WINAPI tt_cue_list_RemoveCue(IMFTimedTextCueList *iface, IMFTimedTextCue *cue)
{
    struct tt_track *track = impl_from_IMFTimedTextCueList(iface);
    struct timed_text *owner = track->owner;
    HRESULT hr = E_INVALIDARG;
    size_t i;

    TRACE("%p, %p.\n", iface, cue);

    if (!cue) return E_POINTER;
    track_lock(track);
    for (i = 0; i < track->cues.count; ++i)
    {
        if (&track->cues.cues[i]->IMFTimedTextCue_iface == cue)
        {
            IMFTimedTextCue_Release(cue);
            memmove(&track->cues.cues[i], &track->cues.cues[i + 1], (track->cues.count - i - 1) * sizeof(*track->cues.cues));
            --track->cues.count;
            hr = S_OK;
            break;
        }
    }
    track_unlock(track, owner);
    return hr;
}

static const IMFTimedTextCueListVtbl tt_cue_list_vtbl =
{
    tt_cue_list_QueryInterface,
    tt_cue_list_AddRef,
    tt_cue_list_Release,
    tt_cue_list_GetLength,
    tt_cue_list_GetCueByIndex,
    tt_cue_list_GetCueById,
    tt_cue_list_GetCueByOriginalId,
    tt_cue_list_AddTextCue,
    tt_cue_list_AddDataCue,
    tt_cue_list_RemoveCue,
};

static struct tt_track *tt_track_create(const WCHAR *label, const WCHAR *language, MF_TIMED_TEXT_TRACK_KIND kind)
{
    struct tt_track *track;

    if (!(track = calloc(1, sizeof(*track)))) return NULL;
    track->IMFTimedTextTrack_iface.lpVtbl = &tt_track_vtbl;
    track->IMFTimedTextCueList_iface.lpVtbl = &tt_cue_list_vtbl;
    track->refcount = 1;
    track->kind = kind;
    track->format = GUID_NULL;
    track->label = heap_wcsdup(label);
    track->language = heap_wcsdup(language);
    track->source.style = tt_style_create(NULL, L"");
    track->source.region = tt_region_create(NULL, L"");
    if (!track->label || !track->language || !track->source.style || !track->source.region)
    {
        IMFTimedTextTrack_Release(&track->IMFTimedTextTrack_iface);
        return NULL;
    }
    return track;
}

/* A snapshot of tracks. */
struct tt_track_list
{
    IMFTimedTextTrackList IMFTimedTextTrackList_iface;
    LONG refcount;
    struct tt_track **tracks;
    size_t count;
};

static struct tt_track_list *impl_from_IMFTimedTextTrackList(IMFTimedTextTrackList *iface)
{
    return CONTAINING_RECORD(iface, struct tt_track_list, IMFTimedTextTrackList_iface);
}

static HRESULT WINAPI tt_track_list_QueryInterface(IMFTimedTextTrackList *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFTimedTextTrackList) || IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFTimedTextTrackList_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI tt_track_list_AddRef(IMFTimedTextTrackList *iface)
{
    return InterlockedIncrement(&impl_from_IMFTimedTextTrackList(iface)->refcount);
}

static ULONG WINAPI tt_track_list_Release(IMFTimedTextTrackList *iface)
{
    struct tt_track_list *list = impl_from_IMFTimedTextTrackList(iface);
    ULONG refcount = InterlockedDecrement(&list->refcount);
    size_t i;

    if (!refcount)
    {
        for (i = 0; i < list->count; ++i)
            IMFTimedTextTrack_Release(&list->tracks[i]->IMFTimedTextTrack_iface);
        free(list->tracks);
        free(list);
    }
    return refcount;
}

static DWORD WINAPI tt_track_list_GetLength(IMFTimedTextTrackList *iface)
{
    return impl_from_IMFTimedTextTrackList(iface)->count;
}

static HRESULT WINAPI tt_track_list_GetTrack(IMFTimedTextTrackList *iface, DWORD index, IMFTimedTextTrack **track)
{
    struct tt_track_list *list = impl_from_IMFTimedTextTrackList(iface);

    if (!track) return E_POINTER;
    if (index >= list->count)
    {
        *track = NULL;
        return E_INVALIDARG;
    }
    IMFTimedTextTrack_AddRef((*track = &list->tracks[index]->IMFTimedTextTrack_iface));
    return S_OK;
}

static HRESULT WINAPI tt_track_list_GetTrackById(IMFTimedTextTrackList *iface, DWORD id, IMFTimedTextTrack **track)
{
    struct tt_track_list *list = impl_from_IMFTimedTextTrackList(iface);
    size_t i;

    if (!track) return E_POINTER;
    *track = NULL;
    for (i = 0; i < list->count; ++i)
    {
        if (list->tracks[i]->id == id)
        {
            IMFTimedTextTrack_AddRef((*track = &list->tracks[i]->IMFTimedTextTrack_iface));
            return S_OK;
        }
    }
    return MF_E_NOT_FOUND;
}

static const IMFTimedTextTrackListVtbl tt_track_list_vtbl =
{
    tt_track_list_QueryInterface,
    tt_track_list_AddRef,
    tt_track_list_Release,
    tt_track_list_GetLength,
    tt_track_list_GetTrack,
    tt_track_list_GetTrackById,
};

enum track_filter
{
    TRACKS_ALL,
    TRACKS_ACTIVE,
    TRACKS_TEXT,
    TRACKS_METADATA,
};

static BOOL track_matches(const struct tt_track *track, enum track_filter filter)
{
    switch (filter)
    {
        case TRACKS_ACTIVE: return track->active;
        case TRACKS_TEXT: return track->kind == MF_TIMED_TEXT_TRACK_KIND_SUBTITLES
                || track->kind == MF_TIMED_TEXT_TRACK_KIND_CAPTIONS;
        case TRACKS_METADATA: return track->kind == MF_TIMED_TEXT_TRACK_KIND_METADATA;
        default: return TRUE;
    }
}

static HRESULT timed_text_get_tracks(struct timed_text *tt, enum track_filter filter, IMFTimedTextTrackList **out)
{
    struct tt_track_list *list;
    size_t i;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!(list = calloc(1, sizeof(*list)))) return E_OUTOFMEMORY;
    list->IMFTimedTextTrackList_iface.lpVtbl = &tt_track_list_vtbl;
    list->refcount = 1;

    EnterCriticalSection(&tt->cs);
    if (tt->track_count && !(list->tracks = calloc(tt->track_count, sizeof(*list->tracks))))
    {
        LeaveCriticalSection(&tt->cs);
        free(list);
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < tt->track_count; ++i)
    {
        if (!track_matches(tt->tracks[i], filter)) continue;
        IMFTimedTextTrack_AddRef(&tt->tracks[i]->IMFTimedTextTrack_iface);
        list->tracks[list->count++] = tt->tracks[i];
    }
    LeaveCriticalSection(&tt->cs);

    *out = &list->IMFTimedTextTrackList_iface;
    return S_OK;
}

/* Events reach the application's IMFTimedTextNotify from a worker, in order, as
 * on Windows, where loading and cue changes are asynchronous. */

static void CALLBACK timed_text_deliver_events(PTP_CALLBACK_INSTANCE instance, void *context)
{
    struct timed_text *tt = context;
    IMFTimedTextNotify *notify;
    struct tt_event *event;
    struct list *head;

    for (;;)
    {
        EnterCriticalSection(&tt->cs);
        if (!(head = list_head(&tt->events)))
        {
            tt->delivering = FALSE;
            LeaveCriticalSection(&tt->cs);
            break;
        }
        list_remove(head);
        if ((notify = tt->notify)) IMFTimedTextNotify_AddRef(notify);
        LeaveCriticalSection(&tt->cs);

        event = LIST_ENTRY(head, struct tt_event, entry);
        if (notify)
        {
            switch (event->type)
            {
                case TT_EVENT_TRACK_ADDED: IMFTimedTextNotify_TrackAdded(notify, event->id); break;
                case TT_EVENT_TRACK_REMOVED: IMFTimedTextNotify_TrackRemoved(notify, event->id); break;
                case TT_EVENT_TRACK_SELECTED: IMFTimedTextNotify_TrackSelected(notify, event->id, event->selected); break;
                case TT_EVENT_TRACK_READY_STATE: IMFTimedTextNotify_TrackReadyStateChanged(notify, event->id); break;
                case TT_EVENT_ERROR: IMFTimedTextNotify_Error(notify, event->code, event->extended, event->id); break;
                case TT_EVENT_CUE:
                    IMFTimedTextNotify_Cue(notify, event->cue_event, event->time,
                            event->cue ? &event->cue->IMFTimedTextCue_iface : NULL);
                    break;
                case TT_EVENT_RESET: IMFTimedTextNotify_Reset(notify); break;
            }
            IMFTimedTextNotify_Release(notify);
        }
        if (event->cue) IMFTimedTextCue_Release(&event->cue->IMFTimedTextCue_iface);
        free(event);
    }
    IMFTimedText_Release(&tt->IMFTimedText_iface);
}

/* Called with the lock held. */
static void timed_text_queue_event(struct timed_text *tt, const struct tt_event *template)
{
    struct tt_event *event;

    if (!tt->notify || !(event = malloc(sizeof(*event)))) return;
    *event = *template;
    if (event->cue) IMFTimedTextCue_AddRef(&event->cue->IMFTimedTextCue_iface);
    list_add_tail(&tt->events, &event->entry);
    if (!tt->delivering)
    {
        IMFTimedText_AddRef(&tt->IMFTimedText_iface);
        if (TrySubmitThreadpoolCallback(timed_text_deliver_events, tt, NULL))
            tt->delivering = TRUE;
        else
            IMFTimedText_Release(&tt->IMFTimedText_iface);
    }
}

static void timed_text_queue_track_event(struct timed_text *tt, enum tt_event_type type, DWORD id, BOOL selected)
{
    struct tt_event event = { .type = type, .id = id, .selected = selected };
    timed_text_queue_event(tt, &event);
}

/* Cue timing: report cues of active tracks as the playback position enters and leaves them. */

static BOOL cue_is_active(const struct tt_cue *cue, double time, double offset)
{
    double start = cue->start + offset;
    return time >= start && time < start + cue->duration;
}

static void timed_text_update_cues(struct timed_text *tt, double time)
{
    struct tt_cue **active = NULL;
    size_t i, j, count = 0, capacity = 0;
    struct tt_event event = { .type = TT_EVENT_CUE, .time = time };

    /* The cues active now, in track then time order. */
    for (i = 0; i < tt->track_count; ++i)
    {
        struct tt_track *track = tt->tracks[i];
        if (!track->active) continue;
        for (j = 0; j < track->cues.count; ++j)
        {
            if (!cue_is_active(track->cues.cues[j], time, tt->cue_offset)) continue;
            if (!text_builder_reserve((void **)&active, &capacity, count + 1, sizeof(*active))) break;
            IMFTimedTextCue_AddRef(&track->cues.cues[j]->IMFTimedTextCue_iface);
            active[count++] = track->cues.cues[j];
        }
    }

    /* Those that ended first, then those that began. */
    event.cue_event = MF_TIMED_TEXT_CUE_EVENT_INACTIVE;
    for (i = 0; i < tt->active_count; ++i)
    {
        for (j = 0; j < count; ++j) if (active[j] == tt->active[i]) break;
        if (j < count) continue;
        event.cue = tt->active[i];
        timed_text_queue_event(tt, &event);
    }
    event.cue_event = MF_TIMED_TEXT_CUE_EVENT_ACTIVE;
    for (i = 0; i < count; ++i)
    {
        for (j = 0; j < tt->active_count; ++j) if (tt->active[j] == active[i]) break;
        if (j < tt->active_count) continue;
        event.cue = active[i];
        timed_text_queue_event(tt, &event);
    }

    for (i = 0; i < tt->active_count; ++i)
        IMFTimedTextCue_Release(&tt->active[i]->IMFTimedTextCue_iface);
    free(tt->active);
    tt->active = active;
    tt->active_count = count;
}

/* Forget the active cues and tell the application to clear what it shows. */
static void timed_text_clear_cues(struct timed_text *tt, double time)
{
    struct tt_event event = { .type = TT_EVENT_CUE, .cue_event = MF_TIMED_TEXT_CUE_EVENT_CLEAR, .time = time };
    size_t i;

    if (!tt->active_count) return;
    for (i = 0; i < tt->active_count; ++i)
        IMFTimedTextCue_Release(&tt->active[i]->IMFTimedTextCue_iface);
    free(tt->active);
    tt->active = NULL;
    tt->active_count = 0;
    timed_text_queue_event(tt, &event);
}

static void CALLBACK timed_text_timer_callback(PTP_CALLBACK_INSTANCE instance, void *context, PTP_TIMER timer)
{
    struct timed_text *tt = context;
    IMFMediaEngineEx *engine;
    double time;

    EnterCriticalSection(&tt->cs);
    engine = tt->engine;
    LeaveCriticalSection(&tt->cs);
    if (!engine) return;

    /* The engine outlives its timer: detaching waits for this callback. */
    time = IMFMediaEngineEx_GetCurrentTime(engine);

    EnterCriticalSection(&tt->cs);
    if (tt->engine) timed_text_update_cues(tt, time);
    LeaveCriticalSection(&tt->cs);
}

/* Run the timer while a track is active and someone listens. Called with the lock held. */
static void timed_text_update_timer(struct timed_text *tt)
{
    BOOL needed = FALSE;
    FILETIME due;
    size_t i;

    if (tt->engine && tt->notify && tt->timer)
        for (i = 0; i < tt->track_count && !needed; ++i)
            needed = tt->tracks[i]->active;

    if (needed == tt->timer_running) return;
    if (needed)
    {
        /* Negative: relative to now, in 100ns units. */
        ULARGE_INTEGER t;
        t.QuadPart = (ULONGLONG)(-(LONGLONG)CUE_POLL_MS * 10000);
        due.dwLowDateTime = t.LowPart;
        due.dwHighDateTime = t.HighPart;
        SetThreadpoolTimer(tt->timer, &due, CUE_POLL_MS, CUE_POLL_MS / 4);
    }
    else
    {
        SetThreadpoolTimer(tt->timer, NULL, 0, 0);
        timed_text_clear_cues(tt, 0.0);
    }
    tt->timer_running = needed;
}

static void timed_text_select(struct timed_text *tt, struct tt_track *track, BOOL selected)
{
    if (track->active == !!selected) return;
    track->active = !!selected;
    timed_text_queue_track_event(tt, TT_EVENT_TRACK_SELECTED, track->id, track->active);
    timed_text_update_timer(tt);
}

static struct tt_track *timed_text_find_track(struct timed_text *tt, DWORD id, size_t *index)
{
    size_t i;

    for (i = 0; i < tt->track_count; ++i)
    {
        if (tt->tracks[i]->id == id)
        {
            if (index) *index = i;
            return tt->tracks[i];
        }
    }
    return NULL;
}

/* Take a track into the object; with the lock held. */
static HRESULT timed_text_add_track(struct timed_text *tt, struct tt_track *track)
{
    struct tt_track **tracks;

    if (!(tracks = realloc(tt->tracks, (tt->track_count + 1) * sizeof(*tracks)))) return E_OUTOFMEMORY;
    tt->tracks = tracks;
    track->id = ++tt->next_track_id;
    track->owner = tt;
    IMFTimedTextTrack_AddRef(&track->IMFTimedTextTrack_iface);
    tt->tracks[tt->track_count++] = track;
    return S_OK;
}

static HRESULT WINAPI timed_text_QueryInterface(IMFTimedText *iface, REFIID riid, void **out)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFTimedText) || IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFTimedText_AddRef(iface);
        return S_OK;
    }
    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI timed_text_AddRef(IMFTimedText *iface)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);
    ULONG refcount = InterlockedIncrement(&tt->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG WINAPI timed_text_Release(IMFTimedText *iface)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);
    ULONG refcount = InterlockedDecrement(&tt->refcount);
    struct tt_event *event, *next;
    size_t i;

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        if (tt->timer)
        {
            SetThreadpoolTimer(tt->timer, NULL, 0, 0);
            WaitForThreadpoolTimerCallbacks(tt->timer, TRUE);
            CloseThreadpoolTimer(tt->timer);
        }
        for (i = 0; i < tt->track_count; ++i)
        {
            tt->tracks[i]->owner = NULL;
            IMFTimedTextTrack_Release(&tt->tracks[i]->IMFTimedTextTrack_iface);
        }
        free(tt->tracks);
        for (i = 0; i < tt->active_count; ++i)
            IMFTimedTextCue_Release(&tt->active[i]->IMFTimedTextCue_iface);
        free(tt->active);
        LIST_FOR_EACH_ENTRY_SAFE(event, next, &tt->events, struct tt_event, entry)
        {
            if (event->cue) IMFTimedTextCue_Release(&event->cue->IMFTimedTextCue_iface);
            free(event);
        }
        if (tt->notify) IMFTimedTextNotify_Release(tt->notify);
        DeleteCriticalSection(&tt->cs);
        free(tt);
    }
    return refcount;
}

static HRESULT WINAPI timed_text_RegisterNotifications(IMFTimedText *iface, IMFTimedTextNotify *notify)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);

    TRACE("%p, %p.\n", iface, notify);

    EnterCriticalSection(&tt->cs);
    if (tt->notify) IMFTimedTextNotify_Release(tt->notify);
    if ((tt->notify = notify)) IMFTimedTextNotify_AddRef(notify);
    timed_text_update_timer(tt);
    LeaveCriticalSection(&tt->cs);
    return S_OK;
}

static HRESULT WINAPI timed_text_SelectTrack(IMFTimedText *iface, DWORD id, BOOL selected)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);
    struct tt_track *track;
    HRESULT hr = S_OK;

    TRACE("%p, %lu, %d.\n", iface, id, selected);

    EnterCriticalSection(&tt->cs);
    if (!(track = timed_text_find_track(tt, id, NULL)))
        hr = E_INVALIDARG;
    else
        timed_text_select(tt, track, selected);
    LeaveCriticalSection(&tt->cs);
    return hr;
}

/* Load a document into a new track, which the application hears about as on
 * Windows: added, loading, then loaded or an error, and selected if default. */
static HRESULT timed_text_add_source(struct timed_text *tt, const BYTE *data, DWORD size, HRESULT read_hr,
        const WCHAR *label, const WCHAR *language, MF_TIMED_TEXT_TRACK_KIND kind, BOOL is_default, DWORD *id)
{
    struct parsed_source source;
    struct tt_track *track;
    HRESULT hr, parse_hr;
    size_t i;

    if (!(track = tt_track_create(label, language, kind))) return E_OUTOFMEMORY;
    parse_hr = FAILED(read_hr) ? read_hr : parse_source(data, size, &source);

    EnterCriticalSection(&tt->cs);
    if (FAILED(hr = timed_text_add_track(tt, track)))
    {
        LeaveCriticalSection(&tt->cs);
        if (SUCCEEDED(parse_hr)) parsed_source_cleanup(&source);
        IMFTimedTextTrack_Release(&track->IMFTimedTextTrack_iface);
        return hr;
    }
    timed_text_queue_track_event(tt, TT_EVENT_TRACK_ADDED, track->id, FALSE);
    track->ready_state = MF_TIMED_TEXT_TRACK_READY_STATE_LOADING;
    timed_text_queue_track_event(tt, TT_EVENT_TRACK_READY_STATE, track->id, FALSE);
    if (SUCCEEDED(parse_hr))
    {
        parsed_source_cleanup(&track->source);
        track->source = source;
        track->format = source.format;
        /* The cues move to the track, which numbers them. */
        track->cues = source.cues;
        memset(&track->source.cues, 0, sizeof(track->source.cues));
        for (i = 0; i < track->cues.count; ++i)
        {
            track->cues.cues[i]->id = ++tt->next_cue_id;
            track->cues.cues[i]->track_id = track->id;
            track->cues.cues[i]->kind = kind;
        }
        track->ready_state = MF_TIMED_TEXT_TRACK_READY_STATE_LOADED;
        TRACE("Track %lu: %Iu cues, format %s.\n", track->id, track->cues.count, debugstr_guid(&track->format));
    }
    else
    {
        struct tt_event event = { .type = TT_EVENT_ERROR, .id = track->id, .extended = parse_hr };

        WARN("Failed to load track %lu, hr %#lx.\n", track->id, parse_hr);
        track->ready_state = MF_TIMED_TEXT_TRACK_READY_STATE_ERROR;
        track->error = event.code = parse_hr == MF_E_INVALID_FILE_FORMAT ? MF_TIMED_TEXT_ERROR_CODE_DATA_FORMAT
                : FAILED(read_hr) ? MF_TIMED_TEXT_ERROR_CODE_NETWORK : MF_TIMED_TEXT_ERROR_CODE_INTERNAL;
        track->extended_error = parse_hr;
        timed_text_queue_event(tt, &event);
    }
    timed_text_queue_track_event(tt, TT_EVENT_TRACK_READY_STATE, track->id, FALSE);
    if (is_default && SUCCEEDED(parse_hr)) timed_text_select(tt, track, TRUE);
    *id = track->id;
    LeaveCriticalSection(&tt->cs);

    IMFTimedTextTrack_Release(&track->IMFTimedTextTrack_iface);
    return S_OK;
}

static HRESULT WINAPI timed_text_AddDataSource(IMFTimedText *iface, IMFByteStream *stream, const WCHAR *label,
        const WCHAR *language, MF_TIMED_TEXT_TRACK_KIND kind, BOOL is_default, DWORD *id)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);
    DWORD size = 0;
    BYTE *data;
    HRESULT hr, read_hr;

    TRACE("%p, %p, %s, %s, %d, %d, %p.\n", iface, stream, debugstr_w(label), debugstr_w(language), kind,
            is_default, id);

    if (!stream || !id) return E_POINTER;
    *id = 0;
    read_hr = read_byte_stream(stream, &data, &size);
    hr = timed_text_add_source(tt, data, size, read_hr, label, language, kind, is_default, id);
    free(data);
    return hr;
}

static HRESULT WINAPI timed_text_AddDataSourceFromUrl(IMFTimedText *iface, const WCHAR *url, const WCHAR *label,
        const WCHAR *language, MF_TIMED_TEXT_TRACK_KIND kind, BOOL is_default, DWORD *id)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);
    MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
    IMFSourceResolver *resolver;
    IMFByteStream *stream = NULL;
    IUnknown *object = NULL;
    DWORD size = 0;
    BYTE *data = NULL;
    HRESULT hr;

    TRACE("%p, %s, %s, %s, %d, %d, %p.\n", iface, debugstr_w(url), debugstr_w(label), debugstr_w(language),
            kind, is_default, id);

    if (!url || !id) return E_POINTER;
    *id = 0;

    /* The resolver opens files and network locations alike. */
    if (SUCCEEDED(hr = MFCreateSourceResolver(&resolver)))
    {
        hr = IMFSourceResolver_CreateObjectFromURL(resolver, url, MF_RESOLUTION_BYTESTREAM
                | MF_RESOLUTION_READ, NULL, &type, &object);
        IMFSourceResolver_Release(resolver);
    }
    if (SUCCEEDED(hr) && FAILED(hr = IUnknown_QueryInterface(object, &IID_IMFByteStream, (void **)&stream)))
        stream = NULL;
    if (object) IUnknown_Release(object);
    if (stream)
    {
        hr = read_byte_stream(stream, &data, &size);
        IMFByteStream_Release(stream);
    }
    hr = timed_text_add_source(tt, data, size, hr, label, language, kind, is_default, id);
    free(data);
    return hr;
}

static HRESULT WINAPI timed_text_AddTrack(IMFTimedText *iface, const WCHAR *label, const WCHAR *language,
        MF_TIMED_TEXT_TRACK_KIND kind, IMFTimedTextTrack **out)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);
    struct tt_track *track;
    HRESULT hr;

    TRACE("%p, %s, %s, %d, %p.\n", iface, debugstr_w(label), debugstr_w(language), kind, out);

    if (!out) return E_POINTER;
    *out = NULL;
    if (!(track = tt_track_create(label, language, kind))) return E_OUTOFMEMORY;

    EnterCriticalSection(&tt->cs);
    if (SUCCEEDED(hr = timed_text_add_track(tt, track)))
    {
        /* A track the application fills itself is loaded from the start. */
        track->ready_state = MF_TIMED_TEXT_TRACK_READY_STATE_LOADED;
        timed_text_queue_track_event(tt, TT_EVENT_TRACK_ADDED, track->id, FALSE);
        *out = &track->IMFTimedTextTrack_iface;
    }
    LeaveCriticalSection(&tt->cs);

    if (FAILED(hr)) IMFTimedTextTrack_Release(&track->IMFTimedTextTrack_iface);
    return hr;
}

static HRESULT WINAPI timed_text_RemoveTrack(IMFTimedText *iface, IMFTimedTextTrack *iface_track)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);
    struct tt_track *track = NULL;
    HRESULT hr = E_INVALIDARG;
    size_t i, index;

    TRACE("%p, %p.\n", iface, iface_track);

    if (!iface_track) return E_POINTER;

    EnterCriticalSection(&tt->cs);
    for (index = 0; index < tt->track_count; ++index)
    {
        if (&tt->tracks[index]->IMFTimedTextTrack_iface == iface_track)
        {
            track = tt->tracks[index];
            break;
        }
    }
    if (track)
    {
        if (track->active) timed_text_select(tt, track, FALSE);
        memmove(&tt->tracks[index], &tt->tracks[index + 1], (tt->track_count - index - 1) * sizeof(*tt->tracks));
        --tt->track_count;
        /* Cues of the track can no longer be active. */
        for (i = 0; i < tt->active_count; )
        {
            if (tt->active[i]->track_id == track->id)
            {
                IMFTimedTextCue_Release(&tt->active[i]->IMFTimedTextCue_iface);
                memmove(&tt->active[i], &tt->active[i + 1], (tt->active_count - i - 1) * sizeof(*tt->active));
                --tt->active_count;
            }
            else ++i;
        }
        timed_text_queue_track_event(tt, TT_EVENT_TRACK_REMOVED, track->id, FALSE);
        track->owner = NULL;
        timed_text_update_timer(tt);
        hr = S_OK;
    }
    LeaveCriticalSection(&tt->cs);

    if (track) IMFTimedTextTrack_Release(&track->IMFTimedTextTrack_iface);
    return hr;
}

static HRESULT WINAPI timed_text_GetCueTimeOffset(IMFTimedText *iface, double *offset)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);

    TRACE("%p, %p.\n", iface, offset);

    if (!offset) return E_POINTER;
    EnterCriticalSection(&tt->cs);
    *offset = tt->cue_offset;
    LeaveCriticalSection(&tt->cs);
    return S_OK;
}

static HRESULT WINAPI timed_text_SetCueTimeOffset(IMFTimedText *iface, double offset)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);

    TRACE("%p, %.8e.\n", iface, offset);

    EnterCriticalSection(&tt->cs);
    tt->cue_offset = offset;
    LeaveCriticalSection(&tt->cs);
    return S_OK;
}

static HRESULT WINAPI timed_text_GetTracks(IMFTimedText *iface, IMFTimedTextTrackList **tracks)
{
    TRACE("%p, %p.\n", iface, tracks);
    return timed_text_get_tracks(impl_from_IMFTimedText(iface), TRACKS_ALL, tracks);
}

static HRESULT WINAPI timed_text_GetActiveTracks(IMFTimedText *iface, IMFTimedTextTrackList **tracks)
{
    TRACE("%p, %p.\n", iface, tracks);
    return timed_text_get_tracks(impl_from_IMFTimedText(iface), TRACKS_ACTIVE, tracks);
}

static HRESULT WINAPI timed_text_GetTextTracks(IMFTimedText *iface, IMFTimedTextTrackList **tracks)
{
    TRACE("%p, %p.\n", iface, tracks);
    return timed_text_get_tracks(impl_from_IMFTimedText(iface), TRACKS_TEXT, tracks);
}

static HRESULT WINAPI timed_text_GetMetadataTracks(IMFTimedText *iface, IMFTimedTextTrackList **tracks)
{
    TRACE("%p, %p.\n", iface, tracks);
    return timed_text_get_tracks(impl_from_IMFTimedText(iface), TRACKS_METADATA, tracks);
}

static HRESULT WINAPI timed_text_SetInBandEnabled(IMFTimedText *iface, BOOL enabled)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);

    TRACE("%p, %d.\n", iface, enabled);

    EnterCriticalSection(&tt->cs);
    tt->in_band = !!enabled;
    LeaveCriticalSection(&tt->cs);
    return S_OK;
}

static BOOL WINAPI timed_text_IsInBandEnabled(IMFTimedText *iface)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);
    BOOL ret;

    TRACE("%p.\n", iface);

    EnterCriticalSection(&tt->cs);
    ret = tt->in_band;
    LeaveCriticalSection(&tt->cs);
    return ret;
}

static const IMFTimedTextVtbl timed_text_vtbl =
{
    timed_text_QueryInterface,
    timed_text_AddRef,
    timed_text_Release,
    timed_text_RegisterNotifications,
    timed_text_SelectTrack,
    timed_text_AddDataSource,
    timed_text_AddDataSourceFromUrl,
    timed_text_AddTrack,
    timed_text_RemoveTrack,
    timed_text_GetCueTimeOffset,
    timed_text_SetCueTimeOffset,
    timed_text_GetTracks,
    timed_text_GetActiveTracks,
    timed_text_GetTextTracks,
    timed_text_GetMetadataTracks,
    timed_text_SetInBandEnabled,
    timed_text_IsInBandEnabled,
};

/* Entry points for the engine */

HRESULT timed_text_create(IMFMediaEngineEx *engine, IMFTimedText **out)
{
    struct timed_text *tt;

    if (!(tt = calloc(1, sizeof(*tt)))) return E_OUTOFMEMORY;
    tt->IMFTimedText_iface.lpVtbl = &timed_text_vtbl;
    tt->refcount = 1;
    tt->engine = engine;
    tt->in_band = TRUE;
    list_init(&tt->events);
    InitializeCriticalSection(&tt->cs);
    if (!(tt->timer = CreateThreadpoolTimer(timed_text_timer_callback, tt, NULL)))
        WARN("Failed to create the cue timer, error %lu.\n", GetLastError());
    *out = &tt->IMFTimedText_iface;
    return S_OK;
}

/* A new source: what was shown belongs to the old one. */
void timed_text_media_changed(IMFTimedText *iface)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);
    struct tt_event event = { .type = TT_EVENT_RESET };

    EnterCriticalSection(&tt->cs);
    timed_text_clear_cues(tt, 0.0);
    timed_text_queue_event(tt, &event);
    LeaveCriticalSection(&tt->cs);
}

/* The engine is shutting down or going away: stop asking it for the time. */
void timed_text_detach(IMFTimedText *iface)
{
    struct timed_text *tt = impl_from_IMFTimedText(iface);

    EnterCriticalSection(&tt->cs);
    tt->engine = NULL;
    timed_text_update_timer(tt);
    LeaveCriticalSection(&tt->cs);
    if (tt->timer) WaitForThreadpoolTimerCallbacks(tt->timer, TRUE);
}
