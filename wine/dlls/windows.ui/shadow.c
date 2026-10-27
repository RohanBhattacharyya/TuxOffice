/* WinRT composition drop shadows.
 * Copyright 2026 TuxOffice contributors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License, version 2.1 or
 * (at your option) any later version.
 */
#include <math.h>
#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ui);

struct drop_shadow
{
    ICompositionObject ICompositionObject_iface;
    IDropShadow IDropShadow_iface;
    IDropShadow2 IDropShadow2_iface;
    ICompositionShadow ICompositionShadow_iface;
    IClosable IClosable_iface;
    LONG ref;
    SRWLOCK lock;
    BOOL closed;
    ICompositor *compositor;
    ICompositionPropertySet *properties;
    ICompositionBrush *mask;
    struct Color color;
    struct Vector3 offset;
    FLOAT blur_radius, opacity;
    enum CompositionDropShadowSourcePolicy policy;
};

static struct drop_shadow *impl_from_ICompositionObject( ICompositionObject *iface )
{
    return CONTAINING_RECORD( iface, struct drop_shadow, ICompositionObject_iface );
}

static HRESULT WINAPI object_QueryInterface( ICompositionObject *iface, REFIID iid, void **out )
{
    struct drop_shadow *impl = impl_from_ICompositionObject( iface );
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID(iid,&IID_IUnknown) || IsEqualGUID(iid,&IID_IInspectable) ||
            IsEqualGUID(iid,&IID_IAgileObject) || IsEqualGUID(iid,&IID_IDropShadow))
        *out = &impl->IDropShadow_iface;
    else if (IsEqualGUID(iid,&IID_IDropShadow2)) *out = &impl->IDropShadow2_iface;
    else if (IsEqualGUID(iid,&IID_ICompositionShadow)) *out = &impl->ICompositionShadow_iface;
    else if (IsEqualGUID(iid,&IID_ICompositionObject)) *out = iface;
    else if (IsEqualGUID(iid,&IID_IClosable)) *out = &impl->IClosable_iface;
    else return E_NOINTERFACE;
    ICompositionObject_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI object_AddRef( ICompositionObject *iface )
{
    return InterlockedIncrement( &impl_from_ICompositionObject(iface)->ref );
}

static ULONG WINAPI object_Release( ICompositionObject *iface )
{
    struct drop_shadow *impl = impl_from_ICompositionObject(iface);
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        if (impl->mask) ICompositionBrush_Release( impl->mask );
        if (impl->properties) ICompositionPropertySet_Release( impl->properties );
        ICompositor_Release( impl->compositor );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI object_GetIids( ICompositionObject *iface, ULONG *count, IID **iids )
{
    IID *values;
    if (!count || !iids) return E_POINTER;
    if (!(values = CoTaskMemAlloc(5 * sizeof(*values)))) return E_OUTOFMEMORY;
    values[0]=IID_IDropShadow; values[1]=IID_IDropShadow2;
    values[2]=IID_ICompositionShadow; values[3]=IID_ICompositionObject; values[4]=IID_IClosable;
    *count=5; *iids=values;
    return S_OK;
}

static HRESULT WINAPI object_GetRuntimeClassName( ICompositionObject *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.UI.Composition.DropShadow";
    if (!name) return E_POINTER;
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name)-1, name );
}

static HRESULT WINAPI object_GetTrustLevel( ICompositionObject *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level=BaseTrust;
    return S_OK;
}

static HRESULT WINAPI object_get_Compositor( ICompositionObject *iface, ICompositor **value )
{
    struct drop_shadow *impl = impl_from_ICompositionObject(iface);
    if (!value) return E_POINTER;
    ICompositor_AddRef( *value=impl->compositor );
    return S_OK;
}

static HRESULT WINAPI object_get_Dispatcher( ICompositionObject *iface, ICoreDispatcher **value )
{
    if (!value) return E_POINTER;
    *value=NULL;
    return S_OK;
}

static HRESULT WINAPI object_get_Properties( ICompositionObject *iface, ICompositionPropertySet **value )
{
    struct drop_shadow *impl = impl_from_ICompositionObject(iface);
    if (!value) return E_POINTER;
    ICompositionPropertySet_AddRef( *value=impl->properties );
    return S_OK;
}

