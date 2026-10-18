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
struct D2D1_BEZIER_SEGMENT;
struct D2D1_ELLIPSE;
struct D2D1_FACTORY_OPTIONS;
struct D2D1_ROUNDED_RECT;
struct D2D1_STROKE_STYLE_PROPERTIES;
struct D2D_MATRIX_3X2_F;
struct D2D_POINT_2F;
struct D2D_RECT_F;
struct ID2D1EllipseGeometry;
struct ID2D1Factory;
struct ID2D1Geometry;
struct ID2D1GeometrySink;
struct ID2D1PathGeometry;
struct ID2D1RectangleGeometry;
struct ID2D1Resource;
struct ID2D1RoundedRectangleGeometry;
struct ID2D1SimplifiedGeometrySink;
struct ID2D1StrokeStyle;
struct ID2D1TessellationSink;
struct ID2D1TransformedGeometry;

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
typedef D2D_RECT_F D2D1_RECT_F;
typedef D2D_MATRIX_3X2_F D2D1_MATRIX_3X2_F;
// <d2d1.h>
enum D2D1_CAP_STYLE {
    D2D1_CAP_STYLE_FLAT = 0,
    D2D1_CAP_STYLE_SQUARE = 1,
    D2D1_CAP_STYLE_ROUND = 2,
    D2D1_CAP_STYLE_TRIANGLE = 3,
    D2D1_CAP_STYLE_FORCE_DWORD = 0xffffffff
};
enum D2D1_DASH_STYLE {
    D2D1_DASH_STYLE_SOLID = 0,
    D2D1_DASH_STYLE_DASH = 1,
    D2D1_DASH_STYLE_DOT = 2,
    D2D1_DASH_STYLE_DASH_DOT = 3,
    D2D1_DASH_STYLE_DASH_DOT_DOT = 4,
    D2D1_DASH_STYLE_CUSTOM = 5,
    D2D1_DASH_STYLE_FORCE_DWORD = 0xffffffff
};
enum D2D1_LINE_JOIN {
    D2D1_LINE_JOIN_MITER = 0,
    D2D1_LINE_JOIN_BEVEL = 1,
    D2D1_LINE_JOIN_ROUND = 2,
    D2D1_LINE_JOIN_MITER_OR_BEVEL = 3,
    D2D1_LINE_JOIN_FORCE_DWORD = 0xffffffff
};
enum D2D1_COMBINE_MODE {
    D2D1_COMBINE_MODE_UNION = 0,
    D2D1_COMBINE_MODE_INTERSECT = 1,
    D2D1_COMBINE_MODE_XOR = 2,
    D2D1_COMBINE_MODE_EXCLUDE = 3,
    D2D1_COMBINE_MODE_FORCE_DWORD = 0xffffffff
};
enum D2D1_GEOMETRY_RELATION {
    D2D1_GEOMETRY_RELATION_UNKNOWN = 0,
    D2D1_GEOMETRY_RELATION_DISJOINT = 1,
    D2D1_GEOMETRY_RELATION_IS_CONTAINED = 2,
    D2D1_GEOMETRY_RELATION_CONTAINS = 3,
    D2D1_GEOMETRY_RELATION_OVERLAP = 4,
    D2D1_GEOMETRY_RELATION_FORCE_DWORD = 0xffffffff
};
enum D2D1_GEOMETRY_SIMPLIFICATION_OPTION {
    D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES = 0,
    D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES = 1,
    D2D1_GEOMETRY_SIMPLIFICATION_OPTION_FORCE_DWORD = 0xffffffff
};
enum D2D1_FIGURE_BEGIN {
    D2D1_FIGURE_BEGIN_FILLED = 0,
    D2D1_FIGURE_BEGIN_HOLLOW = 1,
    D2D1_FIGURE_BEGIN_FORCE_DWORD = 0xffffffff
};
enum D2D1_FIGURE_END {
    D2D1_FIGURE_END_OPEN = 0,
    D2D1_FIGURE_END_CLOSED = 1,
    D2D1_FIGURE_END_FORCE_DWORD = 0xffffffff
};
struct D2D1_BEZIER_SEGMENT {
    D2D1_POINT_2F point1;
    D2D1_POINT_2F point2;
    D2D1_POINT_2F point3;
};
enum D2D1_PATH_SEGMENT {
    D2D1_PATH_SEGMENT_NONE = 0x00000000,
    D2D1_PATH_SEGMENT_FORCE_UNSTROKED = 0x00000001,
    D2D1_PATH_SEGMENT_FORCE_ROUND_LINE_JOIN = 0x00000002,
    D2D1_PATH_SEGMENT_FORCE_DWORD = 0xffffffff
};
enum D2D1_FILL_MODE {
    D2D1_FILL_MODE_ALTERNATE = 0,
    D2D1_FILL_MODE_WINDING = 1,
    D2D1_FILL_MODE_FORCE_DWORD = 0xffffffff
};
struct D2D1_ELLIPSE {
    D2D1_POINT_2F point;
    FLOAT radiusX;
    FLOAT radiusY;
};
struct D2D1_ROUNDED_RECT {
    D2D1_RECT_F rect;
    FLOAT radiusX;
    FLOAT radiusY;
};
struct D2D1_STROKE_STYLE_PROPERTIES {
    D2D1_CAP_STYLE startCap;
    D2D1_CAP_STYLE endCap;
    D2D1_CAP_STYLE dashCap;
    D2D1_LINE_JOIN lineJoin;
    FLOAT miterLimit;
    D2D1_DASH_STYLE dashStyle;
    FLOAT dashOffset;
};
enum D2D1_DEBUG_LEVEL {
    D2D1_DEBUG_LEVEL_NONE = 0,
    D2D1_DEBUG_LEVEL_ERROR = 1,
    D2D1_DEBUG_LEVEL_WARNING = 2,
    D2D1_DEBUG_LEVEL_INFORMATION = 3,
    D2D1_DEBUG_LEVEL_FORCE_DWORD = 0xffffffff
};
enum D2D1_FACTORY_TYPE {
    D2D1_FACTORY_TYPE_SINGLE_THREADED = 0,
    D2D1_FACTORY_TYPE_MULTI_THREADED = 1,
    D2D1_FACTORY_TYPE_FORCE_DWORD = 0xffffffff
};
struct D2D1_FACTORY_OPTIONS {
    D2D1_DEBUG_LEVEL debugLevel;
};
struct __declspec(uuid("2cd90691-12e2-11dc-9fed-001143a055f9")) __declspec(novtable) ID2D1Resource : IUnknown {
    MAICORD_COM_UNUSED(GetFactory);
};
struct __declspec(uuid("2cd9069d-12e2-11dc-9fed-001143a055f9")) __declspec(novtable) ID2D1StrokeStyle : ID2D1Resource {
};
struct __declspec(uuid("2cd906a1-12e2-11dc-9fed-001143a055f9")) __declspec(novtable) ID2D1Geometry : ID2D1Resource {
    virtual __declspec(nothrow) HRESULT __stdcall GetBounds(const D2D1_MATRIX_3X2_F* worldTransform,
        D2D1_RECT_F* bounds) const = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetWidenedBounds(FLOAT strokeWidth, ID2D1StrokeStyle* strokeStyle,
        const D2D1_MATRIX_3X2_F* worldTransform, FLOAT flatteningTolerance, D2D1_RECT_F* bounds) const = 0;
    virtual __declspec(nothrow) HRESULT __stdcall StrokeContainsPoint(D2D1_POINT_2F point, FLOAT strokeWidth,
        ID2D1StrokeStyle* strokeStyle, const D2D1_MATRIX_3X2_F* worldTransform, FLOAT flatteningTolerance,
        BOOL* contains) const = 0;
    virtual __declspec(nothrow) HRESULT __stdcall FillContainsPoint(D2D1_POINT_2F point,
        const D2D1_MATRIX_3X2_F* worldTransform, FLOAT flatteningTolerance, BOOL* contains) const = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CompareWithGeometry(ID2D1Geometry* inputGeometry,
        const D2D1_MATRIX_3X2_F* inputGeometryTransform, FLOAT flatteningTolerance,
        D2D1_GEOMETRY_RELATION* relation) const = 0;
    virtual __declspec(nothrow) HRESULT __stdcall Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION simplificationOption,
        const D2D1_MATRIX_3X2_F* worldTransform, FLOAT flatteningTolerance,
        ID2D1SimplifiedGeometrySink* geometrySink) const = 0;
    virtual __declspec(nothrow) HRESULT __stdcall Tessellate(const D2D1_MATRIX_3X2_F* worldTransform,
        FLOAT flatteningTolerance, ID2D1TessellationSink* tessellationSink) const = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CombineWithGeometry(ID2D1Geometry* inputGeometry,
        D2D1_COMBINE_MODE combineMode, const D2D1_MATRIX_3X2_F* inputGeometryTransform, FLOAT flatteningTolerance,
        ID2D1SimplifiedGeometrySink* geometrySink) const = 0;
    virtual __declspec(nothrow) HRESULT __stdcall Outline(const D2D1_MATRIX_3X2_F* worldTransform,
        FLOAT flatteningTolerance, ID2D1SimplifiedGeometrySink* geometrySink) const = 0;
    virtual __declspec(nothrow) HRESULT __stdcall ComputeArea(const D2D1_MATRIX_3X2_F* worldTransform,
        FLOAT flatteningTolerance, FLOAT* area) const = 0;
    virtual __declspec(nothrow) HRESULT __stdcall ComputeLength(const D2D1_MATRIX_3X2_F* worldTransform,
        FLOAT flatteningTolerance, FLOAT* length) const = 0;
    virtual __declspec(nothrow) HRESULT __stdcall ComputePointAtLength(FLOAT length,
        const D2D1_MATRIX_3X2_F* worldTransform, FLOAT flatteningTolerance, D2D1_POINT_2F* point,
        D2D1_POINT_2F* unitTangentVector) const = 0;
    virtual __declspec(nothrow) HRESULT __stdcall Widen(FLOAT strokeWidth, ID2D1StrokeStyle* strokeStyle,
        const D2D1_MATRIX_3X2_F* worldTransform, FLOAT flatteningTolerance,
        ID2D1SimplifiedGeometrySink* geometrySink) const = 0;
    __declspec(nothrow) HRESULT Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION simplificationOption, const D2D1_MATRIX_3X2_F& worldTransform, FLOAT flatteningTolerance, ID2D1SimplifiedGeometrySink* geometrySink) const { return Simplify(simplificationOption,& worldTransform, flatteningTolerance, geometrySink); }
    __declspec(nothrow) HRESULT Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION simplificationOption, const D2D1_MATRIX_3X2_F* worldTransform, ID2D1SimplifiedGeometrySink* geometrySink) const { return Simplify(simplificationOption, worldTransform, (0.25f), geometrySink); }
    __declspec(nothrow) HRESULT Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION simplificationOption, const D2D1_MATRIX_3X2_F& worldTransform, ID2D1SimplifiedGeometrySink* geometrySink) const { return Simplify(simplificationOption,& worldTransform, (0.25f), geometrySink); }
    __declspec(nothrow) HRESULT CombineWithGeometry(ID2D1Geometry* inputGeometry, D2D1_COMBINE_MODE combineMode, const D2D1_MATRIX_3X2_F& inputGeometryTransform, FLOAT flatteningTolerance, ID2D1SimplifiedGeometrySink* geometrySink) const { return CombineWithGeometry(inputGeometry, combineMode,& inputGeometryTransform, flatteningTolerance, geometrySink); }
    __declspec(nothrow) HRESULT CombineWithGeometry(ID2D1Geometry* inputGeometry, D2D1_COMBINE_MODE combineMode, const D2D1_MATRIX_3X2_F* inputGeometryTransform, ID2D1SimplifiedGeometrySink* geometrySink) const { return CombineWithGeometry(inputGeometry, combineMode, inputGeometryTransform, (0.25f), geometrySink); }
    __declspec(nothrow) HRESULT CombineWithGeometry(ID2D1Geometry* inputGeometry, D2D1_COMBINE_MODE combineMode, const D2D1_MATRIX_3X2_F& inputGeometryTransform, ID2D1SimplifiedGeometrySink* geometrySink) const { return CombineWithGeometry(inputGeometry, combineMode,& inputGeometryTransform, (0.25f), geometrySink); }
    __declspec(nothrow) HRESULT ComputeArea(const D2D1_MATRIX_3X2_F& worldTransform, FLOAT flatteningTolerance, FLOAT* area) const { return ComputeArea(& worldTransform, flatteningTolerance, area); }
    __declspec(nothrow) HRESULT ComputeArea(const D2D1_MATRIX_3X2_F* worldTransform, FLOAT* area) const { return ComputeArea(worldTransform, (0.25f), area); }
    __declspec(nothrow) HRESULT ComputeArea(const D2D1_MATRIX_3X2_F& worldTransform, FLOAT* area) const { return ComputeArea(& worldTransform, (0.25f), area); }
    __declspec(nothrow) HRESULT Widen(FLOAT strokeWidth, ID2D1StrokeStyle* strokeStyle, const D2D1_MATRIX_3X2_F& worldTransform, FLOAT flatteningTolerance, ID2D1SimplifiedGeometrySink* geometrySink) const { return Widen(strokeWidth, strokeStyle,& worldTransform, flatteningTolerance, geometrySink); }
    __declspec(nothrow) HRESULT Widen(FLOAT strokeWidth, ID2D1StrokeStyle* strokeStyle, const D2D1_MATRIX_3X2_F* worldTransform, ID2D1SimplifiedGeometrySink* geometrySink) const { return Widen(strokeWidth, strokeStyle, worldTransform, (0.25f), geometrySink); }
    __declspec(nothrow) HRESULT Widen(FLOAT strokeWidth, ID2D1StrokeStyle* strokeStyle, const D2D1_MATRIX_3X2_F& worldTransform, ID2D1SimplifiedGeometrySink* geometrySink) const { return Widen(strokeWidth, strokeStyle,& worldTransform, (0.25f), geometrySink); }
};
struct __declspec(uuid("2cd906a2-12e2-11dc-9fed-001143a055f9")) __declspec(novtable) ID2D1RectangleGeometry : ID2D1Geometry {
};
struct __declspec(uuid("2cd906a3-12e2-11dc-9fed-001143a055f9")) __declspec(novtable) ID2D1RoundedRectangleGeometry : ID2D1Geometry {
};
struct __declspec(uuid("2cd906a4-12e2-11dc-9fed-001143a055f9")) __declspec(novtable) ID2D1EllipseGeometry : ID2D1Geometry {
};
struct __declspec(uuid("2cd906bb-12e2-11dc-9fed-001143a055f9")) __declspec(novtable) ID2D1TransformedGeometry : ID2D1Geometry {
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
struct __declspec(uuid("2cd9069f-12e2-11dc-9fed-001143a055f9")) __declspec(novtable) ID2D1GeometrySink : ID2D1SimplifiedGeometrySink {
    MAICORD_COM_UNUSED(AddLine);
    virtual __declspec(nothrow) void __stdcall AddBezier(const D2D1_BEZIER_SEGMENT* bezier) = 0;
    __declspec(nothrow) void AddBezier(const D2D1_BEZIER_SEGMENT& bezier) { AddBezier(& bezier); }
};
struct __declspec(uuid("2cd906c1-12e2-11dc-9fed-001143a055f9")) __declspec(novtable) ID2D1TessellationSink : IUnknown {
    MAICORD_COM_UNUSED(AddTriangles);
    virtual __declspec(nothrow) HRESULT __stdcall Close() = 0;
};
struct __declspec(uuid("2cd906a5-12e2-11dc-9fed-001143a055f9")) __declspec(novtable) ID2D1PathGeometry : ID2D1Geometry {
    virtual __declspec(nothrow) HRESULT __stdcall Open(ID2D1GeometrySink** geometrySink) = 0;
};
struct __declspec(uuid("06152247-6f50-465a-9245-118bfd3b6007")) __declspec(novtable) ID2D1Factory : IUnknown {
    MAICORD_COM_UNUSED(ReloadSystemMetrics);
    MAICORD_COM_UNUSED(GetDesktopDpi);
    virtual __declspec(nothrow) HRESULT __stdcall CreateRectangleGeometry(const D2D1_RECT_F* rectangle,
        ID2D1RectangleGeometry** rectangleGeometry) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateRoundedRectangleGeometry(const D2D1_ROUNDED_RECT* roundedRectangle,
        ID2D1RoundedRectangleGeometry** roundedRectangleGeometry) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateEllipseGeometry(const D2D1_ELLIPSE* ellipse,
        ID2D1EllipseGeometry** ellipseGeometry) = 0;
    MAICORD_COM_UNUSED(CreateGeometryGroup);
    virtual __declspec(nothrow) HRESULT __stdcall CreateTransformedGeometry(ID2D1Geometry* sourceGeometry,
        const D2D1_MATRIX_3X2_F* transform, ID2D1TransformedGeometry** transformedGeometry) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreatePathGeometry(ID2D1PathGeometry** pathGeometry) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateStrokeStyle(const D2D1_STROKE_STYLE_PROPERTIES* strokeStyleProperties,
        const FLOAT* dashes, UINT32 dashesCount, ID2D1StrokeStyle** strokeStyle) = 0;
    __declspec(nothrow) HRESULT CreateRoundedRectangleGeometry(const D2D1_ROUNDED_RECT& roundedRectangle, ID2D1RoundedRectangleGeometry** roundedRectangleGeometry) { return CreateRoundedRectangleGeometry(& roundedRectangle, roundedRectangleGeometry); }
    __declspec(nothrow) HRESULT CreateEllipseGeometry(const D2D1_ELLIPSE& ellipse, ID2D1EllipseGeometry** ellipseGeometry) { return CreateEllipseGeometry(& ellipse, ellipseGeometry); }
    __declspec(nothrow) HRESULT CreateTransformedGeometry(ID2D1Geometry* sourceGeometry, const D2D1_MATRIX_3X2_F& transform, ID2D1TransformedGeometry** transformedGeometry) { return CreateTransformedGeometry(sourceGeometry,& transform, transformedGeometry); }
    __declspec(nothrow) HRESULT CreateStrokeStyle(const D2D1_STROKE_STYLE_PROPERTIES& strokeStyleProperties, const FLOAT* dashes, UINT32 dashesCount, ID2D1StrokeStyle** strokeStyle) { return CreateStrokeStyle(& strokeStyleProperties, dashes, dashesCount, strokeStyle); }
};
MAICORD_SYS_TYPES_END

// <d2d1.h>
extern "C" HRESULT WINAPI D2D1CreateFactory(D2D1_FACTORY_TYPE factoryType, const IID& riid,
    const D2D1_FACTORY_OPTIONS* pFactoryOptions, void** ppIFactory);
