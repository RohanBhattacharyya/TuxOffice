/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Extended Linguistic Services script detection, as Office's RichEdit and
 * Outlook use it, and GetStringScripts, which shares its Unicode table.
 * Build: winegcc -b x86_64-windows -mconsole -I$BD/include -I wine/include
 *        -L$BD/dlls/kernel32/x86_64-windows -o els-script-detection.exe els-script-detection.c */
#include <windows.h>
#include <stdio.h>
#include <elscore.h>
#include <elssrvc.h>

static int failures;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static HRESULT (WINAPI *pMappingGetServices)(MAPPING_ENUM_OPTIONS *, MAPPING_SERVICE_INFO **, DWORD *);
static HRESULT (WINAPI *pMappingFreeServices)(MAPPING_SERVICE_INFO *);
static HRESULT (WINAPI *pMappingRecognizeText)(MAPPING_SERVICE_INFO *, const WCHAR *, DWORD, DWORD,
        MAPPING_OPTIONS *, MAPPING_PROPERTY_BAG *);
static HRESULT (WINAPI *pMappingFreePropertyBag)(MAPPING_PROPERTY_BAG *);

/* "0-5 Latn;6-10 Cyrl;" from a bag. */
static void describe(const MAPPING_PROPERTY_BAG *bag, char *out)
{
    DWORD i;
    *out = 0;
    for (i = 0; i < bag->dwRangesCount; i++)
    {
        const MAPPING_DATA_RANGE *r = &bag->prgResultRanges[i];
        out += sprintf(out, "%lu-%lu %ls;", r->dwStartIndex, r->dwEndIndex, (const WCHAR *)r->pData);
    }
}

static void expect(MAPPING_SERVICE_INFO *service, const char *name, const WCHAR *text, DWORD index,
        const char *expected)
{
    MAPPING_PROPERTY_BAG bag = {sizeof(bag)};
    char got[512], what[700];
    HRESULT hr = pMappingRecognizeText(service, text, wcslen(text), index, NULL, &bag);

    if (FAILED(hr)) sprintf(got, "hr %#lx", hr);
    else describe(&bag, got);
    sprintf(what, "%s from %lu: %s (got %s)", name, index, expected, got);
    check(!strcmp(got, expected), what);
    pMappingFreePropertyBag(&bag);
}

struct async { HANDLE done; HRESULT hr; char got[256]; };
static void CALLBACK recognized(MAPPING_PROPERTY_BAG *bag, void *data, DWORD size, HRESULT hr)
{
    struct async *async = data;
    async->hr = hr;
    if (SUCCEEDED(hr)) describe(bag, async->got);
    SetEvent(async->done);
}

