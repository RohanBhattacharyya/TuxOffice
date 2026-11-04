/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A 64-bit process marshals an interface described by a SYS_WIN32 type library,
 * as Office's own libraries are. oleaut32 reports such a library's vtable size
 * in this process's pointer size, so the proxy built from it must count slots
 * in the same unit. Counted in the library's 4-byte unit, every method became
 * a forwarder to a base proxy that does not exist, and Word crashed opening a
 * PDF through IConverter. */
#define COBJMACROS
#include <windows.h>
#include <oleauto.h>
#include <stdio.h>

static const GUID lib_guid={0x5f0d7c11,0x2b7e,0x4c55,{0x9a,0x31,0x7e,0x52,0x10,0x6b,0x3c,0x01}};
static const GUID iid_calc={0x5f0d7c12,0x2b7e,0x4c55,{0x9a,0x31,0x7e,0x52,0x10,0x6b,0x3c,0x01}};

typedef struct calc calc;
typedef struct
{
    HRESULT (WINAPI *QueryInterface)(calc *, REFIID, void **);
    ULONG (WINAPI *AddRef)(calc *);
    ULONG (WINAPI *Release)(calc *);
    HRESULT (WINAPI *Add)(calc *, int, int, int *);
    HRESULT (WINAPI *Twice)(calc *, int, int *);
} calc_vtbl;
struct calc { const calc_vtbl *vtbl; };

static HRESULT WINAPI calc_query(calc *iface, REFIID iid, void **out)
{
    if(IsEqualGUID(iid,&IID_IUnknown) || IsEqualGUID(iid,&iid_calc)) { *out=iface;return S_OK; }
    *out=NULL;return E_NOINTERFACE;
}
static ULONG WINAPI calc_addref(calc *iface) { return 2; }
static ULONG WINAPI calc_release(calc *iface) { return 1; }
static HRESULT WINAPI calc_add(calc *iface, int a, int b, int *out) { *out=a+b;return S_OK; }
static HRESULT WINAPI calc_twice(calc *iface, int a, int *out) { *out=2*a;return S_OK; }
static const calc_vtbl vtbl={calc_query,calc_addref,calc_release,calc_add,calc_twice};
static calc object={&vtbl};

static int failures;
static void check(int ok, const char *what)
{
    printf("%s %s\n",ok?"PASS":"FAIL",what);
    if(!ok) failures++;
}

static HRESULT add_func(ICreateTypeInfo *info, UINT index, const WCHAR *name, UINT inputs)
{
    ELEMDESC params[3]={{{0}}};
    TYPEDESC out_type={{0}};
    FUNCDESC desc={0};
    BSTR names[4];
    UINT i;
    HRESULT hr;

    out_type.vt=VT_I4;
    for(i=0;i<inputs;i++)
    {
        params[i].tdesc.vt=VT_I4;
        params[i].paramdesc.wParamFlags=PARAMFLAG_FIN;
    }
    params[inputs].tdesc.vt=VT_PTR;
    params[inputs].tdesc.lptdesc=&out_type;
    params[inputs].paramdesc.wParamFlags=PARAMFLAG_FOUT|PARAMFLAG_FRETVAL;
    desc.memid=index+1;desc.funckind=FUNC_PUREVIRTUAL;desc.invkind=INVOKE_FUNC;desc.callconv=CC_STDCALL;
    desc.cParams=inputs+1;desc.lprgelemdescParam=params;desc.elemdescFunc.tdesc.vt=VT_HRESULT;
    if(FAILED(hr=ICreateTypeInfo_AddFuncDesc(info,index,&desc))) return hr;
    names[0]=SysAllocString(name);names[1]=SysAllocString(L"a");names[2]=SysAllocString(L"b");names[3]=SysAllocString(L"result");
    hr=ICreateTypeInfo_SetFuncAndParamNames(info,index,names,inputs+1);
    for(i=0;i<4;i++) SysFreeString(names[i]);
    return hr;
}

