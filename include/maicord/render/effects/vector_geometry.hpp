#pragma once
#include "maicord/core/base.hpp"

// The vector geometry engine of the Lottie compiler (src/render/effects/lottie_program.cpp): figures of cubic Beziers
// or polylines in float coordinates, transforms, the four boolean combinations of nonzero-filled outlines, their area
// and stroke outlines. Everything is built in the arenas of one Factory and freed with it; a failure (an arena that
// cannot grow, a coordinate that is not finite) is a null result.
//
// Boolean results keep the original curves between the points where the outlines cross; the flattening tolerance
// bounds how far a crossing may move. Stroke outlines are polygons, overlapping where the stroke does.
namespace maicord::geometry {

struct Point {
    float x, y;
};
// Row vector convention: x' = x * _11 + y * _21 + _31.
struct Matrix {
    float _11, _12, _21, _22, _31, _32;
};

enum CombineMode : u8 { combine_union = 0, combine_intersect = 1, combine_xor = 2, combine_exclude = 3 };
enum CapStyle : u8 { cap_flat = 0, cap_square = 1, cap_round = 2 };
enum LineJoin : u8 { join_miter = 0, join_bevel = 1, join_round = 2 };
struct StrokeStyle {
    float width, miter_limit;
    CapStyle cap;
    LineJoin join;
};

// One figure: points[first] is its start, followed up to points[end] by three points per cubic (a straight segment has
// its handles on its ends) or, in a polyline, one per vertex. A filled figure is closed implicitly.
struct Figure {
    u32 first, end;
    bool closed, polyline;
};
struct Factory;
struct Geometry {
    Factory* factory;
    Point* points;
    Figure* figures;
    u32 point_count, figure_count, point_capacity, figure_capacity;
};

// A factory with its arenas reserved; false when the system refuses.
bool factory_create(Factory** factory) noexcept;
// Frees everything the factory made. The factory itself, its arenas trimmed to their first pages, may be handed to the
// next factory_create instead of freed.
void factory_release(Factory* factory) noexcept;

// An empty geometry, built a figure at a time: begin, append its points, end. Each call reports whether the arena had
// room; end without a figure does nothing.
Geometry* geometry_create(Factory* factory) noexcept;
bool geometry_begin(Geometry* geometry, Point start, bool polyline) noexcept;
bool geometry_append(Geometry* geometry, const Point* points, u32 count) noexcept;
void geometry_end(Geometry* geometry, bool closed) noexcept;

// A rectangle whose corners are quarter ellipses of radii rx and ry (at most half its sides; none when either is 0).
// With radii of half its sides it is an ellipse.
Geometry* geometry_rounded_rectangle(Factory* factory, float left, float top, float right, float bottom, float rx,
                                     float ry) noexcept;
Geometry* geometry_transform(const Geometry* source, const Matrix& transform) noexcept;
// `a` combined with `b` (both flattened to `tolerance`): closed figures, none overlapping, holes opposite to
// outlines.
Geometry* geometry_combine(const Geometry* a, const Geometry* b, CombineMode mode, float tolerance) noexcept;
// The outline of the stroke of every figure (no dashes), flattened to `tolerance`.
Geometry* geometry_stroke(const Geometry* source, const StrokeStyle& style, float tolerance) noexcept;
// The area of the nonzero fill (overlaps counted once) of the outline under `transform` (null: none), flattened to
// `tolerance`.
bool geometry_area(const Geometry* geometry, const Matrix* transform, float tolerance, double* area) noexcept;

} // namespace maicord::geometry
