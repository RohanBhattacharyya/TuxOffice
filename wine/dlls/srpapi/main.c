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

/* Enterprise data protection. No enterprise manages this machine, so there are no
 * enterprise identities and no policy to apply. */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(srpapi);

HRESULT WINAPI SrpGetEnterpriseIds(HANDLE token, ULONG *size, const WCHAR **ids, ULONG *count)
{
    TRACE("%p, %p, %p, %p.\n", token, size, ids, count);

    if (!count) return E_INVALIDARG;
    if (size) *size = 0;
    *count = 0;
    return S_OK;
}

HRESULT WINAPI SrpGetEnterprisePolicy(HANDLE token, DWORD *flags)
{
    TRACE("%p, %p.\n", token, flags);

    if (!flags) return E_INVALIDARG;
    *flags = 0;
    return S_OK;
}

HRESULT WINAPI SrpEnablePermissiveModeFileEncryption(const WCHAR *id)
{
    TRACE("%s.\n", debugstr_w(id));
    return S_OK;
}

HRESULT WINAPI SrpDisablePermissiveModeFileEncryption(void)
{
    TRACE(".\n");
    return S_OK;
}

NTSTATUS WINAPI SrpIsTokenService(HANDLE token, BOOLEAN *service)
{
    TRACE("%p, %p.\n", token, service);

    if (!service) return STATUS_INVALID_PARAMETER;
    *service = FALSE;
    return STATUS_SUCCESS;
}
