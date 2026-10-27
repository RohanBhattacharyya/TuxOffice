/* Exercise DXVK's shared allocation-object pool through its public Vulkan
 * interop interface, concurrently with ordinary D3D resource allocations.
 * The source image stays alive until every imported wrapper is released.
 * Build: wineg++ -b x86_64-windows -mconsole -o import.exe
 *        dxvk-resource-import.cpp -ld3d11 -ldxgi -luuid
 */
#include <windows.h>
#include <d3d11_3.h>
#include <stdio.h>
#include <stdint.h>

/* DXVK interop ABI; avoid requiring its implementation headers. */
struct interop_surface : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDevice(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(uint64_t *, void *, void *) = 0;
};

struct interop_device : IUnknown
{
    virtual void STDMETHODCALLTYPE GetVulkanHandles(void *, void *, void *) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(void *, void *) = 0;
    virtual void STDMETHODCALLTYPE TransitionSurfaceLayout(void *, void *, int, int) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue1(void *, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTexture2DFromVkImage(const D3D11_TEXTURE2D_DESC1 *,
            uint64_t, ID3D11Texture2D **) = 0;
};

static const GUID surface_iid = {0x5546cf8c,0x77e7,0x4341,{0xb0,0x5d,0x8d,0x4d,0x50,0x00,0xe7,0x7d}};
static const GUID device_iid = {0xe2ef5fa5,0xdc21,0x4af7,{0x90,0xc4,0xf6,0x7e,0xf6,0xa0,0x93,0x24}};
static ID3D11Device *device;
static interop_device *interop;
static uint64_t image;
static HANDLE start;
static LONG failures, imports, buffers;

static DWORD WINAPI import_worker(void *)
{
    D3D11_TEXTURE2D_DESC1 desc = {};
    desc.Width = desc.Height = 64;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    WaitForSingleObject(start, INFINITE);
    for (unsigned int i = 0; i < 20000; ++i)
    {
        ID3D11Texture2D *texture;
        if (FAILED(interop->CreateTexture2DFromVkImage(&desc,image,&texture)))
        {
            InterlockedIncrement(&failures);
            break;
        }
        D3D11_TEXTURE2D_DESC actual;
        texture->GetDesc(&actual);
        if (actual.Width != 64 || actual.Height != 64) InterlockedIncrement(&failures);
        texture->Release();
        InterlockedIncrement(&imports);
    }
    return 0;
}

static DWORD WINAPI buffer_worker(void *)
{
    WaitForSingleObject(start, INFINITE);
    for (unsigned int i = 0; i < 40000; ++i)
    {
        D3D11_BUFFER_DESC desc = {};
        ID3D11Buffer *buffer;
        desc.ByteWidth = 256 + (i % 32) * 64;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (FAILED(device->CreateBuffer(&desc,NULL,&buffer)))
        {
            InterlockedIncrement(&failures);
            break;
        }
        buffer->Release();
        InterlockedIncrement(&buffers);
    }
    return 0;
}

int main()
{
    ID3D11Texture2D *source;
    interop_surface *surface;
    D3D11_TEXTURE2D_DESC desc = {};
    HANDLE workers[6];
    HRESULT hr = D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,NULL,0,D3D11_SDK_VERSION,&device,NULL,NULL);
    if (FAILED(hr)) {printf("Device failed %#lx\n",hr); return 1;}
    hr = device->QueryInterface(device_iid,(void **)&interop);
    if (FAILED(hr)) {printf("DXVK Vulkan interop unavailable %#lx\n",hr); return 77;}
    desc.Width = desc.Height = 64;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc,NULL,&source)) ||
            FAILED(source->QueryInterface(surface_iid,(void **)&surface)) ||
            FAILED(surface->GetVulkanImageInfo(&image,NULL,NULL))) return 1;
    start = CreateEventW(NULL,TRUE,FALSE,NULL);
    for (unsigned int i = 0; i < 6; ++i)
        workers[i] = CreateThread(NULL,0,i < 4 ? import_worker : buffer_worker,NULL,0,NULL);
    puts("START concurrent imports and ordinary allocations"); fflush(stdout);
    SetEvent(start);
    if (WaitForMultipleObjects(6,workers,TRUE,90000) != WAIT_OBJECT_0) return 1;
    printf("imports=%ld buffers=%ld failures=%ld\n",imports,buffers,failures); fflush(stdout);
    surface->Release();
    source->Release();
    interop->Release();
    device->Release();
    for (unsigned int i = 0; i < 6; ++i) CloseHandle(workers[i]);
    CloseHandle(start);
    return failures || imports != 80000 || buffers != 80000;
}
