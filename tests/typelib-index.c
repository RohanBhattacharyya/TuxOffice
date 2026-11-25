/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A public type index is independent of the description's segment offset.
 * Reorder a self-created MSFT library and retain references between its types. */
#define COBJMACROS
#include <windows.h>
#include <oleauto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks, failures;
#define CHECK(test) do { ++checks; if (!(test)) { ++failures; \
    printf("FAIL line %u: %s\n", __LINE__, #test); } } while (0)
#define REQUIRE(call) do { HRESULT hr_ = (call); CHECK(hr_ == S_OK); \
    if (hr_ != S_OK) { printf("HRESULT %#lx\n", hr_); return 1; } } while (0)

static int exercise(BOOL help)
{
    static WCHAR names[][7] = {L"First", L"Second", L"Third"};
    static const unsigned int order[] = {2, 0, 1};
    WCHAR directory[MAX_PATH], path[MAX_PATH];
    ICreateTypeLib2 *create;
    ICreateTypeInfo *types[3];
    ITypeLib *lib, *owner;
    ITypeInfo *info, *target;
    TYPEATTR *attr;
    TYPEDESC desc = {0};
    VARDESC var = {0}, *readvar;
    VARIANT value;
    HREFTYPE href;
    GUID guid;
    BSTR name;
    UINT index, i, original[3], table, segment;
    DWORD length, done, *data;
    HANDLE file;
    HRESULT hr;

    GetTempPathW(MAX_PATH, directory);
    GetTempFileNameW(directory, L"idx", 0, path);
    REQUIRE(CreateTypeLib2(sizeof(void *) == 8 ? SYS_WIN64 : SYS_WIN32, path, &create));
    CLSIDFromString(L"{7A193214-870B-459A-96EA-7613E4635761}", &guid);
    REQUIRE(ICreateTypeLib2_SetGuid(create, &guid));
    REQUIRE(ICreateTypeLib2_SetName(create, L"IndexTest"));
    if (help) REQUIRE(ICreateTypeLib2_SetHelpStringDll(create, L"index-help.dll"));
    REQUIRE(ICreateTypeLib2_CreateTypeInfo(create, names[0], TKIND_ENUM, &types[0]));
    REQUIRE(ICreateTypeLib2_CreateTypeInfo(create, names[1], TKIND_ALIAS, &types[1]));
    REQUIRE(ICreateTypeLib2_CreateTypeInfo(create, names[2], TKIND_RECORD, &types[2]));
    for (i = 0; i < 3; ++i)
    {
        ++guid.Data1;
        REQUIRE(ICreateTypeInfo_SetGuid(types[i], &guid));
    }
    VariantInit(&value);
    V_VT(&value) = VT_I4;
    V_I4(&value) = 42;
    var.memid = 42;
    var.varkind = VAR_CONST;
    var.lpvarValue = &value;
    var.elemdescVar.tdesc.vt = VT_I4;
    REQUIRE(ICreateTypeInfo_AddVarDesc(types[0], 0, &var));
    REQUIRE(ICreateTypeInfo_SetVarName(types[0], 0, L"Answer"));
    REQUIRE(ICreateTypeInfo_QueryInterface(types[0], &IID_ITypeInfo, (void **)&info));
    REQUIRE(ICreateTypeInfo_AddRefTypeInfo(types[1], info, &href));
    ITypeInfo_Release(info);
    desc.vt = VT_USERDEFINED;
    desc.hreftype = href;
    REQUIRE(ICreateTypeInfo_SetTypeDescAlias(types[1], &desc));
    REQUIRE(ICreateTypeInfo_QueryInterface(types[1], &IID_ITypeInfo, (void **)&info));
    REQUIRE(ICreateTypeInfo_AddRefTypeInfo(types[2], info, &href));
    ITypeInfo_Release(info);
    memset(&var, 0, sizeof(var));
    var.memid = 43;
    var.varkind = VAR_PERINSTANCE;
    var.elemdescVar.tdesc.vt = VT_USERDEFINED;
    var.elemdescVar.tdesc.hreftype = href;
    REQUIRE(ICreateTypeInfo_AddVarDesc(types[2], 0, &var));
    REQUIRE(ICreateTypeInfo_SetVarName(types[2], 0, L"Field"));
    REQUIRE(ICreateTypeLib2_SaveAllChanges(create));
    for (i = 0; i < 3; ++i) ICreateTypeInfo_Release(types[i]);
    ICreateTypeLib2_Release(create);

    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) return 1;
    length = GetFileSize(file, NULL);
    data = malloc(length);
    CHECK(data != NULL);
    if (!data) return 1;
    CHECK(ReadFile(file, data, length, &done, NULL) && done == length);
    CHECK(data[0] == 0x5446534d && data[8] == 3);
    /* MSFT_Header is 84 bytes; the optional help-string DLL offset precedes
     * the type index table. The directory's first entry locates descriptions. */
    table = 21 + !!(data[5] & 0x100);
    segment = data[table + 3] / sizeof(DWORD);
    memcpy(original, data + table, sizeof(original));
    for (i = 0; i < 3; ++i)
    {
        data[table + i] = original[order[i]];
        index = segment + original[order[i]] / sizeof(DWORD);
        data[index] = (data[index] & 0xffff) | (i << 16);
    }
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    CHECK(WriteFile(file, data, length, &done, NULL) && done == length);
    CloseHandle(file);
    REQUIRE(LoadTypeLibEx(path, REGKIND_NONE, &lib));
    CHECK(ITypeLib_GetTypeInfoCount(lib) == 3);
    for (i = 0; i < 3; ++i)
    {
        REQUIRE(ITypeLib_GetTypeInfo(lib, i, &info));
        REQUIRE(ITypeInfo_GetDocumentation(info, MEMBERID_NIL, &name, NULL, NULL, NULL));
        CHECK(!lstrcmpW(name, names[order[i]]));
        SysFreeString(name);
        REQUIRE(ITypeInfo_GetContainingTypeLib(info, &owner, &index));
        CHECK(index == i);
        ITypeLib_Release(owner);
        REQUIRE(ITypeInfo_GetTypeAttr(info, &attr));
        if (attr->typekind == TKIND_ALIAS)
        {
            CHECK(attr->tdescAlias.vt == VT_USERDEFINED);
            REQUIRE(ITypeInfo_GetRefTypeInfo(info, attr->tdescAlias.hreftype, &target));
            REQUIRE(ITypeInfo_GetDocumentation(target, MEMBERID_NIL, &name, NULL, NULL, NULL));
            CHECK(!lstrcmpW(name, L"First"));
            SysFreeString(name);
            ITypeInfo_Release(target);
        }
        if (attr->typekind == TKIND_RECORD)
        {
            REQUIRE(ITypeInfo_GetVarDesc(info, 0, &readvar));
            REQUIRE(ITypeInfo_GetRefTypeInfo(info, readvar->elemdescVar.tdesc.hreftype, &target));
            REQUIRE(ITypeInfo_GetDocumentation(target, MEMBERID_NIL, &name, NULL, NULL, NULL));
            CHECK(!lstrcmpW(name, L"Second"));
            SysFreeString(name);
            ITypeInfo_Release(target);
            ITypeInfo_ReleaseVarDesc(info, readvar);
        }
        ITypeInfo_ReleaseTypeAttr(info, attr);
        ITypeInfo_Release(info);
    }
    ITypeLib_Release(lib);
    DeleteFileW(path);
    /* Invalid offsets must fail instead of being ignored or dereferenced. */
    for (i = 0; i < 2; ++i)
    {
        GetTempFileNameW(directory, L"bad", 0, path);
        data[table] = i ? 0x7fffffff : 0xffffffff;
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        CHECK(file != INVALID_HANDLE_VALUE);
        CHECK(WriteFile(file, data, length, &done, NULL) && done == length);
        CloseHandle(file);
        hr = LoadTypeLibEx(path, REGKIND_NONE, &lib);
        CHECK(hr == TYPE_E_CANTLOADLIBRARY);
        if (SUCCEEDED(hr)) ITypeLib_Release(lib);
        DeleteFileW(path);
    }
    free(data);
    return 0;
}

int main(void)
{
    REQUIRE(CoInitialize(NULL));
    if (exercise(FALSE) || exercise(TRUE)) return 1;
    CoUninitialize();
    printf("Type library index table: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
