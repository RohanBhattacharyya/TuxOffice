/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* TuxOffice's launch window.
 *
 * Before Office shows anything of its own, the launcher may update the prefix
 * for a new runner, install components, and Wine has to boot: that can take
 * from a few seconds to minutes. The launcher starts this window first. It
 * reads the commands the launcher appends to DIR/status and closes when Office
 * shows its first window (win32u then removes the file TUXOFFICE_SPLASH_READY
 * names, DIR/ready), when the watched process (the launcher, which becomes
 * Office's process) exits, or after a timeout.
 *
 * Usage: tuxoffice-splash --dir DIR --watch PID --name NAME [--app-id ID]
 *            [--accent RRGGBB] [--dark] [--logo PNG] [--version TEXT]
 *            [--record FILE] [--timeout SECONDS]
 * Commands, one per line in DIR/status:
 *   status TEXT   the line above the bar
 *   progress N    a bar filled to N percent
 *   progress      a bar that moves back and forth (the duration is unknown)
 *   launch        Office is starting: wait for DIR/ready to be removed. The
 *                 bar follows how long this took the last times (--record).
 *   error TEXT    show TEXT until clicked, or for 30 seconds
 *   close         close now
 *
 * Native Wayland (xdg-shell, fractional scaling), else X11 through libX11
 * when that can be loaded. Text is drawn with FreeType and fontconfig, the
 * logo is a PNG read with zlib.
 *
 * Build: wayland-scanner the protocols below, then
 *   cc -O2 -o tuxoffice-splash lib/splash.c *-protocol.c \
 *      $(pkg-config --cflags --libs wayland-client freetype2 fontconfig zlib) -lm -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>
#include <zlib.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#if __has_include(<X11/Xlib.h>)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#define HAVE_X11 1
#endif

#define BUTTON_LEFT 0x110 /* BTN_LEFT */

/* Layout, in logical pixels. */
enum
{
    WIDTH = 480, HEIGHT = 172, RADIUS = 12,
    LOGO_X = 26, LOGO_Y = 26, LOGO_SIZE = 120,
    TEXT_X = 170, TEXT_RIGHT = WIDTH - 30,
    TITLE_Y = 66, TITLE_SIZE = 23,
    SUBTITLE_Y = 90, SMALL_SIZE = 13,
    STATUS_Y = 124, ERROR_Y = 118, LINE_HEIGHT = 18,
    BAR_Y = 136, BAR_HEIGHT = 4,
};

struct theme { uint32_t background, border, text, secondary, track, error; };
static const struct theme light_theme = {0xffffffff, 0xffd0d0d0, 0xff1b1b1b, 0xff616161, 0xffe3e3e3, 0xffc42b1c};
static const struct theme dark_theme = {0xff2b2b2b, 0xff4a4a4a, 0xfff3f3f3, 0xffababab, 0xff464646, 0xffff99a4};

static struct
{
    const char *dir, *name, *app_id, *logo, *version, *record;
    const struct theme *theme;
    uint32_t accent;
    double timeout, start;
    int pidfd, status_fd;
    char line[2048];
    size_t line_len;
    char status[512];
    bool determinate;
    double progress, shown;
    bool launching, ready, error, quit, dirty;
    double launch_start, expected, ready_time, error_time, last_tick;
    bool watched_exited;
} app = {.timeout = 120, .pidfd = -1, .status_fd = -1, .accent = 0xff185abd};

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- Drawing ---------------------------------------------------------- */

struct canvas { uint32_t *px; int w, h; };

/* Blends COLOR (straight ARGB) over a premultiplied pixel with coverage. */
static inline void blend(uint32_t *dst, uint32_t color, unsigned coverage)
{
    unsigned a = ((color >> 24) * coverage + 127) / 255, inv = 255 - a;
    uint32_t d = *dst;
    unsigned r = (((color >> 16) & 0xff) * a + ((d >> 16) & 0xff) * inv + 127) / 255;
    unsigned g = (((color >> 8) & 0xff) * a + ((d >> 8) & 0xff) * inv + 127) / 255;
    unsigned b = ((color & 0xff) * a + (d & 0xff) * inv + 127) / 255;
    unsigned da = a + ((d >> 24) * inv + 127) / 255;
    *dst = da << 24 | r << 16 | g << 8 | b;
}

static inline void blend_premultiplied(uint32_t *dst, uint32_t src)
{
    unsigned inv = 255 - (src >> 24), shift;
    uint32_t d = *dst, out = 0;
    for (shift = 0; shift < 32; shift += 8)
    {
        unsigned value = ((src >> shift) & 0xff) + (((d >> shift) & 0xff) * inv + 127) / 255;
        out |= (value > 255 ? 255 : value) << shift;
    }
    *dst = out;
}

/* Antialiased rounded rectangle, from the signed distance of each pixel's
 * centre to its outline. */
static void fill_rounded(struct canvas *c, double x0, double y0, double x1, double y1, double r, uint32_t color)
{
    double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    double hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
    int x, y, left = floor(x0), top = floor(y0), right = ceil(x1), bottom = ceil(y1);

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > c->w) right = c->w;
    if (bottom > c->h) bottom = c->h;
    for (y = top; y < bottom; y++)
        for (x = left; x < right; x++)
        {
            double qx = fabs(x + 0.5 - cx) - hx, qy = fabs(y + 0.5 - cy) - hy;
            double d = hypot(fmax(qx, 0), fmax(qy, 0)) + fmin(fmax(qx, qy), 0) - r;
            double coverage = fmin(fmax(0.5 - d, 0), 1);
            if (coverage > 0) blend(&c->px[y * c->w + x], color, (unsigned)(coverage * 255 + 0.5));
        }
}

