/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* OneNote object-model test on a local notebook: hierarchy, sections, pages,
 * content round trip, search, PDF. A C program because OneNote's methods have
 * typed [out] string parameters, which VBScript cannot pass on Windows either.
 *
 * Usage: onenote-api.exe C:\work\dir
 * Build: winegcc -b x86_64-windows -mconsole -municode -o onenote-api.exe onenote-api.c
 *        -lole32 -loleaut32 -luuid
 */
#define COBJMACROS
#include <stdio.h>
#include <windows.h>
#include <oaidl.h>

static IDispatch *app;
static int passes, failures;

static void check(const char *name, HRESULT hr, BOOL ok)
{
    if (SUCCEEDED(hr) && ok) { printf("PASS %s\n", name); ++passes; }
    else { printf("FAIL %s hr=%#lx\n", name, hr); ++failures; }
    fflush(stdout);
}

/* Arguments are given in reverse of the method's order, as IDispatch wants them. */
static HRESULT invoke(const WCHAR *name, VARIANT *args, UINT count)
{
    DISPPARAMS params = {args, NULL, count, 0};
    OLECHAR *names[1] = {(OLECHAR *)name};
    EXCEPINFO info = {0};
    DISPID id;
    UINT error;
    HRESULT hr;

    if (FAILED(hr = IDispatch_GetIDsOfNames(app, &IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id))) return hr;
    hr = IDispatch_Invoke(app, id, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &params, NULL, &info, &error);
    if (hr == DISP_E_EXCEPTION && info.scode) hr = info.scode;
    SysFreeString(info.bstrSource); SysFreeString(info.bstrDescription); SysFreeString(info.bstrHelpFile);
    return hr;
}

static void set_bstr(VARIANT *v, const WCHAR *text) { V_VT(v) = VT_BSTR; V_BSTR(v) = SysAllocString(text); }
static void set_i4(VARIANT *v, LONG value) { V_VT(v) = VT_I4; V_I4(v) = value; }
static void set_out(VARIANT *v, BSTR *out) { *out = NULL; V_VT(v) = VT_BSTR | VT_BYREF; V_BSTRREF(v) = out; }

