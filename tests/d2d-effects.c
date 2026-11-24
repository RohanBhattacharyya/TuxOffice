/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Direct2D's colour effects, checked pixel by pixel: Luminance To Alpha (PowerPoint
 * masks Cameo's camera picture with it), Grayscale, Saturation and Hue Rotation.
 * Draws a white and a red pixel through each and compares with the documented matrices.
 * Build: winegcc -b x86_64-windows -mconsole -I$BD/include -I wine/include
 *        -o d2d-effects.exe d2d-effects.c -ld2d1 -lole32 -luuid */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <stdio.h>
#include <d2d1_1.h>
#include <d2d1effects_2.h>
#include <wincodec.h>
static int failures;
static void run(ID2D1Factory *factory, IWICImagingFactory *wic, const GUID *clsid, const char *name,
                UINT32 prop, float value, const BYTE expect[2][4])
{
    IWICBitmap *target; ID2D1RenderTarget *rt; ID2D1DeviceContext *dc; ID2D1Bitmap *src; ID2D1Effect *fx;
    D2D1_RENDER_TARGET_PROPERTIES props = {0};
    D2D1_BITMAP_PROPERTIES bp = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96};
    /* BGRA premultiplied: white, pure red */
    BYTE pixels[8] = {255,255,255,255, 0,0,255,255}, out[8];
    WICRect r = {0,0,2,1}; HRESULT hr; int i, ok = 1;
    IWICImagingFactory_CreateBitmap(wic, 2, 1, &GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &target);
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM; props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    hr = ID2D1Factory_CreateWicBitmapRenderTarget(factory, target, &props, &rt);
    if (FAILED(hr)) { printf("rt %#lx\n", hr); failures++; return; }
    ID2D1RenderTarget_QueryInterface(rt, &IID_ID2D1DeviceContext, (void **)&dc);
    ID2D1RenderTarget_CreateBitmap(rt, (D2D1_SIZE_U){2,1}, pixels, 8, &bp, &src);
    hr = ID2D1DeviceContext_CreateEffect(dc, clsid, &fx);
    if (FAILED(hr)) { printf("%s: CreateEffect %#lx\n", name, hr); failures++; return; }
    ID2D1Effect_SetInput(fx, 0, (ID2D1Image *)src, TRUE);
    if (prop != ~0u) ID2D1Effect_SetValue(fx, prop, D2D1_PROPERTY_TYPE_FLOAT, (BYTE *)&value, sizeof(value));
    ID2D1RenderTarget_BeginDraw(rt);
    ID2D1RenderTarget_Clear(rt, &(D2D1_COLOR_F){0,0,0,0});
    ID2D1DeviceContext_DrawImage(dc, (ID2D1Image *)fx, NULL, NULL, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
    hr = ID2D1RenderTarget_EndDraw(rt, NULL, NULL);
    IWICBitmap_CopyPixels(target, &r, 8, 8, out);
    for (i = 0; i < 8; i++) if (abs(out[i] - expect[i/4][i%4]) > 3) ok = 0;
    printf("%s %s: EndDraw %#lx, got %d,%d,%d,%d %d,%d,%d,%d (BGRA)\n", ok && SUCCEEDED(hr) ? "ok  " : "FAIL", name, hr,
           out[0],out[1],out[2],out[3],out[4],out[5],out[6],out[7]);
    if (!ok || FAILED(hr)) failures++;
}
int main(void)
{
    ID2D1Factory *factory; IWICImagingFactory *wic;
    static const BYTE lum[2][4] = {{0,0,0,255},{0,0,0,54}};
    static const BYTE gray[2][4] = {{255,255,255,255},{76,76,76,255}};
    static const BYTE desat[2][4] = {{255,255,255,255},{54,54,54,255}};
    static const BYTE same[2][4] = {{255,255,255,255},{0,0,255,255}};
    /* hue 180 on red, SVG matrix: r=0.213-0.787=-0.574->0, g=0.213+0.213=0.426, b=0.426 */
    static const BYTE hue[2][4] = {{255,255,255,255},{109,109,0,255}};
    CoInitialize(NULL);
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&factory);
    run(factory, wic, &CLSID_D2D1LuminanceToAlpha, "luminance-to-alpha", ~0u, 0, lum);
    run(factory, wic, &CLSID_D2D1Grayscale, "grayscale", ~0u, 0, gray);
    run(factory, wic, &CLSID_D2D1Saturation, "saturation 0", 0, 0.0f, desat);
    run(factory, wic, &CLSID_D2D1Saturation, "saturation 1", 0, 1.0f, same);
    run(factory, wic, &CLSID_D2D1HueRotation, "hue 180", 0, 180.0f, hue);
    printf("%d failures\n", failures);
    return failures;
}
