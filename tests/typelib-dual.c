/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Access builds form-control properties after obtaining both views of a dual
 * interface. Exercise that order, including an actual property invocation. */
#define COBJMACROS
#include <windows.h>
#include <oleauto.h>
#include <stdio.h>

static unsigned int checks, failures, calls;
#define CHECK(test) do { ++checks; if (!(test)) { ++failures; \
    printf("FAIL line %u: %s\n", __LINE__, #test); } } while (0)
#define REQUIRE(call) do { HRESULT hr_ = (call); CHECK(hr_ == S_OK); \
    if (hr_ != S_OK) { printf("HRESULT %#lx\n", hr_); return 1; } } while (0)

static HRESULT WINAPI object_query(IDispatch *iface, REFIID iid, void **out)
{
    *out = NULL;
    if (!IsEqualGUID(iid, &IID_IUnknown) && !IsEqualGUID(iid, &IID_IDispatch))
        return E_NOINTERFACE;
    *out = iface;
    return S_OK;
}
static ULONG WINAPI object_addref(IDispatch *iface) { return 2; }
static ULONG WINAPI object_release(IDispatch *iface) { return 1; }
static HRESULT WINAPI object_count(IDispatch *iface, UINT *count) { return E_NOTIMPL; }
static HRESULT WINAPI object_typeinfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **out)
{ return E_NOTIMPL; }
static HRESULT WINAPI object_ids(IDispatch *iface, REFIID iid, LPOLESTR *names, UINT count,
        LCID lcid, DISPID *ids) { return E_NOTIMPL; }
static HRESULT WINAPI object_invoke(IDispatch *iface, DISPID id, REFIID iid, LCID lcid,
        WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *exception, UINT *arg)
{ return E_NOTIMPL; }
static HRESULT WINAPI object_value(IDispatch *iface, LONG *value)
{
    ++calls;
    *value = 4242;
    return S_OK;
}
static const struct
{
    IDispatchVtbl dispatch;
    HRESULT (WINAPI *get_value)(IDispatch *, LONG *);
} object_vtable =
{
    { object_query, object_addref, object_release, object_count, object_typeinfo,
      object_ids, object_invoke }, object_value
};

struct marshaled_views
{
    IStream *streams[2];
    HRESULT init, unmarshal[2], attr[2];
    TYPEKIND kind[2];
};

static DWORD WINAPI read_marshaled_views(void *param)
{
    struct marshaled_views *data = param;
    ITypeInfo *views[2] = {0};
    TYPEATTR *attr;
    unsigned int i;

    data->init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(data->init)) return 1;
    for (i = 0; i < 2; ++i)
        data->unmarshal[i] = CoGetInterfaceAndReleaseStream(data->streams[i], &IID_ITypeInfo, (void **)&views[i]);
    for (i = 0; i < 2; ++i)
    {
        if (!views[i]) continue;
        data->attr[i] = ITypeInfo_GetTypeAttr(views[i], &attr);
        if (SUCCEEDED(data->attr[i]))
        {
            data->kind[i] = attr->typekind;
            ITypeInfo_ReleaseTypeAttr(views[i], attr);
        }
        ITypeInfo_Release(views[i]);
    }
    CoUninitialize();
    return 0;
}

