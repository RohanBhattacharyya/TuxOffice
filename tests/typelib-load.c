/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Measure a real type library's load time and fingerprint its public metadata.
 * Usage: typelib-load.exe PATH. Run a fresh process for each sample to avoid
 * the process-local type-library cache. Requires the caller's own test library;
 * does not register it or instantiate any of its objects. */
#define COBJMACROS
#include <windows.h>
#include <oleauto.h>
#include <stdio.h>
#include <stdint.h>
static uint64_t fingerprint = UINT64_C(14695981039346656037);
static void hash_bytes(const void *data, unsigned int size)
{
    const unsigned char *p = data;
    while (size--) { fingerprint ^= *p++; fingerprint *= UINT64_C(1099511628211); }
}
int wmain(int argc, WCHAR **argv)
{
    ITypeLib *lib;
    ITypeInfo *info;
    TYPEATTR *attr;
    FUNCDESC *func;
    VARDESC *var;
    BSTR name;
    HRESULT hr;
    LARGE_INTEGER before, loaded, after, frequency;
    unsigned int i, j, count, functions = 0, variables = 0;
    if (argc != 2) return 2;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&before);
    hr = LoadTypeLibEx(argv[1], REGKIND_NONE, &lib);
    QueryPerformanceCounter(&loaded);
    if (FAILED(hr)) { printf("load failed %#lx\n",hr); return 1; }
    count = ITypeLib_GetTypeInfoCount(lib);
    for (i = 0; i < count; i++)
    {
        if (FAILED(ITypeLib_GetTypeInfo(lib,i,&info))) return 1;
        if (FAILED(ITypeInfo_GetTypeAttr(info,&attr))) return 1;
        hash_bytes(&attr->guid,sizeof(attr->guid));
        hash_bytes(&attr->cFuncs,sizeof(attr->cFuncs));
        hash_bytes(&attr->cVars,sizeof(attr->cVars));
        if (FAILED(ITypeInfo_GetDocumentation(info,MEMBERID_NIL,&name,NULL,NULL,NULL))) return 1;
        hash_bytes(name,SysStringLen(name)*sizeof(WCHAR)); SysFreeString(name);
        for (j=0; j<attr->cFuncs; j++)
        {
            if (FAILED(ITypeInfo_GetFuncDesc(info,j,&func))) return 1;
            hash_bytes(&func->memid,sizeof(func->memid));
            if (FAILED(ITypeInfo_GetDocumentation(info,func->memid,&name,NULL,NULL,NULL))) return 1;
            hash_bytes(name,SysStringLen(name)*sizeof(WCHAR)); SysFreeString(name);
            ITypeInfo_ReleaseFuncDesc(info,func); functions++;
        }
        for (j=0; j<attr->cVars; j++)
        {
            if (FAILED(ITypeInfo_GetVarDesc(info,j,&var))) return 1;
            hash_bytes(&var->memid,sizeof(var->memid));
            if (FAILED(ITypeInfo_GetDocumentation(info,var->memid,&name,NULL,NULL,NULL))) return 1;
            hash_bytes(name,SysStringLen(name)*sizeof(WCHAR)); SysFreeString(name);
            ITypeInfo_ReleaseVarDesc(info,var); variables++;
        }
        ITypeInfo_ReleaseTypeAttr(info,attr); ITypeInfo_Release(info);
    }
    QueryPerformanceCounter(&after);
    printf("load_ms=%.3f inspect_ms=%.3f types=%u funcs=%u vars=%u hash=%llx\n",
        1000.0*(loaded.QuadPart-before.QuadPart)/frequency.QuadPart,
        1000.0*(after.QuadPart-loaded.QuadPart)/frequency.QuadPart,
        count,functions,variables,(unsigned long long)fingerprint);
    ITypeLib_Release(lib); CoUninitialize(); return 0;
}
