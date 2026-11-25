/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Verify that cancelling a native chooser preserves the caller's edit focus. */
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <stdio.h>

int main(void)
{
    HWND window, edit, before, at_show, after;
    IFileDialog *dialog;
    HRESULT hr;
    DWORD end;
    MSG msg;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    window = CreateWindowW(L"STATIC", L"Native chooser focus test",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 600, 400, NULL, NULL, NULL, NULL);
    edit = CreateWindowW(L"EDIT", L"Continue typing here", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER,
            20, 20, 400, 40, window, NULL, NULL, NULL);
    if (!window || !edit) return 2;
    SetForegroundWindow(window);
    SetFocus(edit);
    end = GetTickCount() + 500;
    while ((LONG)(end - GetTickCount()) > 0)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    before = GetFocus();
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
            &IID_IFileDialog, (void **)&dialog);
    if (FAILED(hr)) return 2;
    at_show = GetFocus();
    hr = IFileDialog_Show(dialog, window);
    after = GetFocus();
    printf("cancelled=%d initial_edit_focus=%d at_show_edit_focus=%d restored_edit_focus=%d after=%p owner=%p\n",
            hr == HRESULT_FROM_WIN32(ERROR_CANCELLED), before == edit, at_show == edit, after == edit,
            after, window);
    IFileDialog_Release(dialog);
    DestroyWindow(window);
    CoUninitialize();
    return hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) && before == edit && at_show == edit && after == edit ? 0 : 1;
}