/* Build the library as a 32-bit one and save it, so it is read back from disk. */
static HRESULT build_library(const WCHAR *path)
{
    ICreateTypeLib2 *lib;
    ICreateTypeInfo *info;
    ITypeLib *stdole;
    ITypeInfo *unknown;
    HREFTYPE ref;
    HRESULT hr;

    if(FAILED(hr=CreateTypeLib2(SYS_WIN32,path,&lib))) return hr;
    ICreateTypeLib2_SetGuid(lib,&lib_guid);
    ICreateTypeLib2_SetName(lib,(WCHAR *)L"TuxOfficeWin32Marshal");
    ICreateTypeLib2_SetVersion(lib,1,0);
    ICreateTypeLib2_SetLcid(lib,LOCALE_NEUTRAL);
    if(FAILED(hr=ICreateTypeLib2_CreateTypeInfo(lib,(WCHAR *)L"ICalc",TKIND_INTERFACE,&info))) goto done;
    ICreateTypeInfo_SetGuid(info,&iid_calc);
    ICreateTypeInfo_SetTypeFlags(info,TYPEFLAG_FOLEAUTOMATION);
    if(FAILED(hr=LoadTypeLib(L"stdole2.tlb",&stdole))) goto info_done;
    hr=ITypeLib_GetTypeInfoOfGuid(stdole,&IID_IUnknown,&unknown);
    ITypeLib_Release(stdole);
    if(FAILED(hr)) goto info_done;
    hr=ICreateTypeInfo_AddRefTypeInfo(info,unknown,&ref);
    ITypeInfo_Release(unknown);
    if(FAILED(hr) || FAILED(hr=ICreateTypeInfo_AddImplType(info,0,ref))) goto info_done;
    if(FAILED(hr=add_func(info,0,L"Add",2)) || FAILED(hr=add_func(info,1,L"Twice",1))) goto info_done;
    if(SUCCEEDED(hr=ICreateTypeInfo_LayOut(info))) hr=ICreateTypeLib2_SaveAllChanges(lib);
info_done:
    ICreateTypeInfo_Release(info);
done:
    ICreateTypeLib2_Release(lib);
    return hr;
}

static IStream *stream;
static HANDLE ready;

static DWORD WINAPI server(void *arg)
{
    MSG msg;
    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    CoMarshalInterThreadInterfaceInStream(&iid_calc,(IUnknown *)&object,&stream);
    SetEvent(ready);
    while(GetMessageW(&msg,NULL,0,0)) DispatchMessageW(&msg);
    CoUninitialize();
    return 0;
}

int main(void)
{
    WCHAR path[MAX_PATH];
    ITypeLib *lib;
    TYPEATTR *attr;
    ITypeInfo *info;
    HANDLE thread;
    DWORD tid;
    calc *proxy;
    int result;
    HRESULT hr;

    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    GetTempPathW(MAX_PATH,path);wcscat(path,L"tuxoffice-win32-marshal.tlb");
    hr=build_library(path);
    check(hr==S_OK,"a SYS_WIN32 library is built");
    hr=LoadTypeLibEx(path,REGKIND_REGISTER,&lib);
    check(hr==S_OK,"it loads and registers");
    if(FAILED(hr)) return 1;
    ITypeLib_GetTypeInfoOfGuid(lib,&iid_calc,&info);
    ITypeInfo_GetTypeAttr(info,&attr);
    printf("ICalc: %u functions, vtable size %u\n",attr->cFuncs,attr->cbSizeVft);
    ITypeInfo_ReleaseTypeAttr(info,attr);ITypeInfo_Release(info);

    ready=CreateEventW(NULL,FALSE,FALSE,NULL);
    thread=CreateThread(NULL,0,server,NULL,0,&tid);
    WaitForSingleObject(ready,INFINITE);
    hr=CoGetInterfaceAndReleaseStream(stream,&iid_calc,(void **)&proxy);
    check(hr==S_OK,"the interface unmarshals through the type library");
    if(SUCCEEDED(hr))
    {
        result=0;hr=proxy->vtbl->Add(proxy,2,3,&result);
        check(hr==S_OK && result==5,"the first method reaches the object");
        result=0;hr=proxy->vtbl->Twice(proxy,21,&result);
        check(hr==S_OK && result==42,"the second method reaches the object");
        proxy->vtbl->Release(proxy);
    }
    PostThreadMessageW(tid,WM_QUIT,0,0);
    WaitForSingleObject(thread,5000);
    UnRegisterTypeLib(&lib_guid,1,0,LOCALE_NEUTRAL,SYS_WIN32);
    ITypeLib_Release(lib);
    DeleteFileW(path);
    CoUninitialize();
    printf("%s\n",failures?"FAILED":"ALL PASSED");
    return failures!=0;
}
