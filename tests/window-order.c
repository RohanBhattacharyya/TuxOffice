/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Z-order edges must follow restacking, destruction and cross-process reparenting. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct state
{
    ULONGLONG parent_a, parent_b, local, remote_a, remote_b, bottom_b;
    LONG failures;
};
static unsigned int checks;
static HANDLE changed, command;
static struct state *state;

static void check(BOOL ok, const char *message)
{
    ++checks;
    if (ok) return;
    fprintf(stderr,"FAIL %s error=%lu\n",message,GetLastError());
    InterlockedIncrement(&state->failures);
}

static void edges(HWND window, HWND first, HWND last)
{
    HWND actual_first,actual_last;
    SetLastError(0xdeadbeef);
    actual_first=GetWindow(window,GW_HWNDFIRST);
    if(actual_first!=first) printf("edge window=%p actual first=%p expected=%p\n",window,actual_first,first);
    check(actual_first==first,"first sibling");
    check(GetLastError()==0xdeadbeef,"first sibling preserves last error");
    actual_last=GetWindow(window,GW_HWNDLAST);
    if(actual_last!=last) printf("edge window=%p actual last=%p expected=%p\n",window,actual_last,last);
    check(actual_last==last,"last sibling");
    check(GetLastError()==0xdeadbeef,"last sibling preserves last error");
}

