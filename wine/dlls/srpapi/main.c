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

/* Whether a token belongs to a service: its user is one of the service accounts, or it holds
 * the SERVICE group that the service control manager gives the processes it starts. */
NTSTATUS WINAPI SrpIsTokenService(HANDLE token, BOOLEAN *service)
{
    static const DWORD accounts[] =
    {
        SECURITY_LOCAL_SYSTEM_RID, SECURITY_LOCAL_SERVICE_RID, SECURITY_NETWORK_SERVICE_RID,
    };
    union { TOKEN_USER user; BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; } user;
    TOKEN_GROUPS *groups;
    NTSTATUS status;
    ULONG size, i;

    TRACE("%p, %p.\n", token, service);

    if (!service) return STATUS_INVALID_PARAMETER;
    *service = FALSE;

    if ((status = NtQueryInformationToken(token, TokenUser, &user, sizeof(user), &size))) return status;
    for (i = 0; i < ARRAY_SIZE(accounts); i++)
    {
        SID sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { accounts[i] } };

        if (RtlEqualSid(user.user.User.Sid, &sid))
        {
            *service = TRUE;
            return STATUS_SUCCESS;
        }
    }

    status = NtQueryInformationToken(token, TokenGroups, NULL, 0, &size);
    if (status != STATUS_BUFFER_TOO_SMALL) return status;
    if (!(groups = malloc(size))) return STATUS_NO_MEMORY;
    if (!(status = NtQueryInformationToken(token, TokenGroups, groups, size, &size)))
    {
        SID sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_SERVICE_RID } };

        for (i = 0; i < groups->GroupCount; i++)
        {
            if (!(groups->Groups[i].Attributes & SE_GROUP_ENABLED)) continue;
            if (RtlEqualSid(groups->Groups[i].Sid, &sid)) *service = TRUE;
        }
    }
    free(groups);
    return status;
}
