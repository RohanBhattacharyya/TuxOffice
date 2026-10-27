/* WinRT composition geometry and geometric clips.
 *
 * Copyright 2026 TuxOffice contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>
#include <math.h>
#include "private.h"
#include "initguid.h"
#include "d2d1.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ui);

/* The ViewBox interface is only forward-declared in the composition IDL. */
typedef __x_ABI_CWindows_CUI_CComposition_CICompositionViewBox ICompositionViewBox;

struct geometry_object
{
    ICompositionObject ICompositionObject_iface;
    ICompositionRoundedRectangleGeometry ICompositionRoundedRectangleGeometry_iface;
    ICompositionGeometry ICompositionGeometry_iface;
    ICompositionPathGeometry ICompositionPathGeometry_iface;
    ICompositionGeometricClip ICompositionGeometricClip_iface;
    ICompositionClip ICompositionClip_iface;
    IClosable IClosable_iface;
    LONG ref;
    SRWLOCK lock;
    BOOL is_clip, is_path;
    ICompositionPath *path;
    ICompositor *compositor;
    ICompositionPropertySet *properties;
    ICompositionGeometry *geometry;
    ICompositionViewBox *view_box;
    struct Vector2 radius, offset, size;
    FLOAT trim_end, trim_offset, trim_start;
};

static struct geometry_object *impl_from_ICompositionObject( ICompositionObject *iface )
{
    return CONTAINING_RECORD( iface, struct geometry_object, ICompositionObject_iface );
}

static HRESULT WINAPI object_QueryInterface( ICompositionObject *iface, REFIID iid, void **out )
{
    struct geometry_object *impl = impl_from_ICompositionObject( iface );

    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ICompositionObject ))
        *out = iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &impl->IClosable_iface;
    else if (!impl->is_clip && !impl->is_path && IsEqualGUID( iid, &IID_ICompositionRoundedRectangleGeometry ))
        *out = &impl->ICompositionRoundedRectangleGeometry_iface;
    else if (impl->is_path && IsEqualGUID( iid, &IID_ICompositionPathGeometry ))
        *out = &impl->ICompositionPathGeometry_iface;
    else if (!impl->is_clip && IsEqualGUID( iid, &IID_ICompositionGeometry ))
        *out = &impl->ICompositionGeometry_iface;
    else if (impl->is_clip && IsEqualGUID( iid, &IID_ICompositionGeometricClip ))
        *out = &impl->ICompositionGeometricClip_iface;
    else if (impl->is_clip && IsEqualGUID( iid, &IID_ICompositionClip ))
        *out = &impl->ICompositionClip_iface;
    else return E_NOINTERFACE;
    ICompositionObject_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI object_AddRef( ICompositionObject *iface )
{
    return InterlockedIncrement( &impl_from_ICompositionObject( iface )->ref );
}

