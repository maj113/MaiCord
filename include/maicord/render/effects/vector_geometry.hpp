#pragma once
#include "maicord/core/arena.hpp"
#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

// A small vector geometry engine for the Lottie compiler: the part of Direct2D's geometry interface it calls, with
// Direct2D's method names, over curves and lines in float coordinates. Windows keeps Direct2D itself (see
// lottie_geometry.hpp); this is what runs elsewhere, and what the tests compare with Direct2D's results.
//
// A geometry is a set of figures of cubic Beziers and lines, filled by the nonzero rule: figures are closed
// implicitly when filled, and a path never carries a fill mode of its own. Everything is built in the arenas of one
// Factory and freed with it: AddRef and Release exist so the callers' ownership code reads as for Direct2D, and do
// nothing. Boolean results keep the original curves between the points where the outlines cross (the flattening
// tolerance bounds how far a result may leave the exact outline). All failures are results, never exceptions: an
// arena that cannot grow is result_out_of_memory.
namespace maicord::geometry {

// An HRESULT's width and values (long on Windows, int elsewhere), so a sink that overrides Close reads the same.
using Result = HRESULT;
inline constexpr Result result_ok = 0;
inline constexpr Result result_out_of_memory = static_cast<Result>(0x8007000Eu);
inline constexpr Result result_invalid_argument = static_cast<Result>(0x80070057u);

struct Point {
    float x, y;
};
struct Rect {
    float left, top, right, bottom;
};
// Row vector convention, as Direct2D: x' = x * _11 + y * _21 + _31.
struct Matrix {
    float _11, _12, _21, _22, _31, _32;
};
struct BezierSegment {
    Point point1, point2, point3;
};
struct Ellipse {
    Point point;
    float radiusX, radiusY;
};
struct RoundedRect {
    Rect rect;
    float radiusX, radiusY;
};

// The numbering of each enumeration is Direct2D's.
enum FillMode : i32 { fill_alternate = 0, fill_winding = 1 };
enum FigureBegin : i32 { figure_filled = 0, figure_hollow = 1 };
enum FigureEnd : i32 { figure_open = 0, figure_closed = 1 };
enum CombineMode : i32 { combine_union = 0, combine_intersect = 1, combine_xor = 2, combine_exclude = 3 };
enum CapStyle : i32 { cap_flat = 0, cap_square = 1, cap_round = 2 };
enum LineJoin : i32 { join_miter = 0, join_bevel = 1, join_round = 2 };
enum SimplificationOption : i32 { simplify_cubics_and_lines = 0 };
enum PathSegment : i32 { segment_none = 0 };

struct StrokeStyleProperties {
    CapStyle startCap, endCap, dashCap;
    LineJoin lineJoin;
    float miterLimit;
    i32 dashStyle;
    float dashOffset;
};

// Receives figures: SetFillMode, then per figure BeginFigure, lines and cubics, EndFigure; Close ends the stream. The
// defaults do nothing so that a sink overrides only what it reads.
struct SimplifiedGeometrySink {
    virtual void SetFillMode(FillMode) noexcept {}
    virtual void SetSegmentFlags(PathSegment) noexcept {}
    virtual void BeginFigure(Point, FigureBegin) noexcept {}
    virtual void AddLines(const Point*, u32) noexcept {}
    virtual void AddBeziers(const BezierSegment*, u32) noexcept {}
    virtual void EndFigure(FigureEnd) noexcept {}
    virtual Result Close() noexcept { return result_ok; }
};
// What Geometry::Open hands out: a path under construction (its lines go through AddLines, curves through either).
struct GeometrySink : SimplifiedGeometrySink {
    virtual void AddBezier(const BezierSegment&) noexcept {}
    void Release() noexcept {}
};

struct Factory;
struct Figure;
struct StrokeStyle {
    StrokeStyleProperties properties;
    void Release() noexcept {}
};

struct Geometry {
    void AddRef() noexcept {}
    void Release() noexcept {}
    // The exact bounds of the transformed outline; an empty geometry reports an inverted rectangle.
    Result GetBounds(const Matrix* transform, Rect* bounds) const noexcept;
    // Every figure through `sink`, transformed, as cubics and lines: nothing is flattened.
    Result Simplify(SimplificationOption option, const Matrix* transform, float tolerance,
                    SimplifiedGeometrySink* sink) const noexcept;
    // The area of the filled region (overlaps counted once) of the outline flattened to `tolerance`.
    Result ComputeArea(const Matrix* transform, float tolerance, float* area) const noexcept;
    // This combined with `other` (both flattened to `tolerance`), into `sink` (not closed): resolved figures, none
    // overlapping, holes opposite to outlines.
    Result CombineWithGeometry(const Geometry* other, CombineMode mode, const Matrix* other_transform,
                               float tolerance, SimplifiedGeometrySink* sink) const noexcept;
    // The outline of the stroke of every figure (flat, square or round caps; miter, bevel or round joins; no dashes),
    // into `sink` (not closed): polygons, overlapping where the stroke does.
    Result Widen(float width, const StrokeStyle* style, const Matrix* transform, float tolerance,
                 SimplifiedGeometrySink* sink) const noexcept;
    // Starts building a path geometry (only one made by CreatePathGeometry); Close on the sink finishes it.
    Result Open(GeometrySink** sink) noexcept;

    Factory* factory;
    Point* points;
    u8* kinds;
    Figure* figures;
    u32 point_count, kind_count, figure_count;
    u32 point_capacity, kind_capacity, figure_capacity;
    bool building;
};

struct Factory {
    Result CreateRectangleGeometry(const Rect* rectangle, Geometry** geometry) noexcept;
    Result CreateRoundedRectangleGeometry(const RoundedRect& rectangle, Geometry** geometry) noexcept;
    Result CreateEllipseGeometry(const Ellipse& ellipse, Geometry** geometry) noexcept;
    Result CreateTransformedGeometry(const Geometry* source, const Matrix* transform, Geometry** geometry) noexcept;
    Result CreatePathGeometry(Geometry** geometry) noexcept;
    Result CreateStrokeStyle(const StrokeStyleProperties& properties, const float* dashes, u32 dash_count,
                             StrokeStyle** style) noexcept;
    // Frees the factory and everything it made.
    void Release() noexcept;

    Arena permanent, scratch;
};

// A factory with its arenas reserved; false when the system refuses.
bool factory_create(Factory** factory) noexcept;

} // namespace maicord::geometry
