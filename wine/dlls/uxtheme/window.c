/*
 * Window theming support
 *
 * Copyright 2022 Zhiyi Zhang for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "uxthemedll.h"
#include "vssym32.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(uxtheme);

static void uxtheme_draw_menu_button(HTHEME theme, HWND hwnd, HDC hdc, enum NONCLIENT_BUTTON_TYPE type,
                                     RECT rect, BOOL down, BOOL grayed)
{
    int part, state;

    switch (type)
    {
    case MENU_CLOSE_BUTTON:
        part = WP_MDICLOSEBUTTON;
        break;
    case MENU_MIN_BUTTON:
        part = WP_MDIMINBUTTON;
        break;
    case MENU_RESTORE_BUTTON:
        part = WP_MDIRESTOREBUTTON;
        break;
    case MENU_HELP_BUTTON:
        part = WP_MDIHELPBUTTON;
        break;
    /* There is no WP_MDIMAXBUTTON */
    default:
        user_api.pNonClientButtonDraw(hwnd, hdc, type, rect, down, grayed);
        return;
    }

    if (grayed)
        state = MINBS_DISABLED;
    else if (down)
        state = MINBS_PUSHED;
    else
        state = MINBS_NORMAL;

    if (IsThemeBackgroundPartiallyTransparent(theme, part, state))
        DrawThemeParentBackground(hwnd, hdc, &rect);
    DrawThemeBackground(theme, hdc, part, state, &rect, NULL);
}

void WINAPI UXTHEME_NonClientButtonDraw(HWND hwnd, HDC hdc, enum NONCLIENT_BUTTON_TYPE type,
                                        RECT rect, BOOL down, BOOL grayed)
{
    HTHEME theme;

    theme = OpenThemeDataForDpi(NULL, L"Window", GetDpiForWindow(hwnd));
    if (!theme)
    {
        user_api.pNonClientButtonDraw(hwnd, hdc, type, rect, down, grayed);
        return;
    }

    switch (type)
    {
    case MENU_CLOSE_BUTTON:
    case MENU_MIN_BUTTON:
    case MENU_MAX_BUTTON:
    case MENU_RESTORE_BUTTON:
    case MENU_HELP_BUTTON:
        uxtheme_draw_menu_button(theme, hwnd, hdc, type, rect, down, grayed);
        break;
    }

    CloseThemeData(theme);
}

/* Windows nudges a window when panning hits the end of its content. Wine does not move
 * windows for this, which loses only the bounce. */

/***********************************************************************
 *      BeginPanningFeedback                                (UXTHEME.@)
 */
BOOL WINAPI BeginPanningFeedback(HWND hwnd)
{
    TRACE("(%p)\n", hwnd);
    return IsWindow(hwnd);
}

/***********************************************************************
 *      UpdatePanningFeedback                               (UXTHEME.@)
 */
BOOL WINAPI UpdatePanningFeedback(HWND hwnd, LONG x, LONG y, BOOL inertia)
{
    TRACE("(%p, %ld, %ld, %d)\n", hwnd, x, y, inertia);
    return IsWindow(hwnd);
}

/***********************************************************************
 *      EndPanningFeedback                                  (UXTHEME.@)
 */
BOOL WINAPI EndPanningFeedback(HWND hwnd, BOOL animate)
{
    TRACE("(%p, %d)\n", hwnd, animate);
    return IsWindow(hwnd);
}
