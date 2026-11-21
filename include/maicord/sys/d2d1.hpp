#pragma once

// Direct2D, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/com.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <d2d1.h>
#endif

MAICORD_SYS_TYPES_BEGIN
struct ID2D1SimplifiedGeometrySink;

// <dcommon.h>
struct D2D_POINT_2F {
    FLOAT x;
    FLOAT y;
};
struct D2D_RECT_F {
    FLOAT left;
    FLOAT top;
    FLOAT right;
    FLOAT bottom;
};
struct D2D_MATRIX_3X2_F {
    union {
        struct {
            FLOAT m11;
            FLOAT m12;
            FLOAT m21;
            FLOAT m22;
            FLOAT dx;
            FLOAT dy;
        };
        struct {
            FLOAT _11, _12;
            FLOAT _21, _22;
            FLOAT _31, _32;
        };
        FLOAT m[3] [2];
    };
};
typedef D2D_POINT_2F D2D1_POINT_2F;
// <d2d1.h>
enum D2D1_FIGURE_BEGIN {
    D2D1_FIGURE_BEGIN_HOLLOW = 1,
    D2D1_FIGURE_BEGIN_FORCE_DWORD = 0xffffffff
};
enum D2D1_FIGURE_END {
    D2D1_FIGURE_END_CLOSED = 1,
    D2D1_FIGURE_END_FORCE_DWORD = 0xffffffff
};
struct D2D1_BEZIER_SEGMENT {
    D2D1_POINT_2F point1;
    D2D1_POINT_2F point2;
    D2D1_POINT_2F point3;
};
enum D2D1_PATH_SEGMENT {
    D2D1_PATH_SEGMENT_FORCE_DWORD = 0xffffffff
};
enum D2D1_FILL_MODE {
    D2D1_FILL_MODE_WINDING = 1,
    D2D1_FILL_MODE_FORCE_DWORD = 0xffffffff
};
struct __declspec(uuid("2cd9069e-12e2-11dc-9fed-001143a055f9")) __declspec(novtable) ID2D1SimplifiedGeometrySink : IUnknown {
    virtual __declspec(nothrow) void __stdcall SetFillMode(D2D1_FILL_MODE fillMode) = 0;
    virtual __declspec(nothrow) void __stdcall SetSegmentFlags(D2D1_PATH_SEGMENT vertexFlags) = 0;
    virtual __declspec(nothrow) void __stdcall BeginFigure(D2D1_POINT_2F startPoint, D2D1_FIGURE_BEGIN figureBegin) = 0;
    virtual __declspec(nothrow) void __stdcall AddLines(const D2D1_POINT_2F* points, UINT32 pointsCount) = 0;
    virtual __declspec(nothrow) void __stdcall AddBeziers(const D2D1_BEZIER_SEGMENT* beziers, UINT32 beziersCount) = 0;
    virtual __declspec(nothrow) void __stdcall EndFigure(D2D1_FIGURE_END figureEnd) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall Close() = 0;
};
MAICORD_SYS_TYPES_END
