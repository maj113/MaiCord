#include "vector_internal.hpp"
#include "maicord/core/heap_sort.hpp"

// Boolean combination and area of nonzero-filled outlines.
//
// The outlines are flattened (every cubic into the fewest uniform parameter steps that stay within the tolerance of
// the curve) and snapped to an integer grid fine enough that float inputs keep their identity, so equal points are
// equal and every predicate is exact. All the edges are then split at every point where two of them touch or cross
// (repeated until nothing is left to split, since a crossing rounded onto the grid can meet a third edge), equal
// edges merged, and a sweep over the plane, sheared so that no edge is vertical, gives each edge the winding number of
// both operands on its two sides. The edges between a region the combination fills and one it does not are the result:
// they are chained into rings, and where a run of them follows one original curve, that curve between the run's
// parameters replaces the flattened pieces.
namespace maicord::geometry {
namespace {
using i128 = __int128;
using i8 = signed char;

struct Node {
    i32 x, y;
};
constexpr bool same(Node a, Node b) noexcept {
    return a.x == b.x && a.y == b.y;
}
constexpr bool before(i32 ax, i32 ay, i32 bx, i32 by) noexcept {
    return ax < bx || (ax == bx && ay < by);
}

// Control points of an original curve, transformed: x0, y0, ... x3, y3. A line keeps only its ends in 0..3.
struct Curve {
    double p[8];
    u32 line;
};
// An edge from (x0, y0) to (x1, y1), the lesser point first. wa and wb are the winding each operand adds going from
// below the edge to above it; t0 and t1 the curve parameters at the two ends (t1 may be less than t0).
struct Seg {
    i32 x0, y0, x1, y1;
    i32 wa, wb;
    u32 curve;
    float t0, t1;
};
struct Split {
    u32 seg;
    i32 x, y;
};

constexpr double grid_limit = 268435456.0; // 2^28
constexpr u32 passes = 6;

double to_double(i128 value) noexcept {
    const bool negative = value < 0;
    const unsigned __int128 magnitude = negative ? -static_cast<unsigned __int128>(value) : static_cast<unsigned __int128>(value);
    const double result = static_cast<double>(static_cast<u64>(magnitude >> 64)) * 18446744073709551616.0 +
                          static_cast<double>(static_cast<u64>(magnitude));
    return negative ? -result : result;
}

struct Overlay {
    Scratch s;
    double scale, tolerance;
    Vec<Curve> curves;
    Vec<Seg> segs;
    Vec<Split> splits;
    Vec<u32> active;

    Node snap(double x, double y) const noexcept {
        const double sx = x * scale, sy = y * scale;
        return {static_cast<i32>(sx < 0 ? sx - .5 : sx + .5), static_cast<i32>(sy < 0 ? sy - .5 : sy + .5)};
    }
    bool edge(Node a, Node b, u32 curve, float ta, float tb, u32 source) noexcept {
        if (same(a, b))
            return true;
        Seg* e = vec_push(s, segs);
        if (!e)
            return false;
        const bool forward = before(a.x, a.y, b.x, b.y);
        const i32 w = forward ? 1 : -1;
        *e = forward ? Seg{a.x, a.y, b.x, b.y, 0, 0, curve, ta, tb} : Seg{b.x, b.y, a.x, a.y, 0, 0, curve, tb, ta};
        (source ? e->wb : e->wa) = w;
        return true;
    }
    u32 new_curve(const double (&p)[8], bool line) noexcept {
        Curve* c = vec_push(s, curves);
        if (!c)
            return 0;
        copy_bytes(c->p, p, sizeof(c->p));
        c->line = line;
        return curves.count - 1;
    }
    bool add_figures(const Geometry& g, const Matrix* m, u32 source) noexcept {
        for (u32 f = 0; f < g.figure_count; ++f) {
            const Figure& figure = g.figures[f];
            const Point* p = g.points + figure.point_begin;
            Point start = transform_point(m, *p++), current = start;
            const Node first = snap(start.x, start.y);
            Node previous = first;
            for (u32 k = 0; k < figure.kind_count; ++k) {
                if (g.kinds[figure.kind_begin + k] == kind_line) {
                    const Point q = transform_point(m, *p++);
                    const Node next = snap(q.x, q.y);
                    const double line[8]{current.x, current.y, q.x, q.y, 0, 0, 0, 0};
                    if (!same(previous, next) && !edge(previous, next, new_curve(line, true), 0, 1, source))
                        return false;
                    previous = next;
                    current = q;
                    continue;
                }
                const Point c1 = transform_point(m, p[0]), c2 = transform_point(m, p[1]),
                            c3 = transform_point(m, p[2]);
                p += 3;
                const double q[8]{current.x, current.y, c1.x, c1.y, c2.x, c2.y, c3.x, c3.y};
                const u32 id = new_curve(q, false);
                float t_previous = 0;
                bool added = true;
                flatten_cubic(q, tolerance, [&](float t_next, double x, double y) noexcept {
                    const Node next = snap(x, y);
                    added &= edge(previous, next, id, t_previous, t_next, source);
                    previous = next;
                    t_previous = t_next;
                });
                if (!added)
                    return false;
                current = c3;
            }
            if (!same(previous, first)) {
                const double line[8]{current.x, current.y, start.x, start.y, 0, 0, 0, 0};
                if (!edge(previous, first, new_curve(line, true), 0, 1, source))
                    return false;
            }
        }
        return !s.failed;
    }

