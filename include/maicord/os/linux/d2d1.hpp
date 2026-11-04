#pragma once

#include "maicord/render/effects/vector_geometry.hpp"

// The part of Direct2D's geometry interface that portable code calls (the Lottie compiler: path, ellipse, rounded
// rectangle and rectangle geometries, transforms, bounds, simplification, area, boolean combination and stroke
// widening), under Direct2D's names, on Linux (D17). What backs it is the in-house engine of
// src/render/effects/vector_geometry.cpp; maicord/sys/d2d1.hpp declares the real interfaces for Windows. Reference
// counts are not kept here: a Factory owns every geometry it made and frees them together.
#if !defined(STDMETHODCALLTYPE)
#define STDMETHODCALLTYPE
#endif

using ID2D1Factory = maicord::geometry::Factory;
using ID2D1Geometry = maicord::geometry::Geometry;
using ID2D1PathGeometry = maicord::geometry::Geometry;
using ID2D1TransformedGeometry = maicord::geometry::Geometry;
using ID2D1RectangleGeometry = maicord::geometry::Geometry;
using ID2D1RoundedRectangleGeometry = maicord::geometry::Geometry;
using ID2D1EllipseGeometry = maicord::geometry::Geometry;
using ID2D1GeometrySink = maicord::geometry::GeometrySink;
using ID2D1SimplifiedGeometrySink = maicord::geometry::SimplifiedGeometrySink;
using ID2D1StrokeStyle = maicord::geometry::StrokeStyle;

using D2D1_POINT_2F = maicord::geometry::Point;
using D2D1_RECT_F = maicord::geometry::Rect;
using D2D1_MATRIX_3X2_F = maicord::geometry::Matrix;
using D2D1_BEZIER_SEGMENT = maicord::geometry::BezierSegment;
using D2D1_ELLIPSE = maicord::geometry::Ellipse;
using D2D1_ROUNDED_RECT = maicord::geometry::RoundedRect;
using D2D1_STROKE_STYLE_PROPERTIES = maicord::geometry::StrokeStyleProperties;

using D2D1_FILL_MODE = maicord::geometry::FillMode;
using D2D1_FIGURE_BEGIN = maicord::geometry::FigureBegin;
using D2D1_FIGURE_END = maicord::geometry::FigureEnd;
using D2D1_COMBINE_MODE = maicord::geometry::CombineMode;
using D2D1_CAP_STYLE = maicord::geometry::CapStyle;
using D2D1_LINE_JOIN = maicord::geometry::LineJoin;
using D2D1_PATH_SEGMENT = maicord::geometry::PathSegment;
using D2D1_GEOMETRY_SIMPLIFICATION_OPTION = maicord::geometry::SimplificationOption;

inline constexpr D2D1_FILL_MODE D2D1_FILL_MODE_ALTERNATE = maicord::geometry::fill_alternate;
inline constexpr D2D1_FILL_MODE D2D1_FILL_MODE_WINDING = maicord::geometry::fill_winding;
inline constexpr D2D1_FIGURE_BEGIN D2D1_FIGURE_BEGIN_FILLED = maicord::geometry::figure_filled;
inline constexpr D2D1_FIGURE_END D2D1_FIGURE_END_OPEN = maicord::geometry::figure_open;
inline constexpr D2D1_FIGURE_END D2D1_FIGURE_END_CLOSED = maicord::geometry::figure_closed;
inline constexpr D2D1_COMBINE_MODE D2D1_COMBINE_MODE_UNION = maicord::geometry::combine_union;
inline constexpr D2D1_COMBINE_MODE D2D1_COMBINE_MODE_INTERSECT = maicord::geometry::combine_intersect;
inline constexpr D2D1_COMBINE_MODE D2D1_COMBINE_MODE_XOR = maicord::geometry::combine_xor;
inline constexpr D2D1_COMBINE_MODE D2D1_COMBINE_MODE_EXCLUDE = maicord::geometry::combine_exclude;
inline constexpr D2D1_CAP_STYLE D2D1_CAP_STYLE_FLAT = maicord::geometry::cap_flat;
inline constexpr D2D1_CAP_STYLE D2D1_CAP_STYLE_SQUARE = maicord::geometry::cap_square;
inline constexpr D2D1_CAP_STYLE D2D1_CAP_STYLE_ROUND = maicord::geometry::cap_round;
inline constexpr D2D1_LINE_JOIN D2D1_LINE_JOIN_MITER = maicord::geometry::join_miter;
inline constexpr D2D1_LINE_JOIN D2D1_LINE_JOIN_BEVEL = maicord::geometry::join_bevel;
inline constexpr D2D1_LINE_JOIN D2D1_LINE_JOIN_ROUND = maicord::geometry::join_round;
inline constexpr D2D1_GEOMETRY_SIMPLIFICATION_OPTION D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES =
    maicord::geometry::simplify_cubics_and_lines;
