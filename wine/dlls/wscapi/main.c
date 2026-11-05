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
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(wscapi);

#define WSC_SECURITY_PROVIDER_HEALTH_NOTMONITORED 2

HRESULT WINAPI WscGetSecurityProviderHealth(DWORD providers, DWORD *health)
{
    TRACE("%#lx, %p.\n", providers, health);

    if (!health) return E_INVALIDARG;
    *health = WSC_SECURITY_PROVIDER_HEALTH_NOTMONITORED;
    return S_OK;
}

/* Registrations for changes in security provider health. The health of every provider stays
 * "not monitored", as there is no Security Center service, so no registration is ever called;
 * they are kept so that each handle can be unregistered once. */
struct registration
{
    struct list entry;
    LPTHREAD_START_ROUTINE callback;
    void *context;
};

static struct list registrations = LIST_INIT(registrations);
static CRITICAL_SECTION registrations_cs;
static CRITICAL_SECTION_DEBUG registrations_cs_debug =
{
    0, 0, &registrations_cs,
    { &registrations_cs_debug.ProcessLocksList, &registrations_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": registrations_cs") }
};
static CRITICAL_SECTION registrations_cs = { &registrations_cs_debug, -1, 0, 0, 0, 0 };

HRESULT WINAPI WscRegisterForChanges(void *reserved, HANDLE *handle, LPTHREAD_START_ROUTINE callback,
        void *context)
{
    struct registration *registration;

    TRACE("%p, %p, %p, %p.\n", reserved, handle, callback, context);

    if (!handle || !callback) return E_INVALIDARG;
    if (!(registration = malloc(sizeof(*registration)))) return E_OUTOFMEMORY;
    registration->callback = callback;
    registration->context = context;
    EnterCriticalSection(&registrations_cs);
    list_add_tail(&registrations, &registration->entry);
    LeaveCriticalSection(&registrations_cs);
    *handle = registration;
    return S_OK;
}

HRESULT WINAPI WscUnRegisterChanges(HANDLE handle)
{
    struct registration *registration;
    HRESULT hr = E_INVALIDARG;

    TRACE("%p.\n", handle);

    EnterCriticalSection(&registrations_cs);
    LIST_FOR_EACH_ENTRY(registration, &registrations, struct registration, entry)
    {
        if (registration != handle) continue;
        list_remove(&registration->entry);
        free(registration);
        hr = S_OK;
        break;
    }
    LeaveCriticalSection(&registrations_cs);
    return hr;
}

HRESULT WINAPI WscRegisterForUserNotifications(void)
{
    TRACE(".\n");
    return S_OK;
}