int wmain(int argc, WCHAR **argv)
{
    WCHAR path[MAX_PATH], xml[2048];
    BSTR notebook, section, page, text, results;
    VARIANT a[5];
    CLSID clsid;
    HRESULT hr;

    if (argc < 2) return 1;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = CLSIDFromProgID(L"OneNote.Application", &clsid);
    if (SUCCEEDED(hr)) hr = CoCreateInstance(&clsid, NULL, CLSCTX_LOCAL_SERVER, &IID_IDispatch, (void **)&app);
    check("create OneNote.Application", hr, app != NULL);
    if (!app) return 2;

    /* GetHierarchy(start, scope, [out] xml, schema) */
    set_i4(&a[0], 2); set_out(&a[1], &text); set_i4(&a[2], 4); set_bstr(&a[3], L"");
    hr = invoke(L"GetHierarchy", a, 4);
    check("GetHierarchy", hr, text && wcsstr(text, L"Notebooks"));
    printf("hierarchy %u characters\n", text ? SysStringLen(text) : 0);

    /* OpenHierarchy(path, relativeTo, [out] id, createType) */
    swprintf(path, MAX_PATH, L"%ls\\TuxNotebook", argv[1]);
    set_i4(&a[0], 1 /* cftNotebook */); set_out(&a[1], &notebook); set_bstr(&a[2], L""); set_bstr(&a[3], path);
    hr = invoke(L"OpenHierarchy", a, 4);
    check("create local notebook", hr, notebook != NULL);
    set_i4(&a[0], 3 /* cftSection */); set_out(&a[1], &section); set_bstr(&a[2], notebook ? notebook : L"");
    set_bstr(&a[3], L"Test.one");
    hr = invoke(L"OpenHierarchy", a, 4);
    check("create section", hr, section != NULL);

    /* CreateNewPage(section, [out] page, style) */
    set_i4(&a[0], 0); set_out(&a[1], &page); set_bstr(&a[2], section ? section : L"");
    hr = invoke(L"CreateNewPage", a, 3);
    check("create page", hr, page != NULL);

    swprintf(xml, ARRAYSIZE(xml), L"<?xml version=\"1.0\"?><one:Page xmlns:one=\"http://schemas.microsoft.com/office/onenote/2013/onenote\" ID=\"%ls\">"
             L"<one:Title><one:OE><one:T><![CDATA[TuxOffice page]]></one:T></one:OE></one:Title><one:Outline><one:OEChildren><one:OE>"
             L"<one:T><![CDATA[Body text from a test]]></one:T></one:OE></one:OEChildren></one:Outline></one:Page>", page ? page : L"");
    /* UpdatePageContent(xml, lastModified, schema, force) */
    V_VT(&a[0]) = VT_BOOL; V_BOOL(&a[0]) = VARIANT_FALSE; set_i4(&a[1], 2);
    V_VT(&a[2]) = VT_DATE; V_DATE(&a[2]) = 0; set_bstr(&a[3], xml);
    hr = invoke(L"UpdatePageContent", a, 4);
    check("update page", hr, TRUE);

    /* GetPageContent(page, [out] xml, info, schema) */
    set_i4(&a[0], 2); set_i4(&a[1], 0); set_out(&a[2], &text); set_bstr(&a[3], page ? page : L"");
    hr = invoke(L"GetPageContent", a, 4);
    check("read page back", hr, text && wcsstr(text, L"Body text from a test"));

    /* NavigateTo(hierarchy, objectId, newWindow) */
    V_VT(&a[0]) = VT_BOOL; V_BOOL(&a[0]) = VARIANT_FALSE; set_bstr(&a[1], L""); set_bstr(&a[2], page ? page : L"");
    hr = invoke(L"NavigateTo", a, 3);
    check("navigate", hr, TRUE);
    Sleep(2500);

    /* FindPages(start, query, [out] xml, includeUnindexed, display, schema) */
    {
        static const struct { BOOL notebook; VARIANT_BOOL unindexed; } tries[] =
        {
            {FALSE, VARIANT_FALSE}, {TRUE, VARIANT_FALSE}, {TRUE, VARIANT_TRUE},
        };
        OLECHAR *names[1] = {(OLECHAR *)L"FindPages"};
        unsigned int i;
        DISPID id;

        for (i = 0; i < sizeof(tries) / sizeof(tries[0]); ++i)
        {
            VARIANT b[6];
            DISPPARAMS params = {b, NULL, 6, 0};
            EXCEPINFO info = {0};
            UINT error;

            set_i4(&b[0], 2);
            V_VT(&b[1]) = VT_BOOL; V_BOOL(&b[1]) = VARIANT_FALSE;
            V_VT(&b[2]) = VT_BOOL; V_BOOL(&b[2]) = tries[i].unindexed;
            set_out(&b[3], &results);
            set_bstr(&b[4], L"Body");
            set_bstr(&b[5], tries[i].notebook ? (const WCHAR *)notebook : L"");
            hr = IDispatch_GetIDsOfNames(app, &IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id);
            if (SUCCEEDED(hr))
                hr = IDispatch_Invoke(app, id, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &params, NULL, &info, &error);
            printf("INFO FindPages notebook=%d unindexed=%d hr=%#lx scode=%#lx found=%d\n", tries[i].notebook,
                    tries[i].unindexed != VARIANT_FALSE, hr, info.scode, results && wcsstr(results, L"<one:Page") != NULL);
            SysFreeString(info.bstrSource); SysFreeString(info.bstrDescription); SysFreeString(info.bstrHelpFile);
            if (SUCCEEDED(hr)) break;
        }
        check("search", hr, TRUE);
    }

    /* Publish(hierarchy, target, format, formatDetails) */
    swprintf(path, MAX_PATH, L"%ls\\onenote-page.pdf", argv[1]);
    DeleteFileW(path);
    set_bstr(&a[0], L""); set_i4(&a[1], 3 /* pfPDF */); set_bstr(&a[2], path); set_bstr(&a[3], page ? page : L"");
    hr = invoke(L"Publish", a, 4);
    check("publish PDF", hr, GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES);

    printf("RESULT onenote passed=%d failed=%d\n", passes, failures);
    IDispatch_Release(app);
    CoUninitialize();
    return failures != 0;
}