    // Sorted by endpoints, equal edges merged, edges that cancel dropped.
    void normalize() noexcept {
        heap_sort(segs.data, segs.count, [](const Seg& a, const Seg& b) noexcept {
            if (a.x0 != b.x0)
                return a.x0 < b.x0;
            if (a.y0 != b.y0)
                return a.y0 < b.y0;
            if (a.x1 != b.x1)
                return a.x1 < b.x1;
            if (a.y1 != b.y1)
                return a.y1 < b.y1;
            return a.curve < b.curve;
        });
        u32 out = 0;
        for (u32 i = 0; i < segs.count; ++i) {
            Seg& e = segs.data[i];
            if (out && segs.data[out - 1].x0 == e.x0 && segs.data[out - 1].y0 == e.y0 &&
                segs.data[out - 1].x1 == e.x1 && segs.data[out - 1].y1 == e.y1) {
                segs.data[out - 1].wa += e.wa;
                segs.data[out - 1].wb += e.wb;
                continue;
            }
            segs.data[out++] = e;
        }
        u32 keep = 0;
        for (u32 i = 0; i < out; ++i)
            if (segs.data[i].wa || segs.data[i].wb)
                segs.data[keep++] = segs.data[i];
        segs.count = keep;
    }

    // A point on an edge becomes a split of it only strictly between its ends, in their order.
    bool add_split(u32 seg, i32 x, i32 y) noexcept {
        const Seg& e = segs.data[seg];
        if (!before(e.x0, e.y0, x, y) || !before(x, y, e.x1, e.y1))
            return true;
        Split* split = vec_push(s, splits);
        if (!split)
            return false;
        *split = {seg, x, y};
        return true;
    }
    // Whether (x, y), on the line of segment e, lies strictly between its ends.
    static bool inside(const Seg& e, i32 x, i32 y) noexcept {
        return before(e.x0, e.y0, x, y) && before(x, y, e.x1, e.y1);
    }
    static i64 cross(i32 ax, i32 ay, i32 bx, i32 by, i32 cx, i32 cy) noexcept {
        return static_cast<i64>(bx - ax) * (cy - ay) - static_cast<i64>(by - ay) * (cx - ax);
    }
    bool meet(u32 i, u32 j) noexcept {
        const Seg a = segs.data[i], b = segs.data[j];
        if (maximum(a.y0, a.y1) < minimum(b.y0, b.y1) || maximum(b.y0, b.y1) < minimum(a.y0, a.y1))
            return true;
        const i64 d1 = cross(a.x0, a.y0, a.x1, a.y1, b.x0, b.y0), d2 = cross(a.x0, a.y0, a.x1, a.y1, b.x1, b.y1),
                  d3 = cross(b.x0, b.y0, b.x1, b.y1, a.x0, a.y0), d4 = cross(b.x0, b.y0, b.x1, b.y1, a.x1, a.y1);
        bool ok = true;
        if (d1 == 0 && inside(a, b.x0, b.y0))
            ok &= add_split(i, b.x0, b.y0);
        if (d2 == 0 && inside(a, b.x1, b.y1))
            ok &= add_split(i, b.x1, b.y1);
        if (d3 == 0 && inside(b, a.x0, a.y0))
            ok &= add_split(j, a.x0, a.y0);
        if (d4 == 0 && inside(b, a.x1, a.y1))
            ok &= add_split(j, a.x1, a.y1);
        if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
            const double rx = a.x1 - a.x0, ry = a.y1 - a.y0, sx = b.x1 - b.x0, sy = b.y1 - b.y0;
            const double denominator = rx * sy - ry * sx,
                         t = (static_cast<double>(b.x0 - a.x0) * sy - static_cast<double>(b.y0 - a.y0) * sx) /
                             denominator;
            double px = a.x0 + rx * t, py = a.y0 + ry * t;
            const i32 low_x = maximum(minimum(a.x0, a.x1), minimum(b.x0, b.x1)),
                      high_x = minimum(maximum(a.x0, a.x1), maximum(b.x0, b.x1)),
                      low_y = maximum(minimum(a.y0, a.y1), minimum(b.y0, b.y1)),
                      high_y = minimum(maximum(a.y0, a.y1), maximum(b.y0, b.y1));
            i32 x = static_cast<i32>(px < 0 ? px - .5 : px + .5), y = static_cast<i32>(py < 0 ? py - .5 : py + .5);
            x = clamp_value(x, low_x, high_x);
            y = clamp_value(y, low_y, high_y);
            // Rounding can leave the point outside an edge's ends in their order; then it becomes that end.
            const Seg* const order[]{&a, &b, &a};
            for (const Seg* edge : order) {
                if (before(x, y, edge->x0, edge->y0)) {
                    x = edge->x0;
                    y = edge->y0;
                } else if (before(edge->x1, edge->y1, x, y)) {
                    x = edge->x1;
                    y = edge->y1;
                }
            }
            ok &= add_split(i, x, y);
            ok &= add_split(j, x, y);
        }
        return ok;
    }
    // Finds every touching or crossing of two edges (the edges are sorted by their first x); true when there were some.
    bool find_splits(bool& found) noexcept {
        splits.count = 0;
        active.count = 0;
        for (u32 i = 0; i < segs.count; ++i) {
            const i32 x = segs.data[i].x0;
            u32 kept = 0;
            for (u32 k = 0; k < active.count; ++k)
                if (segs.data[active.data[k]].x1 >= x)
                    active.data[kept++] = active.data[k];
            active.count = kept;
            for (u32 k = 0; k < active.count; ++k)
                if (!meet(i, active.data[k]))
                    return false;
            u32* slot = vec_push(s, active);
            if (!slot)
                return false;
            *slot = i;
        }
        found = splits.count != 0;
        return true;
    }
    bool apply_splits() noexcept {
        heap_sort(splits.data, splits.count, [](const Split& a, const Split& b) noexcept {
            if (a.seg != b.seg)
                return a.seg < b.seg;
            return before(a.x, a.y, b.x, b.y);
        });
        Vec<Seg> out{};
        if (!vec_reserve(s, out, segs.count + splits.count))
            return false;
        u32 at = 0;
        for (u32 i = 0; i < segs.count; ++i) {
            const Seg e = segs.data[i];
            const u32 first = at;
            while (at < splits.count && splits.data[at].seg == i)
                ++at;
            if (first == at) {
                out.data[out.count++] = e;
                continue;
            }
            const double dx = static_cast<double>(e.x1) - e.x0, dy = static_cast<double>(e.y1) - e.y0;
            Node previous{e.x0, e.y0};
            float t_previous = e.t0;
            for (u32 k = first; k <= at; ++k) {
                Node next;
                float t_next;
                if (k == at) {
                    next = {e.x1, e.y1};
                    t_next = e.t1;
                } else {
                    next = {splits.data[k].x, splits.data[k].y};
                    const double fraction = absolute(dx) >= absolute(dy) ? (static_cast<double>(next.x) - e.x0) / dx
                                                                         : (static_cast<double>(next.y) - e.y0) / dy;
                    t_next = static_cast<float>(static_cast<double>(e.t0) +
                                                (static_cast<double>(e.t1) - e.t0) * clamp_value(fraction, 0., 1.));
                }
                if (!same(previous, next))
                    out.data[out.count++] = {previous.x, previous.y, next.x, next.y, e.wa, e.wb, e.curve, t_previous, t_next};
                previous = next;
                t_previous = t_next;
            }
        }
        segs = out;
        return true;
    }
    bool node_segments() noexcept {
        for (u32 pass = 0;; ++pass) {
            normalize();
            if (pass == passes)
                return true;
            bool found = false;
            if (!find_splits(found))
                return false;
            if (!found)
                return true;
            if (!apply_splits())
                return false;
        }
    }
};

constexpr bool filled(CombineMode mode, bool a, bool b) noexcept {
    return mode == combine_union ? (a || b) : mode == combine_intersect ? (a && b) : mode == combine_xor ? a != b : (a && !b);
}

// The sheared plane's coordinate along its horizontal axis: lexicographic order of points is numeric order of X.
inline i64 shear(i32 x, i32 y) noexcept {
    return static_cast<i64>(x) * 4294967296ll + y;
}
// Positive when (x, y) lies above the line of the edge in the sheared plane.
inline i128 above(const Seg& e, i32 x, i32 y) noexcept {
    const i64 ax = shear(e.x0, e.y0), bx = shear(e.x1, e.y1), cx = shear(x, y);
    return static_cast<i128>(bx - ax) * (y - e.y0) - static_cast<i128>(e.y1 - e.y0) * (cx - ax);
}
// Whether edge a leaves its first point below edge b (both start at the same point).
inline bool lower_slope(const Seg& a, const Seg& b) noexcept {
    const i64 ax = shear(a.x1, a.y1) - shear(a.x0, a.y0), bx = shear(b.x1, b.y1) - shear(b.x0, b.y0);
    return static_cast<i128>(a.y1 - a.y0) * bx < static_cast<i128>(b.y1 - b.y0) * ax;
}

// +1: the edge is a boundary with the filled region above it (traversed first to second point keeps the region on the
// left); -1: filled below (traversed second to first); 0: not a boundary.
bool classify(Overlay& o, CombineMode mode, Vec<i8>& direction) noexcept {
    const u32 n = o.segs.count;
    Scratch& s = o.s;
    Vec<i32> above_a{}, above_b{};
    Vec<u32> by_end{}, status{}, group{};
    if (!vec_reserve(s, direction, n) || !vec_reserve(s, above_a, n) || !vec_reserve(s, above_b, n) ||
        !vec_reserve(s, by_end, n) || !vec_reserve(s, status, n) || !vec_reserve(s, group, n))
        return false;
    direction.count = n;
    for (u32 i = 0; i < n; ++i) {
        by_end.data[i] = i;
        direction.data[i] = 0;
    }
    by_end.count = n;
    const Seg* e = o.segs.data;
    heap_sort(by_end.data, n, [e](u32 a, u32 b) noexcept {
        return before(e[a].x1, e[a].y1, e[b].x1, e[b].y1);
    });
    u32 next_start = 0, next_end = 0;
    while (next_start < n || next_end < n) {
        Node node;
        if (next_end == n || (next_start < n && !before(e[by_end.data[next_end]].x1, e[by_end.data[next_end]].y1,
                                                        e[next_start].x0, e[next_start].y0))) {
            node = {e[next_start].x0, e[next_start].y0};
        } else {
            node = {e[by_end.data[next_end]].x1, e[by_end.data[next_end]].y1};
        }
        while (next_end < n && e[by_end.data[next_end]].x1 == node.x && e[by_end.data[next_end]].y1 == node.y) {
            const u32 gone = by_end.data[next_end++];
            for (u32 k = 0; k < status.count; ++k)
                if (status.data[k] == gone) {
                    for (u32 m = k + 1; m < status.count; ++m)
                        status.data[m - 1] = status.data[m];
                    --status.count;
                    break;
                }
        }
        const u32 first = next_start;
        while (next_start < n && e[next_start].x0 == node.x && e[next_start].y0 == node.y)
            ++next_start;
        const u32 added = next_start - first;
        if (!added)
            continue;
        // The edges leaving the node, lowest first.
        group.count = added;
        for (u32 k = 0; k < added; ++k) {
            u32 at = first + k, position = k;
            while (position && lower_slope(e[at], e[group.data[position - 1]])) {
                group.data[position] = group.data[position - 1];
                --position;
            }
            group.data[position] = at;
        }
        u32 low = 0, high = status.count;
        while (low < high) {
            const u32 mid = low + (high - low) / 2;
            if (above(e[status.data[mid]], node.x, node.y) > 0)
                low = mid + 1;
            else
                high = mid;
        }
        for (u32 k = status.count; k > low; --k)
            status.data[k - 1 + added] = status.data[k - 1];
        status.count += added;
        i32 below_a = low ? above_a.data[status.data[low - 1]] : 0, below_b = low ? above_b.data[status.data[low - 1]] : 0;
        for (u32 k = 0; k < added; ++k) {
            const u32 id = group.data[k];
            status.data[low + k] = id;
            above_a.data[id] = below_a + e[id].wa;
            above_b.data[id] = below_b + e[id].wb;
            const bool below_filled = filled(mode, below_a != 0, below_b != 0),
                       above_filled = filled(mode, above_a.data[id] != 0, above_b.data[id] != 0);
            direction.data[id] = below_filled == above_filled ? i8{0} : above_filled ? i8{1} : i8{-1};
            below_a = above_a.data[id];
            below_b = above_b.data[id];
        }
    }
    return true;
}

// The region to classify: both operands snapped to one grid, edged, split and merged.
bool prepare(Overlay& o, const Geometry* a, const Matrix* ma, const Geometry* b, const Matrix* mb,
             float tolerance) noexcept {
    double largest = 0;
    const Geometry* operands[]{a, b};
    const Matrix* transforms[]{ma, mb};
    for (u32 k = 0; k < 2; ++k) {
        if (!operands[k])
            continue;
        for (u32 i = 0; i < operands[k]->point_count; ++i) {
            const Point p = transform_point(transforms[k], operands[k]->points[i]);
            if (!is_finite(p.x) || !is_finite(p.y))
                return false;
            largest = maximum(largest, maximum(absolute(p.x), absolute(p.y)));
        }
    }
    double scale = grid_limit;
    if (largest > 1e-30) {
        scale = 1;
        while (largest * scale * 2 <= grid_limit && scale < 1e30)
            scale *= 2;
        while (largest * scale > grid_limit && scale > 1e-30)
            scale *= .5;
    }
    o.scale = scale;
    o.tolerance = tolerance > 1e-9f ? tolerance : 1e-9;
    for (u32 k = 0; k < 2; ++k)
        if (operands[k] && !o.add_figures(*operands[k], transforms[k], k))
            return false;
    return o.node_segments();
}

struct Ends {
    Node start, finish;
};
inline Ends ends_of(const Seg& e, i8 direction) noexcept {
    return direction > 0 ? Ends{{e.x0, e.y0}, {e.x1, e.y1}} : Ends{{e.x1, e.y1}, {e.x0, e.y0}};
}

// The curve of `curve` between parameters low < high, as a cubic.
void sub_cubic(const double (&p)[8], double low, double high, double (&out)[8]) noexcept {
    double q[8];
    copy_bytes(q, p, sizeof(q));
    double head[8], rest[8];
    if (high < 1) {
        split_cubic(q, high, head, rest);
        copy_bytes(q, head, sizeof(q));
    }
    if (low > 0) {
        split_cubic(q, low / high, head, rest);
        copy_bytes(q, rest, sizeof(q));
    }
    copy_bytes(out, q, sizeof(q));
}

struct Emitter {
    Overlay& o;
    SimplifiedGeometrySink* sink;
    Point lines[32];
    BezierSegment cubics[16];
    u32 line_count, cubic_count;
    Point at(Node n) const noexcept {
        const double inverse = 1 / o.scale;
        return {static_cast<float>(n.x * inverse), static_cast<float>(n.y * inverse)};
    }
    void flush() noexcept {
        if (line_count)
            sink->AddLines(lines, line_count);
        if (cubic_count)
            sink->AddBeziers(cubics, cubic_count);
        line_count = cubic_count = 0;
    }
    void line(Node to) noexcept {
        if (cubic_count)
            flush();
        lines[line_count++] = at(to);
        if (line_count == 32)
            flush();
    }
    void cubic(Point a, Point b, Point c) noexcept {
        if (line_count)
            flush();
        cubics[cubic_count++] = {a, b, c};
        if (cubic_count == 16)
            flush();
    }
};

// One ring of boundary edges, in traversal order, as one figure.
void emit_ring(Emitter& em, const Vec<i8>& direction, const u32* ring, u32 count) noexcept {
    Overlay& o = em.o;
    const auto piece = [&](u32 k, float& ts, float& te) noexcept {
        const Seg& e = o.segs.data[ring[k]];
        const bool forward = direction.data[ring[k]] > 0;
        ts = forward ? e.t0 : e.t1;
        te = forward ? e.t1 : e.t0;
        return e.curve;
    };
    em.sink->BeginFigure(em.at(ends_of(o.segs.data[ring[0]], direction.data[ring[0]]).start), figure_filled);
    em.line_count = em.cubic_count = 0;
    for (u32 i = 0; i < count;) {
        float first_t, last_t;
        const u32 curve = piece(i, first_t, last_t);
        u32 j = i;
        for (; j + 1 < count; ++j) {
            float next_start, next_end;
            if (piece(j + 1, next_start, next_end) != curve || next_start != last_t)
                break;
            last_t = next_end;
        }
        const Node from = ends_of(o.segs.data[ring[i]], direction.data[ring[i]]).start,
                   to = ends_of(o.segs.data[ring[j]], direction.data[ring[j]]).finish;
        const Curve& c = o.curves.data[curve];
        i = j + 1;
        if (c.line) {
            if (!same(from, to))
                em.line(to);
            continue;
        }
        const double low = minimum<double>(first_t, last_t), high = maximum<double>(first_t, last_t);
        double part[8];
        sub_cubic(c.p, low, high, part);
        if (first_t > last_t) {
            for (u32 k = 0; k < 2; ++k) {
                const double a = part[k], b = part[2 + k];
                part[k] = part[6 + k];
                part[2 + k] = part[4 + k];
                part[4 + k] = b;
                part[6 + k] = a;
            }
        }
        const Point a = em.at(from), d = em.at(to);
        const Point b{static_cast<float>(part[2]), static_cast<float>(part[3])},
            e{static_cast<float>(part[4]), static_cast<float>(part[5])};
        // A run that stays on its chord is a line.
        const double cx = static_cast<double>(d.x) - a.x, cy = static_cast<double>(d.y) - a.y,
                     length = root(cx * cx + cy * cy), limit = o.tolerance * .01;
        bool straight = length > 0;
        if (straight) {
            const Point controls[]{b, e};
            for (const Point control : controls) {
                const double px = static_cast<double>(control.x) - a.x, py = static_cast<double>(control.y) - a.y,
                             along = (px * cx + py * cy) / length, across = (px * cy - py * cx) / length;
                straight &= along >= 0 && along <= length && absolute(across) <= limit;
            }
        }
        if (straight)
            em.line(to);
        else
            em.cubic(b, e, d);
    }
    em.flush();
    em.sink->EndFigure(figure_closed);
}

Result emit_rings(Overlay& o, const Vec<i8>& direction, SimplifiedGeometrySink* sink) noexcept {
    Scratch& s = o.s;
    const u32 n = o.segs.count;
    Vec<u32> order{}, ring{};
    Vec<u8> used{};
    if (!vec_reserve(s, order, n) || !vec_reserve(s, ring, n))
        return result_out_of_memory;
    for (u32 i = 0; i < n; ++i)
        if (direction.data[i])
            order.data[order.count++] = i;
    const Seg* e = o.segs.data;
    const i8* d = direction.data;
    const auto start = [e, d](u32 i) noexcept { return ends_of(e[i], d[i]).start; };
    heap_sort(order.data, order.count, [&start](u32 a, u32 b) noexcept {
        const Node x = start(a), y = start(b);
        return before(x.x, x.y, y.x, y.y);
    });
    used.data = arena_allocate_zero<u8>(s.arena, order.count ? order.count : 1);
    if (!used.data)
        return result_out_of_memory;
    // `used` is indexed by position in `order`.
    Emitter em{o, sink, {}, {}, 0, 0};
    for (u32 first = 0; first < order.count; ++first) {
        if (used.data[first])
            continue;
        ring.count = 0;
        u32 at = first;
        const Node origin = start(order.data[first]);
        for (;;) {
            used.data[at] = 1;
            ring.data[ring.count++] = order.data[at];
            const Node finish = ends_of(e[order.data[at]], d[order.data[at]]).finish;
            if (same(finish, origin))
                break;
            // The first unused edge leaving `finish`.
            u32 low = 0, high = order.count;
            while (low < high) {
                const u32 mid = low + (high - low) / 2;
                const Node m = start(order.data[mid]);
                if (before(m.x, m.y, finish.x, finish.y))
                    low = mid + 1;
                else
                    high = mid;
            }
            bool found = false;
            for (u32 k = low; k < order.count && same(start(order.data[k]), finish); ++k)
                if (!used.data[k]) {
                    at = k;
                    found = true;
                    break;
                }
            if (!found)
                break;
        }
        if (ring.count >= 3)
            emit_ring(em, direction, ring.data, ring.count);
    }
    return s.failed ? result_out_of_memory : result_ok;
}
} // namespace

