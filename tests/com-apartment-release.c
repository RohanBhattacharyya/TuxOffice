/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A local-server STA object disconnects itself during an RPC. Every reference
 * to its interfaces must still be released in the owning apartment. */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static DWORD owner;
static LONG refs=1, wrong_releases;

static HRESULT WINAPI query(IPersist *iface, REFIID iid, void **out)
{
    *out=NULL;
    if(!IsEqualGUID(iid,&IID_IUnknown) && !IsEqualGUID(iid,&IID_IPersist)) return E_NOINTERFACE;
    *out=iface;IPersist_AddRef(iface);return S_OK;
}
static ULONG WINAPI addref(IPersist *iface) { return InterlockedIncrement(&refs); }
static ULONG WINAPI release(IPersist *iface)
{
    if(GetCurrentThreadId()!=owner) InterlockedIncrement(&wrong_releases);
    return InterlockedDecrement(&refs);
}
static HRESULT WINAPI disconnect(IPersist *iface, CLSID *id)
{
    if(GetCurrentThreadId()!=owner) return RPC_E_WRONG_THREAD;
    *id=IID_IPersist;
    return CoDisconnectObject((IUnknown *)iface,0);
}
static IPersistVtbl vtbl={query,addref,release,disconnect};
static IPersist object={&vtbl};

int main(int argc,char **argv)
{
    WCHAR filename[MAX_PATH],exe[MAX_PATH],command[MAX_PATH*3];
    IStream *stream=NULL;
    HGLOBAL memory;
    HANDLE file;
    DWORD size,done,status;
    HRESULT hr;
    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    if(argc==3 && !strcmp(argv[1],"--client"))
    {
        IPersist *proxy;
        CLSID id;
        MultiByteToWideChar(CP_UTF8,0,argv[2],-1,filename,MAX_PATH);
        file=CreateFileW(filename,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
        if(file==INVALID_HANDLE_VALUE) return 3;
        size=GetFileSize(file,NULL);memory=GlobalAlloc(GMEM_MOVEABLE,size);
        if(!ReadFile(file,GlobalLock(memory),size,&done,NULL) || done!=size) return 3;
        GlobalUnlock(memory);CloseHandle(file);
        CreateStreamOnHGlobal(memory,TRUE,&stream);
        hr=CoUnmarshalInterface(stream,&IID_IPersist,(void **)&proxy);IStream_Release(stream);
        if(FAILED(hr)) return 4;
        hr=IPersist_GetClassID(proxy,&id);IPersist_Release(proxy);CoUninitialize();
        return hr==S_OK && IsEqualGUID(&id,&IID_IPersist) ? 0 : 5;
    }
    else
    {
        PROCESS_INFORMATION process;
        STARTUPINFOW startup={sizeof(startup)};
        MSG msg;
        owner=GetCurrentThreadId();
        GetTempPathW(MAX_PATH,exe);
        if(!GetTempFileNameW(exe,L"tux",0,filename)) return 6;
        CreateStreamOnHGlobal(NULL,TRUE,&stream);
        hr=CoMarshalInterface(stream,&IID_IPersist,(IUnknown *)&object,MSHCTX_LOCAL,NULL,MSHLFLAGS_NORMAL);
        if(FAILED(hr)) return 6;
        GetHGlobalFromStream(stream,&memory);
        size=GlobalSize(memory);
        file=CreateFileW(filename,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
        if(file==INVALID_HANDLE_VALUE || !WriteFile(file,GlobalLock(memory),size,&done,NULL) || done!=size) return 7;
        GlobalUnlock(memory);CloseHandle(file);
        GetModuleFileNameW(NULL,exe,MAX_PATH);
        swprintf(command,MAX_PATH*3,L"\"%ls\" --client \"%ls\"",exe,filename);
        if(!CreateProcessW(exe,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&process)) return 8;
        while((status=MsgWaitForMultipleObjects(1,&process.hProcess,FALSE,15000,QS_ALLINPUT))==WAIT_OBJECT_0+1)
            while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) DispatchMessageW(&msg);
        if(status!=WAIT_OBJECT_0) {TerminateProcess(process.hProcess,9);return 9;}
        GetExitCodeProcess(process.hProcess,&status);
        CloseHandle(process.hThread);CloseHandle(process.hProcess);
        IStream_Release(stream);DeleteFileW(filename);CoUninitialize();
        printf("child_exit=%lu wrong_thread_releases=%ld refs=%ld\n",status,wrong_releases,refs);
        fflush(stdout);
        return status ? status : wrong_releases ? 2 : refs!=1;
    }
}
