/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A layout update must keep a child GPU surface visible until its next
 * present. Uses Win32/D3D only; no Office or private driver APIs. */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    WNDCLASSW gpu_class = {.lpfnWndProc = DefWindowProcW, .lpszClassName = L"GpuChildRegression"};
    WNDCLASSW klass = {.lpfnWndProc = DefWindowProcW, .lpszClassName = L"ChildSurfaceRegression",
                      .hbrBackground = (HBRUSH)(COLOR_APPWORKSPACE + 1)};
    HWND parent, children[3];
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGIDevice *dxgi;
    IDXGIAdapter *adapter;
    IDXGIFactory *factory;
    IDXGISwapChain *chains[3];
    ID3D11RenderTargetView *views[3];
    const FLOAT colors[3][4] = {{.8f,0,0,1},{0,.8f,0,1},{0,0,.8f,1}};
    const DWORD expected[3] = {0xffcc0000,0xff00cc00,0xff0000cc};
    unsigned int i, cycles = 0;
    HRESULT hr;
    MSG message;

    if (argc != 2 && argc != 3) return 2;
    RegisterClassW(&klass);
    RegisterClassW(&gpu_class);
    parent = CreateWindowW(klass.lpszClassName, L"Child GPU surface regression",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 660, 350,
            NULL, NULL, NULL, NULL);
    hr = D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            NULL,0,D3D11_SDK_VERSION,&device,NULL,&context);
    if (FAILED(hr)) return 1;
    ID3D11Device_QueryInterface(device,&IID_IDXGIDevice,(void **)&dxgi);
    IDXGIDevice_GetAdapter(dxgi,&adapter);
    IDXGIAdapter_GetParent(adapter,&IID_IDXGIFactory,(void **)&factory);
    for (i = 0; i < 3; ++i)
    {
        DXGI_SWAP_CHAIN_DESC desc = {0};
        ID3D11Texture2D *texture;
        children[i] = CreateWindowExW(WS_EX_NOACTIVATE | (argc == 3 ? WS_EX_TRANSPARENT : 0),
                gpu_class.lpszClassName, NULL,
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | (argc == 3 ? WS_DISABLED : 0), 20 + 205*i, 55,
                180, 180, parent, NULL, NULL, NULL);
        desc.BufferDesc.Width = desc.BufferDesc.Height = 180;
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = desc.BufferCount = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = children[i];
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (FAILED(IDXGIFactory_CreateSwapChain(factory,(IUnknown *)device,&desc,&chains[i]))) return 1;
        if (FAILED(IDXGISwapChain_GetBuffer(chains[i],0,&IID_ID3D11Texture2D,(void **)&texture))) return 1;
        hr = ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource *)texture,NULL,&views[i]);
        if (FAILED(hr)) return 1;
        ID3D11DeviceContext_ClearRenderTargetView(context,views[i],colors[i]);
        {
            D3D11_TEXTURE2D_DESC read_desc;
            D3D11_MAPPED_SUBRESOURCE mapped;
            ID3D11Texture2D *readback;
            ID3D11Texture2D_GetDesc(texture,&read_desc);
            read_desc.Usage = D3D11_USAGE_STAGING;
            read_desc.BindFlags = read_desc.MiscFlags = 0;
            read_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(ID3D11Device_CreateTexture2D(device,&read_desc,NULL,&readback))) return 1;
            ID3D11DeviceContext_CopyResource(context,(ID3D11Resource *)readback,(ID3D11Resource *)texture);
            if (FAILED(ID3D11DeviceContext_Map(context,(ID3D11Resource *)readback,0,D3D11_MAP_READ,0,&mapped))) return 1;
            if (*(DWORD *)mapped.pData != expected[i])
            {
                fprintf(stderr,"panel=%u buffer_bgra=%#lx expected=%#lx\n",i,
                        *(DWORD *)mapped.pData,expected[i]);
                return 1;
            }
            ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)readback,0);
            ID3D11Texture2D_Release(readback);
        }
        ID3D11Texture2D_Release(texture);
        hr = IDXGISwapChain_Present(chains[i],0,0);
        if (FAILED(hr)) return 1;
    }
    puts("READY child surfaces"); fflush(stdout);
    Sleep(1000);
    while (GetFileAttributesA(argv[1]) == INVALID_FILE_ATTRIBUTES)
    {
        while (PeekMessageW(&message,NULL,0,0,PM_REMOVE)) DispatchMessageW(&message);
        for (i = 0; i < 3; ++i)
            SetWindowPos(children[i],HWND_TOP,20 + 205*i + (cycles & 1),55,0,0,SWP_NOSIZE | SWP_NOACTIVATE);
        RedrawWindow(parent,NULL,NULL,RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        /* A normal layout pass may precede rendering on another thread. */
        Sleep(12);
        for (i = 0; i < 3; ++i)
        {
            FLOAT frame_color[4] = {0,0,0,1};
            frame_color[i] = cycles & 1 ? .65f : .8f;
            ID3D11DeviceContext_OMSetRenderTargets(context,1,&views[i],NULL);
            ID3D11DeviceContext_ClearRenderTargetView(context,views[i],frame_color);
            if (FAILED(IDXGISwapChain_Present(chains[i],0,0))) return 1;
        }
        ++cycles;
        Sleep(8);
    }
    for (i = 0; i < 3; ++i)
    {
        ID3D11RenderTargetView_Release(views[i]);
        IDXGISwapChain_Release(chains[i]);
    }
    IDXGIFactory_Release(factory);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    DestroyWindow(parent);
    printf("cycles=%u failures=0\n",cycles); fflush(stdout);
    return 0;
}
