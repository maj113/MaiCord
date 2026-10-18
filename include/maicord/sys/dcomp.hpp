#pragma once

// DirectComposition, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/d2d1.hpp"
#include "maicord/sys/dxgi.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <dcomp.h>
#endif

MAICORD_SYS_TYPES_BEGIN
struct IDCompositionAnimation;
struct IDCompositionClip;
struct IDCompositionDevice;
struct IDCompositionEffect;
struct IDCompositionTarget;
struct IDCompositionTransform3D;
struct IDCompositionTransform;
struct IDCompositionVisual;

// <dcompanimation.h>
struct __declspec(uuid("CBFD91D9-51B2-45e4-B3DE-D19CCFB863C5")) __declspec(novtable) IDCompositionAnimation : IUnknown {
    virtual HRESULT __stdcall Reset() = 0;
    MAICORD_COM_UNUSED(SetAbsoluteBeginTime);
    MAICORD_COM_UNUSED(AddCubic);
    MAICORD_COM_UNUSED(AddSinusoidal);
    MAICORD_COM_UNUSED(AddRepeat);
    virtual HRESULT __stdcall End(double endOffset, float endValue) = 0;
};
// <dcomp.h>
struct __declspec(uuid("C37EA93A-E7AA-450D-B16F-9746CB0407F3")) __declspec(novtable) IDCompositionDevice : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall Commit() = 0;
    MAICORD_COM_UNUSED(WaitForCommitCompletion);
    MAICORD_COM_UNUSED(GetFrameStatistics);
    virtual __declspec(nothrow) HRESULT __stdcall CreateTargetForHwnd(HWND hwnd, BOOL topmost,
        IDCompositionTarget** target) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateVisual(IDCompositionVisual** visual) = 0;
};
struct __declspec(uuid("eacdd04c-117e-4e17-88f4-d1b12b0e3d89")) __declspec(novtable) IDCompositionTarget : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall SetRoot(IDCompositionVisual* visual) = 0;
};
struct __declspec(uuid("4d93059d-097b-4651-9a60-f0f25116e2f3")) __declspec(novtable) IDCompositionVisual : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall SetOffsetX(float offsetX) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetOffsetX(IDCompositionAnimation* animation) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetOffsetY(float offsetY) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetOffsetY(IDCompositionAnimation* animation) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetTransform(const D2D_MATRIX_3X2_F& matrix) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetTransform(IDCompositionTransform* transform) = 0;
    MAICORD_COM_UNUSED(SetTransformParent);
    MAICORD_COM_UNUSED(SetEffect);
    MAICORD_COM_UNUSED(SetBitmapInterpolationMode);
    MAICORD_COM_UNUSED(SetBorderMode);
    virtual __declspec(nothrow) HRESULT __stdcall SetClip(const D2D_RECT_F& rect) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetClip(IDCompositionClip* clip) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetContent(IUnknown* content) = 0;
};
struct __declspec(uuid("EC81B08F-BFCB-4e8d-B193-A915587999E8")) __declspec(novtable) IDCompositionEffect : public IUnknown {
};
struct __declspec(uuid("71185722-246B-41f2-AAD1-0443F7F4BFC2")) __declspec(novtable) IDCompositionTransform3D : public IDCompositionEffect {
};
struct __declspec(uuid("FD55FAA7-37E0-4c20-95D2-9BE45BC33F55")) __declspec(novtable) IDCompositionTransform : public IDCompositionTransform3D {
};
struct __declspec(uuid("64AC3703-9D3F-45ec-A109-7CAC0E7A13A7")) __declspec(novtable) IDCompositionClip : public IUnknown {
};
MAICORD_SYS_TYPES_END

// <dcomp.h>
extern "C" HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice* dxgiDevice, const IID& iid, void** dcompositionDevice);
