/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Visible WinRT drop-shadow fixture, captured by shadow-render.py. */
#define COBJMACROS
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <stdio.h>
#include <string.h>
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Numerics
#define WIDL_using_Windows_UI
#define WIDL_using_Windows_UI_Composition
#define WIDL_using_Windows_UI_Composition_Core
#define WIDL_using_Windows_UI_Composition_Desktop
#define WIDL_using_Windows_Graphics_DirectX
#include "windows.graphics.directx.h"
#include "windows.ui.composition.h"
#include "windows.ui.composition.core.h"
#include "windows.ui.composition.desktop.h"
#include "windows.ui.composition.interop.h"

#define CHECK(call) do { HRESULT hr=(call); if(FAILED(hr)){fprintf(stderr,"%s hr=%08lx\n",#call,hr);return 2;} } while(0)
struct compositor2;
struct compositor2_vtbl
{
    IInspectableVtbl inspectable;
    void *other_creators[4];
    HRESULT (WINAPI *create_shadow)(struct compositor2 *,IInspectable **);
};
struct compositor2 { const struct compositor2_vtbl *lpVtbl; };
static const GUID compositor2_iid={0x735081dc,0x5e24,0x45da,{0xa3,0x8f,0xe3,0x2c,0xc3,0x49,0xa9,0xa0}};

