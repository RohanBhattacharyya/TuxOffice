/*
 * Copyright 2019 Hans Leidekker for CodeWeavers
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
#include <string.h>
#include "windef.h"
#include "winbase.h"
#define COBJMACROS
#include "objbase.h"
#include "rpcproxy.h"
#include "msdasc.h"
#include "msado15_backcompat.h"

#include "wine/debug.h"

#include "msado15_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(msado15);

typedef HRESULT (*fnCreateInstance)( void **obj );

struct msadocf
{
    IClassFactory    IClassFactory_iface;
    fnCreateInstance pfnCreateInstance;
};

static inline struct msadocf *impl_from_IClassFactory( IClassFactory *iface )
{
    return CONTAINING_RECORD( iface, struct msadocf, IClassFactory_iface );
}

static HRESULT WINAPI msadocf_QueryInterface( IClassFactory *iface, REFIID riid, void **obj )
{
    if (IsEqualGUID( riid, &IID_IUnknown ) || IsEqualGUID( riid, &IID_IClassFactory ))
    {
        IClassFactory_AddRef( iface );
        *obj = iface;
        return S_OK;
    }
    FIXME( "interface %s not implemented\n", debugstr_guid(riid) );
    return E_NOINTERFACE;
}

static ULONG WINAPI msadocf_AddRef( IClassFactory *iface )
{
    return 2;
}

static ULONG WINAPI msadocf_Release( IClassFactory *iface )
{
    return 1;
}

static HRESULT WINAPI msadocf_CreateInstance( IClassFactory *iface, LPUNKNOWN outer, REFIID riid, void **obj )
{
    struct msadocf *cf = impl_from_IClassFactory( iface );
    IUnknown *unknown;
    HRESULT hr;

    TRACE( "%p, %s, %p\n", outer, debugstr_guid(riid), obj );

    *obj = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;

    hr = cf->pfnCreateInstance( (void **)&unknown );
    if (FAILED(hr))
        return hr;

    hr = IUnknown_QueryInterface( unknown, riid, obj );
    IUnknown_Release( unknown );
    return hr;
}

static HRESULT WINAPI msadocf_LockServer( IClassFactory *iface, BOOL dolock )
{
    FIXME( "%p, %d\n", iface, dolock );
    return S_OK;
}

static const struct IClassFactoryVtbl msadocf_vtbl =
{
    msadocf_QueryInterface,
    msadocf_AddRef,
    msadocf_Release,
    msadocf_CreateInstance,
    msadocf_LockServer
};

static struct msadocf command_cf = { { &msadocf_vtbl }, Command_create };
static struct msadocf connection_cf = { { &msadocf_vtbl }, Connection_create };
static struct msadocf recordset_cf = { { &msadocf_vtbl }, Recordset_create };
static struct msadocf stream_cf = { { &msadocf_vtbl }, Stream_create };

/***********************************************************************
 *          DllGetClassObject
 */
HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID iid, void **obj )
{
    IClassFactory *cf = NULL;

    TRACE( "%s, %s, %p\n", debugstr_guid(clsid), debugstr_guid(iid), obj );

    if (IsEqualGUID( clsid, &CLSID_Connection ))
    {
        cf = &connection_cf.IClassFactory_iface;
    }
    else if (IsEqualGUID( clsid, &CLSID_Recordset ))
    {
        cf = &recordset_cf.IClassFactory_iface;
    }
    else if (IsEqualGUID( clsid, &CLSID_Stream ))
    {
        cf = &stream_cf.IClassFactory_iface;
    }
    else if (IsEqualGUID( clsid, &CLSID_Command ))
    {
        cf = &command_cf.IClassFactory_iface;
    }
    if (!cf) return CLASS_E_CLASSNOTAVAILABLE;
    return IClassFactory_QueryInterface( cf, iid, obj );
}

static ITypeLib *typelib;
static ITypeInfo *typeinfos[LAST_tid];

static REFIID tid_ids[] = {
    &IID_ADORecordsetConstruction,
    &IID__Command,
    &IID__Connection,
    &IID_Field,
    &IID_Fields,
    &IID__Parameter,
    &IID_Parameters,
    &IID_Properties,
    &IID_Property,
    &IID__Recordset,
    &IID__Stream,
};

static HRESULT load_typelib(void)
{
    HRESULT hres;
    ITypeLib *tl;

    if(typelib)
        return S_OK;

    hres = LoadRegTypeLib(&LIBID_ADODB, 2, 8, LOCALE_SYSTEM_DEFAULT, &tl);
    if(FAILED(hres)) {
        ERR("LoadRegTypeLib failed: %08lx\n", hres);
        return hres;
    }

    if(InterlockedCompareExchangePointer((void**)&typelib, tl, NULL))
        ITypeLib_Release(tl);
    return hres;
}

HRESULT get_typeinfo(tid_t tid, ITypeInfo **typeinfo)
{
    HRESULT hres;

    if (FAILED(hres = load_typelib()))
        return hres;

    if(!typeinfos[tid]) {
        ITypeInfo *ti;

        hres = ITypeLib_GetTypeInfoOfGuid(typelib, tid_ids[tid], &ti);
        if(FAILED(hres)) {
            ERR("GetTypeInfoOfGuid(%s) failed: %08lx\n", debugstr_guid(tid_ids[tid]), hres);
            return hres;
        }

        if(InterlockedCompareExchangePointer((void**)(typeinfos+tid), ti, NULL))
            ITypeInfo_Release(ti);
    }

    *typeinfo = typeinfos[tid];
    ITypeInfo_AddRef(*typeinfo);
    return S_OK;
}

BOOL is_ado_interface(REFIID requested, REFIID legacy)
{
    static const DWORD reguided[] =
    {
        0x266, 0x400, 0x402, 0x403, 0x505, 0x506, 0x508, 0x50c, 0x50d,
        0x50e, 0x515, 0x54c, 0x54d, 0x54e, 0x54f, 0x550, 0x555, 0x556,
        0x562, 0x564, 0x565, 0x569
    };
    static const IID command61 = {0x986761e8,0x7269,0x4890,{0xaa,0x65,0xad,0x7c,0x03,0x69,0x7a,0x6d}};
    unsigned int i;

    if (IsEqualIID(requested, legacy)) return TRUE;
    if (IsEqualIID(legacy, &IID__Command)) return IsEqualIID(requested, &command61);
    if (legacy->Data2 || legacy->Data3 != 0x10 ||
            requested->Data1 != legacy->Data1 + 0x1000 ||
            memcmp((const BYTE *)requested + sizeof(DWORD),
                   (const BYTE *)legacy + sizeof(DWORD), sizeof(IID) - sizeof(DWORD))) return FALSE;
    for (i = 0; i < ARRAY_SIZE(reguided); ++i)
        if (legacy->Data1 == reguided[i]) return TRUE;
    return FALSE;
}
