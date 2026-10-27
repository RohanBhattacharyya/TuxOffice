/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Configure Office's window effects through the Windows preference API. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void report_fonts(const NONCLIENTMETRICSW *metrics)
{
    printf("UI DPI: %u; menu: %ls height=%ld; message: %ls height=%ld\n",
            GetDpiForSystem(), metrics->lfMenuFont.lfFaceName, metrics->lfMenuFont.lfHeight,
            metrics->lfMessageFont.lfFaceName, metrics->lfMessageFont.lfHeight);
}

static int configure_fonts(void)
{
    NONCLIENTMETRICSW metrics = {.cbSize = sizeof(metrics)};
    LOGFONTW candidate = {0};
    WCHAR actual[LF_FACESIZE] = {0};
    HDC dc;
    HFONT font, previous;
    LOGFONTW *fonts[5];
    OUTLINETEXTMETRICW *otm = NULL;
    unsigned int i, size;

    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) return 1;
    report_fonts(&metrics);
    candidate.lfHeight = -MulDiv(9, GetDpiForSystem(), 72);
    wcscpy(candidate.lfFaceName, L"Segoe UI");
    dc = CreateCompatibleDC(NULL);
    font = CreateFontIndirectW(&candidate);
    if (!dc || !font)
    {
        if (dc) DeleteDC(dc);
        if (font) DeleteObject(font);
        return 1;
    }
    previous = SelectObject(dc, font);
    /* The full name in the font file: GetTextFaceW reports the requested
     * name when a FontSubstitutes entry (Segoe UI -> Selawik) supplies the
     * font, and Segoe UI Semilight has the family name Segoe UI too. */
    if ((size = GetOutlineTextMetricsW(dc, 0, NULL)) && (otm = malloc(size)) &&
        GetOutlineTextMetricsW(dc, size, otm))
        lstrcpynW(actual, (const WCHAR *)((const char *)otm + (UINT_PTR)otm->otmpFaceName), LF_FACESIZE);
    free(otm);
    SelectObject(dc, previous);
    DeleteObject(font);
    DeleteDC(dc);
    printf("Segoe UI resolves to %ls.\n", actual);
    /* Office installs Segoe UI Semilight, which Wine picks for the name;
     * only the regular face or Windows 11's Segoe UI Variable will do. */
    if (wcsicmp(actual, L"Segoe UI") && wcsnicmp(actual, L"Segoe UI Variable", 17))
    {
        printf("Keeping UI fonts until real Segoe UI is installed.\n");
        return 0;
    }
    wcscpy(candidate.lfFaceName, actual);
    fonts[0] = &metrics.lfMenuFont;
    fonts[1] = &metrics.lfMessageFont;
    fonts[2] = &metrics.lfStatusFont;
    fonts[3] = &metrics.lfCaptionFont;
    fonts[4] = &metrics.lfSmCaptionFont;
    /* Windows 10 and 11 use regular-weight Segoe UI 9pt for all of these,
     * including captions, which older defaults made bold. */
    for (i = 0; i < 5; ++i)
    {
        fonts[i]->lfHeight = candidate.lfHeight;
        fonts[i]->lfWidth = 0;
        fonts[i]->lfWeight = FW_NORMAL;
        wcscpy(fonts[i]->lfFaceName, candidate.lfFaceName);
    }
    /* Apply only to this Wine prefix. New app processes load the metrics;
     * broadcasting to Office helper threads can block indefinitely. */
    if (!SystemParametersInfoW(SPI_SETNONCLIENTMETRICS, sizeof(metrics), &metrics,
            SPIF_UPDATEINIFILE)) return 1;
    {
        LOGFONTW icon = candidate;
        icon.lfWeight = FW_NORMAL;
        if (!SystemParametersInfoW(SPI_SETICONTITLELOGFONT, sizeof(icon), &icon, SPIF_UPDATEINIFILE)) return 1;
    }
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) return 1;
    report_fonts(&metrics);
    return wcscmp(metrics.lfMenuFont.lfFaceName, candidate.lfFaceName) ||
            metrics.lfMenuFont.lfHeight != candidate.lfHeight;
}

int main(int argc, char **argv)
{
    BOOL enabled;

    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off") && strcmp(argv[1], "status") && strcmp(argv[1], "fonts")))
    {
        fprintf(stderr, "usage: office-ui.exe on|off|status|fonts\n");
        return 2;
    }
    if (!strcmp(argv[1], "fonts")) return configure_fonts();
    if (strcmp(argv[1], "status") && !SystemParametersInfoW(SPI_SETDROPSHADOW, 0,
            (void *)(ULONG_PTR)!strcmp(argv[1], "on"), SPIF_UPDATEINIFILE))
    {
        fprintf(stderr, "Could not set window shadows: %lu\n", GetLastError());
        return 1;
    }
    if (!SystemParametersInfoW(SPI_GETDROPSHADOW, 0, &enabled, 0)) return 1;
    printf("Window shadows: %s\n", enabled ? "on" : "off");
    if (!strcmp(argv[1], "status"))
    {
        NONCLIENTMETRICSW metrics = {.cbSize = sizeof(metrics)};
        if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) return 1;
        report_fonts(&metrics);
    }
    return 0;
}