int main(int argc,char **argv)
{
    IInspectable *instance,*shadow_instance;
    ICompositorController *controller;
    ICompositor *compositor;
    struct compositor2 *compositor2;
    ICompositorDesktopInterop *interop;
    IDesktopWindowTarget *desktop;
    ICompositionTarget *target;
    ISpriteVisual *root,*sprite;
    ISpriteVisual2 *sprite2;
    IVisual *root_visual,*visual;
    IContainerVisual *container;
    IVisualCollection *children;
    ICompositionColorBrush *color;
    ICompositionBrush *brush;
    IDropShadow *drop;
    ICompositionShadow *shadow;
    IDropShadow2 *drop2;
    HWND hwnd;
    WNDCLASSW wc={.lpfnWndProc=DefWindowProcW,.lpszClassName=L"TuxOfficeShadowScene"};
    WCHAR stop[1024];
    HSTRING name;
    MSG message;
    ULONGLONG start;
    unsigned int frames=0;
    BOOL enabled=argc<3 || strcmp(argv[2],"none");
    struct Vector2 size={256,256};
    struct Vector3 offset={64,64,0},shadow_offset={16,16,0};
    struct Color white={255,255,255,255},red={255,255,0,0};
    static const WCHAR controller_name[]=L"Windows.UI.Composition.Core.CompositorController";
    if(argc<2 || !MultiByteToWideChar(CP_UTF8,0,argv[1],-1,stop,1024)) return 1;
    SetProcessDPIAware();
    CHECK(RoInitialize(RO_INIT_MULTITHREADED));
    RegisterClassW(&wc);
    hwnd=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"TuxOffice shadow regression",
            WS_POPUP|WS_VISIBLE,100,100,256,256,NULL,NULL,NULL,NULL);
    if(!hwnd) return 3;
    CHECK(WindowsCreateString(controller_name,sizeof(controller_name)/sizeof(*controller_name)-1,&name));
    CHECK(RoActivateInstance(name,&instance));WindowsDeleteString(name);
    CHECK(IInspectable_QueryInterface(instance,&IID_ICompositorController,(void **)&controller));
    CHECK(ICompositorController_get_Compositor(controller,&compositor));
    CHECK(ICompositor_QueryInterface(compositor,&IID_ICompositorDesktopInterop,(void **)&interop));
    CHECK(ICompositorDesktopInterop_CreateDesktopWindowTarget(interop,hwnd,TRUE,&desktop));
    CHECK(IDesktopWindowTarget_QueryInterface(desktop,&IID_ICompositionTarget,(void **)&target));
    CHECK(ICompositor_CreateSpriteVisual(compositor,&root));
    CHECK(ISpriteVisual_QueryInterface(root,&IID_IVisual,(void **)&root_visual));
    CHECK(IVisual_put_Size(root_visual,size));
    CHECK(ICompositor_CreateColorBrushWithColor(compositor,white,&color));
    CHECK(ICompositionColorBrush_QueryInterface(color,&IID_ICompositionBrush,(void **)&brush));
    CHECK(ISpriteVisual_put_Brush(root,brush));ICompositionBrush_Release(brush);ICompositionColorBrush_Release(color);
    CHECK(ICompositor_CreateSpriteVisual(compositor,&sprite));
    CHECK(ISpriteVisual_QueryInterface(sprite,&IID_IVisual,(void **)&visual));
    size.X=size.Y=64;
    CHECK(IVisual_put_Size(visual,size));CHECK(IVisual_put_Offset(visual,offset));
    CHECK(ICompositor_CreateColorBrushWithColor(compositor,red,&color));
    CHECK(ICompositionColorBrush_QueryInterface(color,&IID_ICompositionBrush,(void **)&brush));
    CHECK(ISpriteVisual_put_Brush(sprite,brush));ICompositionBrush_Release(brush);ICompositionColorBrush_Release(color);
    CHECK(ICompositor_QueryInterface(compositor,&compositor2_iid,(void **)&compositor2));
    CHECK(compositor2->lpVtbl->create_shadow(compositor2,&shadow_instance));
    CHECK(IInspectable_QueryInterface(shadow_instance,&IID_IDropShadow,(void **)&drop));
    CHECK(IDropShadow_put_BlurRadius(drop,4));CHECK(IDropShadow_put_Opacity(drop,.8f));
    CHECK(IDropShadow_put_Offset(drop,shadow_offset));
    if(argc>2 && !strcmp(argv[2],"surface"))
    {
        ID3D11Device *device;
        IDXGIDevice *dxgi;
        ID2D1Device *d2d;
        ICompositorInterop *graphics_interop;
        ICompositionGraphicsDevice *graphics;
        ICompositionDrawingSurface *drawing;
        ICompositionDrawingSurfaceInterop *drawing_interop;
        ICompositionSurface *surface;
        ICompositionSurfaceBrush *surface_brush;
        ID2D1DeviceContext *drawing_context;
        ID2D1SolidColorBrush *ink;
        struct Size pixels={64,64};
        POINT update;
        D2D1_COLOR_F clear={0},opaque={1,1,1,1};
        D2D1_ELLIPSE ellipse={{32,32},32,32};
        CHECK(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                NULL,0,D3D11_SDK_VERSION,&device,NULL,NULL));
        CHECK(ID3D11Device_QueryInterface(device,&IID_IDXGIDevice,(void **)&dxgi));
        CHECK(D2D1CreateDevice(dxgi,NULL,&d2d));
        CHECK(ICompositor_QueryInterface(compositor,&IID_ICompositorInterop,(void **)&graphics_interop));
        CHECK(ICompositorInterop_CreateGraphicsDevice(graphics_interop,(IUnknown *)d2d,&graphics));
        CHECK(ICompositionGraphicsDevice_CreateDrawingSurface(graphics,pixels,
                DirectXPixelFormat_B8G8R8A8UIntNormalized,DirectXAlphaMode_Premultiplied,&drawing));
        CHECK(ICompositionDrawingSurface_QueryInterface(drawing,&IID_ICompositionDrawingSurfaceInterop,(void **)&drawing_interop));
        CHECK(ICompositionDrawingSurfaceInterop_BeginDraw(drawing_interop,NULL,&IID_ID2D1DeviceContext,(void **)&drawing_context,&update));
        ID2D1DeviceContext_Clear(drawing_context,&clear);
        CHECK(ID2D1DeviceContext_CreateSolidColorBrush(drawing_context,&opaque,NULL,&ink));
        ID2D1DeviceContext_FillEllipse(drawing_context,&ellipse,(ID2D1Brush *)ink);
        ID2D1SolidColorBrush_Release(ink);ID2D1DeviceContext_Release(drawing_context);
        CHECK(ICompositionDrawingSurfaceInterop_EndDraw(drawing_interop));
        CHECK(ICompositionDrawingSurface_QueryInterface(drawing,&IID_ICompositionSurface,(void **)&surface));
        CHECK(ICompositor_CreateSurfaceBrushWithSurface(compositor,surface,&surface_brush));
        CHECK(ICompositionSurfaceBrush_QueryInterface(surface_brush,&IID_ICompositionBrush,(void **)&brush));
        CHECK(IDropShadow_put_Mask(drop,brush));
        ICompositionBrush_Release(brush);ICompositionSurfaceBrush_Release(surface_brush);
        ICompositionSurface_Release(surface);ICompositionDrawingSurfaceInterop_Release(drawing_interop);
        ICompositionDrawingSurface_Release(drawing);ICompositionGraphicsDevice_Release(graphics);
        ICompositorInterop_Release(graphics_interop);ID2D1Device_Release(d2d);
        IDXGIDevice_Release(dxgi);ID3D11Device_Release(device);
    }
    if(argc>2 && (!strcmp(argv[2],"masked") || !strcmp(argv[2],"inherit")))
    {
        struct Color transparent={0};
        CHECK(ICompositor_CreateColorBrushWithColor(compositor,transparent,&color));
        CHECK(ICompositionColorBrush_QueryInterface(color,&IID_ICompositionBrush,(void **)&brush));
        CHECK(IDropShadow_put_Mask(drop,brush));
        ICompositionBrush_Release(brush);ICompositionColorBrush_Release(color);
        if(!strcmp(argv[2],"inherit"))
        {
            CHECK(IDropShadow_QueryInterface(drop,&IID_IDropShadow2,(void **)&drop2));
            CHECK(IDropShadow2_put_SourcePolicy(drop2,CompositionDropShadowSourcePolicy_InheritFromVisualContent));
            IDropShadow2_Release(drop2);
        }
    }
    CHECK(IDropShadow_QueryInterface(drop,&IID_ICompositionShadow,(void **)&shadow));
    CHECK(ISpriteVisual_QueryInterface(sprite,&IID_ISpriteVisual2,(void **)&sprite2));
    if(enabled) CHECK(ISpriteVisual2_put_Shadow(sprite2,shadow));
    CHECK(ISpriteVisual_QueryInterface(root,&IID_IContainerVisual,(void **)&container));
    CHECK(IContainerVisual_get_Children(container,&children));
    CHECK(IVisualCollection_InsertAtTop(children,visual));
    CHECK(ICompositionTarget_put_Root(target,root_visual));
    start=GetTickCount64();
    while(GetFileAttributesW(stop)==INVALID_FILE_ATTRIBUTES && GetTickCount64()-start<60000)
    {
        while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)) DispatchMessageW(&message);
        if(frames<25)
        {
            CHECK(ICompositorController_Commit(controller));
            if(++frames==25){printf("READY shadow=%d\n",enabled);fflush(stdout);}
        }
        Sleep(20);
    }
    CHECK(ICompositionTarget_put_Root(target,NULL));
    IVisualCollection_Release(children);IContainerVisual_Release(container);
    ISpriteVisual2_Release(sprite2);ICompositionShadow_Release(shadow);IDropShadow_Release(drop);
    IInspectable_Release(shadow_instance);IInspectable_Release((IInspectable *)compositor2);
    IVisual_Release(visual);ISpriteVisual_Release(sprite);IVisual_Release(root_visual);ISpriteVisual_Release(root);
    ICompositionTarget_Release(target);IDesktopWindowTarget_Release(desktop);
    ICompositorDesktopInterop_Release(interop);ICompositor_Release(compositor);
    ICompositorController_Release(controller);IInspectable_Release(instance);
    DestroyWindow(hwnd);RoUninitialize();
    return 0;
}
