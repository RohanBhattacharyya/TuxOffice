/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The enterprise policy, security center and sensor activity APIs Office loads:
 * a token's service state, change registrations that can be undone once, and a
 * camera activity monitor that starts and stops.
 * Build: winegcc -b x86_64-windows -mconsole -I$BD/include -I wine/include
 *        -L$BD/libs/mfuuid/x86_64-windows -o security-sensor-apis.exe security-sensor-apis.c -lmfuuid -luuid */
#define COBJMACROS
#include <windows.h>
#include <mfidl.h>
#include <stdio.h>

static int failures;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static HRESULT WINAPI cb_query(IMFSensorActivitiesReportCallback *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMFSensorActivitiesReportCallback)) { *out = iface; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static LONG cb_refs = 1;
static ULONG WINAPI cb_addref(IMFSensorActivitiesReportCallback *iface) { return InterlockedIncrement(&cb_refs); }
static ULONG WINAPI cb_release(IMFSensorActivitiesReportCallback *iface) { return InterlockedDecrement(&cb_refs); }
static HRESULT WINAPI cb_report(IMFSensorActivitiesReportCallback *iface, IMFSensorActivitiesReport *report) { return S_OK; }
static IMFSensorActivitiesReportCallbackVtbl cb_vtbl = {cb_query, cb_addref, cb_release, cb_report};

static DWORD WINAPI changed(void *context) { return 0; }

int main(void)
{
    HMODULE srp = LoadLibraryA("srpapi.dll"), wsc = LoadLibraryA("wscapi.dll"), sensor = LoadLibraryA("mfsensorgroup.dll");
    LONG (WINAPI *pSrpIsTokenService)(HANDLE, BOOLEAN *) = (void *)GetProcAddress(srp, "SrpIsTokenService");
    HRESULT (WINAPI *pWscRegisterForChanges)(void *, HANDLE *, LPTHREAD_START_ROUTINE, void *) = (void *)GetProcAddress(wsc, "WscRegisterForChanges");
    HRESULT (WINAPI *pWscUnRegisterChanges)(HANDLE) = (void *)GetProcAddress(wsc, "WscUnRegisterChanges");
    HRESULT (WINAPI *pMFCreateSensorActivityMonitor)(IMFSensorActivitiesReportCallback *, IMFSensorActivityMonitor **) =
            (void *)GetProcAddress(sensor, "MFCreateSensorActivityMonitor");
    IMFSensorActivitiesReportCallback callback = {&cb_vtbl};
    IMFSensorActivityMonitor *monitor = NULL;
    HANDLE token, first, second;
    BOOLEAN service = TRUE;

    check(pSrpIsTokenService && pWscRegisterForChanges && pWscUnRegisterChanges && pMFCreateSensorActivityMonitor, "the functions are exported");
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    check(!pSrpIsTokenService(token, &service) && !service, "a user's process is not a service");
    check(pSrpIsTokenService(NULL, &service) != 0, "an invalid token is refused");

    check(pWscRegisterForChanges(NULL, &first, changed, NULL) == S_OK, "a change registration");
    check(pWscRegisterForChanges(NULL, &second, changed, NULL) == S_OK && second != first, "a second, distinct one");
    check(pWscUnRegisterChanges(first) == S_OK, "the first is unregistered");
    check(pWscUnRegisterChanges(first) == E_INVALIDARG, "but only once");
    check(pWscUnRegisterChanges(second) == S_OK, "the second is unregistered");

    check(pMFCreateSensorActivityMonitor(NULL, &monitor) == E_INVALIDARG, "a monitor needs a callback");
    check(pMFCreateSensorActivityMonitor(&callback, &monitor) == S_OK && monitor, "a sensor activity monitor");
    check(cb_refs == 2, "it holds the callback");
    check(IMFSensorActivityMonitor_Start(monitor) == S_OK, "it starts");
    check(IMFSensorActivityMonitor_Stop(monitor) == S_OK, "and stops");
    IMFSensorActivityMonitor_Release(monitor);
    check(cb_refs == 1, "and lets the callback go");

    printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures != 0;
}
