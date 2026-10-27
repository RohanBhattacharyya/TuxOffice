/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Word automation probe for an isolated test prefix. Build with winegcc -b x86_64-windows
 * -mconsole -o office-probe.exe office-probe.c -loleaut32 -lole32 -loleacc -luser32 -luuid.
 * word-point [text]: wait for proofing, find text and report its screen bounds.
 * word-click-point [text]: report the center in the input helper's logical coordinates.
 * word-text: print the active document's text as UTF-8.
 * word-save-as PATH: save the active document to a new DOCX file.
 * word-quit: close the test Word instance without saving unsaved changes.
 */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "word-window.h"
#include <oleauto.h>
#include <oleacc.h>

static BOOL CALLBACK find_document_window(HWND window, LPARAM result)
{
    WCHAR name[64];
    GetClassNameW(window, name, 64);
    if (wcscmp(name, !wcscmp(office_window_class,L"XLMAIN") ? L"EXCEL7" : L"_WwG")) return TRUE;
    *(HWND *)result = window;
    return FALSE;
}

static HRESULT invoke(IDispatch *object, WCHAR *name, WORD flags, VARIANT *args, UINT count, VARIANT *result)
{
    DISPID id;
    DISPPARAMS params = {args, NULL, count, 0};
    EXCEPINFO exception = {0};
    DISPID put = DISPID_PROPERTYPUT;
    if (flags & DISPATCH_PROPERTYPUT) { params.rgdispidNamedArgs=&put; params.cNamedArgs=1; }
    HRESULT hr;
    VariantInit(result);
    hr = IDispatch_GetIDsOfNames(object, &IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &id);
    if (SUCCEEDED(hr)) hr = IDispatch_Invoke(object, id, &IID_NULL, LOCALE_USER_DEFAULT,
            flags, &params, result, &exception, NULL);
    if (FAILED(hr)) fprintf(stderr, "%ls failed: %08lx (%08lx)\n", name, hr, exception.scode);
    SysFreeString(exception.bstrSource);
    SysFreeString(exception.bstrDescription);
    SysFreeString(exception.bstrHelpFile);
    return hr;
}

static IDispatch *property(IDispatch *object, WCHAR *name)
{
    VARIANT value;
    if (FAILED(invoke(object, name, DISPATCH_PROPERTYGET, NULL, 0, &value))) return NULL;
    if (V_VT(&value) == VT_DISPATCH) return V_DISPATCH(&value);
    VariantClear(&value);
    return NULL;
}

