#pragma once
#include "maicord/core/arena.hpp"
#include "maicord/core/base.hpp"

// The coverage the Lottie geometry tests compare: a nonzero fill of line edges onto a grid of square cells, 64
// sub-scanlines per cell row and exact horizontal spans. Both Direct2D's output and the in-house engine's go through
// the same one, so a difference is a difference of outline and not of method.
namespace maicord::lottie_raster {

struct Edge {
    float x0, y0, x1, y1;
};

// Collects the lines of figures; cubics are flattened to `tolerance`.
struct Edges {
    Edge* data;
    u32 count, capacity;
    float cx, cy, sx, sy;

    bool add(float x0, float y0, float x1, float y1) noexcept {
        if (count == capacity)
            return false;
        data[count++] = {x0, y0, x1, y1};
        return true;
    }
    void begin(float x, float y) noexcept {
        cx = sx = x;
        cy = sy = y;
    }
    void line(float x, float y) noexcept {
        add(cx, cy, x, y);
        cx = x;
        cy = y;
    }
    void close() noexcept { line(sx, sy); }
    void cubic(float x1, float y1, float x2, float y2, float x3, float y3, float tolerance) noexcept {
        const double ax = cx - 2.0 * x1 + x2, ay = cy - 2.0 * y1 + y2, bx = x1 - 2.0 * x2 + x3, by = y1 - 2.0 * y2 + y3;
        const double m = __builtin_elementwise_sqrt((ax * ax + ay * ay) > (bx * bx + by * by) ? ax * ax + ay * ay
                                                                                               : bx * bx + by * by);
        double bound = __builtin_elementwise_sqrt(.75 * m / tolerance);
        u32 steps = bound < 1 ? 1u : bound > 512 ? 512u : static_cast<u32>(bound) + 1;
        const float x0 = cx, y0 = cy;
        for (u32 i = 1; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps, u = 1 - t;
            const double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
            line(i == steps ? x3 : static_cast<float>(a * x0 + b * x1 + c * x2 + d * x3),
                 i == steps ? y3 : static_cast<float>(a * y0 + b * y1 + c * y2 + d * y3));
        }
    }
};

// The cells of a grid x grid image covering [0, extent)^2, 0..255 each, and their total coverage in cells.
template <u32 Grid> struct Coverage {
    u8 cells[Grid * Grid];
    float total;
};

inline constexpr u32 sub_scanlines = 64;

// Fills `out` from edges (closed polygons: each figure's last edge returns to its start). Needs `work` bytes of the
// arena for the per-row edge lists.
template <u32 Grid>
bool rasterize(Arena* arena, const Edges& edges, float extent, bool even_odd, Coverage<Grid>* out) noexcept {
    out->total = 0;
    const ArenaMark mark = arena_mark(arena);
    const float cell = extent / Grid;
    u32* start = arena_allocate<u32>(arena, Grid + 2);
    if (!start)
        return false;
    for (u32 r = 0; r < Grid + 2; ++r)
        start[r] = 0;
    const auto rows = [&](const Edge& e, i32& first, i32& last) noexcept {
        const float low = e.y0 < e.y1 ? e.y0 : e.y1, high = e.y0 < e.y1 ? e.y1 : e.y0;
        first = static_cast<i32>(low / cell);
        last = static_cast<i32>(high / cell);
        if (low < 0)
            first = 0;
        if (first < 0)
            first = 0;
        if (last >= static_cast<i32>(Grid))
            last = Grid - 1;
        return first <= last && high >= 0 && low < extent;
    };
    for (u32 i = 0; i < edges.count; ++i) {
        i32 first, last;
        if (rows(edges.data[i], first, last))
            for (i32 r = first; r <= last; ++r)
                ++start[r + 1];
    }
    for (u32 r = 0; r < Grid; ++r)
        start[r + 1] += start[r];
    u32* list = arena_allocate<u32>(arena, start[Grid] + 1);
    u32* fill = arena_allocate<u32>(arena, Grid);
    if (!list || !fill) {
        arena_rewind(arena, mark);
        return false;
    }
    for (u32 r = 0; r < Grid; ++r)
        fill[r] = start[r];
    for (u32 i = 0; i < edges.count; ++i) {
        i32 first, last;
        if (rows(edges.data[i], first, last))
            for (i32 r = first; r <= last; ++r)
                list[fill[r]++] = i;
    }
    float row[Grid];
    float xs[4096];
    signed char winding[4096];
    for (u32 r = 0; r < Grid; ++r) {
        for (u32 c = 0; c < Grid; ++c)
            row[c] = 0;
        for (u32 s = 0; s < sub_scanlines; ++s) {
            const float y = (static_cast<float>(r) + (static_cast<float>(s) + .5f) / sub_scanlines) * cell;
            u32 n = 0;
            for (u32 k = start[r]; k < start[r + 1] && n < 4096; ++k) {
                const Edge& e = edges.data[list[k]];
                const bool down = e.y1 > e.y0;
                const float low = down ? e.y0 : e.y1, high = down ? e.y1 : e.y0;
                if (!(y >= low && y < high))
                    continue;
                const float x = e.x0 + (e.x1 - e.x0) * ((y - e.y0) / (e.y1 - e.y0));
                u32 at = n++;
                while (at && xs[at - 1] > x) {
                    xs[at] = xs[at - 1];
                    winding[at] = winding[at - 1];
                    --at;
                }
                xs[at] = x;
                winding[at] = down ? 1 : -1;
            }
            i32 w = 0;
            for (u32 k = 0; k + 1 < n; ++k) {
                w += winding[k];
                if (even_odd ? (w & 1) == 0 : w == 0)
                    continue;
                float a = xs[k], b = xs[k + 1];
                if (b <= 0 || a >= extent)
                    continue;
                a = a < 0 ? 0 : a;
                b = b > extent ? extent : b;
                const u32 first = static_cast<u32>(a / cell), last = static_cast<u32>(b / cell);
                for (u32 c = first; c <= last && c < Grid; ++c) {
                    const float lo = a > c * cell ? a : c * cell, hi = b < (c + 1) * cell ? b : (c + 1) * cell;
                    if (hi > lo)
                        row[c] += (hi - lo) / cell;
                }
            }
        }
        for (u32 c = 0; c < Grid; ++c) {
            float v = row[c] / sub_scanlines;
            v = v > 1 ? 1 : v;
            out->total += v;
            out->cells[r * Grid + c] = static_cast<u8>(v * 255.f + .5f);
        }
    }
    arena_rewind(arena, mark);
    return true;
}

} // namespace maicord::lottie_raster
