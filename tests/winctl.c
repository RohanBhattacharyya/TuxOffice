/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Inspect and drive the windows of every Office app in the prefix, whichever
 * window class it uses (Outlook, OneNote, Access and Publisher have their own).
 *
 *   winctl.exe list                 visible top-level windows and their process
 *   winctl.exe text [FILTER]        all visible windows with their child controls
 *   winctl.exe button TEXT          click a dialog button by caption (prefix match, & ignored)
 *   winctl.exe activate FILTER      bring a top-level window to the foreground
 *   winctl.exe close FILTER         post WM_CLOSE to a top-level window
 *   winctl.exe acc FILTER [DEPTH]   dump the accessibility tree of a window
 *   winctl.exe acc-click FILTER NAME [ROLE]  move to an accessible object and click it
 *   winctl.exe acc-do FILTER NAME [ROLE]     run its default action (no focus or pointer)
 *   winctl.exe click-relative FILTER X Y WIDTH HEIGHT  click a scaled window coordinate
 *   winctl.exe ping FILTER [MS]     does the window's UI thread answer?
 *
 * FILTER matches a window class or a substring of its title or process name.
 * Build: winegcc -b x86_64-windows -mconsole -municode -o winctl.exe winctl.c
 *        -luser32 -loleacc -lole32 -loleaut32 -luuid
 */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <initguid.h>
#include <oleacc.h>
#include <psapi.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static void process_name(HWND hwnd, WCHAR *name, DWORD size, DWORD *pid_out)
{
    DWORD pid = 0;
    HANDLE process;

    name[0] = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid_out) *pid_out = pid;
    if (!(process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))) return;
    if (QueryFullProcessImageNameW(process, 0, name, &size))
    {
        WCHAR *slash = wcsrchr(name, '\\');
        if (slash) memmove(name, slash + 1, (wcslen(slash + 1) + 1) * sizeof(WCHAR));
    }
    CloseHandle(process);
}

static BOOL matches(HWND hwnd, const WCHAR *filter)
{
    WCHAR name[256], title[512], exe[MAX_PATH];

    if (!filter || !filter[0]) return TRUE;
    GetClassNameW(hwnd, name, ARRAY_SIZE(name));
    GetWindowTextW(hwnd, title, ARRAY_SIZE(title));
    process_name(hwnd, exe, ARRAY_SIZE(exe), NULL);
    return !wcscmp(name, filter) || wcsstr(title, filter) || !_wcsicmp(exe, filter);
}

static void print_window(HWND hwnd, const char *indent)
{
    WCHAR name[256], title[512], exe[MAX_PATH];
    DWORD pid;
    RECT rect;

    GetClassNameW(hwnd, name, ARRAY_SIZE(name));
    GetWindowTextW(hwnd, title, ARRAY_SIZE(title));
    GetWindowRect(hwnd, &rect);
    process_name(hwnd, exe, ARRAY_SIZE(exe), &pid);
    wprintf(L"%hs%p %ls/%lu vis=%d en=%d style=%08lx exstyle=%08lx owner=%p dpi=%u rect=%ld,%ld,%ldx%ld class=%ls title=%ls\n",
            indent, hwnd, exe, pid, IsWindowVisible(hwnd), IsWindowEnabled(hwnd),
            GetWindowLongW(hwnd, GWL_STYLE), GetWindowLongW(hwnd, GWL_EXSTYLE),
            GetWindow(hwnd, GW_OWNER), GetDpiForWindow(hwnd), rect.left, rect.top,
            rect.right - rect.left, rect.bottom - rect.top, name, title);
}

static BOOL CALLBACK print_child(HWND hwnd, LPARAM unused)
{
    if (IsWindowVisible(hwnd)) print_window(hwnd, "    ");
    return TRUE;
}

struct walk { const WCHAR *filter; BOOL children; HWND found; };