static int test_reverse_binding(ICreateTypeLib2 *lib, ITypeInfo *base, const FUNCDESC *prototype)
{
    ICreateTypeInfo *creator;
    ITypeInfo *views[2], *bound;
    ITypeComp *comp;
    FUNCDESC desc = *prototype;
    HREFTYPE href;
    MEMBERID id;
    BINDPTR binding;
    DESCKIND kind;
    LPOLESTR names[] = { L"Value", L"OnlyChild", L"Missing" };
    LPOLESTR child_names[] = { L"Value", L"child_retval" };
    MEMBERID ids[2];
    HRESULT hr;
    unsigned int reverse, view, name;

    REQUIRE(ICreateTypeLib2_CreateTypeInfo(lib, L"DerivedControls", TKIND_INTERFACE, &creator));
    REQUIRE(ICreateTypeInfo_SetTypeFlags(creator, TYPEFLAG_FDUAL | TYPEFLAG_FOLEAUTOMATION));
    REQUIRE(ICreateTypeInfo_AddRefTypeInfo(creator, base, &href));
    REQUIRE(ICreateTypeInfo_AddImplType(creator, 0, href));
    desc.memid = 7002;
    desc.oVft = 8 * sizeof(void *);
    REQUIRE(ICreateTypeInfo_AddFuncDesc(creator, 0, &desc));
    REQUIRE(ICreateTypeInfo_SetFuncAndParamNames(creator, 0, child_names, 2));
    desc.memid = 7003;
    desc.oVft = 9 * sizeof(void *);
    REQUIRE(ICreateTypeInfo_AddFuncDesc(creator, 1, &desc));
    REQUIRE(ICreateTypeInfo_SetFuncAndParamNames(creator, 1, &names[1], 1));
    REQUIRE(ICreateTypeInfo_LayOut(creator));
    REQUIRE(ICreateTypeInfo_QueryInterface(creator, &IID_ITypeInfo, (void **)&views[0]));
    REQUIRE(ITypeInfo_GetRefTypeInfo(views[0], -2, &views[1]));
    for (reverse = 0; reverse < 3; ++reverse)
    {
        /* Toggle the flag after acquiring both views; the final pass restores
         * normal precedence and also detects stale binding caches. */
        REQUIRE(ICreateTypeInfo_SetTypeFlags(creator, TYPEFLAG_FDUAL | TYPEFLAG_FOLEAUTOMATION |
                (reverse == 1 ? TYPEFLAG_FREVERSEBIND : 0)));
        for (view = 0; view < 2; ++view)
        {
            REQUIRE(ITypeInfo_GetTypeComp(views[view], &comp));
            for (name = 0; name < 2; ++name)
            {
                MEMBERID expected = name ? 7003 : reverse == 1 ? 6002 : 7002;
                REQUIRE(ITypeInfo_GetIDsOfNames(views[view], &names[name], 1, &id));
                CHECK(id == expected);
                REQUIRE(ITypeComp_Bind(comp, names[name], 0, INVOKE_PROPERTYGET, &bound, &kind, &binding));
                CHECK(kind == DESCKIND_FUNCDESC);
                if (kind == DESCKIND_FUNCDESC)
                {
                    CHECK(binding.lpfuncdesc->memid == expected);
                    CHECK(binding.lpfuncdesc->funckind == (view ? FUNC_DISPATCH : FUNC_PUREVIRTUAL));
                    CHECK(binding.lpfuncdesc->cParams == (view ? 0 : 1));
                    ITypeInfo_ReleaseFuncDesc(bound, binding.lpfuncdesc);
                    ITypeInfo_Release(bound);
                }
            }
            REQUIRE(ITypeComp_Bind(comp, names[2], 0, INVOKE_PROPERTYGET, &bound, &kind, &binding));
            CHECK(kind == DESCKIND_NONE && !bound && !binding.lpfuncdesc);
            CHECK(ITypeInfo_GetIDsOfNames(views[view], &names[2], 1, &id) == DISP_E_UNKNOWNNAME && id == MEMBERID_NIL);
            ITypeComp_Release(comp);
        }
        /* Finding the base member but not its parameter must not silently
         * switch back to the colliding child member. */
        hr = ITypeInfo_GetIDsOfNames(views[0], child_names, 2, ids);
        CHECK(hr == (reverse == 1 ? DISP_E_UNKNOWNNAME : S_OK));
        CHECK(ids[0] == (reverse == 1 ? 6002 : 7002) && ids[1] == (reverse == 1 ? MEMBERID_NIL : 0));
    }
    ITypeInfo_Release(views[0]);
    ITypeInfo_Release(views[1]);
    ICreateTypeInfo_Release(creator);
    return 0;
}

