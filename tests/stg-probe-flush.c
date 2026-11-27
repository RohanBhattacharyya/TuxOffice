/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Opening a non-compound file with StgOpenStorageOnILockBytes must fail
 * without flushing the caller's ILockBytes. Word probes every document this
 * way; Office's cloud document cache closes its stream on Flush, so a .docx
 * opened from OneDrive was gone when Word read it and Word terminated.
 * Exits non-zero if the failed open flushed or the reference count changed.
 * Uses Win32/COM only. */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <stdio.h>

struct lockbytes
{
    ILockBytes ILockBytes_iface;
    LONG ref;
    int flushes;
    BYTE data[4096];
};

static struct lockbytes *impl(ILockBytes *iface) { return CONTAINING_RECORD(iface, struct lockbytes, ILockBytes_iface); }

static HRESULT WINAPI lb_QueryInterface(ILockBytes *iface, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ILockBytes))
    {
        *out = iface;
        ILockBytes_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI lb_AddRef(ILockBytes *iface) { return InterlockedIncrement(&impl(iface)->ref); }
static ULONG WINAPI lb_Release(ILockBytes *iface) { return InterlockedDecrement(&impl(iface)->ref); }
static HRESULT WINAPI lb_ReadAt(ILockBytes *iface, ULARGE_INTEGER offset, void *buf, ULONG cb, ULONG *read)
{
    struct lockbytes *lb = impl(iface);
    ULONG n = 0;
    if (offset.QuadPart < sizeof(lb->data)) n = min(cb, (ULONG)(sizeof(lb->data) - offset.QuadPart));
    memcpy(buf, lb->data + offset.QuadPart, n);
    if (read) *read = n;
    return S_OK;
}
static HRESULT WINAPI lb_WriteAt(ILockBytes *iface, ULARGE_INTEGER offset, const void *buf, ULONG cb, ULONG *written)
{
    return STG_E_ACCESSDENIED;
}
static HRESULT WINAPI lb_Flush(ILockBytes *iface) { impl(iface)->flushes++; return S_OK; }
static HRESULT WINAPI lb_SetSize(ILockBytes *iface, ULARGE_INTEGER size) { return STG_E_ACCESSDENIED; }
static HRESULT WINAPI lb_LockRegion(ILockBytes *iface, ULARGE_INTEGER o, ULARGE_INTEGER cb, DWORD type) { return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI lb_UnlockRegion(ILockBytes *iface, ULARGE_INTEGER o, ULARGE_INTEGER cb, DWORD type) { return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI lb_Stat(ILockBytes *iface, STATSTG *stat, DWORD flag)
{
    memset(stat, 0, sizeof(*stat));
    stat->type = STGTY_LOCKBYTES;
    stat->cbSize.QuadPart = sizeof(impl(iface)->data);
    return S_OK;
}

static const ILockBytesVtbl lb_vtbl =
{
    lb_QueryInterface, lb_AddRef, lb_Release, lb_ReadAt, lb_WriteAt, lb_Flush,
    lb_SetSize, lb_LockRegion, lb_UnlockRegion, lb_Stat,
};

int main(void)
{
    static struct lockbytes lb = {{&lb_vtbl}, 1};
    IStorage *storage = (IStorage *)0xdeadbeef;
    HRESULT hr;
    int ok;

    /* A .docx is a zip file. */
    memcpy(lb.data, "PK\003\004", 4);
    hr = StgOpenStorageOnILockBytes(&lb.ILockBytes_iface, NULL, STGM_READ | STGM_SHARE_DENY_WRITE,
                                    NULL, 0, &storage);
    printf("hr %#lx storage %p flushes %d ref %ld\n", hr, storage, lb.flushes, lb.ref);
    ok = FAILED(hr) && !storage && !lb.flushes && lb.ref == 1;
    puts(ok ? "storage probe: passed" : "storage probe: FAILED");
    return ok ? 0 : 1;
}
