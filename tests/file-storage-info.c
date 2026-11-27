/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* GetFileInformationByHandleEx(FileStorageInfo) must succeed for a file on a
 * local volume: Word's cloud document cache (Csi) gave up on a OneDrive file
 * it had downloaded without it and Word terminated. Prints the reported
 * sector sizes; exits non-zero on failure. Uses Win32 only. */
#include <windows.h>
#include <stdio.h>

#ifndef STORAGE_INFO_FLAGS_ALIGNED_DEVICE
typedef struct
{
    ULONG LogicalBytesPerSector;
    ULONG PhysicalBytesPerSectorForAtomicity;
    ULONG PhysicalBytesPerSectorForPerformance;
    ULONG FileSystemEffectivePhysicalBytesPerSectorForAtomicity;
    ULONG Flags;
    ULONG ByteOffsetForSectorAlignment;
    ULONG ByteOffsetForPartitionAlignment;
} FILE_STORAGE_INFO;
#endif

int main(int argc, char **argv)
{
    FILE_STORAGE_INFO info;
    WCHAR path[MAX_PATH], dir[MAX_PATH];
    HANDLE file;
    BOOL ok;

    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"fsi", 0, path);
    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE) return 2;
    memset(&info, 0xcc, sizeof(info));
    ok = GetFileInformationByHandleEx(file, FileStorageInfo, &info, sizeof(info));
    if (!ok)
    {
        printf("FileStorageInfo failed, error %lu\n", GetLastError());
        CloseHandle(file);
        return 1;
    }
    printf("logical %lu atomic %lu performance %lu fs-atomic %lu flags %#lx offsets %lu %lu\n",
           info.LogicalBytesPerSector, info.PhysicalBytesPerSectorForAtomicity,
           info.PhysicalBytesPerSectorForPerformance,
           info.FileSystemEffectivePhysicalBytesPerSectorForAtomicity, info.Flags,
           info.ByteOffsetForSectorAlignment, info.ByteOffsetForPartitionAlignment);
    CloseHandle(file);
    ok = info.LogicalBytesPerSector >= 512 &&
         info.PhysicalBytesPerSectorForAtomicity >= info.LogicalBytesPerSector;
    puts(ok ? "file storage info: passed" : "file storage info: FAILED");
    return ok ? 0 : 1;
}
