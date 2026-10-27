/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Check what closing a message box returns, as Windows documents it.
 *
 * Usage: msgbox-close.exe
 * An OK box closed with Escape returns IDOK; a Yes/No box has a disabled
 * Close button and ignores Escape and WM_CLOSE; an OK/Cancel box returns
 * IDCANCEL. Prints one "ok"/"FAIL" line per check.
 */
#include <windows.h>
#include <stdio.h>

static int failures;
static UINT box_type;
static int box_result;

static void check( BOOL ok, const char *what, int value, int expected )
{
    printf( "%s %s: %d (expected %d)\n", ok ? "ok" : "FAIL", what, value, expected );
    if (!ok) failures++;
}

static DWORD WINAPI show_box( void *arg )
{
    box_result = MessageBoxW( NULL, L"Message box probe", L"Message box probe", box_type );
    return 0;
}

static HWND find_box(void)
{
    HWND box = NULL;
    int i;

    for (i = 0; i < 100 && !box; i++)
    {
        Sleep( 50 );
        box = FindWindowW( L"#32770", L"Message box probe" );
    }
    return box;
}

/* Returns the box's result, or 0 if it was still open after the requests. */
static int close_box( UINT type, BOOL *close_enabled )
{
    HANDLE thread;
    HWND box;
    UINT state;
    int result;

    box_type = type;
    box_result = 0;
    thread = CreateThread( NULL, 0, show_box, NULL, 0, NULL );
    if (!(box = find_box())) return -1;
    state = GetMenuState( GetSystemMenu( box, FALSE ), SC_CLOSE, MF_BYCOMMAND );
    *close_enabled = state != 0xffffffff && !(state & (MF_GRAYED | MF_DISABLED));
    PostMessageW( box, WM_COMMAND, IDCANCEL, 0 );
    PostMessageW( box, WM_CLOSE, 0, 0 );
    if (WaitForSingleObject( thread, 1000 ) == WAIT_TIMEOUT)
    {
        /* Still open, as it should be when Close means nothing. */
        PostMessageW( box, WM_COMMAND, type == MB_YESNO ? IDNO : IDOK, 0 );
        WaitForSingleObject( thread, 5000 );
        result = 0;
    }
    else result = box_result;
    CloseHandle( thread );
    return result;
}

int main(void)
{
    BOOL enabled;
    int result;

    result = close_box( MB_OK, &enabled );
    check( enabled, "OK box Close enabled", enabled, 1 );
    check( result == IDOK, "OK box closed returns IDOK", result, IDOK );

    result = close_box( MB_OKCANCEL, &enabled );
    check( enabled, "OK/Cancel box Close enabled", enabled, 1 );
    check( result == IDCANCEL, "OK/Cancel box closed returns IDCANCEL", result, IDCANCEL );

    result = close_box( MB_YESNO, &enabled );
    check( !enabled, "Yes/No box Close disabled", enabled, 0 );
    check( result == 0, "Yes/No box stays open", result, 0 );

    printf( "%s\n", failures ? "FAILED" : "PASSED" );
    return failures != 0;
}
