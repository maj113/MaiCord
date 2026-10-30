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
struct IDCompositionTarget;
struct IDCompositionTransform;
struct IDCompositionVisual;

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
MAICORD_SYS_TYPES_END

// <dcomp.h>
extern "C" HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice* dxgiDevice, const IID& iid, void** dcompositionDevice);
