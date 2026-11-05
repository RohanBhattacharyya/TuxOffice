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


#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "mfidl.h"
#include "mferror.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfsensorgroup);

/* Watches the cameras Media Foundation offers for processes streaming from them. A report
 * goes to the callback when that changes. Wine's Media Foundation lists no video capture
 * devices (MFEnumDeviceSources finds none), so there is no sensor whose activity changes
 * and the callback is not called. */
struct monitor
{
    IMFSensorActivityMonitor IMFSensorActivityMonitor_iface;
    LONG refcount;
    IMFSensorActivitiesReportCallback *callback;
    CRITICAL_SECTION cs;
    BOOL started;
};

static struct monitor *impl_from_IMFSensorActivityMonitor(IMFSensorActivityMonitor *iface)
{
    return CONTAINING_RECORD(iface, struct monitor, IMFSensorActivityMonitor_iface);
}

static HRESULT WINAPI monitor_QueryInterface(IMFSensorActivityMonitor *iface, REFIID riid, void **out)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFSensorActivityMonitor) || IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFSensorActivityMonitor_AddRef(iface);
        return S_OK;
    }
    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI monitor_AddRef(IMFSensorActivityMonitor *iface)
{
    struct monitor *monitor = impl_from_IMFSensorActivityMonitor(iface);
    ULONG refcount = InterlockedIncrement(&monitor->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG WINAPI monitor_Release(IMFSensorActivityMonitor *iface)
{
    struct monitor *monitor = impl_from_IMFSensorActivityMonitor(iface);
    ULONG refcount = InterlockedDecrement(&monitor->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        IMFSensorActivitiesReportCallback_Release(monitor->callback);
        monitor->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&monitor->cs);
        free(monitor);
    }
    return refcount;
}

static HRESULT WINAPI monitor_Start(IMFSensorActivityMonitor *iface)
{
    struct monitor *monitor = impl_from_IMFSensorActivityMonitor(iface);

    TRACE("%p.\n", iface);

    EnterCriticalSection(&monitor->cs);
    monitor->started = TRUE;
    LeaveCriticalSection(&monitor->cs);
    return S_OK;
}

static HRESULT WINAPI monitor_Stop(IMFSensorActivityMonitor *iface)
{
    struct monitor *monitor = impl_from_IMFSensorActivityMonitor(iface);

    TRACE("%p.\n", iface);

    EnterCriticalSection(&monitor->cs);
    monitor->started = FALSE;
    LeaveCriticalSection(&monitor->cs);
    return S_OK;
}

static const IMFSensorActivityMonitorVtbl monitor_vtbl =
{
    monitor_QueryInterface,
    monitor_AddRef,
    monitor_Release,
    monitor_Start,
    monitor_Stop,
};

HRESULT WINAPI MFCreateSensorActivityMonitor(IMFSensorActivitiesReportCallback *callback,
        IMFSensorActivityMonitor **out)
{
    struct monitor *monitor;

    TRACE("%p, %p.\n", callback, out);

    if (!out) return E_POINTER;
    *out = NULL;
    if (!callback) return E_INVALIDARG;
    if (!(monitor = calloc(1, sizeof(*monitor)))) return E_OUTOFMEMORY;
    monitor->IMFSensorActivityMonitor_iface.lpVtbl = &monitor_vtbl;
    monitor->refcount = 1;
    monitor->callback = callback;
    IMFSensorActivitiesReportCallback_AddRef(callback);
    InitializeCriticalSectionEx(&monitor->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    monitor->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": monitor.cs");
    *out = &monitor->IMFSensorActivityMonitor_iface;
    return S_OK;
}

/* Sensor groups combine the cameras of one device; Wine's Media Foundation has none to offer. */
HRESULT WINAPI MFCreateSensorGroup(const WCHAR *link, void **group)
{
    FIXME("%s, %p: not implemented.\n", debugstr_w(link), group);
    if (group) *group = NULL;
    return E_NOTIMPL;
}
