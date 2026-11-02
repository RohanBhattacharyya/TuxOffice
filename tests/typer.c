/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Inject keyboard/mouse input into the foreground window via SendInput, for
 * rendering and latency tests inside the Office prefix.
 *
 *   typer.exe type "text" [delay_ms]   type text as Unicode key events
 *   typer.exe key VK                   press and release a virtual key (hex or decimal)
 *   typer.exe wheel CLICKS             mouse wheel (positive = up)
 *   typer.exe move X Y                 move the cursor (absolute, virtual desktop pixels)
 *   typer.exe hover X Y                move there with a real mouse-move input event
 *   typer.exe click                    left click at the current position
 *   typer.exe rclick                   right click at the current position
 *   typer.exe fg                       print the foreground window title
 *   typer.exe ping [ms]                is Word's UI thread answering? (WM_NULL to OpusApp)
 *   typer.exe lclick                   left click at the current position
 *   typer.exe ribbon-click-bench N MS TAB...  click the named ribbon tabs in turn
 *
 * Build: winegcc -mconsole -o typer.exe typer.c -luser32 -loleacc -lole32 -loleaut32
 */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <initguid.h>
#include <oleacc.h>
#include "word-window.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static void send_one(INPUT *in)
{
    if (SendInput(1, in, sizeof(*in)) != 1)
        fprintf(stderr, "SendInput failed: %lu\n", GetLastError());
}

static BOOL target_has_focus(void)
{
    DWORD target = 0, foreground = 0;
    GetWindowThreadProcessId(word_window(), &target);
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    if (target && target == foreground) return TRUE;
    fprintf(stderr, "Target Office app is not the foreground application\n");
    return FALSE;
}

static void type_text(const char *text, DWORD delay, BOOL benchmark)
{
    WCHAR buf[4096];
    int i, n = MultiByteToWideChar(CP_UTF8, 0, text, -1, buf, ARRAY_SIZE(buf)) - 1;

    for (i = 0; i < n; i++)
    {
        INPUT in = {0};
        if (benchmark)
        {
            if (!target_has_focus()) return;
            printf("type index=%d tick=%llu\n", i, GetTickCount64());
            fflush(stdout);
        }
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = buf[i];
        in.ki.dwFlags = KEYEVENTF_UNICODE;
        send_one(&in);
        in.ki.dwFlags |= KEYEVENTF_KEYUP;
        send_one(&in);
        Sleep(delay);
    }
}

static void key(WORD vk)
{
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    send_one(&in);
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    send_one(&in);
}

static void mouse(DWORD flags, LONG data)
{
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    send_one(&in);
}

/* Find a ribbon tab by its accessible name, as a user would see it. Keyboard
 * tab switching needs KeyTips; a missed Alt then types into the document. */
static BOOL find_tab(IAccessible *acc, const WCHAR *name, int depth, RECT *rect)
{
    VARIANT children[64], self = {.vt = VT_I4, .lVal = CHILDID_SELF}, role;
    LONG count = 0, obtained = 0, i, x, y, w, h;
    IAccessible *child;
    BSTR text = NULL;
    BOOL found = FALSE;

    if (depth > 14 || FAILED(IAccessible_get_accChildCount(acc, &count)) || !count) return FALSE;
    if (FAILED(AccessibleChildren(acc, 0, min(count, ARRAY_SIZE(children)), children, &obtained))) return FALSE;
    for (i = 0; i < obtained; ++i)
    {
        if (!found && children[i].vt == VT_DISPATCH && children[i].pdispVal &&
            SUCCEEDED(IDispatch_QueryInterface(children[i].pdispVal, &IID_IAccessible, (void **)&child)))
        {
            if (SUCCEEDED(IAccessible_get_accRole(child, self, &role)) && role.vt == VT_I4 &&
                role.lVal == ROLE_SYSTEM_PAGETAB && IAccessible_get_accName(child, self, &text) == S_OK &&
                text && !wcscmp(text, name) &&
                SUCCEEDED(IAccessible_accLocation(child, &x, &y, &w, &h, self)) && w > 0 && h > 0)
            {
                SetRect(rect, x, y, x + w, y + h);
                found = TRUE;
            }
            SysFreeString(text);
            text = NULL;
            if (!found) found = find_tab(child, name, depth + 1, rect);
            IAccessible_Release(child);
        }
        VariantClear(&children[i]);
    }
    return found;
}