Result overlay_combine(Factory* factory, const Geometry* a, const Matrix* a_transform, const Geometry* b,
                       const Matrix* b_transform, CombineMode mode, float tolerance,
                       SimplifiedGeometrySink* sink) noexcept {
    Overlay o{};
    o.s = {&factory->scratch, false};
    const ArenaMark mark = arena_mark(&factory->scratch);
    Result result = result_invalid_argument;
    Vec<i8> direction{};
    if (prepare(o, a, a_transform, b, b_transform, tolerance)) {
        sink->SetFillMode(fill_winding);
        result = classify(o, mode, direction) ? emit_rings(o, direction, sink) : result_out_of_memory;
    } else if (o.s.failed) {
        result = result_out_of_memory;
    }
    arena_rewind(&factory->scratch, mark);
    return result;
}

Result overlay_area(Factory* factory, const Geometry* geometry, const Matrix* transform, float tolerance,
                    double* area) noexcept {
    Overlay o{};
    o.s = {&factory->scratch, false};
    const ArenaMark mark = arena_mark(&factory->scratch);
    *area = 0;
    Result result = result_invalid_argument;
    Vec<i8> direction{};
    if (prepare(o, geometry, transform, nullptr, nullptr, tolerance)) {
        if (classify(o, combine_union, direction)) {
            i128 twice = 0;
            for (u32 i = 0; i < o.segs.count; ++i) {
                if (!direction.data[i])
                    continue;
                const Ends ends = ends_of(o.segs.data[i], direction.data[i]);
                twice += static_cast<i128>(ends.start.x) * ends.finish.y - static_cast<i128>(ends.finish.x) * ends.start.y;
            }
            const double value = to_double(twice) * .5 / (o.scale * o.scale);
            *area = value < 0 ? -value : value;
            result = result_ok;
        } else {
            result = result_out_of_memory;
        }
    } else if (o.s.failed) {
        result = result_out_of_memory;
    }
    arena_rewind(&factory->scratch, mark);
    return result;
}
} // namespace maicord::geometry
