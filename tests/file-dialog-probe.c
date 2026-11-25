/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Show a common item dialog the way Office does and print the result.
 *
 * Usage: file-dialog-probe.exe save|open [NAME]
 * Sets a title, two filters, a default extension, a file name and the
 * Documents folder, shows the dialog and prints "result PATH" lines, "type N"
 * and "cancelled" or "failed HR". With TuxOffice's runner the desktop file
 * chooser appears instead of Wine's dialog.
 */
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>

static const COMDLG_FILTERSPEC types[] =
{
    { L"Word Document (*.docx)", L"*.docx" },
    { L"PDF (*.pdf)", L"*.pdf" },
    { L"All Files (*.*)", L"*.*" },
};

int wmain(int argc, WCHAR **argv)
{
    BOOL save = argc < 2 || !wcscmp(argv[1], L"save");
    IFileDialog *dialog;
    IFileOpenDialog *open;
    IShellItemArray *items;
    IShellItem *folder, *item;
    DWORD count, i, options;
    UINT type;
    WCHAR *path;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = CoCreateInstance(save ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileDialog, (void **)&dialog);
    if (FAILED(hr)) { printf("failed %#lx\n", hr); return 1; }
    IFileDialog_GetOptions(dialog, &options);
    IFileDialog_SetOptions(dialog, options | FOS_FORCEFILESYSTEM | (save ? 0 : FOS_ALLOWMULTISELECT));
    IFileDialog_SetFileTypes(dialog, sizeof(types) / sizeof(types[0]), types);
    IFileDialog_SetFileTypeIndex(dialog, 1);
    IFileDialog_SetTitle(dialog, save ? L"Save As" : L"Open");
    IFileDialog_SetDefaultExtension(dialog, L"docx");
    if (save) IFileDialog_SetFileName(dialog, argc > 2 ? argv[2] : L"probe");
    IFileDialog_SetOkButtonLabel(dialog, save ? L"&Save" : L"&Open");
    if (SUCCEEDED(SHGetKnownFolderItem(&FOLDERID_Documents, 0, NULL, &IID_IShellItem, (void **)&folder)))
    {
        IFileDialog_SetFolder(dialog, folder);
        IShellItem_Release(folder);
    }

    hr = IFileDialog_Show(dialog, NULL);
    printf("show %#lx\n", hr);
    fflush(stdout);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) printf("cancelled\n");
    else if (FAILED(hr)) printf("failed %#lx\n", hr);
    else
    {
        if (save)
        {
            if (SUCCEEDED(IFileDialog_GetResult(dialog, &item)))
            {
                IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path);
                printf("result %ls\n", path);
                CoTaskMemFree(path);
                IShellItem_Release(item);
            }
        }
        /* IFileDialog may be implemented by IFileDialog2. Query the separate
         * open interface rather than treating the next vtable slots as it. */
        else if (SUCCEEDED(IFileDialog_QueryInterface(dialog, &IID_IFileOpenDialog, (void **)&open)))
        {
            if (SUCCEEDED(IFileOpenDialog_GetResults(open, &items)))
            {
                IShellItemArray_GetCount(items, &count);
                for (i = 0; i < count; i++)
                {
                    IShellItemArray_GetItemAt(items, i, &item);
                    IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path);
                    printf("result %ls\n", path);
                    CoTaskMemFree(path);
                    IShellItem_Release(item);
                }
                IShellItemArray_Release(items);
            }
            IFileOpenDialog_Release(open);
        }
        IFileDialog_GetFileTypeIndex(dialog, &type);
        printf("type %u\n", type);
    }
    fflush(stdout);
    IFileDialog_Release(dialog);
    CoUninitialize();
    return 0;
}