static ULONG WINAPI object_Release( ICompositionObject *iface )
{
    struct geometry_object *impl = impl_from_ICompositionObject( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->path) ICompositionPath_Release( impl->path );
        if (impl->geometry) ICompositionGeometry_Release( impl->geometry );
        if (impl->view_box) IInspectable_Release( (IInspectable *)impl->view_box );
        if (impl->properties) ICompositionPropertySet_Release( impl->properties );
        ICompositor_Release( impl->compositor );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI object_GetIids( ICompositionObject *iface, ULONG *count, IID **iids )
{
    struct geometry_object *impl = impl_from_ICompositionObject( iface );
    IID *values;

    if (!count || !iids) return E_POINTER;
    *count = 0;
    *iids = NULL;
    if (!(values = CoTaskMemAlloc( 4 * sizeof(*values) ))) return E_OUTOFMEMORY;
    values[0] = IID_ICompositionObject;
    values[1] = IID_IClosable;
    values[2] = impl->is_clip ? IID_ICompositionGeometricClip :
            impl->is_path ? IID_ICompositionPathGeometry : IID_ICompositionRoundedRectangleGeometry;
    values[3] = impl->is_clip ? IID_ICompositionClip : IID_ICompositionGeometry;
    *count = 4;
    *iids = values;
    return S_OK;
}

static HRESULT WINAPI object_GetRuntimeClassName( ICompositionObject *iface, HSTRING *name )
{
    const WCHAR *value = impl_from_ICompositionObject( iface )->is_clip ?
        L"Windows.UI.Composition.CompositionGeometricClip" :
        impl_from_ICompositionObject( iface )->is_path ? L"Windows.UI.Composition.CompositionPathGeometry" :
        L"Windows.UI.Composition.CompositionRoundedRectangleGeometry";

    if (!name) return E_POINTER;
    return WindowsCreateString( value, wcslen( value ), name );
}

static HRESULT WINAPI object_GetTrustLevel( ICompositionObject *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI object_get_Compositor( ICompositionObject *iface, ICompositor **value )
{
    if (!value) return E_POINTER;
    *value = impl_from_ICompositionObject( iface )->compositor;
    ICompositor_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI object_get_Dispatcher( ICompositionObject *iface, ICoreDispatcher **value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI object_get_Properties( ICompositionObject *iface, ICompositionPropertySet **value )
{
    struct geometry_object *impl = impl_from_ICompositionObject( iface );
    ICompositionPropertySet *properties;
    HRESULT hr;

    if (!value) return E_POINTER;
    *value = NULL;
    if (!impl->properties)
    {
        if (FAILED(hr = composition_property_set_create( impl->compositor, &properties ))) return hr;
        if (InterlockedCompareExchangePointer( (void **)&impl->properties, properties, NULL ))
            ICompositionPropertySet_Release( properties );
    }
    ICompositionPropertySet_AddRef( *value = impl->properties );
    return S_OK;
}

static HRESULT WINAPI object_StartAnimation( ICompositionObject *iface, HSTRING name, ICompositionAnimation *animation )
{
    FIXME( "iface %p, name %s, animation %p.\n", iface, debugstr_hstring( name ), animation );
    return E_NOTIMPL;
}

static HRESULT WINAPI object_StopAnimation( ICompositionObject *iface, HSTRING name )
{
    return S_OK;
}

static const ICompositionObjectVtbl object_vtbl =
{
    object_QueryInterface, object_AddRef, object_Release, object_GetIids,
    object_GetRuntimeClassName, object_GetTrustLevel, object_get_Compositor,
    object_get_Dispatcher, object_get_Properties, object_StartAnimation, object_StopAnimation,
};

DEFINE_IINSPECTABLE( rounded, ICompositionRoundedRectangleGeometry, struct geometry_object, ICompositionObject_iface )
DEFINE_IINSPECTABLE( path_geometry, ICompositionPathGeometry, struct geometry_object, ICompositionObject_iface )
DEFINE_IINSPECTABLE( geometry, ICompositionGeometry, struct geometry_object, ICompositionObject_iface )
DEFINE_IINSPECTABLE( geometric_clip, ICompositionGeometricClip, struct geometry_object, ICompositionObject_iface )
DEFINE_IINSPECTABLE( clip, ICompositionClip, struct geometry_object, ICompositionObject_iface )
DEFINE_IINSPECTABLE( closable, IClosable, struct geometry_object, ICompositionObject_iface )

#define VECTOR_PROPERTY(name, field, nonnegative) \
static HRESULT WINAPI rounded_get_##name( ICompositionRoundedRectangleGeometry *iface, struct Vector2 *value ) \
{ \
    struct geometry_object *impl = impl_from_ICompositionRoundedRectangleGeometry( iface ); \
    if (!value) return E_POINTER; \
    AcquireSRWLockShared( &impl->lock ); \
    *value = impl->field; \
    ReleaseSRWLockShared( &impl->lock ); \
    return S_OK; \
} \
static HRESULT WINAPI rounded_put_##name( ICompositionRoundedRectangleGeometry *iface, struct Vector2 value ) \
{ \
    struct geometry_object *impl = impl_from_ICompositionRoundedRectangleGeometry( iface ); \
    if (!isfinite(value.X) || !isfinite(value.Y) || (nonnegative && (value.X < 0 || value.Y < 0))) \
        return E_INVALIDARG; \
    AcquireSRWLockExclusive( &impl->lock ); \
    impl->field = value; \
    ReleaseSRWLockExclusive( &impl->lock ); \
    return S_OK; \
}

VECTOR_PROPERTY( CornerRadius, radius, TRUE )
VECTOR_PROPERTY( Offset, offset, FALSE )
VECTOR_PROPERTY( Size, size, TRUE )

static const ICompositionRoundedRectangleGeometryVtbl rounded_vtbl =
{
    rounded_QueryInterface, rounded_AddRef, rounded_Release, rounded_GetIids,
    rounded_GetRuntimeClassName, rounded_GetTrustLevel,
    rounded_get_CornerRadius, rounded_put_CornerRadius,
    rounded_get_Offset, rounded_put_Offset, rounded_get_Size, rounded_put_Size,
};

#define FLOAT_PROPERTY(name, field) \
static HRESULT WINAPI geometry_get_##name( ICompositionGeometry *iface, FLOAT *value ) \
{ \
    struct geometry_object *impl = impl_from_ICompositionGeometry( iface ); \
    if (!value) return E_POINTER; \
    AcquireSRWLockShared( &impl->lock ); \
    *value = impl->field; \
    ReleaseSRWLockShared( &impl->lock ); \
    return S_OK; \
} \
static HRESULT WINAPI geometry_put_##name( ICompositionGeometry *iface, FLOAT value ) \
{ \
    struct geometry_object *impl = impl_from_ICompositionGeometry( iface ); \
    if (!isfinite(value)) return E_INVALIDARG; \
    AcquireSRWLockExclusive( &impl->lock ); \
    impl->field = value; \
    ReleaseSRWLockExclusive( &impl->lock ); \
    return S_OK; \
}

FLOAT_PROPERTY( TrimEnd, trim_end )
FLOAT_PROPERTY( TrimOffset, trim_offset )
FLOAT_PROPERTY( TrimStart, trim_start )

static const ICompositionGeometryVtbl geometry_vtbl =
{
    geometry_QueryInterface, geometry_AddRef, geometry_Release, geometry_GetIids,
    geometry_GetRuntimeClassName, geometry_GetTrustLevel,
    geometry_get_TrimEnd, geometry_put_TrimEnd, geometry_get_TrimOffset, geometry_put_TrimOffset,
    geometry_get_TrimStart, geometry_put_TrimStart,
};

static HRESULT WINAPI geometric_clip_get_Geometry( ICompositionGeometricClip *iface, ICompositionGeometry **value )
{
    struct geometry_object *impl = impl_from_ICompositionGeometricClip( iface );
    if (!value) return E_POINTER;
    AcquireSRWLockShared( &impl->lock );
    *value = impl->geometry;
    if (*value) ICompositionGeometry_AddRef( *value );
    ReleaseSRWLockShared( &impl->lock );
    return S_OK;
}

static HRESULT WINAPI geometric_clip_put_Geometry( ICompositionGeometricClip *iface, ICompositionGeometry *value )
{
    struct geometry_object *impl = impl_from_ICompositionGeometricClip( iface );
    ICompositionGeometry *old;

    if (value) ICompositionGeometry_AddRef( value );
    AcquireSRWLockExclusive( &impl->lock );
    old = impl->geometry;
    impl->geometry = value;
    ReleaseSRWLockExclusive( &impl->lock );
    if (old) ICompositionGeometry_Release( old );
    return S_OK;
}

static HRESULT WINAPI geometric_clip_get_ViewBox( ICompositionGeometricClip *iface, ICompositionViewBox **value )
{
    if (!value) return E_POINTER;
    *value = impl_from_ICompositionGeometricClip( iface )->view_box;
    if (*value) IInspectable_AddRef( (IInspectable *)*value );
    return S_OK;
}

static HRESULT WINAPI geometric_clip_put_ViewBox( ICompositionGeometricClip *iface, ICompositionViewBox *value )
{
    /* A view box additionally scales the geometry. Do not silently ignore it. */
    if (value) return E_NOTIMPL;
    return S_OK;
}

static const ICompositionGeometricClipVtbl geometric_clip_vtbl =
{
    geometric_clip_QueryInterface, geometric_clip_AddRef, geometric_clip_Release, geometric_clip_GetIids,
    geometric_clip_GetRuntimeClassName, geometric_clip_GetTrustLevel,
    geometric_clip_get_Geometry, geometric_clip_put_Geometry,
    geometric_clip_get_ViewBox, geometric_clip_put_ViewBox,
};

static HRESULT WINAPI path_geometry_get_Path( ICompositionPathGeometry *iface, ICompositionPath **value )
{
    struct geometry_object *impl = impl_from_ICompositionPathGeometry( iface );
    if (!value) return E_POINTER;
    AcquireSRWLockShared( &impl->lock );
    *value = impl->path;
    if (*value) ICompositionPath_AddRef( *value );
    ReleaseSRWLockShared( &impl->lock );
    return S_OK;
}

static HRESULT WINAPI path_geometry_put_Path( ICompositionPathGeometry *iface, ICompositionPath *value )
{
    struct geometry_object *impl = impl_from_ICompositionPathGeometry( iface );
    ICompositionPath *old;
    if (value) ICompositionPath_AddRef( value );
    AcquireSRWLockExclusive( &impl->lock );
    old = impl->path;
    impl->path = value;
    ReleaseSRWLockExclusive( &impl->lock );
    if (old) ICompositionPath_Release( old );
    return S_OK;
}

static const ICompositionPathGeometryVtbl path_geometry_vtbl =
{
    path_geometry_QueryInterface, path_geometry_AddRef, path_geometry_Release, path_geometry_GetIids,
    path_geometry_GetRuntimeClassName, path_geometry_GetTrustLevel, path_geometry_get_Path, path_geometry_put_Path,
};

static const ICompositionClipVtbl clip_vtbl =
{
    clip_QueryInterface, clip_AddRef, clip_Release, clip_GetIids, clip_GetRuntimeClassName, clip_GetTrustLevel,
};

static HRESULT WINAPI closable_Close( IClosable *iface )
{
    return S_OK;
}

static const IClosableVtbl closable_vtbl =
{
    closable_QueryInterface, closable_AddRef, closable_Release, closable_GetIids,
    closable_GetRuntimeClassName, closable_GetTrustLevel, closable_Close,
};

static struct geometry_object *geometry_object_create( ICompositor *compositor, BOOL is_clip )
{
    struct geometry_object *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->ICompositionObject_iface.lpVtbl = &object_vtbl;
    impl->ICompositionRoundedRectangleGeometry_iface.lpVtbl = &rounded_vtbl;
    impl->ICompositionGeometry_iface.lpVtbl = &geometry_vtbl;
    impl->ICompositionPathGeometry_iface.lpVtbl = &path_geometry_vtbl;
    impl->ICompositionGeometricClip_iface.lpVtbl = &geometric_clip_vtbl;
    impl->ICompositionClip_iface.lpVtbl = &clip_vtbl;
    impl->IClosable_iface.lpVtbl = &closable_vtbl;
    impl->ref = 1;
    InitializeSRWLock( &impl->lock );
    impl->is_clip = is_clip;
    impl->trim_end = 1.0f;
    ICompositor_AddRef( impl->compositor = compositor );
    return impl;
}

HRESULT composition_rounded_rectangle_geometry_create( ICompositor *compositor, IInspectable **result )
{
    struct geometry_object *impl;

    if (!result) return E_POINTER;
    *result = NULL;
    if (!(impl = geometry_object_create( compositor, FALSE ))) return E_OUTOFMEMORY;
    *result = (IInspectable *)&impl->ICompositionRoundedRectangleGeometry_iface;
    return S_OK;
}

HRESULT composition_geometric_clip_create( ICompositor *compositor, IInspectable *geometry, IInspectable **result )
{
    struct geometry_object *impl;
    ICompositionGeometry *base = NULL;
    HRESULT hr;

    if (!result) return E_POINTER;
    *result = NULL;
    if (geometry && FAILED(hr = IInspectable_QueryInterface( geometry, &IID_ICompositionGeometry, (void **)&base )))
        return hr;
    if (!(impl = geometry_object_create( compositor, TRUE )))
    {
        if (base) ICompositionGeometry_Release( base );
        return E_OUTOFMEMORY;
    }
    impl->geometry = base;
    *result = (IInspectable *)&impl->ICompositionGeometricClip_iface;
    return S_OK;
}

HRESULT composition_path_geometry_create( ICompositor *compositor, IInspectable *path, IInspectable **result )
{
    struct geometry_object *impl;
    ICompositionPath *base = NULL;
    HRESULT hr;
    if (!result) return E_POINTER;
    *result = NULL;
    if (path && FAILED(hr = IInspectable_QueryInterface( path, &IID_ICompositionPath, (void **)&base ))) return hr;
    if (!(impl = geometry_object_create( compositor, FALSE )))
    {
        if (base) ICompositionPath_Release( base );
        return E_OUTOFMEMORY;
    }
    impl->is_path = TRUE;
    impl->path = base;
    *result = (IInspectable *)&impl->ICompositionPathGeometry_iface;
    return S_OK;
}

HRESULT composition_geometry_get_d2d_geometry( ICompositionGeometry *geometry, ID2D1Factory *factory,
        ID2D1Geometry **result )
{
    ICompositionRoundedRectangleGeometry *rounded;
    ICompositionPathGeometry *path_geometry;
    ICompositionPath *path;
    struct Vector2 offset, size, radius;
    D2D1_ROUNDED_RECT rect;
    HRESULT hr;

    *result = NULL;
    if (!geometry) return S_FALSE;
    if (SUCCEEDED(ICompositionGeometry_QueryInterface( geometry, &IID_ICompositionRoundedRectangleGeometry,
            (void **)&rounded )))
    {
        hr = ICompositionRoundedRectangleGeometry_get_Offset( rounded, &offset );
        if (SUCCEEDED(hr)) hr = ICompositionRoundedRectangleGeometry_get_Size( rounded, &size );
        if (SUCCEEDED(hr)) hr = ICompositionRoundedRectangleGeometry_get_CornerRadius( rounded, &radius );
        ICompositionRoundedRectangleGeometry_Release( rounded );
        if (FAILED(hr)) return hr;
        rect = (D2D1_ROUNDED_RECT){{offset.X, offset.Y, offset.X + size.X, offset.Y + size.Y}, radius.X, radius.Y};
        return ID2D1Factory_CreateRoundedRectangleGeometry( factory, &rect, (ID2D1RoundedRectangleGeometry **)result );
    }
    if (SUCCEEDED(ICompositionGeometry_QueryInterface( geometry, &IID_ICompositionPathGeometry, (void **)&path_geometry )))
    {
        hr = ICompositionPathGeometry_get_Path( path_geometry, &path );
        ICompositionPathGeometry_Release( path_geometry );
        if (FAILED(hr)) return hr;
        hr = composition_path_get_d2d_geometry( path, factory, result );
        if (path) ICompositionPath_Release( path );
        return hr;
    }
    return E_NOTIMPL;
}