static BOOL wait_event(HANDLE event)
{
    ULONGLONG deadline=GetTickCount64()+10000;
    MSG message;
    DWORD result;
    do
    {
        result=MsgWaitForMultipleObjects(1,&event,FALSE,100,QS_ALLINPUT);
        if(result==WAIT_OBJECT_0) return TRUE;
        while(PeekMessageW(&message,NULL,0,0,PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    } while(GetTickCount64()<deadline);
    check(FALSE,"synchronization timeout");
    return FALSE;
}

static HWND window(HWND parent)
{
    HWND hwnd=CreateWindowExW(WS_EX_NOPARENTNOTIFY,L"TuxOfficeWindowOrder",L"order regression",
            parent?WS_CHILD:WS_OVERLAPPED,0,0,100,100,parent,NULL,GetModuleHandleW(NULL),NULL);
    check(hwnd!=NULL,"create window");
    if(parent) check(SetWindowPos(hwnd,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE),"place new child");
    return hwnd;
}

static void place(HWND hwnd, HWND after)
{
    check(SetWindowPos(hwnd,after,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE),"restack window");
}

static int child(const WCHAR *name)
{
    WCHAR event_name[128];
    HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
    HWND a,b,parent_a,parent_b,local,bottom_b;
    if(!mapping) return 2;
    state=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(*state));
    if(!state) return 2;
    swprintf(event_name,128,L"%ls-changed",name);changed=OpenEventW(EVENT_ALL_ACCESS,FALSE,event_name);
    swprintf(event_name,128,L"%ls-command",name);command=OpenEventW(EVENT_ALL_ACCESS,FALSE,event_name);
    parent_a=(HWND)(ULONG_PTR)state->parent_a;parent_b=(HWND)(ULONG_PTR)state->parent_b;
    local=(HWND)(ULONG_PTR)state->local;bottom_b=(HWND)(ULONG_PTR)state->bottom_b;
    a=window(parent_a);b=window(parent_a);
    state->remote_a=(ULONG_PTR)a;state->remote_b=(ULONG_PTR)b;
    edges(a,b,local);SetEvent(changed);
    if(!wait_event(command)) return 3;
    edges(a,local,a);
    check(SetParent(a,parent_b)==parent_a,"cross-process reparent");
    place(a,HWND_TOP);
    edges(b,local,b);edges(a,a,bottom_b);SetEvent(changed);
    if(!wait_event(command)) return 3;
    check(!IsWindow(a),"foreign parent destruction destroys child");
    edges(b,local,b);DestroyWindow(b);SetEvent(changed);
    printf("child checks=%u\n",checks);fflush(stdout);
    UnmapViewOfFile(state);CloseHandle(mapping);CloseHandle(changed);CloseHandle(command);
    return 0;
}

int main(int argc,char **argv)
{
    WNDCLASSW cls={0};
    WCHAR name[128],event_name[128],executable[1024],line[1400];
    struct state storage={0};
    STARTUPINFOW startup={sizeof(startup)};
    PROCESS_INFORMATION process={0};
    HANDLE mapping;
    HWND parent_a,parent_b,a,b,c,bottom_b;
    LARGE_INTEGER before,after,frequency;
    DWORD exit_code;
    unsigned int i;
    LONG failures;
    cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"TuxOfficeWindowOrder";
    if(!RegisterClassW(&cls)) return 2;
    if(argc==3 && !strcmp(argv[1],"--child"))
    {
        MultiByteToWideChar(CP_UTF8,0,argv[2],-1,name,128);
        return child(name);
    }
    state=&storage;
    parent_a=window(NULL);parent_b=window(NULL);
    a=window(parent_a);b=window(parent_a);c=window(parent_a);
    edges(b,c,a);
    place(a,HWND_TOP);edges(b,a,b);
    place(a,HWND_BOTTOM);edges(b,c,a);
    DestroyWindow(c);edges(b,b,a);
    DestroyWindow(a);edges(b,b,b);
    a=window(parent_a);bottom_b=window(parent_b);
    check(SetParent(a,parent_b)==parent_a,"local reparent");
    place(a,HWND_TOP);
    edges(b,b,b);edges(bottom_b,a,bottom_b);

    swprintf(name,128,L"TuxOfficeOrder-%lu",GetCurrentProcessId());
    mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,sizeof(*state),name);
    if(!mapping) return 2;
    state=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(*state));
    if(!state) return 2;
    *state=storage;
    state->parent_a=(ULONG_PTR)parent_a;state->parent_b=(ULONG_PTR)parent_b;
    state->local=(ULONG_PTR)b;state->bottom_b=(ULONG_PTR)bottom_b;
    swprintf(event_name,128,L"%ls-changed",name);changed=CreateEventW(NULL,FALSE,FALSE,event_name);
    swprintf(event_name,128,L"%ls-command",name);command=CreateEventW(NULL,FALSE,FALSE,event_name);
    GetModuleFileNameW(NULL,executable,1024);
    swprintf(line,1400,L"\"%ls\" --child %ls",executable,name);
    check(CreateProcessW(NULL,line,NULL,NULL,FALSE,0,NULL,NULL,&startup,&process),"launch foreign child owner");
    if(!process.hProcess || !wait_event(changed)) return 3;
    edges((HWND)(ULONG_PTR)state->remote_a,(HWND)(ULONG_PTR)state->remote_b,b);
    place(b,HWND_TOP);SetEvent(command);
    if(!wait_event(changed)) return 3;
    edges(b,b,(HWND)(ULONG_PTR)state->remote_b);
    edges(a,(HWND)(ULONG_PTR)state->remote_a,bottom_b);
    DestroyWindow(parent_b);SetEvent(command);
    if(!wait_event(changed) || !wait_event(process.hProcess)) return 3;
    GetExitCodeProcess(process.hProcess,&exit_code);check(exit_code==0,"foreign owner exits cleanly");
    edges(b,b,b);

    QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&before);
    for(i=0;i<100000;++i)
    {
        if(GetWindow(b,GW_HWNDFIRST)!=b || GetWindow(b,GW_HWNDLAST)!=b)
        {check(FALSE,"stable sibling benchmark");break;}
    }
    QueryPerformanceCounter(&after);
    failures=state->failures;
    printf("parent checks=%u failures=%ld edge_queries=%u elapsed_ms=%.3f\n",checks,failures,i*2,
            (double)(after.QuadPart-before.QuadPart)*1000/frequency.QuadPart);
    DestroyWindow(parent_a);CloseHandle(process.hProcess);CloseHandle(process.hThread);
    CloseHandle(changed);CloseHandle(command);UnmapViewOfFile(state);CloseHandle(mapping);
    return failures?3:0;
}