/* ---- PNG -------------------------------------------------------------- */

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static unsigned char paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* 8-bit RGB or RGBA, not interlaced: what icons and the logo use. Returns
 * premultiplied ARGB. */
static uint32_t *load_png(const char *path, int *width, int *height)
{
    unsigned char *file = NULL, *idat = NULL, *raw = NULL;
    uint32_t *pixels = NULL;
    size_t size = 0, idat_len = 0, pos = 8;
    int w = 0, h = 0, channels = 0, x, y;
    struct stat st;
    FILE *f;

    if (!path || !(f = fopen(path, "rb"))) return NULL;
    if (!fstat(fileno(f), &st) && st.st_size > 8 && st.st_size < (64 << 20) && (file = malloc(st.st_size)))
        size = fread(file, 1, st.st_size, f);
    fclose(f);
    if (size < 8 || memcmp(file, "\x89PNG\r\n\x1a\n", 8)) goto done;
    while (pos + 12 <= size)
    {
        uint32_t len = be32(file + pos);
        const unsigned char *type = file + pos + 4, *data = file + pos + 8;
        if (len > size - pos - 12) goto done;
        if (!memcmp(type, "IHDR", 4) && len >= 13)
        {
            w = be32(data);
            h = be32(data + 4);
            if (data[8] != 8 || data[12] || w <= 0 || h <= 0 || w > 4096 || h > 4096) goto done;
            channels = data[9] == 6 ? 4 : data[9] == 2 ? 3 : 0;
            if (!channels) goto done;
        }
        else if (!memcmp(type, "IDAT", 4))
        {
            unsigned char *grown = realloc(idat, idat_len + len);
            if (!grown) goto done;
            idat = grown;
            memcpy(idat + idat_len, data, len);
            idat_len += len;
        }
        else if (!memcmp(type, "IEND", 4)) break;
        pos += 12 + len;
    }
    if (!channels || !idat) goto done;
    {
        size_t stride = (size_t)w * channels;
        uLongf raw_len = (stride + 1) * h;
        if (!(raw = malloc(raw_len)) || uncompress(raw, &raw_len, idat, idat_len) != Z_OK ||
            raw_len != (stride + 1) * h || !(pixels = malloc((size_t)w * h * 4)))
            goto done;
        for (y = 0; y < h; y++)
        {
            unsigned char *row = raw + y * (stride + 1) + 1, *prev = y ? row - stride - 1 : NULL;
            unsigned char filter = row[-1];
            for (x = 0; x < (int)stride; x++)
            {
                int a = x >= channels ? row[x - channels] : 0, b = prev ? prev[x] : 0;
                int c = prev && x >= channels ? prev[x - channels] : 0;
                switch (filter)
                {
                case 1: row[x] += a; break;
                case 2: row[x] += b; break;
                case 3: row[x] += (a + b) / 2; break;
                case 4: row[x] += paeth(a, b, c); break;
                }
            }
            for (x = 0; x < w; x++)
            {
                const unsigned char *p = row + x * channels;
                unsigned alpha = channels == 4 ? p[3] : 255;
                pixels[y * w + x] = alpha << 24 | (p[0] * alpha + 127) / 255 << 16 |
                                    (p[1] * alpha + 127) / 255 << 8 | (p[2] * alpha + 127) / 255;
            }
        }
        *width = w;
        *height = h;
    }
done:
    free(file);
    free(idat);
    free(raw);
    return pixels;
}

/* Box filter: each destination pixel averages the source area it covers. */
static void resample_axis(const float *src, float *dst, int count, int src_len, int dst_len, int src_step,
                          int dst_step, int line_step_src, int line_step_dst)
{
    double ratio = (double)src_len / dst_len;
    int line, i, k, ch;

    for (line = 0; line < count; line++)
        for (i = 0; i < dst_len; i++)
        {
            double s0 = i * ratio, s1 = (i + 1) * ratio, sum[4] = {0};
            for (k = (int)s0; k < src_len && k < s1; k++)
            {
                double weight = fmin(s1, k + 1) - fmax(s0, k);
                for (ch = 0; ch < 4; ch++) sum[ch] += weight * src[line * line_step_src + k * src_step + ch];
            }
            for (ch = 0; ch < 4; ch++) dst[line * line_step_dst + i * dst_step + ch] = sum[ch] / ratio;
        }
}

static uint32_t *resample(const uint32_t *src, int sw, int sh, int dw, int dh)
{
    float *in = malloc(sizeof(float) * 4 * sw * sh), *mid = malloc(sizeof(float) * 4 * dw * sh);
    float *out = malloc(sizeof(float) * 4 * dw * dh);
    uint32_t *result = malloc(sizeof(uint32_t) * dw * dh);
    int i, ch;

    if (in && mid && out && result)
    {
        for (i = 0; i < sw * sh; i++)
            for (ch = 0; ch < 4; ch++) in[i * 4 + ch] = (src[i] >> (ch * 8)) & 0xff;
        resample_axis(in, mid, sh, sw, dw, 4, 4, sw * 4, dw * 4);
        resample_axis(mid, out, dw, sh, dh, dw * 4, dw * 4, 4, 4);
        for (i = 0; i < dw * dh; i++)
        {
            uint32_t value = 0;
            for (ch = 0; ch < 4; ch++) value |= (uint32_t)fmin(255, out[i * 4 + ch] + 0.5) << (ch * 8);
            result[i] = value;
        }
    }
    else
    {
        free(result);
        result = NULL;
    }
    free(in);
    free(mid);
    free(out);
    return result;
}

