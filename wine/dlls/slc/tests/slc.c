/*
 * Copyright 2014 Sebastian Lackner
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

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "slpublic.h"
#include "slerror.h"

#include <wine/test.h>

static const SLID test_license_id = {0x01234567, 0x89ab, 0xcdef, {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};
static const SLID test_private_license_id = {0xfedcba98, 0x7654, 0x3210, {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10}};

static void test_SLInstallLicense(void)
{
    static const BYTE license[] =
        "<rg:licenseGroup><r:license licenseId=\"{01234567-89ab-cdef-0123-456789abcdef}\">"
        "<tm:infoStr name=\"licenseType\">msft:sl/EUL/GENERIC/PUBLIC</tm:infoStr>"
        "<tm:infoStr name=\"licenseVersion\">2.0</tm:infoStr>"
        "<tm:infoStr name=\"productDescription\">Public license</tm:infoStr></r:license>"
        "<r:license licenseId=\"{fedcba98-7654-3210-fedc-ba9876543210}\">"
        "<tm:infoStr name=\"licenseType\">msft:sl/EUL/GENERIC/PRIVATE</tm:infoStr></r:license>"
        "</rg:licenseGroup>";
    WCHAR system[MAX_PATH], path[MAX_PATH];
    SLDATATYPE type;
    BYTE buffer[sizeof(license)];
    BYTE *value;
    DWORD read;
    UINT count, size;
    HANDLE file;
    HRESULT hr;
    HSLC handle;
    SLID id, *ids;

    hr = SLOpen(&handle);
    ok(hr == S_OK, "got %#lx\n", hr);
    if (FAILED(hr)) return;

    hr = SLInstallLicense(NULL, sizeof(license) - 1, license, &id);
    ok(hr == E_INVALIDARG, "got %#lx\n", hr);
    hr = SLInstallLicense(handle, 0, license, &id);
    ok(hr == E_INVALIDARG, "got %#lx\n", hr);
    hr = SLInstallLicense(handle, sizeof(license) - 1, NULL, &id);
    ok(hr == E_INVALIDARG, "got %#lx\n", hr);
    hr = SLInstallLicense(handle, sizeof(license) - 1, license, NULL);
    ok(hr == E_INVALIDARG, "got %#lx\n", hr);

    if (!winetest_platform_is_wine) return;

    memset(&id, 0xcc, sizeof(id));
    hr = SLInstallLicense(handle, sizeof(license) - 1, license, &id);
    ok(hr == S_OK, "got %#lx\n", hr);
    ok(IsEqualGUID(&id, &test_license_id), "got %s\n", wine_dbgstr_guid(&id));

    count = 0xdeadbeef;
    ids = (SLID *)0xdeadbeef;
    hr = SLGetSLIDList(handle, SL_ID_LICENSE_FILE, &id, SL_ID_LICENSE, &count, &ids);
    ok(hr == S_OK, "got %#lx\n", hr);
    ok(count == 2, "got %u ids\n", count);
    ok(ids && IsEqualGUID(ids, &test_license_id), "got %s\n", wine_dbgstr_guid(ids));
    ok(ids && count > 1 && IsEqualGUID(ids + 1, &test_private_license_id), "got %s\n",
       ids && count > 1 ? wine_dbgstr_guid(ids + 1) : "no id");
    LocalFree(ids);

    type = SL_DATA_NONE;
    size = 0;
    value = NULL;
    hr = SLGetLicenseInformation(handle, &test_license_id, L"LicenseType", &type, &size, &value);
    ok(hr == S_OK, "got %#lx\n", hr);
    ok(type == SL_DATA_SZ, "got type %u\n", type);
    ok(size == sizeof(L"msft:sl/EUL/GENERIC/PUBLIC"), "got size %u\n", size);
    ok(value && !wcscmp((WCHAR *)value, L"msft:sl/EUL/GENERIC/PUBLIC"), "got %s\n", wine_dbgstr_w((WCHAR *)value));
    LocalFree(value);

    value = NULL;
    hr = SLGetLicenseInformation(handle, &test_license_id, L"Version", &type, &size, &value);
    ok(hr == S_OK, "got %#lx\n", hr);
    ok(type == SL_DATA_SZ, "got type %u\n", type);
    ok(value && !wcscmp((WCHAR *)value, L"2.0"), "got %s\n", wine_dbgstr_w((WCHAR *)value));
    LocalFree(value);

    value = NULL;
    hr = SLGetLicenseInformation(handle, &test_license_id, L"Description", &type, &size, &value);
    ok(hr == S_OK, "got %#lx\n", hr);
    ok(type == SL_DATA_SZ, "got type %u\n", type);
    ok(value && !wcscmp((WCHAR *)value, L"Public license"), "got %s\n", wine_dbgstr_w((WCHAR *)value));
    LocalFree(value);

    type = SL_DATA_NONE;
    size = 0;
    value = NULL;
    hr = SLGetLicenseInformation(handle, &test_private_license_id, L"LicenseType", &type, &size, &value);
    ok(hr == S_OK, "got %#lx\n", hr);
    ok(type == SL_DATA_SZ, "got type %u\n", type);
    ok(value && !wcscmp((WCHAR *)value, L"msft:sl/EUL/GENERIC/PRIVATE"), "got %s\n", wine_dbgstr_w((WCHAR *)value));
    LocalFree(value);

    hr = SLGetLicenseInformation(handle, &test_license_id, L"Missing", &type, &size, &value);
    ok(hr == SL_E_VALUE_NOT_FOUND, "got %#lx\n", hr);

    GetSystemDirectoryW(system, ARRAY_SIZE(system));
    swprintf(path, ARRAY_SIZE(path), L"%s\\spp\\store\\2.0\\licenses\\{01234567-89AB-CDEF-0123-456789ABCDEF}.xrm-ms", system);
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(file != INVALID_HANDLE_VALUE, "failed to open %s, error %lu\n", wine_dbgstr_w(path), GetLastError());
    if (file != INVALID_HANDLE_VALUE)
    {
        ok(ReadFile(file, buffer, sizeof(buffer), &read, NULL), "error %lu\n", GetLastError());
        ok(read == sizeof(license) - 1, "got %lu bytes\n", read);
        ok(!memcmp(buffer, license, sizeof(license) - 1), "stored blob differs\n");
        CloseHandle(file);
        DeleteFileW(path);
    }

    SLClose(handle);
}

static void test_SLGetWindowsInformationDWORD(void)
{
    DWORD value;
    HRESULT res;

    res = SLGetWindowsInformationDWORD(L"Nonexistent-License-Value", NULL);
    ok(res == E_INVALIDARG, "expected E_INVALIDARG, got %08lx\n", res);

    res = SLGetWindowsInformationDWORD(NULL, &value);
    ok(res == E_INVALIDARG, "expected E_INVALIDARG, got %08lx\n", res);

    value = 0xdeadbeef;
    res = SLGetWindowsInformationDWORD(L"Nonexistent-License-Value", &value);
    ok(res == SL_E_VALUE_NOT_FOUND, "expected SL_E_VALUE_NOT_FOUND, got %08lx\n", res);
    ok(value == 0xdeadbeef, "expected value = 0xdeadbeef, got %lu\n", value);

    value = 0xdeadbeef;
    res = SLGetWindowsInformationDWORD(L"", &value);
    ok(res == SL_E_RIGHT_NOT_GRANTED || broken(res == 0xd000000d) /* Win 8 */,
       "expected SL_E_RIGHT_NOT_GRANTED, got %08lx\n", res);
    ok(value == 0xdeadbeef, "expected value = 0xdeadbeef, got %lu\n", value);

    value = 0xdeadbeef;
    res = SLGetWindowsInformationDWORD(L"Kernel-MUI-Language-Allowed", &value);
    ok(res == SL_E_DATATYPE_MISMATCHED, "expected SL_E_DATATYPE_MISMATCHED, got %08lx\n", res);
    ok(value == 0xdeadbeef, "expected value = 0xdeadbeef, got %lu\n", value);

    value = 0xdeadbeef;
    res = SLGetWindowsInformationDWORD(L"Kernel-MUI-Number-Allowed", &value);
    ok(res == S_OK, "expected S_OK, got %lu\n", res);
    ok(value != 0xdeadbeef, "expected value != 0xdeadbeef\n");
}