static BOOL CALLBACK list_top(HWND hwnd, LPARAM data)
{
    struct walk *walk = (void *)data;
    WCHAR exe[MAX_PATH];

    if (!IsWindowVisible(hwnd) || !matches(hwnd, walk->filter)) return TRUE;
    process_name(hwnd, exe, ARRAY_SIZE(exe), NULL);
    if (!_wcsicmp(exe, L"winctl.exe") || !_wcsicmp(exe, L"explorer.exe")) return TRUE;
    print_window(hwnd, "");
    if (walk->children) EnumChildWindows(hwnd, print_child, 0);
    return TRUE;
}

static BOOL CALLBACK find_top(HWND hwnd, LPARAM data)
{
    struct walk *walk = (void *)data;
    WCHAR exe[MAX_PATH];

    if (!IsWindowVisible(hwnd) || !matches(hwnd, walk->filter)) return TRUE;
    process_name(hwnd, exe, ARRAY_SIZE(exe), NULL);
    if (!_wcsicmp(exe, L"winctl.exe")) return TRUE;
    walk->found = hwnd;
    return FALSE;
}

static HWND find_window(const WCHAR *filter)
{
    struct walk walk = {filter};
    EnumWindows(find_top, (LPARAM)&walk);
    return walk.found;
}

static BOOL caption_matches(const WCHAR *caption, const WCHAR *wanted)
{
    WCHAR clean[256];
    unsigned int i, n = 0;

    for (i = 0; caption[i] && n < ARRAY_SIZE(clean) - 1; ++i)
        if (caption[i] != '&') clean[n++] = caption[i];
    clean[n] = 0;
    return !_wcsnicmp(clean, wanted, wcslen(wanted));
}

struct button_search { const WCHAR *text; HWND found; };

static BOOL CALLBACK find_button_child(HWND hwnd, LPARAM data)
{
    struct button_search *search = (void *)data;
    WCHAR name[64], text[256];

    if (!IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd)) return TRUE;
    GetClassNameW(hwnd, name, ARRAY_SIZE(name));
    if (_wcsicmp(name, L"Button")) return TRUE;
    GetWindowTextW(hwnd, text, ARRAY_SIZE(text));
    if (!caption_matches(text, search->text)) return TRUE;
    search->found = hwnd;
    return FALSE;
}

static BOOL CALLBACK find_button_top(HWND hwnd, LPARAM data)
{
    if (IsWindowVisible(hwnd)) EnumChildWindows(hwnd, find_button_child, data);
    return !((struct button_search *)data)->found;
}

/* Accessibility */

struct acc_search { const WCHAR *name; LONG role; IAccessible *found; VARIANT child; };

static void dump_acc(IAccessible *acc, int depth, int max_depth)
{
    VARIANT children[256], self = {.vt = VT_I4, .lVal = CHILDID_SELF}, role;
    LONG count = 0, obtained = 0, i, x, y, w, h;

    if (depth > max_depth || FAILED(IAccessible_get_accChildCount(acc, &count)) || !count) return;
    if (FAILED(AccessibleChildren(acc, 0, min(count, ARRAY_SIZE(children)), children, &obtained))) return;
    for (i = 0; i < obtained; ++i)
    {
        IAccessible *child = NULL;
        VARIANT id = self;
        BSTR text = NULL, value = NULL;

        if (children[i].vt == VT_DISPATCH && children[i].pdispVal)
            IDispatch_QueryInterface(children[i].pdispVal, &IID_IAccessible, (void **)&child);
        else if (children[i].vt == VT_I4)
        {
            id = children[i];
            child = acc;
            IAccessible_AddRef(child);
        }
        if (child)
        {
            role.vt = VT_EMPTY;
            IAccessible_get_accRole(child, id, &role);
            IAccessible_get_accName(child, id, &text);
            IAccessible_get_accValue(child, id, &value);
            x = y = w = h = 0;
            IAccessible_accLocation(child, &x, &y, &w, &h, id);
            if (text || (w > 0 && h > 0))
                wprintf(L"%*ls[%ld] %ld,%ld,%ldx%ld %ls%ls%ls\n", depth * 2, L"", role.vt == VT_I4 ? role.lVal : -1,
                        x, y, w, h, text ? text : L"", value ? L" = " : L"", value ? value : L"");
            SysFreeString(text);
            SysFreeString(value);
            if (id.lVal == CHILDID_SELF) dump_acc(child, depth + 1, max_depth);
            IAccessible_Release(child);
        }
        VariantClear(&children[i]);
    }
}