/* ---- Text ------------------------------------------------------------- */

static FT_Library freetype;
static FT_Face title_face, text_face;

static FT_Face load_face(const char *pattern)
{
    FcPattern *query = FcNameParse((const FcChar8 *)pattern), *match;
    FcChar8 *file;
    FcResult result;
    FT_Face face = NULL;
    int index = 0;

    if (!query) return NULL;
    FcConfigSubstitute(NULL, query, FcMatchPattern);
    FcDefaultSubstitute(query);
    if ((match = FcFontMatch(NULL, query, &result)))
    {
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
        {
            FcPatternGetInteger(match, FC_INDEX, 0, &index);
            if (FT_New_Face(freetype, (const char *)file, index, &face)) face = NULL;
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(query);
    return face;
}

static uint32_t next_codepoint(const char **text)
{
    const unsigned char *s = (const unsigned char *)*text;
    uint32_t cp = *s++;
    int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;

    if (extra) cp &= 0x3f >> extra;
    while (extra-- && (*s & 0xc0) == 0x80) cp = cp << 6 | (*s++ & 0x3f);
    *text = (const char *)s;
    return cp;
}

/* Width in pixels of LEN bytes of TEXT (all of it when LEN < 0). */
static double text_width(FT_Face face, const char *text, int len)
{
    const char *end = len < 0 ? text + strlen(text) : text + len;
    FT_UInt previous = 0;
    FT_Pos width = 0;

    while (text < end)
    {
        FT_UInt glyph = FT_Get_Char_Index(face, next_codepoint(&text));
        FT_Vector kerning;
        if (previous && FT_HAS_KERNING(face) && !FT_Get_Kerning(face, previous, glyph, FT_KERNING_DEFAULT, &kerning))
            width += kerning.x;
        if (!FT_Load_Glyph(face, glyph, FT_LOAD_TARGET_LIGHT)) width += face->glyph->advance.x;
        previous = glyph;
    }
    return width / 64.0;
}

static void draw_glyphs(struct canvas *c, FT_Face face, double x, double baseline, const char *text, int len,
                        uint32_t color)
{
    const char *end = text + len;
    FT_UInt previous = 0;
    FT_Pos pen = lround(x * 64);
    int gx, gy;

    while (text < end)
    {
        FT_UInt glyph = FT_Get_Char_Index(face, next_codepoint(&text));
        FT_Vector kerning;
        FT_Bitmap *bitmap;
        int left, top;

        if (previous && FT_HAS_KERNING(face) && !FT_Get_Kerning(face, previous, glyph, FT_KERNING_DEFAULT, &kerning))
            pen += kerning.x;
        previous = glyph;
        if (FT_Load_Glyph(face, glyph, FT_LOAD_TARGET_LIGHT) ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LIGHT))
            continue;
        bitmap = &face->glyph->bitmap;
        left = (int)((pen + 32) >> 6) + face->glyph->bitmap_left;
        top = (int)lround(baseline) - face->glyph->bitmap_top;
        if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY)
            for (gy = 0; gy < (int)bitmap->rows; gy++)
                for (gx = 0; gx < (int)bitmap->width; gx++)
                {
                    int px = left + gx, py = top + gy;
                    unsigned coverage = bitmap->buffer[gy * bitmap->pitch + gx];
                    if (coverage && px >= 0 && py >= 0 && px < c->w && py < c->h)
                        blend(&c->px[py * c->w + px], color, coverage);
                }
        pen += face->glyph->advance.x;
    }
}

/* Draws TEXT, shortened with an ellipsis to fit MAX_WIDTH. */
static void draw_text(struct canvas *c, FT_Face face, double x, double baseline, const char *text,
                      double max_width, uint32_t color)
{
    static const char ellipsis[] = "\xe2\x80\xa6";
    int len = strlen(text);

    if (!face) return;
    if (text_width(face, text, len) <= max_width)
    {
        draw_glyphs(c, face, x, baseline, text, len, color);
        return;
    }
    max_width -= text_width(face, ellipsis, -1);
    while (len > 0)
    {
        do len--; while (len > 0 && (text[len] & 0xc0) == 0x80);
        if (text_width(face, text, len) <= max_width) break;
    }
    while (len > 0 && text[len - 1] == ' ') len--;
    draw_glyphs(c, face, x, baseline, text, len, color);
    draw_glyphs(c, face, x + text_width(face, text, len), baseline, ellipsis, strlen(ellipsis), color);
}

/* Breaks TEXT at spaces into at most MAX_LINES lines; the last is shortened. */
static void draw_wrapped(struct canvas *c, FT_Face face, double x, double baseline, double line_height,
                         const char *text, double max_width, int max_lines, uint32_t color)
{
    int line;

    if (!face) return;
    for (line = 0; line < max_lines && *text; line++)
    {
        int len = strlen(text), fit = 0, i;
        if (line < max_lines - 1 && text_width(face, text, len) > max_width)
        {
            for (i = 1; i <= len; i++)
                if ((text[i] == ' ' || !text[i]) && text_width(face, text, i) <= max_width) fit = i;
            if (fit)
            {
                char *part = strndup(text, fit);
                if (part) draw_text(c, face, x, baseline + line * line_height, part, max_width, color);
                free(part);
                text += fit;
                while (*text == ' ') text++;
                continue;
            }
        }
        draw_text(c, face, x, baseline + line * line_height, text, max_width, color);
        break;
    }
}

/* ---- The window's contents -------------------------------------------- */

static uint32_t *logo_source;
static int logo_width, logo_height;
static struct { uint32_t *px; int w, h; double scale; bool rounded; unsigned generation; } base;
static struct { uint32_t *px; int size; } logo;

