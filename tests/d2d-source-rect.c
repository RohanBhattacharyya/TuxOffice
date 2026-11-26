/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* DrawBitmap with a source rectangle must not sample past it. Office draws
 * nine-slice images (the Shapes gallery's More button) as one small bitmap with
 * a source rectangle per slice and stretches a one pixel wide middle; linear
 * filtering that reached the neighbouring border pixel shaded half the button.
 * Stretches the middle pixel of a red, green, blue bitmap tenfold and expects
 * pure green; a stretch of the whole bitmap still blends.
 * Build: winegcc -b x86_64-windows -mconsole -I$BD/include -I wine/include
 *        -o d2d-source-rect.exe d2d-source-rect.c -ld2d1 -lole32 */
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <stdio.h>
#include <d2d1_1.h>
#include <wincodec.h>


int main(void)
{
    static const DWORD pixels[3] = {0xffff0000, 0xff00ff00, 0xff0000ff};
    D2D1_RENDER_TARGET_PROPERTIES rt_desc = {0};
    D2D1_BITMAP_PROPERTIES bitmap_desc = {0};
    IWICImagingFactory *wic; IWICBitmap *wic_bitmap; IWICBitmapLock *lock;
    ID2D1Factory *factory; ID2D1RenderTarget *rt; ID2D1Bitmap *bitmap;
    D2D1_RECT_F src = {1.0f, 0.0f, 2.0f, 1.0f}, dst = {0.0f, 0.0f, 30.0f, 1.0f}, dst2 = {0.0f, 1.0f, 30.0f, 2.0f};
    D2D1_SIZE_U size = {3, 1};
    WICRect rect = {0, 0, 30, 2};
    UINT stride, len; BYTE *data; HRESULT hr; int x, fail = 0;

    CoInitialize(NULL);
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic);
    IWICImagingFactory_CreateBitmap(wic, 30, 2, &GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &wic_bitmap);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&factory);
    rt_desc.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rt_desc.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    rt_desc.dpiX = rt_desc.dpiY = 96.0f;
    hr = ID2D1Factory_CreateWicBitmapRenderTarget(factory, wic_bitmap, &rt_desc, &rt);
    if (FAILED(hr)) { printf("rt %#lx\n", hr); return 1; }
    bitmap_desc.pixelFormat = rt_desc.pixelFormat;
    bitmap_desc.dpiX = bitmap_desc.dpiY = 96.0f;
    ID2D1RenderTarget_CreateBitmap(rt, size, pixels, 12, &bitmap_desc, &bitmap);
    ID2D1RenderTarget_BeginDraw(rt);
    ID2D1RenderTarget_DrawBitmap(rt, bitmap, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
    ID2D1RenderTarget_DrawBitmap(rt, bitmap, &dst2, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
    hr = ID2D1RenderTarget_EndDraw(rt, NULL, NULL);
    IWICBitmap_Lock(wic_bitmap, &rect, WICBitmapLockRead, &lock);
    IWICBitmapLock_GetStride(lock, &stride);
    IWICBitmapLock_GetDataPointer(lock, &len, &data);
    printf("source rect:");
    for (x = 0; x < 30; ++x)
    {
        DWORD p = ((DWORD *)data)[x];
        printf(" %06lx", p & 0xffffff);
        if ((p & 0xffffff) != 0x00ff00) fail = 1;
    }
    printf("\nwhole bitmap:");
    for (x = 0; x < 30; x += 3) printf(" %06lx", ((DWORD *)(data + stride))[x] & 0xffffff);
    printf("\n%s (hr %#lx)\n", fail ? "FAIL: sampled past the source rectangle" : "PASS", hr);
    return fail;
}
