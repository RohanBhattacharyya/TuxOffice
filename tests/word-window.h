/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Office also creates hidden OpusApp windows. Exercise the document window. */
static const WCHAR *office_window_class = L"OpusApp";

static BOOL CALLBACK find_visible_word(HWND hwnd, LPARAM result)
{
    WCHAR name[64];
    if (!IsWindowVisible(hwnd)) return TRUE;
    GetClassNameW(hwnd, name, sizeof(name) / sizeof(*name));
    if (wcscmp(name, office_window_class)) return TRUE;
    *(HWND *)result = hwnd;
    return FALSE;
}

static HWND word_window(void)
{
    HWND hwnd = GetAncestor(GetForegroundWindow(), GA_ROOT);
    WCHAR name[64];
    if (hwnd && IsWindowVisible(hwnd))
    {
        GetClassNameW(hwnd, name, sizeof(name) / sizeof(*name));
        if (!wcscmp(name, office_window_class)) return hwnd;
    }
    hwnd = NULL;
    EnumWindows(find_visible_word, (LPARAM)&hwnd);
    return hwnd;
}
