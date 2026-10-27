/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Direct2D must preserve its draw transaction while another user of the shared
 * immediate D3D context changes state. This models composition/application
 * interop without Office. Each completed frame must remain solid red. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11_4.h>
#include <stdio.h>

static ID3D11DeviceContext *immediate;
static LONG stop;
static DWORD WINAPI change_state(void *unused)
{
    while(!InterlockedCompareExchange(&stop,0,0)) ID3D11DeviceContext_ClearState(immediate);
    return 0;
}
#define HR(call) do {HRESULT hr=(call);if(FAILED(hr)){fprintf(stderr,"%s failed %08lx\n",#call,hr);return 3;}} while(0)

int main(void)
{
    ID3D11Device *d3d;
    ID3D11Multithread *multithread;
    IDXGIDevice *dxgi;
    ID2D1Device *device;
    ID2D1DeviceContext *context;
    ID2D1Bitmap1 *bitmap;
    ID2D1SolidColorBrush *brush;
    IDXGISurface *surface;
    ID3D11Texture2D *target,*staging;
    D2D1_BITMAP_PROPERTIES1 properties={0};
    D2D1_SIZE_U size={16,16};
    D2D1_COLOR_F red={1,0,0,1},blue={0,0,1,1};
    D2D1_RECT_F rect={0,0,16,16};
    D3D11_TEXTURE2D_DESC description;
    D3D11_MAPPED_SUBRESOURCE mapping;
    HANDLE worker;
    unsigned int i,wrong=0;

    HR(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            NULL,0,D3D11_SDK_VERSION,&d3d,NULL,&immediate));
    HR(ID3D11DeviceContext_QueryInterface(immediate,&IID_ID3D11Multithread,(void **)&multithread));
    ID3D11Multithread_SetMultithreadProtected(multithread,TRUE);
    HR(ID3D11Device_QueryInterface(d3d,&IID_IDXGIDevice,(void **)&dxgi));
    HR(D2D1CreateDevice(dxgi,NULL,&device));
    HR(ID2D1Device_CreateDeviceContext(device,0,&context));
    properties.pixelFormat.format=DXGI_FORMAT_B8G8R8A8_UNORM;
    properties.pixelFormat.alphaMode=D2D1_ALPHA_MODE_IGNORE;
    properties.dpiX=properties.dpiY=96;
    properties.bitmapOptions=D2D1_BITMAP_OPTIONS_TARGET;
    HR(ID2D1DeviceContext_CreateBitmap(context,size,NULL,0,&properties,&bitmap));
    ID2D1DeviceContext_SetTarget(context,(ID2D1Image *)bitmap);
    HR(ID2D1DeviceContext_CreateSolidColorBrush(context,&red,NULL,&brush));
    HR(ID2D1Bitmap1_GetSurface(bitmap,&surface));
    HR(IDXGISurface_QueryInterface(surface,&IID_ID3D11Texture2D,(void **)&target));
    ID3D11Texture2D_GetDesc(target,&description);
    description.Usage=D3D11_USAGE_STAGING;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    description.BindFlags=description.MiscFlags=0;
    HR(ID3D11Device_CreateTexture2D(d3d,&description,NULL,&staging));
    worker=CreateThread(NULL,0,change_state,NULL,0,NULL);
    for(i=0;i<500;++i)
    {
        ID2D1DeviceContext_BeginDraw(context);
        ID2D1DeviceContext_Clear(context,&blue);
        ID2D1DeviceContext_FillRectangle(context,&rect,(ID2D1Brush *)brush);
        HR(ID2D1DeviceContext_EndDraw(context,NULL,NULL));
        ID3D11DeviceContext_CopyResource(immediate,(ID3D11Resource *)staging,(ID3D11Resource *)target);
        HR(ID3D11DeviceContext_Map(immediate,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapping));
        if((*(DWORD *)mapping.pData & 0x00ffffff)!=0x00ff0000) ++wrong;
        ID3D11DeviceContext_Unmap(immediate,(ID3D11Resource *)staging,0);
    }
    InterlockedExchange(&stop,1);WaitForSingleObject(worker,10000);CloseHandle(worker);
    ID3D11Texture2D_Release(staging);ID3D11Texture2D_Release(target);IDXGISurface_Release(surface);
    ID2D1SolidColorBrush_Release(brush);ID2D1Bitmap1_Release(bitmap);ID2D1DeviceContext_Release(context);
    ID2D1Device_Release(device);IDXGIDevice_Release(dxgi);ID3D11Multithread_Release(multithread);
    ID3D11DeviceContext_Release(immediate);ID3D11Device_Release(d3d);
    printf("frames=500 wrong_color_frames=%u\n",wrong);
    return wrong?2:0;
}
