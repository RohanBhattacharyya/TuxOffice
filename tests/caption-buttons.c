/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Check the caption buttons Wine draws for ordinary windows and dialogs.
 *
 * Usage: caption-buttons.exe OUTDIR
 * Creates a system-DPI-aware overlapped window, measures each caption button
 * by hit testing along the caption, drives the hover state with the
 * non-client mouse messages Windows sends and saves the window's frame as
 * BMP files. Prints one "ok"/"FAIL" line per check.
 */
#include <windows.h>
#include <stdio.h>

static int failures;
static const char *outdir;

static void check( BOOL ok, const char *what, int value, int expected )
{
    printf( "%s %s: %d (expected %d)\n", ok ? "ok" : "FAIL", what, value, expected );
    if (!ok) failures++;
}

static void pump(void)
{
    MSG msg;
    DWORD end = GetTickCount() + 300;

    while (GetTickCount() < end)
    {
        while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE ))
        {
            /* Leave messages from the real pointer would clear the hover state. */
            if (msg.message == WM_NCMOUSELEAVE || msg.message == WM_MOUSELEAVE) continue;
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
        Sleep( 10 );
    }
}

/* Copies the window, frame included, from its window DC. */
static COLORREF *grab( HWND hwnd, const char *name, int *width, int *height )
{
    BITMAPINFO info = {{ sizeof(BITMAPINFOHEADER) }};
    BITMAPFILEHEADER file = { 0x4d42 };
    COLORREF *bits;
    HBITMAP bitmap;
    HDC hdc, mem;
    char path[MAX_PATH];
    RECT rect;
    FILE *out;

    GetWindowRect( hwnd, &rect );
    *width = rect.right - rect.left;
    *height = rect.bottom - rect.top;
    info.bmiHeader.biWidth = *width;
    info.bmiHeader.biHeight = -*height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    hdc = GetWindowDC( hwnd );
    mem = CreateCompatibleDC( hdc );
    bitmap = CreateDIBSection( mem, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 );
    SelectObject( mem, bitmap );
    BitBlt( mem, 0, 0, *width, *height, hdc, 0, 0, SRCCOPY );
    ReleaseDC( hwnd, hdc );
    GdiFlush();

    snprintf( path, sizeof(path), "%s\\%s.bmp", outdir, name );
    if ((out = fopen( path, "wb" )))
    {
        DWORD size = *width * *height * 4;
        file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
        file.bfSize = file.bfOffBits + size;
        fwrite( &file, sizeof(file), 1, out );
        fwrite( &info.bmiHeader, sizeof(info.bmiHeader), 1, out );
        fwrite( bits, size, 1, out );
        fclose( out );
    }
    {
        COLORREF *copy = malloc( *width * *height * 4 );
        memcpy( copy, bits, *width * *height * 4 );
        DeleteDC( mem );
        DeleteObject( bitmap );
        return copy;
    }
}

/* DIB pixels are 0x00RRGGBB. */
static BOOL is_red( COLORREF pixel )
{
    return ((pixel >> 16) & 0xff) > 0xb0 && ((pixel >> 8) & 0xff) < 0x60 && (pixel & 0xff) < 0x60;
}

static int lightness( COLORREF pixel )
{
    return ((pixel >> 16) & 0xff) + ((pixel >> 8) & 0xff) + (pixel & 0xff);
}

int main( int argc, char **argv )
{
    WNDCLASSW cls = { 0 };
    int width, height, x, y, dpi, expected, i, close_left = 0, close_right = 0;
    int runs[3] = { 0 }, centres[3] = { 0 }, frame_x, frame_y;
    static const LRESULT hits[3] = { HTCLOSE, HTMAXBUTTON, HTMINBUTTON };
    static const char *names[3] = { "close", "maximize", "minimize" };
    COLORREF *pixels;
    HWND hwnd;
    RECT rect;

    outdir = argc > 1 ? argv[1] : ".";
    SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_SYSTEM_AWARE );
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW( NULL );
    cls.hbrBackground = GetSysColorBrush( COLOR_WINDOW );
    cls.lpszClassName = L"TuxOfficeCaptionProbe";
    cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    RegisterClassW( &cls );
    hwnd = CreateWindowExW( 0, cls.lpszClassName, L"Caption probe", WS_OVERLAPPEDWINDOW,
                            200, 200, 640, 320, NULL, NULL, cls.hInstance, NULL );
    ShowWindow( hwnd, SW_SHOWNORMAL );
    SetForegroundWindow( hwnd );
    UpdateWindow( hwnd );
    pump();

    dpi = GetDpiForWindow( hwnd );
    expected = MulDiv( 46, dpi, 96 );
    GetWindowRect( hwnd, &rect );
    frame_x = GetSystemMetrics( SM_CXFRAME );
    frame_y = GetSystemMetrics( SM_CYFRAME );
    y = rect.top + frame_y + GetSystemMetrics( SM_CYCAPTION ) / 2;
    for (x = rect.right - 1; x >= rect.left; x--)
    {
        LRESULT hit = SendMessageW( hwnd, WM_NCHITTEST, 0, MAKELPARAM( x, y ));
        for (i = 0; i < 3; i++)
        {
            if (hit != hits[i]) continue;
            runs[i]++;
            centres[i] = x;
            if (i == 0)
            {
                if (!close_right) close_right = x;
                close_left = x;
            }
        }
    }
    printf( "dpi %d caption %d frame %d,%d\n", dpi, GetSystemMetrics( SM_CYCAPTION ), frame_x, frame_y );
    for (i = 0; i < 3; i++)
    {
        char what[64];
        snprintf( what, sizeof(what), "%s button width", names[i] );
        check( runs[i] == expected, what, runs[i], expected );
        centres[i] += runs[i] / 2;
    }

    pixels = grab( hwnd, "normal", &width, &height );
    /* A point inside the close button, away from its glyph. */
    x = close_left - rect.left + 3;
    y = frame_y + 3;
    check( lightness( pixels[y * width + x] ) > 700, "close button background is light", lightness( pixels[y * width + x] ), 765 );
    free( pixels );

    SendMessageW( hwnd, WM_NCMOUSEMOVE, HTCLOSE, MAKELPARAM( centres[0], rect.top + frame_y + 4 ));
    pixels = grab( hwnd, "close-hot", &width, &height );
    check( is_red( pixels[y * width + x] ), "hovered close button is red", pixels[y * width + x], 0xc42b1c );
    free( pixels );

    SendMessageW( hwnd, WM_NCMOUSEMOVE, HTMAXBUTTON, MAKELPARAM( centres[1], rect.top + frame_y + 4 ));
    pixels = grab( hwnd, "maximize-hot", &width, &height );
    check( !is_red( pixels[y * width + x] ), "close button returns to normal", pixels[y * width + x], 0xffffff );
    i = y * width + (centres[1] - rect.left - runs[1] / 2 + 3);
    check( lightness( pixels[i] ) < 700 && lightness( pixels[i] ) > 600, "hovered maximize button is grey",
           lightness( pixels[i] ), 690 );
    free( pixels );

    SendMessageW( hwnd, WM_NCMOUSELEAVE, 0, 0 );
    pixels = grab( hwnd, "left", &width, &height );
    check( lightness( pixels[i] ) > 700, "maximize button returns to normal", lightness( pixels[i] ), 765 );
    free( pixels );

    ShowWindow( hwnd, SW_MAXIMIZE );
    pump();
    free( grab( hwnd, "maximized", &width, &height ));
    DestroyWindow( hwnd );
    printf( "%s\n", failures ? "FAILED" : "PASSED" );
    return failures != 0;
}