/* Print the accessible tree below a NetUI window, for comparing layouts. */
static void dump_accessible(IAccessible *acc, int depth, int max_depth)
{
    VARIANT children[128], self = {.vt = VT_I4, .lVal = CHILDID_SELF}, role;
    LONG count = 0, obtained = 0, i, x, y, w, h;
    IAccessible *child;
    BSTR text;

    if (depth > max_depth || FAILED(IAccessible_get_accChildCount(acc, &count)) || !count) return;
    if (FAILED(AccessibleChildren(acc, 0, min(count, ARRAY_SIZE(children)), children, &obtained))) return;
    for (i = 0; i < obtained; ++i)
    {
        if (children[i].vt == VT_DISPATCH && children[i].pdispVal &&
            SUCCEEDED(IDispatch_QueryInterface(children[i].pdispVal, &IID_IAccessible, (void **)&child)))
        {
            text = NULL;
            role.vt = VT_EMPTY;
            IAccessible_get_accRole(child, self, &role);
            IAccessible_get_accName(child, self, &text);
            x = y = w = h = 0;
            IAccessible_accLocation(child, &x, &y, &w, &h, self);
            if (w > 0 && h > 0)
                printf("acc %d role=%ld rect=%ld,%ld,%ldx%ld name=%ls\n", depth, role.vt == VT_I4 ? role.lVal : -1,
                       x, y, w, h, text ? text : L"");
            SysFreeString(text);
            dump_accessible(child, depth + 1, max_depth);
            IAccessible_Release(child);
        }
        VariantClear(&children[i]);
    }
}

static BOOL CALLBACK dump_netui_window(HWND hwnd, LPARAM depth)
{
    IAccessible *acc;
    WCHAR name[32];
    RECT rect;

    GetClassNameW(hwnd, name, ARRAY_SIZE(name));
    if (wcscmp(name, L"NetUIHWND") || !IsWindowVisible(hwnd)) return TRUE;
    GetWindowRect(hwnd, &rect);
    printf("netui %p rect=%ld,%ld,%ld,%ld\n", hwnd, rect.left, rect.top, rect.right, rect.bottom);
    if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, &IID_IAccessible, (void **)&acc))) return TRUE;
    dump_accessible(acc, 0, depth);
    IAccessible_Release(acc);
    return TRUE;
}

struct tab_search { WCHAR name[64]; RECT rect; BOOL found; };

/* The ribbon is drawn by Office's own NetUIHWND windows; ask them directly. */
static BOOL CALLBACK find_tab_window(HWND hwnd, LPARAM data)
{
    struct tab_search *search = (void *)data;
    IAccessible *acc;
    WCHAR name[32];

    GetClassNameW(hwnd, name, ARRAY_SIZE(name));
    if (wcscmp(name, L"NetUIHWND") || !IsWindowVisible(hwnd)) return TRUE;
    if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, &IID_IAccessible, (void **)&acc))) return TRUE;
    search->found = find_tab(acc, search->name, 0, &search->rect);
    IAccessible_Release(acc);
    return !search->found;
}

static BOOL ribbon_tab_rect(const char *name, RECT *rect)
{
    struct tab_search search = {0};

    MultiByteToWideChar(CP_UTF8, 0, name, -1, search.name, ARRAY_SIZE(search.name));
    EnumChildWindows(word_window(), find_tab_window, (LPARAM)&search);
    *rect = search.rect;
    return search.found;
}

/* A real pointer move: Wayland cannot warp the pointer, so SetCursorPos
 * does not necessarily decide where the next click lands. */
static void move_pointer(int x, int y)
{
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dx = MulDiv(x - GetSystemMetrics(SM_XVIRTUALSCREEN), 65535, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    in.mi.dy = MulDiv(y - GetSystemMetrics(SM_YVIRTUALSCREEN), 65535, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    send_one(&in);
}

static BOOL CALLBACK dump_window(HWND hwnd, LPARAM unused)
{
    WCHAR name[128], title[128];
    RECT rect;
    GetClassNameW(hwnd, name, ARRAY_SIZE(name));
    GetWindowTextW(hwnd, title, ARRAY_SIZE(title));
    GetWindowRect(hwnd, &rect);
    printf("%p parent=%p visible=%d rect=%ld,%ld,%ld,%ld class=%ls title=%ls\n",
            hwnd, GetParent(hwnd), IsWindowVisible(hwnd), rect.left, rect.top, rect.right, rect.bottom, name, title);
    return TRUE;
}

static BOOL CALLBACK dump_word_top_window(HWND hwnd, LPARAM pid)
{
    DWORD owner;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner == (DWORD)pid && IsWindowVisible(hwnd)) dump_window(hwnd, 0);
    return TRUE;
}

struct popup_search { DWORD pid; HWND hwnd; };

static BOOL CALLBACK find_popup(HWND hwnd, LPARAM data)
{
    struct popup_search *search = (void *)data;
    WCHAR name[64];
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid || !IsWindowVisible(hwnd)) return TRUE;
    GetClassNameW(hwnd, name, ARRAY_SIZE(name));
    if (wcscmp(name, L"Net UI Tool Window")) return TRUE;
    search->hwnd = hwnd;
    return FALSE;
}

