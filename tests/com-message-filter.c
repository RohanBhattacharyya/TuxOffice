/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* An STA's message filter sees calls that arrive while the apartment waits for
 * an outgoing call as CALLTYPE_TOPLEVEL_CALLPENDING, and a call it answers
 * with SERVERCALL_RETRYLATER runs later instead of straight away. Excel's
 * filter relies on both: without them a call from PowerPoint ran in the middle
 * of Excel's own outgoing call and crashed it. */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <stdio.h>

struct object { IPersist IPersist_iface; DWORD sleep; DWORD ran_at; };

static HRESULT WINAPI object_query(IPersist *iface, REFIID iid, void **out)
{
    *out=NULL;
    if(!IsEqualGUID(iid,&IID_IUnknown) && !IsEqualGUID(iid,&IID_IPersist)) return E_NOINTERFACE;
    *out=iface;return S_OK;
}
static ULONG WINAPI object_addref(IPersist *iface) { return 2; }
static ULONG WINAPI object_release(IPersist *iface) { return 1; }
static HRESULT WINAPI object_class(IPersist *iface, CLSID *id)
{
    struct object *object=CONTAINING_RECORD(iface,struct object,IPersist_iface);
    object->ran_at=GetTickCount();
    if(object->sleep) Sleep(object->sleep);
    *id=IID_IPersist;return S_OK;
}
static IPersistVtbl object_vtbl={object_query,object_addref,object_release,object_class};
static struct object slow={{&object_vtbl},500}, busy={{&object_vtbl},0};

static DWORD calltypes[8], ncalltypes, retry_first;
static HRESULT WINAPI filter_query(IMessageFilter *iface, REFIID iid, void **out)
{
    *out=NULL;
    if(!IsEqualGUID(iid,&IID_IUnknown) && !IsEqualGUID(iid,&IID_IMessageFilter)) return E_NOINTERFACE;
    *out=iface;return S_OK;
}
static ULONG WINAPI filter_addref(IMessageFilter *iface) { return 2; }
static ULONG WINAPI filter_release(IMessageFilter *iface) { return 1; }
static DWORD WINAPI filter_incoming(IMessageFilter *iface, DWORD type, HTASK caller, DWORD ticks, INTERFACEINFO *info)
{
    if(!IsEqualGUID(&info->iid,&IID_IPersist)) return SERVERCALL_ISHANDLED;
    if(ncalltypes<ARRAYSIZE(calltypes)) calltypes[ncalltypes++]=type;
    if(type==CALLTYPE_TOPLEVEL_CALLPENDING) return SERVERCALL_RETRYLATER;
    if(retry_first) { retry_first=0;return SERVERCALL_RETRYLATER; }
    return SERVERCALL_ISHANDLED;
}
static DWORD WINAPI filter_retry(IMessageFilter *iface, HTASK callee, DWORD ticks, DWORD type) { return ~0u; }
static DWORD WINAPI filter_pending(IMessageFilter *iface, HTASK callee, DWORD ticks, DWORD type)
{
    return PENDINGMSG_WAITDEFPROCESS;
}
static IMessageFilterVtbl filter_vtbl={filter_query,filter_addref,filter_release,filter_incoming,filter_retry,filter_pending};
static IMessageFilter filter={&filter_vtbl};

static IStream *slow_stream, *busy_stream;
static HANDLE ready, go;
static DWORD outgoing_done;

static void pump_until(HANDLE event)
{
    MSG msg;
    for(;;)
    {
        DWORD r=MsgWaitForMultipleObjects(event?1:0,&event,FALSE,INFINITE,QS_ALLINPUT);
        if(event && r==WAIT_OBJECT_0) return;
        while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE))
        {
            if(msg.message==WM_QUIT) return;
            DispatchMessageW(&msg);
        }
    }
}

static DWORD WINAPI slow_thread(void *arg)
{
    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    CoMarshalInterThreadInterfaceInStream(&IID_IPersist,(IUnknown *)&slow.IPersist_iface,&slow_stream);
    SetEvent(ready);
    pump_until(NULL);
    CoUninitialize();
    return 0;
}

static DWORD WINAPI busy_thread(void *arg)
{
    IPersist *other;
    CLSID id;
    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    CoRegisterMessageFilter(&filter,NULL);
    CoMarshalInterThreadInterfaceInStream(&IID_IPersist,(IUnknown *)&busy.IPersist_iface,&busy_stream);
    SetEvent(ready);
    pump_until(go);
    CoGetInterfaceAndReleaseStream(slow_stream,&IID_IPersist,(void **)&other);
    IPersist_GetClassID(other,&id);   /* outgoing call, half a second */
    outgoing_done=GetTickCount();
    IPersist_Release(other);
    pump_until(NULL);
    CoRegisterMessageFilter(NULL,NULL);
    CoUninitialize();
    return 0;
}

static int failures;
static void check(int ok, const char *what)
{
    printf("%s %s\n",ok?"PASS":"FAIL",what);
    if(!ok) failures++;
}

int main(void)
{
    HANDLE threads[2];
    DWORD ids[2], start, took;
    IPersist *busy_proxy;
    CLSID id;
    HRESULT hr;

    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    ready=CreateEventW(NULL,FALSE,FALSE,NULL);go=CreateEventW(NULL,FALSE,FALSE,NULL);
    threads[0]=CreateThread(NULL,0,slow_thread,NULL,0,&ids[0]);WaitForSingleObject(ready,INFINITE);
    threads[1]=CreateThread(NULL,0,busy_thread,NULL,0,&ids[1]);WaitForSingleObject(ready,INFINITE);
    CoGetInterfaceAndReleaseStream(busy_stream,&IID_IPersist,(void **)&busy_proxy);

    retry_first=1;ncalltypes=0;start=GetTickCount();
    hr=IPersist_GetClassID(busy_proxy,&id);took=GetTickCount()-start;
    printf("put off: hr %#lx after %lu ms, calltypes:",hr,took);
    for(DWORD i=0;i<ncalltypes;i++) printf(" %lu",calltypes[i]);
    printf("\n");
    check(hr==S_OK,"a call the filter puts off still succeeds");
    check(took>=80,"the call ran again after a pause");
    check(ncalltypes==2 && calltypes[0]==CALLTYPE_TOPLEVEL && calltypes[1]==CALLTYPE_TOPLEVEL,
          "an idle apartment sees top-level calls");

    ncalltypes=0;SetEvent(go);Sleep(150);
    hr=IPersist_GetClassID(busy_proxy,&id);
    check(hr==S_OK,"a call made during an outgoing call succeeds");
    check(ncalltypes>=1 && calltypes[0]==CALLTYPE_TOPLEVEL_CALLPENDING,
          "a call during an outgoing call is TOPLEVEL_CALLPENDING");
    check(ncalltypes>=2 && calltypes[ncalltypes-1]==CALLTYPE_TOPLEVEL,"the put-off call runs as top-level");
    check(outgoing_done && (LONG)(busy.ran_at-outgoing_done)>=0,"the put-off call runs after the outgoing call");
    printf("calltypes:");for(DWORD i=0;i<ncalltypes;i++) printf(" %lu",calltypes[i]);printf("\n");

    IPersist_Release(busy_proxy);
    PostThreadMessageW(ids[1],WM_QUIT,0,0);PostThreadMessageW(ids[0],WM_QUIT,0,0);
    WaitForMultipleObjects(2,threads,TRUE,5000);
    CoUninitialize();
    printf("%s\n",failures?"FAILED":"ALL PASSED");
    return failures!=0;
}