static uint32_t lighten(uint32_t color, double amount)
{
    uint32_t out = color & 0xff000000;
    int shift;
    for (shift = 0; shift < 24; shift += 8)
    {
        double value = (color >> shift) & 0xff;
        out |= (uint32_t)(value + (255 - value) * amount + 0.5) << shift;
    }
    return out;
}

static uint32_t accent_color(void)
{
    return app.theme == &dark_theme ? lighten(app.accent, 0.35) : app.accent;
}

/* Everything but the bar, redrawn when the text or the scale changes. */
static void render_base(double scale, bool rounded)
{
    struct canvas c;
    double border = fmax(1, round(scale)), radius = rounded ? RADIUS * scale : 0;
    double text_x = TEXT_X * scale, column = (TEXT_RIGHT - TEXT_X) * scale;
    char subtitle[128];
    int w = lround(WIDTH * scale), h = lround(HEIGHT * scale), size, x, y;

    if (base.w != w || base.h != h)
    {
        free(base.px);
        base.px = calloc((size_t)w * h, sizeof(uint32_t));
        base.w = w;
        base.h = h;
    }
    if (!base.px) return;
    base.scale = scale;
    base.rounded = rounded;
    base.generation++;
    c = (struct canvas){base.px, w, h};
    memset(base.px, 0, (size_t)w * h * 4);
    fill_rounded(&c, 0, 0, w, h, radius, app.theme->border);
    fill_rounded(&c, border, border, w - border, h - border, fmax(radius - border, 0), app.theme->background);

    size = lround(LOGO_SIZE * scale);
    if (logo_source && logo.size != size)
    {
        free(logo.px);
        logo.px = resample(logo_source, logo_width, logo_height, size, size);
        logo.size = logo.px ? size : 0;
    }
    if (logo.px)
    {
        int left = lround(LOGO_X * scale), top = lround(LOGO_Y * scale);
        for (y = 0; y < size && top + y < h; y++)
            for (x = 0; x < size && left + x < w; x++)
                blend_premultiplied(&base.px[(top + y) * w + left + x], logo.px[y * size + x]);
    }

    if (title_face) FT_Set_Char_Size(title_face, 0, lround(TITLE_SIZE * scale * 64), 72, 72);
    if (text_face) FT_Set_Char_Size(text_face, 0, lround(SMALL_SIZE * scale * 64), 72, 72);
    draw_text(&c, title_face, text_x, TITLE_Y * scale, app.name, column, app.theme->text);
    snprintf(subtitle, sizeof(subtitle), "TuxOffice%s%s", app.version ? " " : "", app.version ? app.version : "");
    draw_text(&c, text_face, text_x, SUBTITLE_Y * scale, subtitle, column, app.theme->secondary);
    if (app.error)
        draw_wrapped(&c, text_face, text_x, ERROR_Y * scale, LINE_HEIGHT * scale, app.status, column, 3,
                     app.theme->error);
    else
        draw_text(&c, text_face, text_x, STATUS_Y * scale, app.status, column, app.theme->text);
}

static bool animating(void)
{
    if (app.error) return false;
    if (app.ready || app.launching || !app.determinate) return true;
    return fabs(app.shown - app.progress) > 0.001;
}

/* The filled part of the bar, as fractions of its width. */
static bool bar_extent(double now, double *from, double *to)
{
    double dt = fmin(now - app.last_tick, 0.1);

    app.last_tick = now;
    if (app.error) return false;
    *from = 0;
    if (app.ready || (app.launching && app.expected > 0) || app.determinate)
    {
        double target = app.progress;
        if (app.ready) target = 1;
        else if (app.launching)
            target = 0.95 * (1 - exp(-2.3 * (now - app.launch_start) / app.expected));
        if (app.ready || target < app.shown || !app.launching)
            app.shown += (target - app.shown) * fmin(1, dt * (app.ready ? 14 : 8));
        else
            app.shown = target;
        *to = app.shown;
        return true;
    }
    else
    {
        /* Indeterminate: a segment that sweeps across, easing in and out. */
        double t = fmod(now - app.start, 1.6) / 1.6, pos = 0.5 - 0.5 * cos(M_PI * t);
        *from = fmax(0, pos * 1.4 - 0.4);
        *to = fmin(1, pos * 1.4);
        return *to > *from;
    }
}

/* Redraws the static part when the text or the scale changed. */
static void update_base(double scale, bool rounded)
{
    if (app.dirty || !base.px || base.scale != scale || base.rounded != rounded)
    {
        render_base(scale, rounded);
        app.dirty = false;
    }
}

/* Draws the window into PX (W x H pixels at SCALE, after update_base). With
 * FULL false, PX already holds the current base and only the bar's rows are
 * drawn; they are returned in DIRTY_TOP and DIRTY_BOTTOM. */
static void render(uint32_t *px, int w, int h, double scale, double now, bool full,
                   int *dirty_top, int *dirty_bottom)
{
    struct canvas c = {px, w, h};
    double from, to, x0 = TEXT_X * scale, x1 = TEXT_RIGHT * scale, y0 = BAR_Y * scale;
    double y1 = (BAR_Y + BAR_HEIGHT) * scale, r = (y1 - y0) / 2;
    int top = floor(y0) - 1, bottom = ceil(y1) + 1, y;

    if (!base.px || base.w != w || base.h != h) return;
    if (full)
    {
        memcpy(px, base.px, (size_t)w * h * 4);
        top = 0;
        bottom = h;
    }
    else
        for (y = top; y < bottom; y++) memcpy(px + y * w, base.px + y * w, w * 4);
    if (bar_extent(now, &from, &to))
    {
        fill_rounded(&c, x0, y0, x1, y1, r, app.theme->track);
        if (to > from)
        {
            double a = x0 + (x1 - x0) * from, b = x0 + (x1 - x0) * to;
            if (b - a < 2 * r) b = a + 2 * r;
            if (b > x1) { a -= b - x1; b = x1; }
            fill_rounded(&c, a, y0, b, y1, r, accent_color());
        }
    }
    *dirty_top = top;
    *dirty_bottom = bottom;
}

