#pragma once
#include "maicord/render/effects/vector_geometry.hpp"
#include "maicord/core/memory.hpp"

// What the parts of the geometry engine share: the figure table of a Geometry, the scratch containers and the
// operations implemented outside vector_geometry.cpp.
namespace maicord::geometry {

// One figure: `kind_count` segments from kinds[kind_begin], over points[point_begin] (the start) followed by one point
// per line and three per cubic. Kinds: 0 a line, 1 a cubic.
struct Figure {
    u32 kind_begin, kind_count, point_begin;
    u32 closed;
};
inline constexpr u8 kind_line = 0, kind_cubic = 1;

inline double root(double value) noexcept {
    return __builtin_elementwise_sqrt(value);
}
inline double absolute(double value) noexcept {
    return value < 0 ? -value : value;
}
inline Point transform_point(const Matrix* m, Point p) noexcept {
    if (!m)
        return p;
    return {p.x * m->_11 + p.y * m->_21 + m->_31, p.x * m->_12 + p.y * m->_22 + m->_32};
}
// A cubic as x0, y0, x1, y1, x2, y2, x3, y3 splits at t into the part before and the part after.
inline void split_cubic(const double (&in)[8], double t, double (&left)[8], double (&right)[8]) noexcept {
    double mid[6], more[4], last[2];
    for (u32 k = 0; k < 6; ++k)
        mid[k] = in[k] + (in[k + 2] - in[k]) * t;
    for (u32 k = 0; k < 4; ++k)
        more[k] = mid[k] + (mid[k + 2] - mid[k]) * t;
    for (u32 k = 0; k < 2; ++k)
        last[k] = more[k] + (more[k + 2] - more[k]) * t;
    for (u32 k = 0; k < 2; ++k) {
        left[k] = in[k];
        left[2 + k] = mid[k];
        left[4 + k] = more[k];
        left[6 + k] = last[k];
        right[k] = last[k];
        right[2 + k] = more[2 + k];
        right[4 + k] = mid[4 + k];
        right[6 + k] = in[6 + k];
    }
}

// Whether a cubic stays within `tolerance` of its chord: a curve strays from it by at most three quarters of how far
// its handles do (and a handle beyond the chord's ends counts by how far past them it is).
inline bool cubic_flat(const double (&c)[8], double tolerance) noexcept {
    const double vx = c[6] - c[0], vy = c[7] - c[1], square = vx * vx + vy * vy, length = root(square);
    double worst = 0;
    for (u32 k = 1; k <= 2; ++k) {
        const double px = c[k * 2] - c[0], py = c[k * 2 + 1] - c[1];
        double far;
        if (square > 1e-18) {
            const double along = (px * vx + py * vy) / square, across = (px * vy - py * vx) / length;
            const double excess = along < 0 ? -along * length : along > 1 ? (along - 1) * length : 0;
            far = root(across * across + excess * excess);
        } else {
            far = root(px * px + py * py);
        }
        worst = maximum(worst, far);
    }
    return .75 * worst <= tolerance;
}

// Calls vertex(t, x, y) at the end of each piece a cubic is cut into, in order, the last at t = 1 and the cubic's own end
// point. Pieces are halves of halves, so every t is a short binary fraction and exact in a float. A straight cubic,
// whatever its handles, is one piece.
template <typename Vertex> void flatten_cubic(const double (&curve)[8], double tolerance, Vertex&& vertex) noexcept {
    struct Piece {
        double c[8];
        double t0, t1;
        u32 depth;
    };
    Piece stack[48];
    u32 top = 1;
    copy_bytes(stack[0].c, curve, sizeof(curve));
    stack[0].t0 = 0;
    stack[0].t1 = 1;
    stack[0].depth = 0;
    while (top) {
        const Piece piece = stack[--top];
        if (piece.depth >= 16 || top + 2 >= 48 || cubic_flat(piece.c, tolerance)) {
            vertex(static_cast<float>(piece.t1), piece.c[6], piece.c[7]);
            continue;
        }
        const double mid = (piece.t0 + piece.t1) * .5;
        Piece& right = stack[top];
        Piece& left = stack[top + 1];
        split_cubic(piece.c, .5, left.c, right.c);
        left.t0 = piece.t0;
        left.t1 = right.t0 = mid;
        right.t1 = piece.t1;
        left.depth = right.depth = piece.depth + 1;
        top += 2;
    }
}

// A growable array in an arena that is rewound as a whole; T is trivially copyable.
template <typename T> struct Vec {
    T* data;
    u32 count, capacity;
};
struct Scratch {
    Arena* arena;
    bool failed;
};
template <typename T> bool vec_reserve(Scratch& s, Vec<T>& v, u32 needed) noexcept {
    if (needed <= v.capacity)
        return true;
    u32 next = v.capacity * 2;
    if (next < needed)
        next = needed;
    if (next < 16)
        next = 16;
    T* fresh = arena_allocate<T>(s.arena, next);
    if (!fresh) {
        s.failed = true;
        return false;
    }
    if (v.count)
        copy_bytes(fresh, v.data, static_cast<u32>(v.count * sizeof(T)));
    v.data = fresh;
    v.capacity = next;
    return true;
}
template <typename T> T* vec_push(Scratch& s, Vec<T>& v) noexcept {
    if (v.count == v.capacity && !vec_reserve(s, v, v.count + 1))
        return nullptr;
    return &v.data[v.count++];
}

// The outline of `a` and `b` (null: nothing) combined by `mode`, flattened to `tolerance`, into `sink` as resolved rings.
Result overlay_combine(Factory* factory, const Geometry* a, const Matrix* a_transform, const Geometry* b,
                       const Matrix* b_transform, CombineMode mode, float tolerance,
                       SimplifiedGeometrySink* sink) noexcept;
// The area of the nonzero fill of `geometry`, flattened to `tolerance`.
Result overlay_area(Factory* factory, const Geometry* geometry, const Matrix* transform, float tolerance,
                    double* area) noexcept;
Result stroke_outline(Factory* factory, const Geometry* geometry, float width, const StrokeStyleProperties& style,
                      float tolerance, SimplifiedGeometrySink* sink) noexcept;

} // namespace maicord::geometry