static BOOL find_acc(IAccessible *acc, struct acc_search *search, int depth)
{
    VARIANT children[256], self = {.vt = VT_I4, .lVal = CHILDID_SELF}, role;
    LONG count = 0, obtained = 0, i;
    BOOL found = FALSE;

    if (depth > 24 || FAILED(IAccessible_get_accChildCount(acc, &count)) || !count) return FALSE;
    if (FAILED(AccessibleChildren(acc, 0, min(count, ARRAY_SIZE(children)), children, &obtained))) return FALSE;
    for (i = 0; i < obtained; ++i)
    {
        IAccessible *child = NULL;
        VARIANT id = self;
        BSTR text = NULL;

        if (!found && children[i].vt == VT_DISPATCH && children[i].pdispVal)
            IDispatch_QueryInterface(children[i].pdispVal, &IID_IAccessible, (void **)&child);
        else if (!found && children[i].vt == VT_I4)
        {
            id = children[i];
            child = acc;
            IAccessible_AddRef(child);
        }
        if (child)
        {
            role.vt = VT_EMPTY;
            IAccessible_get_accRole(child, id, &role);
            /* Office keeps hidden copies of many commands: prefer the one on screen. */
            LONG x = 0, y = 0, w = 0, h = 0;
            IAccessible_accLocation(child, &x, &y, &w, &h, id);
            if (w > 0 && h > 0 && IAccessible_get_accName(child, id, &text) == S_OK && text &&
                !wcscmp(text, search->name) && (!search->role || (role.vt == VT_I4 && role.lVal == search->role)))
            {
                search->found = child;
                IAccessible_AddRef(child);
                search->child = id;
                found = TRUE;
            }
            SysFreeString(text);
            if (!found && id.lVal == CHILDID_SELF) found = find_acc(child, search, depth + 1);
            IAccessible_Release(child);
        }
        VariantClear(&children[i]);
    }
    return found;
}

static void mouse(DWORD flags)
{
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

static void move_pointer(int x, int y)
{
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dx = MulDiv(x - GetSystemMetrics(SM_XVIRTUALSCREEN), 65535, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    in.mi.dy = MulDiv(y - GetSystemMetrics(SM_YVIRTUALSCREEN), 65535, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(in));
}

struct acc_child_search { struct acc_search *search; };

static BOOL CALLBACK find_acc_window(HWND hwnd, LPARAM data)
{
    struct acc_search *search = (void *)data;
    IAccessible *acc;

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, &IID_IAccessible, (void **)&acc))) return TRUE;
    find_acc(acc, search, 0);
    IAccessible_Release(acc);
    return !search->found;
}

static BOOL CALLBACK dump_acc_window(HWND hwnd, LPARAM depth)
{
    IAccessible *acc;
    WCHAR name[64];

    if (!IsWindowVisible(hwnd)) return TRUE;
    GetClassNameW(hwnd, name, ARRAY_SIZE(name));
    if (wcsncmp(name, L"NetUI", 5)) return TRUE;
    print_window(hwnd, "");
    if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, &IID_IAccessible, (void **)&acc))) return TRUE;
    dump_acc(acc, 1, depth);
    IAccessible_Release(acc);
    return TRUE;
}