/* ---- Launcher state --------------------------------------------------- */

static void load_expected(void)
{
    FILE *f = app.record ? fopen(app.record, "r") : NULL;
    double value;
    if (!f) return;
    if (fscanf(f, "%lf", &value) == 1 && value > 0.5 && value < 600) app.expected = value;
    fclose(f);
}

static void save_expected(double seconds)
{
    char temp[4096];
    FILE *f;
    double value = app.expected > 0 ? app.expected * 0.6 + seconds * 0.4 : seconds;

    if (!app.record || seconds <= 0.2 || seconds > 600) return;
    snprintf(temp, sizeof(temp), "%s.%d", app.record, getpid());
    if (!(f = fopen(temp, "w"))) return;
    fprintf(f, "%.2f\n", value);
    if (fclose(f) || rename(temp, app.record)) unlink(temp);
}

static void command(char *line)
{
    char *arg = strchr(line, ' ');

    if (arg) *arg++ = 0;
    else arg = line + strlen(line);
    if (!strcmp(line, "status") || !strcmp(line, "error"))
    {
        snprintf(app.status, sizeof(app.status), "%s", arg);
        if (!strcmp(line, "error"))
        {
            app.error = true;
            app.error_time = now_seconds();
        }
        app.dirty = true;
    }
    else if (!strcmp(line, "progress"))
    {
        app.determinate = *arg != 0;
        app.progress = fmin(fmax(atof(arg) / 100, 0), 1);
        if (!app.determinate) app.shown = 0;
    }
    else if (!strcmp(line, "launch"))
    {
        app.launching = true;
        app.determinate = false;
        app.launch_start = now_seconds();
        app.shown = 0;
        load_expected();
    }
    else if (!strcmp(line, "close"))
        app.quit = true;
}

static void read_commands(void)
{
    char buffer[1024];
    ssize_t len, i;

    if (app.status_fd < 0) return;
    while ((len = read(app.status_fd, buffer, sizeof(buffer))) > 0)
        for (i = 0; i < len; i++)
        {
            if (buffer[i] == '\n')
            {
                app.line[app.line_len] = 0;
                command(app.line);
                app.line_len = 0;
            }
            else if (app.line_len < sizeof(app.line) - 1)
                app.line[app.line_len++] = buffer[i];
        }
}

/* Called every loop: reads commands and decides when to close. */
static void tick(double now)
{
    char path[4096];

    read_commands();
    if (app.launching && !app.ready && !app.error)
    {
        snprintf(path, sizeof(path), "%s/ready", app.dir);
        if (access(path, F_OK) && errno == ENOENT)
        {
            app.ready = true;
            app.ready_time = now;
            save_expected(now - app.launch_start);
        }
        else if (now - app.launch_start > app.timeout)
            app.quit = true;
    }
    if (app.ready && now - app.ready_time > 0.35) app.quit = true;
    if (app.error && now - app.error_time > 30) app.quit = true;
    if (app.watched_exited && !app.error) app.quit = true;
}

static void watched_exited(void)
{
    app.watched_exited = true;
    close(app.pidfd);
    app.pidfd = -1;
    read_commands(); /* an error the launcher wrote just before exiting */
}

static void click(void)
{
    if (app.error) app.quit = true;
}

/* ---- Wayland ---------------------------------------------------------- */

struct shm_buffer { struct wl_buffer *buffer; uint32_t *px; int w, h; size_t size; bool busy; unsigned generation; };

static struct
{
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct xdg_activation_v1 *activation;
    struct wp_cursor_shape_manager_v1 *cursor_manager;
    struct wp_cursor_shape_device_v1 *cursor;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *fractional;
    struct wl_callback *frame;
    struct shm_buffer buffers[2];
    uint32_t compositor_version, serial;
    int scale120, buffer_scale;
    bool configured, activated;
} wl = {.buffer_scale = 1};

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    ((struct shm_buffer *)data)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = {buffer_release};

static bool create_buffer(struct shm_buffer *b, int w, int h)
{
    struct wl_shm_pool *pool;
    int fd;

    if (b->buffer) wl_buffer_destroy(b->buffer);
    if (b->px) munmap(b->px, b->size);
    memset(b, 0, sizeof(*b));
    b->size = (size_t)w * h * 4;
    if ((fd = memfd_create("tuxoffice-splash", MFD_CLOEXEC)) < 0) return false;
    if (ftruncate(fd, b->size) || (b->px = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED)
    {
        b->px = NULL;
        close(fd);
        return false;
    }
    pool = wl_shm_create_pool(wl.shm, fd, b->size);
    b->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->buffer, &buffer_listener, b);
    b->w = w;
    b->h = h;
    return true;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
    wl_callback_destroy(callback);
    wl.frame = NULL;
}
static const struct wl_callback_listener frame_listener = {frame_done};

