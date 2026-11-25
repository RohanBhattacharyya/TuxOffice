/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Check the NT-to-Unix path conversion used by the native chooser, including
 * a relative directory handle, a missing output file and a short buffer.
 * Run in the disposable prefix; build for x86_64-windows and i386-windows. */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>

#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xc0000023)
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xc000000f)
static int failures;
static void check(const char *name, BOOL passed, NTSTATUS status)
{
    printf("%s %s status=%#lx\n", passed ? "PASS" : "FAIL", name, status);
    failures += !passed;
}

static BOOL same_file(const WCHAR *path, const char *unix_path)
{
    BY_HANDLE_FILE_INFORMATION expected, actual;
    WCHAR converted[2048], *p;
    HANDLE first, second;
    BOOL ret = FALSE;

    converted[0] = 'Z';
    converted[1] = ':';
    if (!MultiByteToWideChar(CP_UTF8, 0, unix_path, -1, converted + 2, 2046)) return FALSE;
    for (p = converted + 2; *p; p++) if (*p == '/') *p = '\\';
    first = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (first == INVALID_HANDLE_VALUE) return FALSE;
    second = CreateFileW(converted, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (second != INVALID_HANDLE_VALUE)
    {
        ret = GetFileInformationByHandle(first, &expected) && GetFileInformationByHandle(second, &actual) &&
              expected.dwVolumeSerialNumber == actual.dwVolumeSerialNumber &&
              expected.nFileIndexHigh == actual.nFileIndexHigh && expected.nFileIndexLow == actual.nFileIndexLow;
        CloseHandle(second);
    }
    CloseHandle(first);
    return ret;
}

static void check_system_path(NTSTATUS (WINAPI *convert)(const OBJECT_ATTRIBUTES *, char *, ULONG *, UINT),
                              const char *label, const WCHAR *path)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    char buffer[2048];
    ULONG size = sizeof(buffer);
    NTSTATUS status;

    if (!RtlDosPathNameToNtPathName_U(path, &name, NULL, NULL))
    {
        check(label, FALSE, 0);
        return;
    }
    InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = convert(&attr, buffer, &size, FILE_OPEN);
    check(label, !status && same_file(path, buffer), status);
    RtlFreeUnicodeString(&name);
}

#ifndef _WIN64
static void check_system_relative(NTSTATUS (WINAPI *convert)(const OBJECT_ATTRIBUTES *, char *, ULONG *, UINT),
                                  const WCHAR *directory, const WCHAR *filename)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    char buffer[2048], absolute[4096], directory_path[2048];
    const char *unix_path = buffer;
    ULONG size = sizeof(buffer);
    NTSTATUS status;
    HANDLE root;

    root = CreateFileW(directory, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (root == INVALID_HANDLE_VALUE)
    {
        check("open system directory handle", FALSE, 0);
        return;
    }
    RtlInitUnicodeString(&name, filename);
    InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, root, NULL);
    status = convert(&attr, buffer, &size, FILE_OPEN);
    if (!status && buffer[0] != '/')
    {
        UNICODE_STRING directory_name;
        ULONG directory_size = sizeof(directory_path);

        if (!RtlDosPathNameToNtPathName_U(directory, &directory_name, NULL, NULL))
        {
            check("system directory NT path", FALSE, 0);
            CloseHandle(root);
            return;
        }
        InitializeObjectAttributes(&attr, &directory_name, OBJ_CASE_INSENSITIVE, NULL, NULL);
        status = convert(&attr, directory_path, &directory_size, FILE_OPEN);
        RtlFreeUnicodeString(&directory_name);
        if (!status)
        {
            snprintf(absolute, sizeof(absolute), "%s/%s", directory_path, buffer);
            unix_path = absolute;
        }
    }
    check("relative system directory redirection", !status &&
          same_file(L"C:\\windows\\system32\\notepad.exe", unix_path), status);
    CloseHandle(root);
}
#endif

int main(void)
{
    NTSTATUS (WINAPI *convert)(const OBJECT_ATTRIBUTES *, char *, ULONG *, UINT);
    WCHAR directory[MAX_PATH], path[MAX_PATH], filename[80];
    char absolute[2048], relative[2048], basename[240], short_buffer[1];
    UNICODE_STRING nt_name, name;
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    ULONG size, needed;
    HANDLE file, root;
    DWORD written;

    convert = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_nt_to_unix_file_name");
    if (!convert || !GetTempPathW(MAX_PATH, directory)) return 2;
    swprintf(filename, 80, L"native-path-%lu-\x03bb.tmp", GetCurrentProcessId());
    WideCharToMultiByte(CP_UTF8, 0, filename, -1, basename, sizeof(basename), NULL, NULL);
    wcscpy(path, directory);
    wcscat(path, filename);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return 2;
    WriteFile(file, "native path fixture", 19, &written, NULL);
    CloseHandle(file);
    if (!RtlDosPathNameToNtPathName_U(path, &nt_name, NULL, NULL)) return 2;
    InitializeObjectAttributes(&attr, &nt_name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    size = sizeof(short_buffer);
    status = convert(&attr, short_buffer, &size, FILE_OPEN);
    check("short buffer", status == STATUS_BUFFER_TOO_SMALL && size > 1, status);
    needed = size;
    size = sizeof(absolute);
    status = convert(&attr, absolute, &size, FILE_OPEN);
    check("existing Unicode path", status == 0 && size > 1 && absolute[0] == '/' && same_file(path, absolute), status);
    if (status) goto done;
    check("required size", needed == strlen(absolute) + 1 && size == needed, status);
    printf("PATH %s\n", absolute);

    root = CreateFileW(directory, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (root == INVALID_HANDLE_VALUE) return 2;
    RtlInitUnicodeString(&name, filename);
    InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, root, NULL);
    size = sizeof(relative);
    status = convert(&attr, relative, &size, FILE_OPEN);
    printf("RELATIVE %s\n", status ? "(failed)" : relative);
    /* A directory-relative NT name remains relative to that handle. */
    check("relative directory handle", status == 0 &&
          (!strcmp(relative, basename) || (!strncmp(relative, "./", 2) && !strcmp(relative + 2, basename))), status);
    CloseHandle(root);
    DeleteFileW(path);
    InitializeObjectAttributes(&attr, &nt_name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    size = sizeof(relative);
    status = convert(&attr, relative, &size, FILE_OPEN_IF);
    check("missing output path", status == STATUS_NO_SUCH_FILE && !strcmp(absolute, relative), status);

done:
    RtlFreeUnicodeString(&nt_name);
    DeleteFileW(path);
    check_system_path(convert, "system directory redirection", L"C:\\windows\\system32\\notepad.exe");
#ifndef _WIN64
    check_system_path(convert, "native system directory", L"C:\\windows\\sysnative\\notepad.exe");
    check_system_relative(convert, L"C:\\windows", L"system32\\notepad.exe");
    check_system_relative(convert, L"C:\\windows\\sysnative", L"notepad.exe");
    {
        void *old;
        BOOL disabled = Wow64DisableWow64FsRedirection(&old);
        check("disable filesystem redirection", disabled, 0);
        if (disabled)
        {
            check_system_path(convert, "disabled system directory redirection", L"C:\\windows\\system32\\notepad.exe");
            check_system_path(convert, "disabled native system directory", L"C:\\windows\\sysnative\\notepad.exe");
            Wow64RevertWow64FsRedirection(old);
        }
    }
#endif
    printf("RESULT native-path failed=%d\n", failures);
    return failures != 0;
}
