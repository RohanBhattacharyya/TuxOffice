/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Cross-process DPI mapping must preserve negative screen coordinates. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv)
{
    HWND window;
    RECT rect, observed;
    WCHAR exe[MAX_PATH],command[2048];
    STARTUPINFOW startup={sizeof(startup)};
    PROCESS_INFORMATION process;
    DWORD status;
    UINT dpi;
    if(argc==7 && !strcmp(argv[1],"--read")) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
        window=(HWND)(UINT_PTR)strtoull(argv[2],NULL,16);
        if(!GetWindowRect(window,&observed)) return 2;
        rect.left=atoi(argv[3]);rect.top=atoi(argv[4]);rect.right=atoi(argv[5]);rect.bottom=atoi(argv[6]);
        printf("cross-process rect=%ld,%ld,%ld,%ld expected=%ld,%ld,%ld,%ld\n",
                observed.left,observed.top,observed.right,observed.bottom,rect.left,rect.top,rect.right,rect.bottom);
        return EqualRect(&rect,&observed)?0:3;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    window=CreateWindowW(L"static",L"DPI coordinate regression",WS_POPUP,-10,-20,800,600,NULL,NULL,NULL,NULL);
    if(!window) return 2;
    dpi=GetDpiForWindow(window);
    if(dpi==96) {puts("SKIP requires a scaled display/prefix");return 77;}
    GetWindowRect(window,&rect);
    GetModuleFileNameW(NULL,exe,MAX_PATH);
    swprintf(command,2048,L"\"%ls\" --read %p %d %d %d %d",exe,window,
            MulDiv(rect.left,96,dpi),MulDiv(rect.top,96,dpi),MulDiv(rect.right,96,dpi),MulDiv(rect.bottom,96,dpi));
    if(!CreateProcessW(exe,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&process)) return 2;
    WaitForSingleObject(process.hProcess,10000);
    if(!GetExitCodeProcess(process.hProcess,&status)) status=2;
    CloseHandle(process.hProcess);CloseHandle(process.hThread);DestroyWindow(window);
    return status;
}