int main(void)
{
    HMODULE els = LoadLibraryA("elscore.dll");
    MAPPING_ENUM_OPTIONS enum_options = {sizeof(enum_options)};
    MAPPING_OPTIONS options = {sizeof(options)};
    MAPPING_PROPERTY_BAG bag = {sizeof(bag)};
    MAPPING_SERVICE_INFO *services = NULL, *service;
    struct async async = {0};
    WCHAR scripts[64];
    DWORD count = 0;
    GUID guid = ELS_GUID_SCRIPT_DETECTION, language = ELS_GUID_LANGUAGE_DETECTION;
    HRESULT hr;

    check(els != NULL, "elscore loads");
    if (!els) return 1;
    pMappingGetServices = (void *)GetProcAddress(els, "MappingGetServices");
    pMappingFreeServices = (void *)GetProcAddress(els, "MappingFreeServices");
    pMappingRecognizeText = (void *)GetProcAddress(els, "MappingRecognizeText");
    pMappingFreePropertyBag = (void *)GetProcAddress(els, "MappingFreePropertyBag");

    enum_options.pGuid = &guid;
    hr = pMappingGetServices(&enum_options, &services, &count);
    check(hr == S_OK && count == 1, "the script detection service is found by GUID");
    if (hr != S_OK || !count) return 1;
    service = services;
    check(!wcscmp(service->pszCategory, L"Script Detection"), "in the Script Detection category");

    enum_options.pGuid = NULL;
    enum_options.pszCategory = (WCHAR *)L"script detection";
    {
        MAPPING_SERVICE_INFO *found = NULL; DWORD n = 0;
        hr = pMappingGetServices(&enum_options, &found, &n);
        check(hr == S_OK && n == 1, "and by category");
        if (found) pMappingFreeServices(found);
        enum_options.pszCategory = NULL;
        enum_options.pGuid = &language;
        hr = pMappingGetServices(&enum_options, &found, &n);
        check(hr == S_OK && !n, "a service that is not there is not listed");
    }

    expect(service, "latin", L"Hello world", 0, "0-10 Latn;");
    expect(service, "latin then cyrillic", L"Hello, \x043c\x0438\x0440!", 0, "0-6 Latn;7-10 Cyrl;");
    expect(service, "han after digits, then latin", L"123 \x65e5\x672c\x8a9e abc", 0, "0-7 Hani;8-10 Latn;");
    expect(service, "hiragana then katakana", L"\x3053\x3093\x306b\x3061\x306f \x30ab\x30bf", 0, "0-5 Hira;6-7 Kana;");
    expect(service, "hebrew then arabic", L"\x05e9\x05dc\x05d5\x05dd \x0645\x0631\x062d\x0628\x0627", 0, "0-4 Hebr;5-9 Arab;");
    expect(service, "combining accents", L"e\x0301t\x00e9 \x03b1\x03b2", 0, "0-4 Latn;5-6 Grek;");
    expect(service, "supplementary planes", L"\xd835\xdc00 x \xd800\xdf30", 0, "0-4 Latn;5-6 Goth;");
    expect(service, "only common characters", L"12, 34.", 0, "0-6 ;");
    expect(service, "from an index", L"abc \x043c\x0438\x0440", 4, "4-6 Cyrl;");
    {
        MAPPING_PROPERTY_BAG short_bag = {sizeof(short_bag) - 1};
        check(pMappingRecognizeText(service, L"abc", 3, 0, NULL, &short_bag) == E_INVALIDARG, "a bag of the wrong size is refused");
        check(pMappingRecognizeText(service, L"abc", 3, 3, NULL, &bag) == E_INVALIDARG, "an index past the text is refused");
    }

    async.done = CreateEventW(NULL, FALSE, FALSE, NULL);
    options.pfnRecognizeCallback = recognized;
    options.pRecognizeCallerData = &async;
    options.dwRecognizeCallerDataSize = sizeof(async);
    hr = pMappingRecognizeText(service, L"Grüße \x0393\x03b5\x03b9\x03b1", 10, 0, &options, &bag);
    check(hr == S_OK, "recognition starts asynchronously");
    check(WaitForSingleObject(async.done, 5000) == WAIT_OBJECT_0 && async.hr == S_OK, "the callback reports the result");
    check(!strcmp(async.got, "0-5 Latn;6-9 Grek;"), "German then Greek");
    check(bag.pCallerData == &async, "the bag carries the caller data");
    pMappingFreePropertyBag(&bag);
    check(pMappingFreeServices(services) == S_OK, "the services are freed");

    count = GetStringScripts(0, L"Hello \x043c\x0438\x0440 \xd800\xdf30 \x0301 12", -1, scripts, 64);
    check(count && !wcscmp(scripts, L"Cyrl;Goth;Latn;"), "GetStringScripts names each script once");
    count = GetStringScripts(GSS_ALLOW_INHERITED_COMMON, L"a\x0301 1", -1, scripts, 64);
    check(count && !wcscmp(scripts, L"Latn;Qaai;Zyyy;"), "and common and inherited ones when asked");

    printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures != 0;
}
