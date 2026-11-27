/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A window region span that starts and ends inside one byte of the 1bpp shape
 * mask showed the pixels after it instead of its own: the tip of Office's
 * callout beak was hidden and seven pixels beside it showed. Shows a red popup
 * whose region has one-pixel-wide spikes above it, at every offset within a
 * byte, until the file named on the command line exists. Uses Win32 only. */
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    WNDCLASSW klass = {.lpfnWndProc = DefWindowProcW, .lpszClassName = L"RegionSpan",
                       .hbrBackground = NULL};
    HRGN region, spike;
    HWND hwnd;
    MSG message;
    int i;

    if (argc != 2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    klass.hbrBackground = CreateSolidBrush(RGB(255, 0, 0));
    RegisterClassW(&klass);
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, klass.lpszClassName, L"Region span regression", WS_POPUP,
            0, 0, 200, 60, NULL, NULL, NULL, NULL);
    /* a body below row 20, and above it a one pixel column at x = 16 * i + i
     * for i in 0..7: offsets 0 to 7 within their byte */
    region = CreateRectRgn(0, 20, 200, 60);
    for (i = 0; i < 8; i++)
    {
        spike = CreateRectRgn(16 * i + i, 0, 16 * i + i + 1, 20);
        CombineRgn(region, region, spike, RGN_OR);
        DeleteObject(spike);
        printf("spike %d\n", 16 * i + i);
    }
    SetWindowRgn(hwnd, region, FALSE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    puts("READY region span");
    fflush(stdout);
    while (GetFileAttributesA(argv[1]) == INVALID_FILE_ATTRIBUTES)
    {
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(50);
    }
    DestroyWindow(hwnd);
    return 0;
}