static void wayland_draw(double now)
{
    double scale = wl.scale120 && wl.viewport ? wl.scale120 / 120.0 : wl.buffer_scale;
    int w = lround(WIDTH * scale), h = lround(HEIGHT * scale), i, top, bottom;
    struct shm_buffer *b = NULL;

    for (i = 0; i < 2 && !b; i++)
        if (!wl.buffers[i].busy) b = &wl.buffers[i];
    if (!b) return;
    if ((b->w != w || b->h != h) && !create_buffer(b, w, h)) return;
    update_base(scale, true);
    /* Each buffer is redrawn whole when the base changed since its last frame. */
    render(b->px, w, h, scale, now, b->generation != base.generation, &top, &bottom);
    b->generation = base.generation;
    wl_surface_attach(wl.surface, b->buffer, 0, 0);
    if (wl.viewport) wp_viewport_set_destination(wl.viewport, WIDTH, HEIGHT);
    else wl_surface_set_buffer_scale(wl.surface, wl.buffer_scale);
    wl_surface_damage_buffer(wl.surface, 0, top, w, bottom - top);
    if (animating())
    {
        wl.frame = wl_surface_frame(wl.surface);
        wl_callback_add_listener(wl.frame, &frame_listener, NULL);
    }
    b->busy = true;
    wl_surface_commit(wl.surface);
    if (!wl.activated)
    {
        const char *token = getenv("XDG_ACTIVATION_TOKEN");
        /* The desktop's launch feedback ends when the launched app's window
         * uses the token it was started with. */
        if (wl.activation && token && *token) xdg_activation_v1_activate(wl.activation, token, wl.surface);
        wl.activated = true;
    }
}

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {wm_base_ping};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
    wl.configured = true;
    app.dirty = true;
}
static const struct xdg_surface_listener xdg_surface_listener = {xdg_surface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h,
                               struct wl_array *states) {}
static void toplevel_close(void *data, struct xdg_toplevel *toplevel) { app.quit = true; }
static void toplevel_bounds(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h) {}
static void toplevel_capabilities(void *data, struct xdg_toplevel *toplevel, struct wl_array *caps) {}
static const struct xdg_toplevel_listener toplevel_listener =
{
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_capabilities,
};

static void preferred_scale(void *data, struct wp_fractional_scale_v1 *fractional, uint32_t scale)
{
    if ((int)scale != wl.scale120) app.dirty = true;
    wl.scale120 = scale;
}
static const struct wp_fractional_scale_v1_listener fractional_listener = {preferred_scale};

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output) {}
static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output) {}
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
static void surface_buffer_scale(void *data, struct wl_surface *surface, int32_t scale)
{
    if (scale != wl.buffer_scale && scale > 0) app.dirty = true;
    if (scale > 0) wl.buffer_scale = scale;
}
static void surface_transform(void *data, struct wl_surface *surface, uint32_t transform) {}
#endif
static const struct wl_surface_listener surface_listener =
{
    .enter = surface_enter,
    .leave = surface_leave,
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
    .preferred_buffer_scale = surface_buffer_scale,
    .preferred_buffer_transform = surface_transform,
#endif
};

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t x, wl_fixed_t y)
{
    if (wl.cursor) wp_cursor_shape_device_v1_set_shape(wl.cursor, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {}
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y) {}
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    if (button != BUTTON_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED) return;
    if (app.error) click();
    else xdg_toplevel_move(wl.toplevel, wl.seat, serial); /* drag it out of the way */
}
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {}
static void pointer_frame(void *data, struct wl_pointer *pointer) {}
static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) {}
static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {}
static const struct wl_pointer_listener pointer_listener =
{
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl.pointer)
    {
        wl.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl.pointer, &pointer_listener, NULL);
        if (wl.cursor_manager) wl.cursor = wp_cursor_shape_manager_v1_get_pointer(wl.cursor_manager, wl.pointer);
    }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) {}
static const struct wl_seat_listener seat_listener = {seat_capabilities, seat_name};

static uint32_t min_version(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                            uint32_t version)
{
    if (!strcmp(interface, wl_compositor_interface.name))
    {
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
        wl.compositor_version = min_version(version, 6);
#else
        wl.compositor_version = min_version(version, 4);
#endif
        wl.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, wl.compositor_version);
    }
    else if (!strcmp(interface, wl_shm_interface.name))
        wl.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, xdg_wm_base_interface.name))
    {
        wl.wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, min_version(version, 5));
        xdg_wm_base_add_listener(wl.wm_base, &wm_base_listener, NULL);
    }
    else if (!strcmp(interface, wl_seat_interface.name) && !wl.seat)
    {
        wl.seat = wl_registry_bind(registry, name, &wl_seat_interface, min_version(version, 5));
        wl_seat_add_listener(wl.seat, &seat_listener, NULL);
    }
    else if (!strcmp(interface, wp_viewporter_interface.name))
        wl.viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name))
        wl.fractional_manager = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    else if (!strcmp(interface, xdg_activation_v1_interface.name))
        wl.activation = wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1);
    else if (!strcmp(interface, wp_cursor_shape_manager_v1_interface.name))
        wl.cursor_manager = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
}
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {registry_global, registry_remove};