int main(void)
{
    ICreateTypeLib2 *create_lib;
    ICreateTypeInfo *create_info, *roundtrip_creator;
    ITypeLib *stdole, *lib;
    ITypeInfo *base, *canonical, *dispatch, *roundtrip, *again, *bound;
    IUnknown *identity, *dual_identity;
    ITypeComp *comp;
    TYPEATTR *attr;
    HREFTYPE href;
    FUNCDESC desc = {0}, *result_desc;
    ELEMDESC retval = {0};
    TYPEDESC value_type = {0};
    BINDPTR binding;
    DESCKIND kind;
    DISPPARAMS params = {0};
    VARIANT result;
    EXCEPINFO exception = {0};
    IDispatch object = { (IDispatchVtbl *)&object_vtable.dispatch };
    LPOLESTR names[] = { L"Value", L"retval" };
    BSTR returned_names[2] = {0};
    UINT count, arg = 0;
    DISPID id;
    HRESULT hr;
    struct marshaled_views marshaled = {0};
    HANDLE thread;
    MSG message;
    DWORD wait;

    REQUIRE(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
    REQUIRE(LoadTypeLib(L"stdole2.tlb", &stdole));
    REQUIRE(ITypeLib_GetTypeInfoOfGuid(stdole, &IID_IDispatch, &base));
#ifdef _WIN64
    REQUIRE(CreateTypeLib2(SYS_WIN64, L"dual-view-unused.tlb", &create_lib));
#else
    REQUIRE(CreateTypeLib2(SYS_WIN32, L"dual-view-unused.tlb", &create_lib));
#endif
    REQUIRE(ICreateTypeLib2_CreateTypeInfo(create_lib, L"FormControls", TKIND_INTERFACE, &create_info));
    REQUIRE(ICreateTypeInfo_SetTypeFlags(create_info, TYPEFLAG_FDUAL | TYPEFLAG_FOLEAUTOMATION));
    REQUIRE(ICreateTypeInfo_AddRefTypeInfo(create_info, base, &href));
    REQUIRE(ICreateTypeInfo_AddImplType(create_info, 0, href));
    REQUIRE(ICreateTypeInfo_QueryInterface(create_info, &IID_ITypeInfo, (void **)&canonical));
    REQUIRE(ITypeInfo_GetRefTypeInfo(canonical, -2, &dispatch));
    REQUIRE(ITypeInfo_GetRefTypeInfo(dispatch, -2, &roundtrip));
    CHECK(roundtrip == canonical);
    REQUIRE(ITypeInfo_GetRefTypeInfo(canonical, -2, &again));
    CHECK(again == dispatch);
    ITypeInfo_Release(again);
    REQUIRE(ITypeInfo_QueryInterface(roundtrip, &IID_ICreateTypeInfo, (void **)&roundtrip_creator));
    CHECK(roundtrip_creator == create_info);
    REQUIRE(ITypeInfo_QueryInterface(canonical, &IID_IUnknown, (void **)&identity));
    REQUIRE(ITypeInfo_QueryInterface(dispatch, &IID_IUnknown, (void **)&dual_identity));
    CHECK(identity != dual_identity);
    IUnknown_Release(identity);
    IUnknown_Release(dual_identity);

    value_type.vt = VT_I4;
    retval.tdesc.vt = VT_PTR;
    retval.tdesc.lptdesc = &value_type;
    retval.paramdesc.wParamFlags = PARAMFLAG_FOUT | PARAMFLAG_FRETVAL;
    desc.memid = 6002;
    desc.funckind = FUNC_PUREVIRTUAL;
    desc.invkind = INVOKE_PROPERTYGET;
    desc.callconv = CC_STDCALL;
    desc.cParams = 1;
    desc.oVft = 7 * sizeof(void *);
    desc.elemdescFunc.tdesc.vt = VT_HRESULT;
    desc.lprgelemdescParam = &retval;
    REQUIRE(ICreateTypeInfo_AddFuncDesc(roundtrip_creator, 0, &desc));
    REQUIRE(ICreateTypeInfo_SetFuncAndParamNames(roundtrip_creator, 0, names, 2));
    REQUIRE(ICreateTypeInfo_LayOut(roundtrip_creator));

    REQUIRE(ICreateTypeLib2_QueryInterface(create_lib, &IID_ITypeLib, (void **)&lib));
    REQUIRE(ITypeLib_GetTypeInfo(lib, 0, &again));
    REQUIRE(ITypeInfo_GetTypeAttr(again, &attr));
    CHECK(attr->typekind == TKIND_INTERFACE && attr->cFuncs == 1);
    ITypeInfo_ReleaseTypeAttr(again, attr);
    ITypeInfo_Release(again);
    REQUIRE(ITypeInfo_GetTypeAttr(dispatch, &attr));
    CHECK(attr->typekind == TKIND_DISPATCH && attr->cFuncs == 8);
    ITypeInfo_ReleaseTypeAttr(dispatch, attr);
    hr = ITypeInfo_GetIDsOfNames(dispatch, names, 1, &id);
    CHECK(hr == S_OK && id == 6002);
    hr = ITypeInfo_GetFuncDesc(canonical, 0, &result_desc);
    CHECK(hr == S_OK);
    if (SUCCEEDED(hr))
    {
        CHECK(result_desc->memid == 6002 && result_desc->funckind == FUNC_PUREVIRTUAL);
        CHECK(result_desc->cParams == 1 && result_desc->elemdescFunc.tdesc.vt == VT_HRESULT);
        ITypeInfo_ReleaseFuncDesc(canonical, result_desc);
    }
    hr = ITypeInfo_GetFuncDesc(dispatch, 7, &result_desc);
    CHECK(hr == S_OK);
    if (SUCCEEDED(hr))
    {
        CHECK(result_desc->memid == 6002 && result_desc->funckind == FUNC_DISPATCH);
        CHECK(result_desc->cParams == 0 && result_desc->elemdescFunc.tdesc.vt == VT_I4);
        ITypeInfo_ReleaseFuncDesc(dispatch, result_desc);
    }
    count = 0;
    hr = ITypeInfo_GetNames(dispatch, 6002, returned_names, 2, &count);
    CHECK(hr == S_OK && count == 1 && !lstrcmpW(returned_names[0], L"Value"));
    SysFreeString(returned_names[0]);
    returned_names[0] = returned_names[1] = NULL;
    hr = ITypeInfo_GetNames(canonical, 6002, returned_names, 2, &count);
    CHECK(hr == S_OK && count == 2 && !lstrcmpW(returned_names[0], L"Value") &&
          !lstrcmpW(returned_names[1], L"retval"));
    SysFreeString(returned_names[0]);
    SysFreeString(returned_names[1]);

    /* COM caches proxies by IUnknown identity and IID. Both views use
     * IID_ITypeInfo, so sharing their COM identity would erase the type kind. */
    REQUIRE(CoMarshalInterThreadInterfaceInStream(&IID_ITypeInfo, (IUnknown *)canonical, &marshaled.streams[0]));
    REQUIRE(CoMarshalInterThreadInterfaceInStream(&IID_ITypeInfo, (IUnknown *)dispatch, &marshaled.streams[1]));
    thread = CreateThread(NULL, 0, read_marshaled_views, &marshaled, 0, NULL);
    CHECK(thread != NULL);
    if (!thread) return 1;
    while ((wait = MsgWaitForMultipleObjects(1, &thread, FALSE, 10000, QS_ALLINPUT)) == WAIT_OBJECT_0 + 1)
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    CHECK(wait == WAIT_OBJECT_0);
    if (wait != WAIT_OBJECT_0) return 1;
    CloseHandle(thread);
    CHECK(SUCCEEDED(marshaled.init));
    CHECK(marshaled.unmarshal[0] == S_OK && marshaled.unmarshal[1] == S_OK);
    CHECK(marshaled.attr[0] == S_OK && marshaled.attr[1] == S_OK);
    CHECK(marshaled.kind[0] == TKIND_INTERFACE && marshaled.kind[1] == TKIND_DISPATCH);

    REQUIRE(ITypeInfo_GetTypeComp(dispatch, &comp));
    REQUIRE(ITypeComp_Bind(comp, names[0], 0, INVOKE_PROPERTYGET, &bound, &kind, &binding));
    CHECK(kind == DESCKIND_FUNCDESC);
    if (kind == DESCKIND_FUNCDESC)
    {
        CHECK(bound == dispatch && binding.lpfuncdesc->funckind == FUNC_DISPATCH);
        CHECK(binding.lpfuncdesc->cParams == 0 && binding.lpfuncdesc->elemdescFunc.tdesc.vt == VT_I4);
        ITypeInfo_ReleaseFuncDesc(bound, binding.lpfuncdesc);
        ITypeInfo_Release(bound);
    }
    ITypeComp_Release(comp);
    if (test_reverse_binding(create_lib, canonical, &desc)) return 1;

    /* The retained dispatch view must also keep the editable description and
     * its parent library alive after every original interface is released. */
    ITypeLib_Release(lib);
    ICreateTypeInfo_Release(roundtrip_creator);
    ITypeInfo_Release(roundtrip);
    ITypeInfo_Release(canonical);
    ICreateTypeInfo_Release(create_info);
    ICreateTypeLib2_Release(create_lib);
    VariantInit(&result);
    hr = ITypeInfo_Invoke(dispatch, &object, 6002, DISPATCH_PROPERTYGET, &params, &result, &exception, &arg);
    CHECK(hr == S_OK);
    CHECK(calls == 1 && V_VT(&result) == VT_I4 && V_I4(&result) == 4242);
    VariantClear(&result);
    SysFreeString(exception.bstrSource);
    SysFreeString(exception.bstrDescription);
    SysFreeString(exception.bstrHelpFile);

    ITypeInfo_Release(dispatch);
    ITypeInfo_Release(base);
    ITypeLib_Release(stdole);
    CoUninitialize();
    printf("dual-view: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
