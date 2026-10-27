/* Install a prefix-local file printer so Office can compute print layout and
 * export PDFs even when the host has no configured printers. */
#include <windows.h>
#include <winspool.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    WCHAR ppd[1024], driver[MAX_PATH];
    WCHAR name[]=L"TuxOffice PostScript", environment[]=L"Windows x64";
    WCHAR processor[]=L"wineps", raw[]=L"RAW", port[]=L"FILE:";
    DRIVER_INFO_3W info={0};
    PRINTER_INFO_2W printer={0};
    HANDLE handle;
    DWORD default_size=0;
    if(argc!=2) return 1;
    GetDefaultPrinterW(NULL,&default_size);
    if(default_size)
    {
        puts("Keeping the configured default printer");
        return 0;
    }
    MultiByteToWideChar(CP_UTF8,0,argv[1],-1,ppd,1024);
    GetSystemDirectoryW(driver,MAX_PATH);
    wcscat(driver,L"\\wineps.drv");
    AddPrintProcessorW(NULL,environment,driver,processor);
    info.cVersion=3;info.pName=name;info.pEnvironment=environment;
    info.pDriverPath=driver;info.pConfigFile=driver;info.pDataFile=ppd;info.pDefaultDataType=raw;
    if(!AddPrinterDriverExW(NULL,3,(BYTE *)&info,APD_COPY_NEW_FILES|APD_COPY_FROM_DIRECTORY)
            && GetLastError()!=ERROR_PRINTER_DRIVER_ALREADY_INSTALLED)
    {printf("AddPrinterDriver error %lu\n",GetLastError());return 2;}
    printer.pPrinterName=name;printer.pPortName=port;printer.pDriverName=name;
    printer.pPrintProcessor=processor;printer.pDatatype=raw;printer.Attributes=PRINTER_ATTRIBUTE_LOCAL;
    if(!(handle=AddPrinterW(NULL,2,(BYTE *)&printer)))
    {
        if(!OpenPrinterW(name,&handle,NULL)) {printf("Add/OpenPrinter error %lu\n",GetLastError());return 3;}
    }
    ClosePrinter(handle);
    if(!SetDefaultPrinterW(name)) {printf("SetDefaultPrinter error %lu\n",GetLastError());return 4;}
    puts("Prefix-local print layout ready (file output)");
    return 0;
}