static int popup_bench(int x, int y, int count)
{
    struct popup_search search = {0};
    ULONGLONG start, visible, deadline;
    INPUT move = {0};
    RECT rect;
    int i;

    GetWindowThreadProcessId(word_window(), &search.pid);
    move.type = INPUT_MOUSE;
    move.mi.dx = MulDiv(x, 65535, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    move.mi.dy = MulDiv(y, 65535, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    for (i = 0; i < count; ++i)
    {
        if (!target_has_focus()) return 5;
        key(VK_ESCAPE);
        Sleep(300);
        search.hwnd = NULL;
        EnumWindows(find_popup, (LPARAM)&search);
        if (search.hwnd) { fprintf(stderr, "Previous popup did not close\n"); return 6; }
        send_one(&move);
        Sleep(100);
        start = GetTickCount64();
        mouse(MOUSEEVENTF_LEFTDOWN, 0);
        mouse(MOUSEEVENTF_LEFTUP, 0);
        deadline = start + 5000;
        do
        {
            EnumWindows(find_popup, (LPARAM)&search);
            if (search.hwnd) break;
            Sleep(5);
        } while (GetTickCount64() < deadline);
        visible = GetTickCount64();
        if (!search.hwnd) { fprintf(stderr, "Popup did not appear\n"); return 7; }
        GetWindowRect(search.hwnd, &rect);
        printf("popup round=%d click=%llu visible=%llu delay=%llu hwnd=%p size=%ldx%ld rect=%ld,%ld,%ld,%ld\n",
                i, start, visible, visible - start, search.hwnd, rect.right-rect.left, rect.bottom-rect.top,
                rect.left,rect.top,rect.right,rect.bottom);
        fflush(stdout);
        Sleep(1000); /* Allow the card's asynchronous content to finish. */
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 3 && !strcmp(argv[1], "--app"))
    {
        if (!strcmp(argv[2], "excel")) office_window_class = L"XLMAIN";
        else if (!strcmp(argv[2], "powerpoint")) office_window_class = L"PPTFrameClass";
        else if (!strcmp(argv[2], "outlook")) office_window_class = L"rctrl_renwnd32";
        else if (!strcmp(argv[2], "onenote")) office_window_class = L"Framework::CFrame";
        else if (!strcmp(argv[2], "access")) office_window_class = L"OMain";
        else if (!strcmp(argv[2], "publisher")) office_window_class = L"MSWinPub";
        else if (strcmp(argv[2], "word")) return 1;
        argc -= 2; argv += 2;
    }
    if (argc < 2) return 1;
    if (!strcmp(argv[1], "type") || !strcmp(argv[1], "type-bench") || !strcmp(argv[1], "key") || !strcmp(argv[1], "wheel") ||
        !strcmp(argv[1], "click") || !strcmp(argv[1], "lclick") || !strcmp(argv[1], "rclick") ||
        !strcmp(argv[1], "popup-bench") || !strcmp(argv[1], "scroll-bench") ||
        !strcmp(argv[1], "dialog-click") || !strcmp(argv[1], "chord") ||
        !strcmp(argv[1], "cell-bench") || !strcmp(argv[1], "ribbon-bench") ||
        !strcmp(argv[1], "ribbon-click-bench") || !strcmp(argv[1], "chord-bench"))
    {
        if (!target_has_focus()) return 5;
    }
    if ((!strcmp(argv[1], "type") || !strcmp(argv[1], "type-bench")) && argc > 2)
        type_text(argv[2], argc > 3 ? atoi(argv[3]) : 30, !strcmp(argv[1], "type-bench"));
    else if (!strcmp(argv[1], "popup-rect"))
    {
        struct popup_search search = {0};
        RECT rect;
        GetWindowThreadProcessId(word_window(), &search.pid);
        EnumWindows(find_popup,(LPARAM)&search);
        if (!search.hwnd || !GetWindowRect(search.hwnd,&rect)) return 7;
        printf("%ld %ld %ld %ld\n",rect.left,rect.top,rect.right,rect.bottom);
    }
    else if (!strcmp(argv[1], "chord-bench") && argc > 5)
    {
        unsigned int i, count = atoi(argv[2]);
        DWORD interval = atoi(argv[3]);
        INPUT in = {0};

        if (!count || count > 400 || interval > 5000) return 1;
        in.type = INPUT_KEYBOARD;
        for (i = 0; i < count; ++i)
        {
            if (!target_has_focus()) return 5;
            printf("chord index=%u tick=%llu\n", i, GetTickCount64());
            fflush(stdout);
            in.ki.wVk = strtoul(argv[4], NULL, 0);
            in.ki.dwFlags = 0;
            send_one(&in);
            key(strtoul(argv[5], NULL, 0));
            in.ki.dwFlags = KEYEVENTF_KEYUP;
            send_one(&in);
            Sleep(interval);
        }
    }
    else if (!strcmp(argv[1], "chord") && argc > 3)
    {
        INPUT in = {0};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = strtoul(argv[2],NULL,0);
        send_one(&in);
        key(strtoul(argv[3],NULL,0));
        in.ki.dwFlags=KEYEVENTF_KEYUP;
        send_one(&in);
    }
    else if (!strcmp(argv[1], "dialog-click") && argc > 4)
    {
        WCHAR title[128];
        HWND dialog;
        RECT rect;
        MultiByteToWideChar(CP_UTF8,0,argv[2],-1,title,ARRAY_SIZE(title));
        dialog=FindWindowW(L"NUIDialog",title);
        if(!dialog || !IsWindowVisible(dialog) || !GetWindowRect(dialog,&rect)) return 8;
        SetCursorPos(rect.left+atoi(argv[3]),rect.top+atoi(argv[4]));
        mouse(MOUSEEVENTF_LEFTDOWN,0);
        mouse(MOUSEEVENTF_LEFTUP,0);
    }
    else if (!strcmp(argv[1], "popup-bench") && argc > 3)
        return popup_bench(atoi(argv[2]), atoi(argv[3]), argc > 4 ? atoi(argv[4]) : 5);
    else if (!strcmp(argv[1], "cell-bench") || !strcmp(argv[1], "ribbon-bench"))
    {
        unsigned int i, count = argc > 2 ? atoi(argv[2]) : 30;
        DWORD interval = argc > 3 ? atoi(argv[3]) : 140;
        BOOL cells = !strcmp(argv[1], "cell-bench");
        WORD tabs[3] = {'H', 'N', !wcscmp(office_window_class, L"PPTFrameClass") ? 'G' : 'P'};
        if (!count || count > 120 || interval > 1000) return 1;
        for (i = 0; i < count; ++i)
        {
            if (!target_has_focus()) return 5;
            printf("%s index=%u tick=%llu\n", cells ? "cell" : "ribbon", i, GetTickCount64());
            fflush(stdout);
            if (cells)
            {
                char number[16];
                sprintf(number, "%u", i + 1);
                type_text(number, 4, FALSE);
                key(VK_RETURN);
            }
            else
            {
                key(VK_MENU);
                Sleep(35);
                key(tabs[i % 3]);
            }
            Sleep(interval);
        }
    }
    else if (!strcmp(argv[1], "dpi-windows"))
    {
        /* Visible windows of the Office process whose DPI awareness makes
         * the compositor scale them, which blurs their text. */
        struct { DWORD pid; } search;
        HWND hwnd = NULL;
        GetWindowThreadProcessId(word_window(), &search.pid);
        while ((hwnd = FindWindowExW(NULL, hwnd, NULL, NULL)))
        {
            DWORD pid;
            DPI_AWARENESS_CONTEXT context;
            WCHAR name[64], title[128];
            RECT rect;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != search.pid || !IsWindowVisible(hwnd)) continue;
            context = GetWindowDpiAwarenessContext(hwnd);
            GetClassNameW(hwnd, name, ARRAY_SIZE(name));
            GetWindowTextW(hwnd, title, ARRAY_SIZE(title));
            GetWindowRect(hwnd, &rect);
            printf("%p awareness=%d dpi=%u rect=%ld,%ld,%ld,%ld class=%ls title=%ls\n", hwnd,
                   GetAwarenessFromDpiAwarenessContext(context), GetDpiForWindow(hwnd),
                   rect.left, rect.top, rect.right, rect.bottom, name, title);
        }
    }
    else if (!strcmp(argv[1], "ribbon-dump"))
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        EnumChildWindows(word_window(), dump_netui_window, argc > 2 ? atoi(argv[2]) : 12);
        CoUninitialize();
    }
    else if (!strcmp(argv[1], "ribbon-click-bench") && argc > 4)
    {
        unsigned int i, count = atoi(argv[2]), tabs = argc - 4;
        DWORD interval = atoi(argv[3]);
        RECT rects[8];
        POINT home;

        if (!count || count > 400 || interval > 1000 || tabs > ARRAY_SIZE(rects)) return 1;
        /* Accessible locations and SetCursorPos must use the same pixels. */
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        for (i = 0; i < tabs; ++i)
        {
            if (!ribbon_tab_rect(argv[4 + i], &rects[i]))
            {
                fprintf(stderr, "Ribbon tab %s not found\n", argv[4 + i]);
                return 11;
            }
            printf("tab %s rect=%ld,%ld,%ld,%ld\n", argv[4 + i], rects[i].left, rects[i].top,
                   rects[i].right, rects[i].bottom);
        }
        GetCursorPos(&home);
        for (i = 0; i < count; ++i)
        {
            RECT *rect = &rects[i % tabs];
            if (!target_has_focus()) return 5;
            printf("ribbon index=%u tick=%llu\n", i, GetTickCount64());
            fflush(stdout);
            move_pointer((rect->left + rect->right) / 2, (rect->top + rect->bottom) / 2);
            mouse(MOUSEEVENTF_LEFTDOWN, 0);
            mouse(MOUSEEVENTF_LEFTUP, 0);
            Sleep(interval);
        }
        move_pointer(home.x, home.y);
        CoUninitialize();
    }
    else if (!strcmp(argv[1], "scroll-bench"))
    {
        unsigned int i, count = argc > 2 ? atoi(argv[2]) : 60;
        DWORD interval = argc > 3 ? atoi(argv[3]) : 50;
        for (i = 0; i < count; ++i)
        {
            if (!target_has_focus()) return 5;
            printf("scroll round=%u tick=%llu\n", i, GetTickCount64());
            fflush(stdout);
            mouse(MOUSEEVENTF_WHEEL, (i < count / 2 ? -1 : 1) * WHEEL_DELTA);
            Sleep(interval);
        }
    }
    else if (!strcmp(argv[1], "key") && argc > 2)
        key((WORD)strtoul(argv[2], NULL, 0));
    else if (!strcmp(argv[1], "wheel") && argc > 2)
        mouse(MOUSEEVENTF_WHEEL, atoi(argv[2]) * WHEEL_DELTA);
    else if (!strcmp(argv[1], "move") && argc > 3)
        SetCursorPos(atoi(argv[2]), atoi(argv[3]));
    else if (!strcmp(argv[1], "shadows"))
    {
        BOOL enabled;
        if (argc>2 && !SystemParametersInfoW(SPI_SETDROPSHADOW,0,
                (void *)(ULONG_PTR)!!atoi(argv[2]),SPIF_UPDATEINIFILE)) return 10;
        if (!SystemParametersInfoW(SPI_GETDROPSHADOW,0,&enabled,0)) return 10;
        printf("shadows=%d\n",enabled);
    }
    else if (!strcmp(argv[1], "foreground"))
    {
        WCHAR title[512],name[64];
        HWND hwnd=GetAncestor(GetForegroundWindow(),GA_ROOT);
        GetWindowTextW(hwnd,title,ARRAY_SIZE(title));
        GetClassNameW(hwnd,name,ARRAY_SIZE(name));
        printf("foreground=%p class=%ls title=%ls\n",hwnd,name,title);
    }
    else if (!strcmp(argv[1], "window-rect"))
    {
        RECT rect;
        HWND hwnd=word_window();
        if (!hwnd || !GetWindowRect(hwnd,&rect)) return 9;
        printf("%ld %ld %ld %ld\n",rect.left,rect.top,rect.right,rect.bottom);
    }
    else if (!strcmp(argv[1], "raise"))
    {
        HWND hwnd=word_window();
        if (!hwnd || !SetWindowPos(hwnd,HWND_TOP,0,0,0,0,
                SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS)) return 9;
    }
    else if (!strcmp(argv[1], "hit-test"))
    {
        WCHAR title[512];
        POINT point;
        HWND hwnd;
        GetCursorPos(&point);
        hwnd=GetAncestor(WindowFromPoint(point),GA_ROOT);
        GetWindowTextW(hwnd,title,ARRAY_SIZE(title));
        printf("hit=%p title=%ls\n",hwnd,title);
    }
    else if (!strcmp(argv[1], "top-windows"))
    {
        BOOL result;
        WCHAR desktop[128]={0};
        DWORD length;
        SetLastError(0);
        result=EnumWindows(dump_window,0);
        printf("enum=%d error=%lu\n",result,GetLastError());
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof(desktop),&length);
        printf("desktop=%ls\n",desktop);
    }
    else if (!strcmp(argv[1], "windows"))
    {
        HWND word = word_window();
        DWORD pid = 0;
        GetWindowThreadProcessId(word, &pid);
        EnumWindows(dump_word_top_window, pid);
        EnumChildWindows(word, dump_window, 0);
    }
    else if (!strcmp(argv[1], "pos"))
    {
        POINT pos;
        GetCursorPos(&pos);
        printf("cursor=%ld,%ld desktop=%dx%d dpi=%u\n", pos.x, pos.y,
                GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), GetDpiForSystem());
    }
    else if (!strcmp(argv[1], "resize") && argc > 3)
    {
        HWND word = word_window();
        if (!word) return 2;
        ShowWindowAsync(word, SW_RESTORE);
        if (!SetWindowPos(word, NULL, 0, 0, atoi(argv[2]), atoi(argv[3]),
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)) return 3;
    }
    else if (!strcmp(argv[1], "restore"))
    {
        HWND word = word_window();
        if (!word) return 2;
        ShowWindowAsync(word, SW_RESTORE);
    }
    else if (!strcmp(argv[1], "maximize"))
    {
        HWND word = word_window();
        if (!word) return 2;
        ShowWindowAsync(word, SW_MAXIMIZE);
    }
    else if (!strcmp(argv[1], "hover") && argc > 3)
    {
        INPUT in = {0};
        in.type = INPUT_MOUSE;
        in.mi.dx = MulDiv(atoi(argv[2]), 65535, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
        in.mi.dy = MulDiv(atoi(argv[3]), 65535, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        send_one(&in);
    }
    else if (!strcmp(argv[1], "click"))
    {
        printf("click tick=%llu\n", GetTickCount64());
        mouse(MOUSEEVENTF_LEFTDOWN, 0);
        mouse(MOUSEEVENTF_LEFTUP, 0);
    }
    else if (!strcmp(argv[1], "rclick"))
    {
        mouse(MOUSEEVENTF_RIGHTDOWN, 0);
        mouse(MOUSEEVENTF_RIGHTUP, 0);
    }
    else if (!strcmp(argv[1], "ping"))
    {
        HWND word = word_window();
        DWORD start = GetTickCount();
        DWORD_PTR result;
        UINT timeout = argc > 2 ? atoi(argv[2]) : 1000;

        if (!word) { printf("noword\n"); return 2; }
        if (!IsWindowEnabled(word)) { printf("modal\n"); return 4; }
        if (!SendMessageTimeoutW(word, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, timeout, &result))
        {
            printf("hung %lu\n", GetTickCount() - start);
            return 3;
        }
        printf("ok %lu\n", GetTickCount() - start);
    }
    else if (!strcmp(argv[1], "dismiss-startup"))
    {
        HWND dialog = FindWindowW(L"#32770", L"Microsoft Word");
        WCHAR text[1024] = {0};
        HWND child;

        /* Answer only Word's known post-crash safe-mode question. */
        if (dialog && GetDlgItem(dialog, IDNO))
        {
            for (child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            {
                GetWindowTextW(child, text, ARRAY_SIZE(text));
                if (wcsstr(text, L"Word couldn't start last time."))
                {
                    PostMessageW(GetDlgItem(dialog, IDNO), BM_CLICK, 0, 0);
                    printf("dismissed-safe-mode\n");
                    break;
                }
            }
        }
    }
    else if (!strcmp(argv[1], "lclick"))
    {
        printf("click tick=%llu\n", GetTickCount64());
        mouse(MOUSEEVENTF_LEFTDOWN, 0);
        mouse(MOUSEEVENTF_LEFTUP, 0);
    }
    else if (!strcmp(argv[1], "fg"))
    {
        WCHAR title[256] = {0};
        GetWindowTextW(GetForegroundWindow(), title, ARRAY_SIZE(title));
        printf("%ls\n", title);
    }
    else return 1;
    return 0;
}
