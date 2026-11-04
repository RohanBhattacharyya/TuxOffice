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

/* Extended Linguistic Services. No services are installed: Office's text editors ask for
 * them and do without, while a missing DLL ended the process when they asked. */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(elscore);

HRESULT WINAPI MappingGetServices(void *options, void **services, DWORD *count)
{
    TRACE("%p, %p, %p.\n", options, services, count);

    if (!services || !count) return E_INVALIDARG;
    *services = NULL;
    *count = 0;
    return S_OK;
}

HRESULT WINAPI MappingFreeServices(void *services)
{
    TRACE("%p.\n", services);
    return S_OK;
}

HRESULT WINAPI MappingRecognizeText(void *service, const WCHAR *text, DWORD length, DWORD index,
        void *options, void *bag)
{
    FIXME("%p, %s, %lu, %lu, %p, %p: no services.\n", service, debugstr_wn(text, length), length, index,
            options, bag);
    return E_INVALIDARG;
}

HRESULT WINAPI MappingDoAction(void *bag, const WCHAR *action, void *options)
{
    FIXME("%p, %s, %p: no services.\n", bag, debugstr_w(action), options);
    return E_INVALIDARG;
}

HRESULT WINAPI MappingFreePropertyBag(void *bag)
{
    TRACE("%p.\n", bag);
    return S_OK;
}