static void test_product_policy_guid(void)
{
    static const SLID app = {0xaabbccdd,0x1234,0x5678,{0x90,0xab,0xcd,0xef,0x01,0x23,0x45,0x67}};
    static const SLID product = {0x10203040,0x5060,0x7080,{0x90,0xa0,0xb0,0xc0,0xd0,0xe0,0xf0,0x00}};
    static const char user_license[] =
        "<r:license licenseId=\"{abcdef01-1234-5678-90ab-cdef01234567}\">"
        "<tm:infoStr name=\"licenseType\">msft:sl/grace-period</tm:infoStr>"
        "<tm:infoStr name=\"applicationId\">{aabbccdd-1234-5678-90ab-cdef01234567}</tm:infoStr>"
        "<tm:infoStr name=\"productSkuId\">{10203040-5060-7080-90a0-b0c0d0e0f000}</tm:infoStr>"
        "<sl:duration>P30D</sl:duration></r:license>";
    static const char policy_format[] =
        "<r:license licenseId=\"{abcdef02-1234-5678-90ab-cdef01234567}\">"
        "<r:title>%s PPD License</r:title>"
        "<tm:infoStr name=\"licenseType\">msft:sl/PPD</tm:infoStr>"
        "<tm:infoStr name=\"applicationId\">{aabbccdd-1234-5678-90ab-cdef01234567}</tm:infoStr>"
        "<sl:policyInt name=\"Test-Policy\">7</sl:policyInt></r:license>";
    static const char *titles[] =
    {
        "Product10203040-5060-7080-90A0-B0C0D0E0F000",
        "Product{10203040-5060-7080-90A0-B0C0D0E0F000}"
    };
    HRESULT (WINAPI *consume)(HSLC,const SLID *,const SLID *,PCWSTR,void *);
    HRESULT (WINAPI *get_policy)(HSLC,PCWSTR,SLDATATYPE *,UINT *,BYTE **);
    WCHAR system[MAX_PATH], path[MAX_PATH];
    HMODULE module;
    char policy[1024];
    HSLC context;
    SLID id;
    HRESULT hr;
    SLDATATYPE type;
    UINT size, i;
    BYTE *value;

    /* The Wine implementation accepts unsigned fixture licenses. */
    if (!winetest_platform_is_wine) return;
    module = LoadLibraryW(L"sppc.dll");
    consume = (void *)GetProcAddress(module, "SLConsumeRight");
    get_policy = (void *)GetProcAddress(module, "SLGetPolicyInformation");
    if (!consume || !get_policy) { win_skip("SPP policy functions unavailable.\n"); goto unload; }
    hr = SLOpen(&context);
    ok(hr == S_OK, "got %#lx\n", hr);
    if (FAILED(hr)) goto unload;
    hr = SLInstallLicense(context, sizeof(user_license) - 1, (const BYTE *)user_license, &id);
    ok(hr == S_OK, "got %#lx\n", hr);
    hr = consume(context, &app, &product, NULL, NULL);
    ok(hr == S_OK, "got %#lx\n", hr);
    for (i = 0; i < ARRAY_SIZE(titles); i++)
    {
        sprintf(policy, policy_format, titles[i]);
        hr = SLInstallLicense(context, strlen(policy), (const BYTE *)policy, &id);
        ok(hr == S_OK, "got %#lx\n", hr);
        value = NULL;
        size = 0;
        type = SL_DATA_NONE;
        hr = get_policy(context, L"Test-Policy", &type, &size, &value);
        ok(hr == S_OK, "title %s: got %#lx\n", titles[i], hr);
        ok(type == SL_DATA_DWORD, "got type %u\n", type);
        ok(size == sizeof(DWORD), "got size %u\n", size);
        ok(value && *(DWORD *)value == 7, "unexpected policy value\n");
        LocalFree(value);
    }
    SLClose(context);
    GetSystemDirectoryW(system, ARRAY_SIZE(system));
    swprintf(path, ARRAY_SIZE(path), L"%s\\spp\\store\\2.0\\licenses\\{ABCDEF01-1234-5678-90AB-CDEF01234567}.xrm-ms", system);
    DeleteFileW(path);
    swprintf(path, ARRAY_SIZE(path), L"%s\\spp\\store\\2.0\\licenses\\{ABCDEF02-1234-5678-90AB-CDEF01234567}.xrm-ms", system);
    DeleteFileW(path);
unload:
    FreeLibrary(module);
}


START_TEST(slc)
{
    test_SLInstallLicense();
    test_SLGetWindowsInformationDWORD();
    test_product_policy_guid();
}
