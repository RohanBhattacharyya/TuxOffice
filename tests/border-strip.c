/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A frame window plus an Office-style border-effect strip on its right edge,
 * drawn once with UpdateLayeredWindow before it is shown, as Office draws its
 * dialog shadows: a red opaque top half, a half-transparent black bottom half.
 * Checked by border-strip.py. Usage: border-strip.exe [seconds [inset [plain]]] */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char **argv)
{
    WNDCLASSW wc = {0};
    HWND frame, strip;
    int seconds = argc > 1 ? atoi(argv[1]) : 15, x = 600, y = 300, w = 600, h = 400, sw = 16, i, j;
    BITMAPINFO bi = {{sizeof(bi.bmiHeader), sw, -h, 1, 32, BI_RGB}};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    POINT src = {0, 0}, pos = {x + w - (argc > 2 ? atoi(argv[2]) : 0), y};
    SIZE size = {sw, h};
    DWORD *bits;
    HDC dc;
    HBITMAP bmp;
    MSG msg;
    DWORD end;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = GetStockObject(WHITE_BRUSH); wc.lpszClassName = L"ShadowTestFrame";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    wc.lpszClassName = argc > 3 ? L"PlainLayeredStrip" : L"MSO_BORDEREFFECT_WINDOW_CLASS"; wc.hbrBackground = NULL;
    RegisterClassW(&wc);

    frame = CreateWindowExW(0, L"ShadowTestFrame", L"shadow test", WS_POPUP | WS_VISIBLE, x, y, w, h, NULL, NULL, NULL, NULL);
    end = GetTickCount() + 1500;
    while (GetTickCount() < end)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(10);
    }
    strip = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                            wc.lpszClassName, L"", WS_POPUP, pos.x, pos.y, sw, h, NULL, NULL, NULL, NULL);
    dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    SelectObject(dc, bmp);
    for (j = 0; j < h; j++)
        for (i = 0; i < sw; i++)
            bits[j * sw + i] = j < h / 2 ? 0xffff0000 : 0x80000000;
    printf("ulw %d\n", UpdateLayeredWindow(strip, NULL, &pos, &size, dc, &src, 0, &blend, ULW_ALPHA));
    ShowWindow(strip, SW_SHOWNOACTIVATE);
    SetWindowPos(strip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    fflush(stdout);
    end = GetTickCount() + seconds * 1000;
    while (GetTickCount() < end)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(10);
    }
    return 0;
}
