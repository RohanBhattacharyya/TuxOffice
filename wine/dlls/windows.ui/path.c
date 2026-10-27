/* WinRT composition paths. See geometry.c for licensing terms. */
#include <stdlib.h>
#include "private.h"
#include "initguid.h"
#include "d2d1.h"
#include "wine/debug.h"
WINE_DEFAULT_DEBUG_CHANNEL(uipath);

struct composition_path
{
    ICompositionPath ICompositionPath_iface;
    LONG ref;
    IGeometrySource2D *source;
};

static struct composition_path *impl_from_ICompositionPath( ICompositionPath *iface )
{
    return CONTAINING_RECORD( iface, struct composition_path, ICompositionPath_iface );
}

static HRESULT path_class_name( HSTRING *name )
{
    static const WCHAR value[] = L"Windows.UI.Composition.CompositionPath";
    if (!name) return E_POINTER;
    return WindowsCreateString( value, ARRAY_SIZE(value) - 1, name );
}

static HRESULT WINAPI path_QueryInterface( ICompositionPath *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s.\n", iface, debugstr_guid(iid) );
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualGUID(iid, &IID_IUnknown) && !IsEqualGUID(iid, &IID_IInspectable) &&
        !IsEqualGUID(iid, &IID_IAgileObject) && !IsEqualGUID(iid, &IID_ICompositionPath)) return E_NOINTERFACE;
    ICompositionPath_AddRef( iface );
    *out = iface;
    return S_OK;
}

static ULONG WINAPI path_AddRef( ICompositionPath *iface )
{
    return InterlockedIncrement( &impl_from_ICompositionPath( iface )->ref );
}

static ULONG WINAPI path_Release( ICompositionPath *iface )
{
    struct composition_path *impl = impl_from_ICompositionPath( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        IGeometrySource2D_Release( impl->source );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI path_GetIids( ICompositionPath *iface, ULONG *count, IID **iids )
{
    if (!count || !iids) return E_POINTER;
    *count = 0;
    if (!(*iids = CoTaskMemAlloc( sizeof(**iids) ))) return E_OUTOFMEMORY;
    **iids = IID_ICompositionPath;
    *count = 1;
    return S_OK;
}

static HRESULT WINAPI path_GetRuntimeClassName( ICompositionPath *iface, HSTRING *name )
{
    return path_class_name( name );
}

static HRESULT WINAPI path_GetTrustLevel( ICompositionPath *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static const ICompositionPathVtbl path_vtbl =
{
    path_QueryInterface, path_AddRef, path_Release, path_GetIids, path_GetRuntimeClassName, path_GetTrustLevel,
};

/* Native interop interface from windows.graphics.interop.h. */
static const GUID geometry_source_interop_iid =
    {0x0657af73,0x53fd,0x47cf,{0x84,0xff,0xc8,0x49,0x2d,0x2a,0x80,0xa3}};
struct geometry_source_interop;
struct geometry_source_interop_vtbl
{
    HRESULT (WINAPI *QueryInterface)( struct geometry_source_interop *, REFIID, void ** );
    ULONG (WINAPI *AddRef)( struct geometry_source_interop * );
    ULONG (WINAPI *Release)( struct geometry_source_interop * );
    HRESULT (WINAPI *GetGeometry)( struct geometry_source_interop *, ID2D1Geometry ** );
    HRESULT (WINAPI *TryGetGeometryUsingFactory)( struct geometry_source_interop *, ID2D1Factory *, ID2D1Geometry ** );
};
struct geometry_source_interop { const struct geometry_source_interop_vtbl *lpVtbl; };

HRESULT composition_path_get_d2d_geometry( ICompositionPath *path, ID2D1Factory *factory, ID2D1Geometry **result )
{
    struct geometry_source_interop *interop;
    HRESULT hr;
    *result = NULL;
    if (!path) return S_FALSE;
    if (path->lpVtbl != &path_vtbl) return E_NOTIMPL;
    hr = IGeometrySource2D_QueryInterface( impl_from_ICompositionPath( path )->source,
            &geometry_source_interop_iid, (void **)&interop );
    TRACE( "path %p, interop query hr %#lx.\n", path, hr );
    if (FAILED(hr)) return hr;
    hr = interop->lpVtbl->TryGetGeometryUsingFactory( interop, factory, result );
    if (FAILED(hr) || !*result) hr = interop->lpVtbl->GetGeometry( interop, result );
    TRACE( "path %p, geometry %p, hr %#lx.\n", path, *result, hr );
    if (TRACE_ON(uipath) && SUCCEEDED(hr) && *result)
    {
        D2D1_RECT_F bounds;
        HRESULT bounds_hr = ID2D1Geometry_GetBounds( *result, NULL, &bounds );
        TRACE("geometry bounds %.2f,%.2f-%.2f,%.2f hr %#lx.\n", bounds.left, bounds.top,
                bounds.right, bounds.bottom, bounds_hr);
    }
    interop->lpVtbl->Release( interop );
    return hr;
}

struct path_factory
{
    IActivationFactory IActivationFactory_iface;
    ICompositionPathFactory ICompositionPathFactory_iface;
    LONG ref;
};

static inline struct path_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct path_factory, IActivationFactory_iface );
}

static HRESULT WINAPI path_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct path_factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out );
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = iface;
    else if (IsEqualGUID( iid, &IID_ICompositionPathFactory ))
        *out = &impl->ICompositionPathFactory_iface;
    else return E_NOINTERFACE;

    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI path_factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_IActivationFactory( iface )->ref );
}

static ULONG WINAPI path_factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory( iface )->ref );
}

static HRESULT WINAPI path_factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    if (!count || !iids) return E_POINTER;
    *count = 0;
    *iids = CoTaskMemAlloc( sizeof(**iids) );
    if (!*iids) return E_OUTOFMEMORY;
    **iids = IID_ICompositionPathFactory;
    *count = 1;
    return S_OK;
}

static HRESULT WINAPI path_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return path_class_name( name );
}

static HRESULT WINAPI path_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI path_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    if (!instance) return E_POINTER;
    *instance = NULL;
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl path_factory_vtbl =
{
    path_factory_QueryInterface,
    path_factory_AddRef,
    path_factory_Release,
    path_factory_GetIids,
    path_factory_GetRuntimeClassName,
    path_factory_GetTrustLevel,
    path_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( path_constructor, ICompositionPathFactory, struct path_factory,
                    IActivationFactory_iface )

static HRESULT WINAPI path_constructor_Create( ICompositionPathFactory *iface,
        IGeometrySource2D *source, ICompositionPath **result )
{
    struct composition_path *impl;
    TRACE( "source %p, result %p.\n", source, result );
    if (!result) return E_POINTER;
    *result = NULL;
    if (!source) return E_INVALIDARG;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ICompositionPath_iface.lpVtbl = &path_vtbl;
    impl->ref = 1;
    IGeometrySource2D_AddRef( impl->source = source );
    *result = &impl->ICompositionPath_iface;
    return S_OK;
}

static const ICompositionPathFactoryVtbl path_constructor_vtbl =
{
    path_constructor_QueryInterface,
    path_constructor_AddRef,
    path_constructor_Release,
    path_constructor_GetIids,
    path_constructor_GetRuntimeClassName,
    path_constructor_GetTrustLevel,
    path_constructor_Create,
};

static struct path_factory path_factory_impl =
{
    {&path_factory_vtbl},
    {&path_constructor_vtbl},
    1,
};

IActivationFactory *composition_path_factory = &path_factory_impl.IActivationFactory_iface;
