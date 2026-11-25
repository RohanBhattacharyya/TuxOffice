/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* An in-process OLE DB fixture checks ADO provider/session lifetime without a
 * database driver. Covers Close, releasing an open connection, and failed Open.
 * CoRegisterClassObject overrides MSDASQL only inside this process; no registry
 * or system provider is changed.
 * Build: winegcc -b x86_64-windows -mconsole -o ado-lifetime.exe ado-lifetime.c
 *        -lole32 -loleaut32 -luuid
 */
#define COBJMACROS
#define DBINITCONSTANTS
#include <initguid.h>
#include <msado15_backcompat.h>
#include <msdasql.h>
#include <oledb.h>
#include <stdio.h>
static LONG refs, session_refs, initialized, uninitialized;
static HRESULT init_result = S_OK;
static IDBInitialize init;
static IDBProperties props;
static IDBCreateSession create_session;
static IUnknown session;
static HRESULT WINAPI qi(IDBInitialize *iface, REFIID iid, void **out)
{
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDBInitialize))
        *out = &init;
    else if (IsEqualIID(iid, &IID_IDBProperties))
        *out = &props;
    else if (IsEqualIID(iid, &IID_IDBCreateSession))
        *out = &create_session;
    else
        return E_NOINTERFACE;
    ++refs;
    return S_OK;
}
static ULONG WINAPI addref(IDBInitialize *iface)
{
    return ++refs;
}
static ULONG WINAPI release(IDBInitialize *iface)
{
    return --refs;
}
static HRESULT WINAPI initialize(IDBInitialize *iface)
{
    ++initialized;
    return init_result;
}
static HRESULT WINAPI uninitialize(IDBInitialize *iface)
{
    ++uninitialized;
    return S_OK;
}
static IDBInitializeVtbl init_vtbl = {qi, addref, release, initialize, uninitialize};
static IDBInitialize init = {&init_vtbl};
static HRESULT WINAPI props_qi(IDBProperties *iface, REFIID iid, void **out)
{
    return qi(&init, iid, out);
}
static ULONG WINAPI props_addref(IDBProperties *iface)
{
    return ++refs;
}
static ULONG WINAPI props_release(IDBProperties *iface)
{
    return --refs;
}
static HRESULT WINAPI getprops(IDBProperties *i, ULONG n, const DBPROPIDSET *s, ULONG *count, DBPROPSET **p)
{
    return E_NOTIMPL;
}
static HRESULT WINAPI getinfo(IDBProperties *i, ULONG n, const DBPROPIDSET *s, ULONG *count,
                              DBPROPINFOSET **p, OLECHAR **d)
{
    return E_NOTIMPL;
}
static HRESULT WINAPI setprops(IDBProperties *i, ULONG n, DBPROPSET *s)
{
    return S_OK;
}
static IDBPropertiesVtbl props_vtbl = {props_qi, props_addref, props_release, getprops, getinfo, setprops};
static IDBProperties props = {&props_vtbl};
static HRESULT WINAPI session_qi(IUnknown *i, REFIID iid, void **out)
{
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown))
        return E_NOINTERFACE;
    *out = i;
    ++session_refs;
    return S_OK;
}
static ULONG WINAPI session_addref(IUnknown *i)
{
    return ++session_refs;
}
static ULONG WINAPI session_release(IUnknown *i)
{
    return --session_refs;
}
static IUnknownVtbl session_vtbl = {session_qi, session_addref, session_release};
static IUnknown session = {&session_vtbl};
static HRESULT WINAPI create_qi(IDBCreateSession *iface, REFIID iid, void **out)
{
    return qi(&init, iid, out);
}
static ULONG WINAPI create_addref(IDBCreateSession *i)
{
    return ++refs;
}
static ULONG WINAPI create_release(IDBCreateSession *i)
{
    return --refs;
}
static HRESULT WINAPI createsession(IDBCreateSession *i, IUnknown *outer, REFIID iid, IUnknown **out)
{
    return session_qi(&session, iid, (void **)out);
}
static IDBCreateSessionVtbl create_vtbl = {create_qi, create_addref, create_release, createsession};
static IDBCreateSession create_session = {&create_vtbl};
static HRESULT WINAPI factory_qi(IClassFactory *i, REFIID iid, void **out)
{
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IClassFactory))
        return E_NOINTERFACE;
    *out = i;
    return S_OK;
}
static ULONG WINAPI factory_addref(IClassFactory *i)
{
    return 2;
}
static ULONG WINAPI factory_release(IClassFactory *i)
{
    return 1;
}
static HRESULT WINAPI factory_create(IClassFactory *i, IUnknown *outer, REFIID iid, void **out)
{
    return qi(&init, iid, out);
}
static HRESULT WINAPI factory_lock(IClassFactory *i, BOOL lock)
{
    return S_OK;
}
static IClassFactoryVtbl factory_vtbl = {factory_qi, factory_addref, factory_release, factory_create,
                                         factory_lock};
static IClassFactory factory = {&factory_vtbl};
int main(void)
{
    static const char *names[] = {"explicit Close", "release open connection", "failed Open"};
    DWORD cookie;
    HRESULT hr, close_hr;
    _Connection *connection;
    BSTR str;
    int n, failures = 0;
    ULONG remaining;
    BOOL passed;

    CoInitialize(NULL);
    hr = CoRegisterClassObject(&CLSID_MSDASQL, (IUnknown *)&factory, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE,
                               &cookie);
    if (FAILED(hr))
    {
        printf("FAIL register fixture %#lx\n", hr);
        return 2;
    }
    for (n = 0; n < 3; n++)
    {
        refs = session_refs = initialized = uninitialized = 0;
        init_result = n == 2 ? E_FAIL : S_OK;
        hr = CoCreateInstance(&CLSID_Connection, NULL, CLSCTX_INPROC_SERVER, &IID__Connection,
                              (void **)&connection);
        if (FAILED(hr))
        {
            printf("FAIL create connection %#lx\n", hr);
            failures++;
            break;
        }
        str = SysAllocString(L"Provider=MSDASQL;");
        hr = _Connection_Open(connection, str, NULL, NULL, adConnectUnspecified);
        SysFreeString(str);
        passed = hr == (n == 2 ? E_FAIL : S_OK);
        close_hr = S_OK;
        if (n == 0 && SUCCEEDED(hr))
            close_hr = _Connection_Close(connection);
        remaining = _Connection_Release(connection);
        passed = passed && close_hr == S_OK && !remaining && !refs && !session_refs && initialized == 1 &&
                 uninitialized == 1;
        printf("%s %s: open=%#lx close=%#lx provider_refs=%ld session_refs=%ld init=%ld uninit=%ld\n",
               passed ? "PASS" : "FAIL", names[n], hr, close_hr, refs, session_refs, initialized,
               uninitialized);
        failures += !passed;
    }
    CoRevokeClassObject(cookie);
    CoUninitialize();
    printf("RESULT ado-lifetime failed=%d\n", failures);
    return failures != 0;
}
