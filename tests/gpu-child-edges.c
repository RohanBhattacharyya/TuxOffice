/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* GPU children must cover their whole window under a fractional scale: one
 * whose edge falls between device pixels showed a line of its parent there
 * (white under Office's ribbon and along Publisher's scroll bar). Shows a
 * grid of children with every position and size modulo 5 and stays up until
 * the file named on the command line exists. Uses Win32/D3D only. */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

#define GRID 5

static BOOL wait_for_image(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture)
{
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11Texture2D *readback;

    ID3D11Texture2D_GetDesc(texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &readback))) return FALSE;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)readback, (ID3D11Resource *)texture);
    if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)readback, 0, D3D11_MAP_READ, 0, &mapped)))
        return FALSE;
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0);
    ID3D11Texture2D_Release(readback);
    return TRUE;
}

int main(int argc, char **argv)
{
    WNDCLASSW gpu_class = {.lpfnWndProc = DefWindowProcW, .lpszClassName = L"GpuChildEdges"};
    WNDCLASSW klass = {.lpfnWndProc = DefWindowProcW, .lpszClassName = L"GpuChildEdgesParent",
                       .hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH)};
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGIDevice *dxgi;
    IDXGIAdapter *adapter;
    IDXGIFactory *factory;
    IDXGISwapChain *chains[GRID * GRID];
    ID3D11RenderTargetView *views[GRID * GRID];
    FLOAT colors[GRID * GRID][4];
    HWND parent, child;
    unsigned int row, col, i;
    MSG message;

    if (argc != 2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RegisterClassW(&klass);
    RegisterClassW(&gpu_class);
    parent = CreateWindowW(klass.lpszClassName, L"GPU child edges regression",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 640, 660, NULL, NULL, NULL, NULL);
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context))) return 1;
    ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi);
    IDXGIDevice_GetAdapter(dxgi, &adapter);
    IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory);
    ShowWindow(parent, SW_SHOW);

    for (row = 0; row < GRID; row++)
    for (col = 0; col < GRID; col++)
    {
        /* positions and sizes cover every remainder modulo 5 */
        int x = 10 + 106 * col, y = 10 + 106 * row, width = 90 + row, height = 90 + col;
        FLOAT *color = colors[row * GRID + col];
        DXGI_SWAP_CHAIN_DESC desc = {0};

        color[0] = (50 + 40 * col) / 255.0f;
        color[1] = (50 + 40 * row) / 255.0f;
        color[2] = color[3] = 1.0f;
        ID3D11Texture2D *texture;

        child = CreateWindowExW(WS_EX_NOACTIVATE, gpu_class.lpszClassName, NULL,
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, x, y, width, height, parent, NULL, NULL, NULL);
        desc.BufferDesc.Width = width;
        desc.BufferDesc.Height = height;
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = desc.BufferCount = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = child;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (FAILED(IDXGIFactory_CreateSwapChain(factory, (IUnknown *)device, &desc, &chains[row * GRID + col])))
            return 1;
        if (FAILED(IDXGISwapChain_GetBuffer(chains[row * GRID + col], 0, &IID_ID3D11Texture2D, (void **)&texture)))
            return 1;
        if (FAILED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)texture, NULL,
                &views[row * GRID + col]))) return 1;
        ID3D11DeviceContext_ClearRenderTargetView(context, views[row * GRID + col], color);
        /* A back buffer that was only cleared shows black when presented
         * (DXVK defers the clear); reading it back runs the clear. */
        if (!wait_for_image(device, context, texture)) return 1;
        ID3D11Texture2D_Release(texture);
        printf("child %u %u %d %d %d %d\n", row, col, x, y, width, height);
    }
    puts("READY gpu child edges");
    fflush(stdout);
    while (GetFileAttributesA(argv[1]) == INVALID_FILE_ATTRIBUTES)
    {
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        for (i = 0; i < GRID * GRID; i++)
            if (FAILED(IDXGISwapChain_Present(chains[i], 0, 0))) return 1;
        Sleep(50);
    }
    for (i = 0; i < GRID * GRID; i++)
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
    return 0;
}