static HRESULT WINAPI object_StartAnimation( ICompositionObject *iface, HSTRING name, ICompositionAnimation *animation )
{
    struct drop_shadow *impl = impl_from_ICompositionObject(iface);
    struct Vector3 vector;
    FLOAT value;
    HRESULT hr;
    const WCHAR *property;
    if (!name || !animation) return E_INVALIDARG;
    property=WindowsGetStringRawBuffer(name,NULL);
    if (!wcscmp(property,L"Offset"))
    {
        if (FAILED(hr=vector3_key_frame_animation_get_final_value(animation,&vector))) return hr;
        return IDropShadow_put_Offset(&impl->IDropShadow_iface,vector);
    }
    if (wcscmp(property,L"Opacity") && wcscmp(property,L"BlurRadius")) return E_NOTIMPL;
    if (FAILED(hr=scalar_key_frame_animation_get_final_value(animation,&value))) return hr;
    if (!wcscmp(property,L"Opacity")) return IDropShadow_put_Opacity(&impl->IDropShadow_iface,value);
    return IDropShadow_put_BlurRadius(&impl->IDropShadow_iface,value);
}

static HRESULT WINAPI object_StopAnimation( ICompositionObject *iface, HSTRING name ) { return S_OK; }

static const ICompositionObjectVtbl object_vtbl =
{
    object_QueryInterface, object_AddRef, object_Release, object_GetIids,
    object_GetRuntimeClassName, object_GetTrustLevel, object_get_Compositor,
    object_get_Dispatcher, object_get_Properties, object_StartAnimation, object_StopAnimation,
};

DEFINE_IINSPECTABLE( drop, IDropShadow, struct drop_shadow, ICompositionObject_iface )
DEFINE_IINSPECTABLE( drop2, IDropShadow2, struct drop_shadow, ICompositionObject_iface )
DEFINE_IINSPECTABLE( shadow, ICompositionShadow, struct drop_shadow, ICompositionObject_iface )
DEFINE_IINSPECTABLE( closable, IClosable, struct drop_shadow, ICompositionObject_iface )

#define SHADOW_PROPERTY(name,type,field,valid) \
static HRESULT WINAPI drop_get_##name( IDropShadow *iface, type *value ) \
{ \
    struct drop_shadow *impl=impl_from_IDropShadow(iface); \
    HRESULT hr=S_OK; \
    if (!value) return E_POINTER; \
    AcquireSRWLockShared(&impl->lock); \
    if (impl->closed) hr=RO_E_CLOSED; else *value=impl->field; \
    ReleaseSRWLockShared(&impl->lock); \
    return hr; \
} \
static HRESULT WINAPI drop_put_##name( IDropShadow *iface, type value ) \
{ \
    struct drop_shadow *impl=impl_from_IDropShadow(iface); \
    HRESULT hr=S_OK; \
    if (!(valid)) return E_INVALIDARG; \
    AcquireSRWLockExclusive(&impl->lock); \
    if (impl->closed) hr=RO_E_CLOSED; else impl->field=value; \
    ReleaseSRWLockExclusive(&impl->lock); \
    if (SUCCEEDED(hr)) composition_notify_changed(impl->compositor); \
    return hr; \
}

SHADOW_PROPERTY(BlurRadius,FLOAT,blur_radius,isfinite(value) && value>=0.0f && value<=250.0f)
SHADOW_PROPERTY(Opacity,FLOAT,opacity,isfinite(value) && value>=0.0f && value<=1.0f)
SHADOW_PROPERTY(Color,struct Color,color,TRUE)
SHADOW_PROPERTY(Offset,struct Vector3,offset,isfinite(value.X) && isfinite(value.Y) && isfinite(value.Z))

static HRESULT WINAPI drop_get_Mask( IDropShadow *iface, ICompositionBrush **value )
{
    struct drop_shadow *impl=impl_from_IDropShadow(iface);
    HRESULT hr=S_OK;
    if (!value) return E_POINTER;
    *value=NULL;
    AcquireSRWLockShared(&impl->lock);
    if (impl->closed) hr=RO_E_CLOSED;
    else if ((*value=impl->mask)) ICompositionBrush_AddRef(*value);
    ReleaseSRWLockShared(&impl->lock);
    return hr;
}

