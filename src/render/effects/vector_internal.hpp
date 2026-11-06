#pragma once
#include "maicord/render/effects/vector_geometry.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/memory.hpp"

// What the parts of the geometry engine share: the factory, curve arithmetic, growable arrays.
namespace maicord::geometry {
// What a combination leaves for the next ones that take its result as an operand (see vector_overlay.cpp): the
// result's boundary edges, already split, and the curves they follow. The factory keeps two (say a clip region and
// the leaf just clipped by it), each in an arena of its own, and replaces the one used or made least recently.
struct Reuse {
    const Geometry* geometry;
    void* edges;
    void* curves;
    u32 edge_count, curve_count, lines, stamp;
    double scale, tolerance;
};
struct Factory {
    Arena permanent, scratch, reused[2];
    Reuse reuse[2];
    u32 clock;
};

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

// A growable array in an arena that is rewound as a whole; T is trivially copyable.
template <typename T> struct Vec {
    T* data;
    u32 count, capacity;
};
struct Scratch {
    Arena* arena;
    bool failed;
};
// grow in the scratch arena; a failure marks `s`.
bool vec_grow(Scratch& s, void** data, u32* capacity, u32 count, u32 needed, u32 size) noexcept;
template <typename T> bool vec_reserve(Scratch& s, Vec<T>& v, u32 needed) noexcept {
    return needed <= v.capacity || vec_grow(s, reinterpret_cast<void**>(&v.data), &v.capacity, v.count, needed, sizeof(T));
}
template <typename T> T* vec_push(Scratch& s, Vec<T>& v) noexcept {
    if (v.count == v.capacity && !vec_reserve(s, v, v.count + 1))
        return nullptr;
    return &v.data[v.count++];
}

// The end of each piece a cubic is cut into, in order, appended to `out`: pieces that stay within `tolerance` of their
// chords and, with a `min_cosine` above -1, turn by no more than its angle. The last is at t = 1, the cubic's own end
// point. Pieces are halves of halves, so every t is a short binary fraction and exact in a float. A straight cubic,
// whatever its handles, is one piece.
struct Flat {
    double x, y;
    float t;
};
bool flatten_cubic(Scratch& s, const double (&curve)[8], double tolerance, double min_cosine, Vec<Flat>& out) noexcept;

} // namespace maicord::geometry