int wmain(int argc, WCHAR **argv)
{
    if (argc < 2) return 1;
    
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (!wcscmp(argv[1], L"list") || !wcscmp(argv[1], L"text"))
    {
        struct walk walk = {argc > 2 ? argv[2] : NULL, !wcscmp(argv[1], L"text")};
        EnumWindows(list_top, (LPARAM)&walk);
    }
    else if (!wcscmp(argv[1], L"button") && argc > 2)
    {
        struct button_search search = {argv[2]};
        EnumWindows(find_button_top, (LPARAM)&search);
        if (!search.found) { wprintf(L"no button %ls\n", argv[2]); return 3; }
        PostMessageW(search.found, BM_CLICK, 0, 0);
    }
    else if (!wcscmp(argv[1], L"activate") && argc > 2)
    {
        HWND hwnd = find_window(argv[2]);
        if (!hwnd) return 2;
        SetForegroundWindow(hwnd);
    }
    else if (!wcscmp(argv[1], L"close") && argc > 2)
    {
        HWND hwnd = find_window(argv[2]);
        if (!hwnd) return 2;
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    else if (!wcscmp(argv[1], L"click-relative") && argc == 7)
    {
        HWND hwnd = find_window(argv[2]);
        int x = _wtoi(argv[3]), y = _wtoi(argv[4]);
        int width = _wtoi(argv[5]), height = _wtoi(argv[6]);
        RECT rect;

        if (!hwnd) return 2;
        if (width <= 0 || height <= 0 || x < 0 || x >= width || y < 0 || y >= height) return 4;
        if (!GetWindowRect(hwnd, &rect)) return 3;
        x = rect.left + MulDiv(x, rect.right - rect.left, width);
        y = rect.top + MulDiv(y, rect.bottom - rect.top, height);
        SetForegroundWindow(hwnd);
        move_pointer(x, y);
        Sleep(60);
        mouse(MOUSEEVENTF_LEFTDOWN);
        mouse(MOUSEEVENTF_LEFTUP);
        wprintf(L"clicked %d,%d\n", x, y);
    }
    else if (!wcscmp(argv[1], L"ping") && argc > 2)
    {
        HWND hwnd = find_window(argv[2]);
        DWORD start = GetTickCount();
        DWORD_PTR result;

        if (!hwnd) { wprintf(L"none\n"); return 2; }
        if (!SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, argc > 3 ? _wtoi(argv[3]) : 1000, &result))
        {
            wprintf(L"hung %lu\n", GetTickCount() - start);
            return 3;
        }
        wprintf(L"ok %lu%ls\n", GetTickCount() - start, IsWindowEnabled(hwnd) ? L"" : L" modal");
    }
    else if (!wcscmp(argv[1], L"acc") && argc > 2)
    {
        HWND hwnd = find_window(argv[2]);
        IAccessible *acc;

        if (!hwnd) return 2;
        dump_acc_window(hwnd, argc > 3 ? _wtoi(argv[3]) : 12);
        EnumChildWindows(hwnd, dump_acc_window, argc > 3 ? _wtoi(argv[3]) : 12);
        if (0 && SUCCEEDED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, &IID_IAccessible, (void **)&acc)))
            IAccessible_Release(acc);
    }
    else if ((!wcscmp(argv[1], L"acc-click") || !wcscmp(argv[1], L"acc-do")) && argc > 3)
    {
        HWND hwnd = find_window(argv[2]);
        struct acc_search search = {argv[3], argc > 4 ? _wtoi(argv[4]) : 0};
        LONG x = 0, y = 0, w = 0, h = 0;
        HRESULT hr;

        if (!hwnd) { wprintf(L"no window %ls\n", argv[2]); return 2; }
        find_acc_window(hwnd, (LPARAM)&search);
        if (!search.found) EnumChildWindows(hwnd, find_acc_window, (LPARAM)&search);
        if (!search.found) { wprintf(L"no object %ls\n", argv[3]); return 3; }
        if (!wcscmp(argv[1], L"acc-do"))
        {
            hr = IAccessible_accDoDefaultAction(search.found, search.child);
            wprintf(L"action %#lx\n", hr);
        }
        else
        {
            IAccessible_accLocation(search.found, &x, &y, &w, &h, search.child);
            if (w <= 0 || h <= 0) { wprintf(L"object has no location\n"); return 4; }
            move_pointer(x + w / 2, y + h / 2);
            Sleep(60);
            mouse(MOUSEEVENTF_LEFTDOWN);
            mouse(MOUSEEVENTF_LEFTUP);
            wprintf(L"clicked %ld,%ld\n", x + w / 2, y + h / 2);
        }
        IAccessible_Release(search.found);
    }
    else return 1;
    fflush(stdout);
    CoUninitialize();
    return 0;
}