static HRESULT WINAPI drop_put_Mask( IDropShadow *iface, ICompositionBrush *value )
{
    struct drop_shadow *impl=impl_from_IDropShadow(iface);
    ICompositionBrush *old;
    if (value) ICompositionBrush_AddRef(value);
    AcquireSRWLockExclusive(&impl->lock);
    if (impl->closed)
    {
        ReleaseSRWLockExclusive(&impl->lock);
        if (value) ICompositionBrush_Release(value);
        return RO_E_CLOSED;
    }
    old=impl->mask; impl->mask=value;
    ReleaseSRWLockExclusive(&impl->lock);
    if (old) ICompositionBrush_Release(old);
    composition_notify_changed(impl->compositor);
    return S_OK;
}

static HRESULT WINAPI drop2_get_SourcePolicy( IDropShadow2 *iface, enum CompositionDropShadowSourcePolicy *value )
{
    struct drop_shadow *impl=impl_from_IDropShadow2(iface);
    HRESULT hr=S_OK;
    if (!value) return E_POINTER;
    AcquireSRWLockShared(&impl->lock);
    if (impl->closed) hr=RO_E_CLOSED; else *value=impl->policy;
    ReleaseSRWLockShared(&impl->lock);
    return hr;
}

static HRESULT WINAPI drop2_put_SourcePolicy( IDropShadow2 *iface, enum CompositionDropShadowSourcePolicy value )
{
    struct drop_shadow *impl=impl_from_IDropShadow2(iface);
    if (value!=CompositionDropShadowSourcePolicy_Default && value!=CompositionDropShadowSourcePolicy_InheritFromVisualContent)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&impl->lock);
    if (impl->closed) { ReleaseSRWLockExclusive(&impl->lock); return RO_E_CLOSED; }
    impl->policy=value;
    ReleaseSRWLockExclusive(&impl->lock);
    composition_notify_changed(impl->compositor);
    return S_OK;
}

static HRESULT WINAPI closable_Close( IClosable *iface )
{
    struct drop_shadow *impl=impl_from_IClosable(iface);
    ICompositionBrush *mask;
    AcquireSRWLockExclusive(&impl->lock);
    impl->closed=TRUE; mask=impl->mask; impl->mask=NULL;
    ReleaseSRWLockExclusive(&impl->lock);
    if (mask) ICompositionBrush_Release(mask);
    composition_notify_changed(impl->compositor);
    return S_OK;
}

static const IDropShadowVtbl drop_vtbl =
{
    drop_QueryInterface,drop_AddRef,drop_Release,drop_GetIids,drop_GetRuntimeClassName,drop_GetTrustLevel,
    drop_get_BlurRadius,drop_put_BlurRadius,drop_get_Color,drop_put_Color,drop_get_Mask,drop_put_Mask,
    drop_get_Offset,drop_put_Offset,drop_get_Opacity,drop_put_Opacity,
};
static const IDropShadow2Vtbl drop2_vtbl =
{
    drop2_QueryInterface,drop2_AddRef,drop2_Release,drop2_GetIids,drop2_GetRuntimeClassName,drop2_GetTrustLevel,
    drop2_get_SourcePolicy,drop2_put_SourcePolicy,
};
static const ICompositionShadowVtbl shadow_vtbl =
{shadow_QueryInterface,shadow_AddRef,shadow_Release,shadow_GetIids,shadow_GetRuntimeClassName,shadow_GetTrustLevel};
static const IClosableVtbl closable_vtbl =
{closable_QueryInterface,closable_AddRef,closable_Release,closable_GetIids,closable_GetRuntimeClassName,closable_GetTrustLevel,closable_Close};

HRESULT composition_drop_shadow_create( ICompositor *compositor, IInspectable **result )
{
    struct drop_shadow *impl;
    HRESULT hr;
    if (!result) return E_POINTER;
    *result=NULL;
    TRACE("compositor %p, result %p.\n",compositor,result);
    if (!(impl=calloc(1,sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->ICompositionObject_iface.lpVtbl=&object_vtbl;
    impl->IDropShadow_iface.lpVtbl=&drop_vtbl;
    impl->IDropShadow2_iface.lpVtbl=&drop2_vtbl;
    impl->ICompositionShadow_iface.lpVtbl=&shadow_vtbl;
    impl->IClosable_iface.lpVtbl=&closable_vtbl;
    impl->ref=1; impl->blur_radius=9.0f; impl->opacity=1.0f; impl->color.A=255;
    ICompositor_AddRef(impl->compositor=compositor);
    if (FAILED(hr=composition_property_set_create(compositor,&impl->properties)))
    { ICompositionObject_Release(&impl->ICompositionObject_iface); return hr; }
    *result=(IInspectable *)&impl->IDropShadow_iface;
    return S_OK;
}
