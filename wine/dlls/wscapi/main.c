/*
 * Copyright 2026 The TuxOffice authors
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

/* Windows Security Center. Nothing here watches security providers, so none is reported as
 * either good or bad: "not monitored" is what Windows says without a Security Center, and
 * claiming more would switch off the warnings applications give without protection. */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wscapi);

#define WSC_SECURITY_PROVIDER_HEALTH_NOTMONITORED 2

HRESULT WINAPI WscGetSecurityProviderHealth(DWORD providers, DWORD *health)
{
    TRACE("%#lx, %p.\n", providers, health);

    if (!health) return E_INVALIDARG;
    *health = WSC_SECURITY_PROVIDER_HEALTH_NOTMONITORED;
    return S_OK;
}

HRESULT WINAPI WscRegisterForChanges(void *reserved, HANDLE *handle, LPTHREAD_START_ROUTINE callback,
        void *context)
{
    TRACE("%p, %p, %p, %p.\n", reserved, handle, callback, context);

    /* Nothing ever changes, so the callback is never called. */
    if (!handle || !callback) return E_INVALIDARG;
    *handle = (HANDLE)(ULONG_PTR)0x5c5c;
    return S_OK;
}

HRESULT WINAPI WscUnRegisterChanges(HANDLE handle)
{
    TRACE("%p.\n", handle);
    return S_OK;
}

HRESULT WINAPI WscRegisterForUserNotifications(void)
{
    TRACE(".\n");
    return S_OK;
}
