/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A child window owned by another process, as WebView2's browser window in
 * Office, must show what that process paints with GDI. The parent process
 * creates a toplevel; a second instance of this program creates a child of
 * it, paints three colored bands and, in a third window, keeps repainting a
 * counter. The parent reads its own window back through a window DC.
 * Uses Win32 only. Exit code 0 means every check passed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static const COLORREF bands[3] = {RGB(204, 0, 0), RGB(0, 204, 0), RGB(0, 0, 204)};
static const COLORREF frames[3] = {RGB(255, 255, 0), RGB(0, 255, 255), RGB(255, 0, 255)};
static int frame;

static LRESULT CALLBACK child_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect;
        int i;

        GetClientRect(hwnd, &rect);
        for (i = 0; i < 3; i++)
        {
            RECT band = {rect.right * i / 3, 0, rect.right * (i + 1) / 3, rect.bottom};
            HBRUSH brush = CreateSolidBrush(bands[i]);
            FillRect(hdc, &band, brush);
            DeleteObject(brush);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* Draw outside WM_PAINT with GetDC, as Chromium's software output does. */
static void draw_frame(HWND hwnd)
{
    HDC hdc = GetDC(hwnd);
    HBRUSH brush = CreateSolidBrush(frames[frame % 3]);
    RECT rect;

    GetClientRect(hwnd, &rect);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
    ReleaseDC(hwnd, hdc);
    frame++;
}

static int run_child(HWND parent)
{
    WNDCLASSW klass = {.lpfnWndProc = child_proc, .lpszClassName = L"CrossProcessChild"};
    HWND child, animated;
    MSG msg;
    DWORD end;

    RegisterClassW(&klass);
    child = CreateWindowW(klass.lpszClassName, NULL, WS_CHILD | WS_VISIBLE, 20, 20, 300, 100,
                          parent, NULL, NULL, NULL);
    animated = CreateWindowW(L"Static", NULL, WS_CHILD | WS_VISIBLE, 20, 140, 100, 50,
                             parent, NULL, NULL, NULL);
    if (!child || !animated) return 1;
    end = GetTickCount() + 30000;
    while ((int)(end - GetTickCount()) > 0 && IsWindow(parent))
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        draw_frame(animated);
        Sleep(50);
    }
    return 0;
}

/* Read the toplevel's own pixels, not clipped by the children. */
static COLORREF read_pixel(HWND hwnd, int x, int y)
{
    HDC hdc = GetDCEx(hwnd, 0, DCX_CACHE);
    COLORREF color = GetPixel(hdc, x, y);
    ReleaseDC(hwnd, hdc);
    return color;
}

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG msg;

    while ((int)(end - GetTickCount()) > 0)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(10);
    }
}

static BOOL close_to(COLORREF a, COLORREF b)
{
    return abs(GetRValue(a) - GetRValue(b)) < 8 && abs(GetGValue(a) - GetGValue(b)) < 8 &&
           abs(GetBValue(a) - GetBValue(b)) < 8;
}

int main(int argc, char **argv)
{
    WNDCLASSW klass = {.lpfnWndProc = DefWindowProcW, .lpszClassName = L"CrossProcessParent",
                       .hbrBackground = GetStockObject(WHITE_BRUSH)};
    PROCESS_INFORMATION pi;
    STARTUPINFOA si = {sizeof(si)};
    char cmdline[MAX_PATH + 64], path[MAX_PATH];
    COLORREF first, later;
    int failures = 0, i, changes = 0;
    HWND parent;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc == 3 && !strcmp(argv[1], "child")) return run_child((HWND)(ULONG_PTR)strtoull(argv[2], NULL, 16));

    RegisterClassW(&klass);
    parent = CreateWindowW(klass.lpszClassName, L"Cross-process child", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                           100, 100, 400, 260, NULL, NULL, NULL, NULL);
    pump(500);
    GetModuleFileNameA(NULL, path, sizeof(path));
    snprintf(cmdline, sizeof(cmdline), "\"%s\" child %Ix", path, (ULONG_PTR)parent);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        printf("FAIL cannot start the child process\n");
        return 1;
    }
    pump(3000);

    for (i = 0; i < 3; i++)
    {
        COLORREF color = read_pixel(parent, 20 + 50 + i * 100, 20 + 50);
        BOOL ok = close_to(color, bands[i]);
        printf("%s band %d %06lx expected %06lx\n", ok ? "PASS" : "FAIL", i, color, bands[i]);
        failures += !ok;
    }
    first = read_pixel(parent, 70, 165);
    for (i = 0; i < 20; i++)
    {
        pump(100);
        later = read_pixel(parent, 70, 165);
        if (later != first && (close_to(later, frames[0]) || close_to(later, frames[1]) || close_to(later, frames[2])))
            changes++;
        first = later;
    }
    printf("%s repaints from GetDC drawing: %d changes in 20 samples\n", changes >= 10 ? "PASS" : "FAIL", changes);
    failures += changes < 10;

    DestroyWindow(parent);
    WaitForSingleObject(pi.hProcess, 5000);
    printf("RESULT %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