int main(int argc, char **argv)
{
    IUnknown *unknown;
    IDispatch *word = NULL, *doc = NULL, *range = NULL, *find = NULL, *window = NULL, *errors = NULL;
    CLSID clsid;
    VARIANT args[5], result;
    LONG x = 0, y = 0, width = 0, height = 0;
    WCHAR text[256];
    HRESULT hr;
    int status = 1, i;
    if (argc < 2) return 2;
    if (strcmp(argv[1], "word-point") && strcmp(argv[1], "word-click-point") &&
        strcmp(argv[1], "word-text") && strcmp(argv[1], "word-save-as") && strcmp(argv[1], "word-quit") && strcmp(argv[1], "word-open") &&
        strcmp(argv[1], "word-new-benchmark") && strcmp(argv[1], "excel-chart-check") && strcmp(argv[1], "excel-zoom") && strcmp(argv[1], "excel-quit") &&
        strcmp(argv[1], "powerpoint-slide-count") && strcmp(argv[1], "powerpoint-quit")) return 2;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!strncmp(argv[1], "excel-", 6)) office_window_class=L"XLMAIN";
    if (!strncmp(argv[1], "powerpoint-", 11)) office_window_class=L"PPTFrameClass";
    CLSIDFromProgID(!wcscmp(office_window_class,L"PPTFrameClass")?L"PowerPoint.Application":
            !wcscmp(office_window_class,L"XLMAIN")?L"Excel.Application":L"Word.Application", &clsid);
    if (FAILED(hr = GetActiveObject(&clsid, NULL, &unknown)))
    {
        HWND document_window = NULL;
        if (!wcscmp(office_window_class,L"PPTFrameClass")) goto done;
        EnumChildWindows(word_window(), find_document_window, (LPARAM)&document_window);
        if (!document_window || FAILED(hr = AccessibleObjectFromWindow(document_window,
                OBJID_NATIVEOM, &IID_IDispatch, (void **)&window)))
        {
            fprintf(stderr, "Could not reach Word automation: %08lx\n", hr);
            goto done;
        }
        word = property(window, L"Application");
        IDispatch_Release(window);
        window = NULL;
    }
    else
    {
        hr = IUnknown_QueryInterface(unknown, &IID_IDispatch, (void **)&word);
        IUnknown_Release(unknown);
    }
    if (!word) goto done;
    if (!strcmp(argv[1], "word-open"))
    {
        if(argc<3 || !(range=property(word,L"Documents"))) goto done;
        if(!MultiByteToWideChar(CP_UTF8,0,argv[2],-1,text,sizeof(text)/sizeof(*text))) goto done;
        VariantInit(&args[0]);V_VT(&args[0])=VT_BSTR;V_BSTR(&args[0])=SysAllocString(text);
        hr=invoke(range,L"Open",DISPATCH_METHOD,args,1,&result);
        VariantClear(&args[0]);VariantClear(&result);
        if(SUCCEEDED(hr)) status=0;
        goto done;
    }
    if (!strcmp(argv[1], "powerpoint-slide-count"))
    {
        if (!(doc=property(word,L"ActivePresentation")) || !(range=property(doc,L"Slides"))) goto done;
        if (FAILED(invoke(range,L"Count",DISPATCH_PROPERTYGET,NULL,0,&result))) goto done;
        printf("slides=%ld\n",V_I4(&result));VariantClear(&result);status=0;
        goto done;
    }
    if (!strcmp(argv[1], "excel-zoom"))
    {
        if(argc<3 || !(window=property(word,L"ActiveWindow"))) goto done;
        VariantInit(&args[0]);V_VT(&args[0])=VT_I4;V_I4(&args[0])=atoi(argv[2]);
        hr=invoke(window,L"Zoom",DISPATCH_PROPERTYPUT,args,1,&result);VariantClear(&result);
        if(SUCCEEDED(hr)) status=0;
        goto done;
    }
    if (!strcmp(argv[1], "excel-chart-check"))
    {
        if (!(doc=property(word,L"ActiveWorkbook")) || !(range=property(word,L"ActiveChart"))) goto done;
        if(FAILED(invoke(range,L"ChartType",DISPATCH_PROPERTYGET,NULL,0,&result))) goto done;
        printf("chart_type=%ld\n",V_I4(&result));
        if(V_I4(&result)!=-4169 && V_I4(&result)!=74) {VariantClear(&result);goto done;}
        VariantClear(&result);
        if(argc>2) {
            MultiByteToWideChar(CP_UTF8,0,argv[2],-1,text,256);
            VariantInit(&args[0]);V_VT(&args[0])=VT_I4;V_I4(&args[0])=51;
            VariantInit(&args[1]);V_VT(&args[1])=VT_BSTR;V_BSTR(&args[1])=SysAllocString(text);
            hr=invoke(doc,L"SaveAs",DISPATCH_METHOD,args,2,&result);
            VariantClear(&args[1]);VariantClear(&result);
            if(FAILED(hr)) goto done;
        }
        status=0;goto done;
    }

    if (!strcmp(argv[1], "word-new-benchmark"))
    {
        IDispatch *documents = property(word, L"Documents");
        WCHAR *content;
        int offset = 0;
        if (!documents) goto done;
        hr = invoke(documents, L"Add", DISPATCH_METHOD, NULL, 0, &result);
        IDispatch_Release(documents);
        if (FAILED(hr) || V_VT(&result) != VT_DISPATCH) goto done;
        doc = V_DISPATCH(&result);
        if (!(range = property(doc, L"Content"))) goto done;
        content = malloc(300000 * sizeof(*content));
        if (!content) goto done;
        for (i = 0; i < 1000; ++i)
            offset += swprintf(content + offset, 300000 - offset,
                    L"%04d. Scrolling performance test. The quick brown fox jumps over the lazy dog. "
                    L"Editing, layout, text rendering, and responsive scrolling.\r", i);
        VariantInit(&args[0]);
        V_VT(&args[0]) = VT_BSTR;
        V_BSTR(&args[0]) = SysAllocStringLen(content, offset);
        free(content);
        hr = invoke(range, L"InsertAfter", DISPATCH_METHOD, args, 1, &result);
        VariantClear(&args[0]);
        VariantClear(&result);
        if (SUCCEEDED(hr)) { puts("Created separate 1000-paragraph benchmark document"); status = 0; }
        goto done;
    }
    if (!strcmp(argv[1], "word-quit") || !strcmp(argv[1], "excel-quit") || !strcmp(argv[1], "powerpoint-quit"))
    {
        VariantInit(&args[0]);
        V_VT(&args[0]) = VT_I4;
        V_I4(&args[0]) = 0; /* wdDoNotSaveChanges */
        hr = invoke(word, L"Quit", DISPATCH_METHOD, args, !strcmp(argv[1], "word-quit") ? 1 : 0, &result);
        VariantClear(&result);
        if (SUCCEEDED(hr)) status = 0;
        goto done;
    }
    if (!(doc = property(word, L"ActiveDocument"))) goto done;
    if (!strcmp(argv[1], "word-save-as"))
    {
        if (argc < 3) goto done;
        if (!MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, text, sizeof(text) / sizeof(*text))) goto done;
        for (i = 0; i < 2; ++i) VariantInit(&args[i]);
        V_VT(&args[0]) = VT_I4;
        V_I4(&args[0]) = 12; /* wdFormatXMLDocument */
        V_VT(&args[1]) = VT_BSTR;
        V_BSTR(&args[1]) = SysAllocString(text);
        hr = invoke(doc, L"SaveAs2", DISPATCH_METHOD, args, 2, &result);
        VariantClear(&args[1]);
        VariantClear(&result);
        if (SUCCEEDED(hr)) status = 0;
        goto done;
    }
    if (!strcmp(argv[1], "word-text"))
    {
        if (!(range = property(doc, L"Content"))) goto done;
        if (SUCCEEDED(invoke(range, L"Text", DISPATCH_PROPERTYGET, NULL, 0, &result)))
        {
            if (V_VT(&result) == VT_BSTR)
            {
                int length = WideCharToMultiByte(CP_UTF8, 0, V_BSTR(&result), -1, NULL, 0, NULL, NULL);
                char *utf8 = malloc(length);
                if (utf8)
                {
                    WideCharToMultiByte(CP_UTF8, 0, V_BSTR(&result), -1, utf8, length, NULL, NULL);
                    fputs(utf8, stdout);
                    free(utf8);
                    status = 0;
                }
            }
            VariantClear(&result);
        }
        goto done;
    }
    if (!(errors = property(doc, L"SpellingErrors"))) goto done;
    if (FAILED(invoke(errors, L"Count", DISPATCH_PROPERTYGET, NULL, 0, &result))) goto done;
    fprintf(stderr, "spelling_errors=%ld\n", V_I4(&result));
    VariantClear(&result);
    if (!(range = property(doc, L"Content"))) goto done;
    if (!(find = property(range, L"Find"))) goto done;
    MultiByteToWideChar(CP_UTF8, 0, argc > 2 ? argv[2] : "consectetur", -1, text, 256);
    VariantInit(&args[0]);
    V_VT(&args[0]) = VT_BSTR;
    V_BSTR(&args[0]) = SysAllocString(text);
    hr = invoke(find, L"Execute", DISPATCH_METHOD, args, 1, &result);
    VariantClear(&args[0]);
    if (FAILED(hr)) goto done;
    i = V_BOOL(&result);
    VariantClear(&result);
    if (!i) { fprintf(stderr, "word not found\n"); goto done; }
    if (!(window = property(word, L"ActiveWindow"))) goto done;
    for (i = 0; i < 5; i++) VariantInit(&args[i]);
    V_VT(&args[0]) = VT_DISPATCH;
    V_DISPATCH(&args[0]) = range;
    V_VT(&args[1]) = V_VT(&args[2]) = V_VT(&args[3]) = V_VT(&args[4]) = VT_I4 | VT_BYREF;
    V_I4REF(&args[1]) = &height;
    V_I4REF(&args[2]) = &width;
    V_I4REF(&args[3]) = &y;
    V_I4REF(&args[4]) = &x;
    if (FAILED(invoke(window, L"GetPoint", DISPATCH_METHOD, args, 5, &result))) goto done;
    VariantClear(&result);
    if (!strcmp(argv[1], "word-click-point"))
    {
        UINT dpi = GetDpiForWindow(word_window());
        printf("%d %d\n", MulDiv(x + width / 2, 96, dpi), MulDiv(y + height / 2, 96, dpi));
    }
    else printf("%ld %ld %ld %ld\n", x, y, width, height);
    fprintf(stderr, "word_dpi=%u caller_dpi=%u\n", GetDpiForWindow(word_window()), GetDpiForSystem());
    status = 0;
done:
    if (window) IDispatch_Release(window);
    if (find) IDispatch_Release(find);
    if (range) IDispatch_Release(range);
    if (errors) IDispatch_Release(errors);
    if (doc) IDispatch_Release(doc);
    if (word) IDispatch_Release(word);
    CoUninitialize();
    return status;
}
