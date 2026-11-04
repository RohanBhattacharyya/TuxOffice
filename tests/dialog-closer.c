/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Keep an Office app moving while tests/mso-crash-hunt.py drives it.
 *
 * Commands can open modal dialogs, which block automation until they close. After a
 * dialog has been up long enough to have painted, ask it to close, then press Escape
 * in it, then Alt+F4 if it is still there. Also sends Escape to the focused window from
 * time to time, which leaves Backstage and dismisses menus and galleries.
 *
 * Build: winegcc -b x86_64-windows -mconsole -o dialog-closer.exe dialog-closer.c -luser32
 * Usage: dialog-closer.exe PROCESS_ID
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAX_TRACKED 64

static struct { HWND hwnd; DWORD seen; int attempts; } tracked[MAX_TRACKED];
static DWORD target;

static BOOL is_dialog_class(const char *name)
{
    return !strcmp(name, "#32770") || !strcmp(name, "NUIDialog") || !strncmp(name, "bosa_sdm", 8)
            || !strcmp(name, "MsoCommandBarPopup") || !strncmp(name, "ThunderDFrame", 13)
            || !strcmp(name, "Net UI Tool Window");
}

static void handle_dialog(HWND hwnd, DWORD now)
{
    int i, free_slot = -1;

    for (i = 0; i < MAX_TRACKED; i++)
    {
        if (tracked[i].hwnd == hwnd) break;
        if (!tracked[i].hwnd && free_slot < 0) free_slot = i;
    }
    if (i == MAX_TRACKED)
    {
        if (free_slot < 0) return;
        i = free_slot;
        tracked[i].hwnd = hwnd;
        tracked[i].seen = now;
        tracked[i].attempts = 0;
        return;
    }
    /* Let it paint first: drawing is where a dialog is most likely to fault. */
    if (now - tracked[i].seen < 900 + 700u * tracked[i].attempts) return;
    switch (tracked[i].attempts++ % 3)
    {
    case 0:
        PostMessageA(hwnd, WM_CLOSE, 0, 0);
        break;
    case 1:
        PostMessageA(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        PostMessageA(hwnd, WM_KEYUP, VK_ESCAPE, 0);
        break;
    default:
        PostMessageA(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
        break;
    }
}

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM param)
{
    char name[64];
    DWORD pid = 0;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != target || !IsWindowVisible(hwnd)) return TRUE;
    if (!GetClassNameA(hwnd, name, sizeof(name))) return TRUE;
    if (is_dialog_class(name)) handle_dialog(hwnd, (DWORD)param);
    return TRUE;
}

int main(int argc, char **argv)
{
    HANDLE process;
    DWORD last_escape = 0;

    if (argc < 2) return 2;
    target = strtoul(argv[1], NULL, 0);
    if (!(process = OpenProcess(SYNCHRONIZE, FALSE, target))) return 1;

    while (WaitForSingleObject(process, 250) == WAIT_TIMEOUT)
    {
        DWORD now = GetTickCount();
        int i;

        for (i = 0; i < MAX_TRACKED; i++)
            if (tracked[i].hwnd && !IsWindow(tracked[i].hwnd)) tracked[i].hwnd = NULL;
        EnumWindows(enum_proc, now);

        if (now - last_escape > 1500)
        {
            GUITHREADINFO info = {sizeof(info)};
            HWND foreground = GetForegroundWindow();
            DWORD pid = 0;

            last_escape = now;
            if (foreground) GetWindowThreadProcessId(foreground, &pid);
            if (pid == target && GetGUIThreadInfo(GetWindowThreadProcessId(foreground, NULL), &info))
            {
                HWND focus = info.hwndFocus ? info.hwndFocus : foreground;

                PostMessageA(focus, WM_KEYDOWN, VK_ESCAPE, 0);
                PostMessageA(focus, WM_KEYUP, VK_ESCAPE, 0);
            }
        }
    }
    return 0;
}