static bool run_wayland(void)
{
    struct wl_registry *registry;
    double now;

    if (!(wl.display = wl_display_connect(NULL))) return false;
    registry = wl_display_get_registry(wl.display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(wl.display);
    wl_display_roundtrip(wl.display); /* the seat's capabilities */
    if (!wl.compositor || !wl.shm || !wl.wm_base)
    {
        wl_display_disconnect(wl.display);
        return false;
    }
    wl.surface = wl_compositor_create_surface(wl.compositor);
    wl_surface_add_listener(wl.surface, &surface_listener, NULL);
    if (wl.viewporter) wl.viewport = wp_viewporter_get_viewport(wl.viewporter, wl.surface);
    if (wl.viewport && wl.fractional_manager)
    {
        wl.fractional = wp_fractional_scale_manager_v1_get_fractional_scale(wl.fractional_manager, wl.surface);
        wp_fractional_scale_v1_add_listener(wl.fractional, &fractional_listener, NULL);
    }
    wl.xdg_surface = xdg_wm_base_get_xdg_surface(wl.wm_base, wl.surface);
    xdg_surface_add_listener(wl.xdg_surface, &xdg_surface_listener, NULL);
    wl.toplevel = xdg_surface_get_toplevel(wl.xdg_surface);
    xdg_toplevel_add_listener(wl.toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(wl.toplevel, app.name);
    if (app.app_id) xdg_toplevel_set_app_id(wl.toplevel, app.app_id);
    xdg_toplevel_set_min_size(wl.toplevel, WIDTH, HEIGHT);
    xdg_toplevel_set_max_size(wl.toplevel, WIDTH, HEIGHT);
    wl_surface_commit(wl.surface);

    while (!app.quit)
    {
        struct pollfd fds[2] = {{wl_display_get_fd(wl.display), POLLIN}, {app.pidfd, POLLIN}};
        int count;

        now = now_seconds();
        tick(now);
        if (app.quit) break;
        if (wl.configured && !wl.frame && (app.dirty || animating())) wayland_draw(now);
        while (wl_display_prepare_read(wl.display))
            if (wl_display_dispatch_pending(wl.display) < 0) return true;
        if (wl_display_flush(wl.display) < 0 && errno != EAGAIN)
        {
            wl_display_cancel_read(wl.display);
            break;
        }
        count = poll(fds, app.pidfd >= 0 ? 2 : 1, wl.frame || !animating() ? 50 : 16);
        if (count > 0 && (fds[0].revents & POLLIN))
        {
            if (wl_display_read_events(wl.display) < 0) break;
        }
        else
            wl_display_cancel_read(wl.display);
        if (count > 0 && (fds[0].revents & (POLLERR | POLLHUP))) break;
        if (count > 0 && app.pidfd >= 0 && (fds[1].revents & (POLLIN | POLLHUP))) watched_exited();
        if (wl_display_dispatch_pending(wl.display) < 0) break;
    }
    wl_display_disconnect(wl.display);
    return true;
}

/* ---- X11 -------------------------------------------------------------- */

#ifdef HAVE_X11
#define X11_FUNCTIONS \
    X(XOpenDisplay) X(XCloseDisplay) X(XMatchVisualInfo) X(XCreateColormap) X(XCreateWindow) \
    X(XInternAtom) X(XChangeProperty) X(XMapWindow) X(XCreateImage) X(XPutImage) X(XCreateGC) \
    X(XPending) X(XNextEvent) X(XFlush) X(XGetDefault) X(XGetSelectionOwner) X(XSetWMProtocols) \
    X(XSendEvent) X(XUngrabPointer)
#define X(name) static __typeof__(name) *p##name;
X11_FUNCTIONS
#undef X

static bool run_x11(void)
{
    void *lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    XSetWindowAttributes attrs = {0};
    XVisualInfo info;
    Display *display;
    Window root, window;
    Visual *visual;
    XImage *image;
    Atom wm_delete, type;
    GC gc;
    const char *dpi;
    double scale = 1, now, last_frame = 0;
    uint32_t *px;
    long hints[18] = {0}, motif[5] = {2, 0, 0, 0, 0}, pid = getpid();
    char class_hint[256], selection[32];
    int screen, depth, w, h, top, bottom, class_len;
    bool rounded = false, mapped = false;

    if (!lib) return false;
#define X(name) if (!(p##name = (__typeof__(name) *)dlsym(lib, #name))) return false;
    X11_FUNCTIONS
#undef X
    if (!(display = pXOpenDisplay(NULL))) return false;
    screen = DefaultScreen(display);
    root = RootWindow(display, screen);
    if ((dpi = pXGetDefault(display, "Xft", "dpi")) && atof(dpi) >= 96) scale = atof(dpi) / 96;
    w = lround(WIDTH * scale);
    h = lround(HEIGHT * scale);
    /* Round corners need an ARGB visual and a compositing manager. */
    snprintf(selection, sizeof(selection), "_NET_WM_CM_S%d", screen);
    if (pXMatchVisualInfo(display, screen, 32, TrueColor, &info) &&
        pXGetSelectionOwner(display, pXInternAtom(display, selection, False)))
    {
        visual = info.visual;
        depth = 32;
        rounded = true;
    }
    else
    {
        visual = DefaultVisual(display, screen);
        depth = DefaultDepth(display, screen);
    }
    attrs.colormap = pXCreateColormap(display, root, visual, AllocNone);
    attrs.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask;
    window = pXCreateWindow(display, root, (DisplayWidth(display, screen) - w) / 2,
                            (DisplayHeight(display, screen) - h) / 2, w, h, 0, depth, InputOutput, visual,
                            CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &attrs);
    type = pXInternAtom(display, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    pXChangeProperty(display, window, pXInternAtom(display, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32,
                     PropModeReplace, (unsigned char *)&type, 1);
    pXChangeProperty(display, window, pXInternAtom(display, "_NET_WM_NAME", False),
                     pXInternAtom(display, "UTF8_STRING", False), 8, PropModeReplace,
                     (unsigned char *)app.name, strlen(app.name));
    class_len = snprintf(class_hint, sizeof(class_hint), "%s%cTuxOffice", app.app_id ? app.app_id : "tuxoffice", 0);
    pXChangeProperty(display, window, XA_WM_CLASS, XA_STRING, 8, PropModeReplace, (unsigned char *)class_hint,
                     class_len + 1);
    pXChangeProperty(display, window, pXInternAtom(display, "_MOTIF_WM_HINTS", False),
                     pXInternAtom(display, "_MOTIF_WM_HINTS", False), 32, PropModeReplace, (unsigned char *)motif, 5);
    pXChangeProperty(display, window, pXInternAtom(display, "_NET_WM_PID", False), XA_CARDINAL, 32,
                     PropModeReplace, (unsigned char *)&pid, 1);
    hints[0] = PMinSize | PMaxSize;
    hints[5] = hints[7] = w;
    hints[6] = hints[8] = h;
    pXChangeProperty(display, window, XA_WM_NORMAL_HINTS, XA_WM_SIZE_HINTS, 32, PropModeReplace,
                     (unsigned char *)hints, 18);
    wm_delete = pXInternAtom(display, "WM_DELETE_WINDOW", False);
    pXSetWMProtocols(display, window, &wm_delete, 1);
    if (!(px = calloc((size_t)w * h, 4))) return false;
    image = pXCreateImage(display, visual, depth, ZPixmap, 0, (char *)px, w, h, 32, w * 4);
    gc = pXCreateGC(display, window, 0, NULL);
    pXMapWindow(display, window);
    pXFlush(display);

    while (!app.quit)
    {
        struct pollfd fds[2] = {{ConnectionNumber(display), POLLIN}, {app.pidfd, POLLIN}};
        bool full = false;

        while (pXPending(display))
        {
            XEvent event;
            pXNextEvent(display, &event);
            if (event.type == MapNotify) mapped = full = true;
            else if (event.type == Expose) full = true;
            else if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == wm_delete) app.quit = true;
            else if (event.type == ButtonPress && event.xbutton.button == Button1)
            {
                if (app.error) click();
                else
                {
                    /* Let the window manager move the window. */
                    XEvent move = {.xclient = {.type = ClientMessage, .window = window, .format = 32,
                                   .message_type = pXInternAtom(display, "_NET_WM_MOVERESIZE", False),
                                   .data.l = {event.xbutton.x_root, event.xbutton.y_root, 8, Button1, 1}}};
                    pXUngrabPointer(display, CurrentTime);
                    pXSendEvent(display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &move);
                }
            }
        }
        now = now_seconds();
        tick(now);
        if (app.quit) break;
        if (mapped && (full || app.dirty || (animating() && now - last_frame > 1 / 60.0)))
        {
            update_base(scale, rounded);
            render(px, w, h, scale, now, true, &top, &bottom);
            if (!rounded)
                for (top = 0; top < w * h; top++) px[top] |= 0xff000000;
            pXPutImage(display, window, gc, image, 0, 0, 0, 0, w, h);
            pXFlush(display);
            last_frame = now;
        }
        if (poll(fds, app.pidfd >= 0 ? 2 : 1, animating() ? 16 : 50) > 0 && app.pidfd >= 0 &&
            (fds[1].revents & (POLLIN | POLLHUP)))
            watched_exited();
    }
    pXCloseDisplay(display);
    return true;
}
#else
static bool run_x11(void) { return false; }
#endif

/* ---- Main ------------------------------------------------------------- */

static void cleanup(void)
{
    char path[4096];
    if (!app.dir) return;
    snprintf(path, sizeof(path), "%s/status", app.dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/ready", app.dir);
    unlink(path);
    rmdir(app.dir);
}

int main(int argc, char **argv)
{
    const char *watch = NULL;
    char path[4096];
    int i;

    for (i = 1; i < argc; i++)
    {
        const char *opt = argv[i], *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(opt, "--dark")) { app.theme = &dark_theme; continue; }
        if (!value) break;
        i++;
        if (!strcmp(opt, "--dir")) app.dir = value;
        else if (!strcmp(opt, "--watch")) watch = value;
        else if (!strcmp(opt, "--name")) app.name = value;
        else if (!strcmp(opt, "--app-id")) app.app_id = value;
        else if (!strcmp(opt, "--accent")) app.accent = 0xff000000 | strtoul(value, NULL, 16);
        else if (!strcmp(opt, "--logo")) app.logo = value;
        else if (!strcmp(opt, "--version")) app.version = value;
        else if (!strcmp(opt, "--record")) app.record = value;
        else if (!strcmp(opt, "--timeout")) app.timeout = atof(value);
        else
        {
            fprintf(stderr, "tuxoffice-splash: unknown option %s\n", opt);
            return 2;
        }
    }
    if (!app.dir || !app.name)
    {
        fprintf(stderr, "usage: tuxoffice-splash --dir DIR --watch PID --name NAME [options]\n");
        return 2;
    }
    if (!app.theme) app.theme = &light_theme;
    snprintf(app.status, sizeof(app.status), "Starting\xe2\x80\xa6");
    signal(SIGPIPE, SIG_IGN);
    snprintf(path, sizeof(path), "%s/status", app.dir);
    app.status_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (watch && (app.pidfd = syscall(SYS_pidfd_open, atoi(watch), 0)) < 0 && errno == ESRCH)
    {
        cleanup();
        return 0;
    }
    app.start = app.last_tick = now_seconds();
    app.dirty = true;

    if (!FT_Init_FreeType(&freetype) && FcInit())
    {
        title_face = load_face("sans-serif:weight=semibold");
        text_face = load_face("sans-serif");
    }
    logo_source = load_png(app.logo, &logo_width, &logo_height);

    if (!run_wayland()) run_x11();
    cleanup();
    return 0;
}
