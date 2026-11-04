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

/* Media Foundation sensor groups, which cameras would be in. None is reported. */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfsensorgroup);

HRESULT WINAPI MFCreateSensorActivityMonitor(void *callback, void **monitor)
{
    FIXME("%p, %p: not implemented.\n", callback, monitor);
    if (monitor) *monitor = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI MFCreateSensorGroup(const WCHAR *link, void **group)
{
    FIXME("%s, %p: not implemented.\n", debugstr_w(link), group);
    if (group) *group = NULL;
    return E_NOTIMPL;
}
