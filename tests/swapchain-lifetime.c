/* A popup can be destroyed while its last Present is queued on Wine's CS.
 * Changing the interval then recreates the Vulkan swapchain for a dead HWND.
 * Build with winegcc -b x86_64-windows -mconsole -o swapchain-lifetime.exe
 * swapchain-lifetime.c -ld3d11 -ldxgi -luser32 -luuid.
 */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

int main(void)
{
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain *swapchain;
    DXGI_SWAP_CHAIN_DESC desc = {0};
    HWND parent, child;
    HRESULT hr;
    unsigned int i;

    parent = CreateWindowW(L"static", L"Swapchain lifetime regression", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            0, 0, 320, 240, NULL, NULL, NULL, NULL);
    child = CreateWindowW(L"static", NULL, WS_CHILD | WS_VISIBLE,
            0, 0, 160, 120, parent, NULL, NULL, NULL);
    desc.BufferDesc.Width = 160;
    desc.BufferDesc.Height = 120;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = child;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
            &desc, &swapchain, &device, NULL, &context);
    if (FAILED(hr)) { printf("create failed: %#lx\n", hr); return 1; }
    {
        IDXGIDevice *dxgi;
        IDXGIAdapter *adapter;
        DXGI_ADAPTER_DESC adapter_desc;
        if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi)))
        {
            IDXGIDevice_GetAdapter(dxgi, &adapter);
            IDXGIAdapter_GetDesc(adapter, &adapter_desc);
            printf("adapter=%ls vendor=%04x device=%04x\n", adapter_desc.Description,
                    adapter_desc.VendorId, adapter_desc.DeviceId);
            IDXGIAdapter_Release(adapter);
            IDXGIDevice_Release(dxgi);
        }
    }
    hr = IDXGISwapChain_Present(swapchain, 1, 0);
    printf("live present: %#lx\n", hr);
    ID3D11DeviceContext_Flush(context);
    Sleep(300);
    DestroyWindow(child);
    for (i = 0; i < 4; ++i)
    {
        hr = IDXGISwapChain_Present(swapchain, i % 2, 0);
        printf("destroyed present %u: %#lx\n", i, hr);
        ID3D11DeviceContext_Flush(context);
        Sleep(100);
    }
    IDXGISwapChain_Release(swapchain);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    DestroyWindow(parent);
    puts("survived destroyed popup and swapchain cleanup");
    return 0;
}
