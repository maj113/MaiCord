#include "vector_internal.hpp"

// Boolean combination and area of nonzero-filled outlines.
//
// The outlines are flattened (every cubic is cut where it would leave the tolerance of a chord, see flatten_cubic) and
// snapped to an integer grid fine enough that float inputs keep their identity, so equal points are equal and every
// predicate is exact. All the edges are then split at every point where two of them touch or cross: a sweep in x over
// the edges sorted by their first point tests each against the edges it overlaps in x and y, and repeats for the pieces
// the splits made (a crossing rounded onto the grid can meet a third edge) until nothing is left to split. Equal edges
// are merged, and a sweep over the plane, sheared so that no edge is vertical, gives each edge the winding number of
// both operands on its two sides. The edges between a region the combination fills and one it does not are the result:
// they are chained into rings, and where a run of them follows one original curve, that curve between the run's
// parameters replaces the flattened pieces. Sorting is by 64-bit point keys, a byte at a time.
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
// A point as a key whose unsigned order is the order of points (x, then y).
constexpr u64 key_of(i32 x, i32 y) noexcept {
    return static_cast<u64>(static_cast<u32>(x) ^ 0x80000000u) << 32 | (static_cast<u32>(y) ^ 0x80000000u);
}

// Sorts `count` keys ascending and carries `values` along, stably: the runs already in order (or strictly against it,
// reversed first) are merged pairwise until one is left, so the long monotone runs of flattened curves cost little.
// The spare arrays have room for `count` each, `runs` for count + 1.
void sort_keys(u64* keys, u32* values, u64* spare_keys, u32* spare_values, u32* runs, u32 count) noexcept {
    u32 run_count = 0;
    for (u32 i = 0, j; i < count; i = j) {
        runs[run_count++] = i;
        j = i + 1;
        if (j < count && keys[j] < keys[i]) {
            while (j + 1 < count && keys[j + 1] < keys[j])
                ++j;
            for (u32 a = i, b = j++; a < b; ++a, --b) {
                const u64 key = keys[a];
                keys[a] = keys[b];
                keys[b] = key;
                const u32 value = values[a];
                values[a] = values[b];
                values[b] = value;
            }
        } else {
            while (j < count && keys[j] >= keys[j - 1])
                ++j;
        }
    }
    runs[run_count] = count;
    u64 *from_keys = keys, *to_keys = spare_keys;
    u32 *from_values = values, *to_values = spare_values;
    while (run_count > 1) {
        u32 merged = 0;
        for (u32 r = 0; r < run_count; r += 2) {
            const u32 low = runs[r], middle = runs[r + 1], high = runs[r + 2 <= run_count ? r + 2 : r + 1];
            for (u32 a = low, b = middle, out = low; out < high; ++out) {
                const bool second = b < high && (a == middle || from_keys[b] < from_keys[a]);
                const u32 at = second ? b++ : a++;
                to_keys[out] = from_keys[at];
                to_values[out] = from_values[at];
            }
            runs[merged++] = low;
        }
        runs[merged] = count;
        run_count = merged;
        u64* keys_now = to_keys;
        to_keys = from_keys;
        from_keys = keys_now;
        u32* values_now = to_values;
        to_values = from_values;
        from_values = values_now;
    }
    if (from_keys != keys) {
        copy_bytes(keys, from_keys, count * 8);
        copy_bytes(values, from_values, count * 4);
    }
}

// Control points of an original cubic, transformed: x0, y0, ... x3, y3.
struct Curve {
    double p[8];
};
// The curve ids of straight segments: no Curve, and a count of their own so that the pieces of one line are told from
// those of the next.
constexpr u32 straight = 0x80000000u;
// An edge from (x0, y0) to (x1, y1), the lesser point first. wa and wb are the winding each operand adds going from
// below the edge to above it; t0 and t1 the curve parameters at the two ends (t1 may be less than t0). Flags: `fresh`,
// a piece of the last round of splits; `reused` shifted by the operand, an edge of an operand's reused boundary.
struct Seg {
    i32 x0, y0, x1, y1;
    i32 wa, wb;
    u32 curve;
    float t0, t1;
    u32 flags;
};
constexpr u32 fresh = 1, reused = 2;
// A point an edge is split at, in a list per edge.
struct Split {
    i32 x, y;
    u32 next;
};
// An edge of the x sweep in the list of one band, while it reaches the current x: its last x and its y range.
struct Active {
    i32 x1, low, high;
    u32 id, next;
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
i64 cross(i32 ax, i32 ay, i32 bx, i32 by, i32 cx, i32 cy) noexcept {
    return static_cast<i64>(bx - ax) * (cy - ay) - static_cast<i64>(by - ay) * (cx - ax);
}
i32 nearest(double v) noexcept {
    return static_cast<i32>(v < 0 ? v - .5 : v + .5);
}

struct Overlay {
    Scratch s;
    double scale, tolerance;
    u32 lines;
    Vec<Curve> curves;
    Vec<Seg> segs, other;
    Vec<Split> splits;
    Vec<u32> heads, order, spare_order, runs;
    Vec<u64> keys, spare_keys;
    Vec<Active> active;
    Vec<Node> points;
    Vec<Point> output;
    Vec<Flat> flat;
    // A reused boundary's edges outside the other operand's span, and where that span begins (see load and classify).
    Vec<Seg> outside;
    i32 left;

    Node snap(double x, double y) const noexcept {
        return {nearest(x * scale), nearest(y * scale)};
    }
    void edge(Node a, Node b, u32 curve, float ta, float tb, u32 source) noexcept {
        if (same(a, b))
            return;
        Seg* e = vec_push(s, segs);
        if (!e)
            return;
        const i32 w = before(a.x, a.y, b.x, b.y) ? 1 : -1;
        *e = w > 0 ? Seg{a.x, a.y, b.x, b.y, 0, 0, curve, ta, tb, 0} : Seg{b.x, b.y, a.x, a.y, 0, 0, curve, tb, ta, 0};
        (source ? e->wb : e->wa) = w;
    }
    void line(Node a, Node b, u32 source) noexcept {
        if (!same(a, b))
            edge(a, b, straight | lines++, 0, 1, source);
    }
    bool add_figures(const Geometry& g, const Matrix* m, u32 source) noexcept {
        for (u32 f = 0; f < g.figure_count; ++f) {
            const Figure& figure = g.figures[f];
            Point current = transform_point(m, g.points[figure.first]);
            const Node first = snap(current.x, current.y);
            Node previous = first;
            const u32 step = figure.polyline ? 1 : 3;
            for (u32 at = figure.first + step; at < figure.end; at += step) {
                const Point* q = g.points + at;
                // A cubic with its handles on its ends is straight.
                if (step == 1 || (q[-2].x == q[-3].x && q[-2].y == q[-3].y && q[-1].x == q[0].x && q[-1].y == q[0].y)) {
                    current = transform_point(m, q[0]);
                    const Node next = snap(current.x, current.y);
                    line(previous, next, source);
                    previous = next;
                    continue;
                }
                const Point c1 = transform_point(m, q[-2]), c2 = transform_point(m, q[-1]), c3 = transform_point(m, q[0]);
                Curve* curve = vec_push(s, curves);
                if (!curve)
                    return false;
                const double c[8]{current.x, current.y, c1.x, c1.y, c2.x, c2.y, c3.x, c3.y};
                copy_bytes(curve->p, c, sizeof(c));
                flat.count = 0;
                if (!flatten_cubic(s, c, tolerance, -2, flat))
                    return false;
                float t_previous = 0;
                for (u32 k = 0; k < flat.count; ++k) {
                    const Node next = snap(flat.data[k].x, flat.data[k].y);
                    edge(previous, next, curves.count - 1, t_previous, flat.data[k].t, source);
                    previous = next;
                    t_previous = flat.data[k].t;
                }
                current = c3;
            }
            line(previous, first, source);
        }
        return !s.failed;
    }
    // The boundary a combination left (see remember) as the edges of operand `source`, its curves after this one's.
    // Its edges wholly outside [left, right], the other operand's span of x, meet nothing of that operand: they go to
    // `outside` when `keep` (the combination keeps the operand where the other is not) and are dropped otherwise.
    bool load(const Reuse& r, u32 source, i32 left, i32 right, bool keep) noexcept {
        const u32 curve_base = curves.count;
        if (!vec_reserve(s, curves, curve_base + r.curve_count) || !vec_reserve(s, segs, segs.count + r.edge_count) ||
            !vec_reserve(s, outside, outside.count + r.edge_count))
            return false;
        copy_bytes(curves.data + curve_base, r.curves, r.curve_count * static_cast<u32>(sizeof(Curve)));
        curves.count += r.curve_count;
        const Seg* from = static_cast<const Seg*>(r.edges);
        for (u32 i = 0; i < r.edge_count; ++i) {
            Seg e = from[i];
            e.curve = e.curve & straight ? straight | ((e.curve & ~straight) + lines) : e.curve + curve_base;
            if (source) {
                e.wb = e.wa;
                e.wa = 0;
            }
            e.flags = reused << source;
            if (e.x1 >= left && e.x0 <= right)
                segs.data[segs.count++] = e;
            else if (keep)
                outside.data[outside.count++] = e;
        }
        lines += r.lines;
        return true;
    }

    bool reserve(u32 n) noexcept {
        return vec_reserve(s, keys, n) && vec_reserve(s, spare_keys, n) && vec_reserve(s, order, n) &&
               vec_reserve(s, spare_order, n) && vec_reserve(s, runs, n + 1) && vec_reserve(s, other, n);
    }
    // Sorts `order` (0..n) by `keys`.
    void sort(u32 n) noexcept {
        for (u32 i = 0; i < n; ++i)
            order.data[i] = i;
        sort_keys(keys.data, order.data, spare_keys.data, spare_order.data, runs.data, n);
    }
    // Sorted by first point, then last; equal edges merged, edges that cancel dropped.
    bool normalize() noexcept {
        const u32 n = segs.count;
        if (!reserve(n))
            return false;
        for (u32 i = 0; i < n; ++i)
            keys.data[i] = key_of(segs.data[i].x0, segs.data[i].y0);
        sort(n);
        Seg* out = other.data;
        u32 count = 0;
        for (u32 i = 0, first = 0; i < n; ++i) {
            if (!i || keys.data[i] != keys.data[i - 1])
                first = count;
            const Seg& e = segs.data[order.data[i]];
            u32 at = count;
            while (at > first && before(e.x1, e.y1, out[at - 1].x1, out[at - 1].y1))
                --at;
            if (at > first && out[at - 1].x1 == e.x1 && out[at - 1].y1 == e.y1) {
                out[at - 1].wa += e.wa;
                out[at - 1].wb += e.wb;
                out[at - 1].flags = ((out[at - 1].flags | e.flags) & fresh) | (out[at - 1].flags & e.flags & ~fresh);
                continue;
            }
            for (u32 k = count; k > at; --k)
                out[k] = out[k - 1];
            out[at] = e;
            ++count;
        }
        u32 keep = 0;
        for (u32 i = 0; i < count; ++i)
            if (out[i].wa || out[i].wb)
                out[keep++] = out[i];
        other.count = keep;
        const Vec<Seg> swap = segs;
        segs = other;
        other = swap;
        return true;
    }

    // A point on an edge becomes a split of it only strictly between its ends, in their order.
    void add_split(u32 seg, i32 x, i32 y) noexcept {
        const Seg& e = segs.data[seg];
        if (!before(e.x0, e.y0, x, y) || !before(x, y, e.x1, e.y1))
            return;
        if (Split* split = vec_push(s, splits)) {
            *split = {x, y, heads.data[seg]};
            heads.data[seg] = splits.count - 1;
        }
    }
    void meet(u32 i, u32 j) noexcept {
        const Seg a = segs.data[i], b = segs.data[j];
        const i64 d1 = cross(a.x0, a.y0, a.x1, a.y1, b.x0, b.y0), d2 = cross(a.x0, a.y0, a.x1, a.y1, b.x1, b.y1);
        // An edge wholly on one side of the other's line cannot meet it; edges that share an end (the neighbours
        // along an outline) meet elsewhere only when they lie on one line.
        if ((d1 > 0 && d2 > 0) || (d1 < 0 && d2 < 0) ||
            ((d1 || d2) && ((a.x0 == b.x0 && a.y0 == b.y0) || (a.x0 == b.x1 && a.y0 == b.y1) ||
                            (a.x1 == b.x0 && a.y1 == b.y0) || (a.x1 == b.x1 && a.y1 == b.y1))))
            return;
        const i64 d3 = cross(b.x0, b.y0, b.x1, b.y1, a.x0, a.y0), d4 = cross(b.x0, b.y0, b.x1, b.y1, a.x1, a.y1);
        // An end of one on the other (add_split keeps only the points strictly between the ends).
        if (!d1)
            add_split(i, b.x0, b.y0);
        if (!d2)
            add_split(i, b.x1, b.y1);
        if (!d3)
            add_split(j, a.x0, a.y0);
        if (!d4)
            add_split(j, a.x1, a.y1);
        if (!d1 || !d2 || !d3 || !d4 || (d3 > 0) == (d4 > 0))
            return;
        // The crossing is where a meets b's line (a's ends are d3 and d4 from it), rounded within both y ranges.
        const double t = static_cast<double>(d3) / static_cast<double>(d3 - d4);
        i32 x = nearest(a.x0 + (a.x1 - a.x0) * t), y = nearest(a.y0 + (a.y1 - a.y0) * t);
        y = clamp_value(y, maximum(minimum(a.y0, a.y1), minimum(b.y0, b.y1)),
                        minimum(maximum(a.y0, a.y1), maximum(b.y0, b.y1)));
        // Rounding can leave the point outside an edge's ends in their order; then it becomes that end.
        const Seg* const ends[]{&a, &b, &a};
        for (const Seg* edge : ends) {
            if (before(x, y, edge->x0, edge->y0)) {
                x = edge->x0;
                y = edge->y0;
            } else if (before(edge->x1, edge->y1, x, y)) {
                x = edge->x1;
                y = edge->y1;
            }
        }
        add_split(i, x, y);
        add_split(j, x, y);
    }
    // Finds where edges touch or cross (the edges are sorted by their first x). Edges of one reused boundary are not
    // tested against each other, and after the first round only pairs with a fresh piece are: every other pair was.
    bool find_splits(bool all) noexcept {
        const u32 n = segs.count;
        splits.count = 0;
        active.count = 0;
        if (!vec_reserve(s, heads, n))
            return false;
        memset(heads.data, 0xff, n * sizeof(u32));
        // The sweep's edges are kept in lists per band of y, an edge in each band its y range meets: an edge is tested
        // against those of its bands, a pair in the band where the overlap of their y ranges begins.
        i32 top = 0x7fffffff, bottom = -0x7fffffff - 1;
        for (u32 i = 0; i < n; ++i) {
            top = minimum(top, minimum(segs.data[i].y0, segs.data[i].y1));
            bottom = maximum(bottom, maximum(segs.data[i].y0, segs.data[i].y1));
        }
        u32 bands = 1, shift = 0;
        while (bands < 64 && bands * bands * 4 < n)
            bands *= 2;
        while ((static_cast<u32>(bottom) - static_cast<u32>(top)) >> shift >= bands)
            ++shift;
        const auto band = [shift, top](i32 y) noexcept { return static_cast<u32>(y - top) >> shift; };
        // After the first round an edge that is not fresh needs only the fresh ones: they have lists of their own.
        u32 lists[2][64];
        memset(lists, 0xff, sizeof(lists));
        for (u32 i = 0; i < n; ++i) {
            const Seg& e = segs.data[i];
            const i32 low = minimum(e.y0, e.y1), high = maximum(e.y0, e.y1);
            const u32 first = band(low), last = band(high), set = !all && !(e.flags & fresh);
            for (u32 b = first; b <= last; ++b) {
                for (u32* link = &lists[set][b]; *link != ~0u;) {
                    const Active& a = active.data[*link];
                    if (a.x1 < e.x0) {
                        *link = a.next;
                        continue;
                    }
                    link = &active.data[*link].next;
                    if (a.low <= high && low <= a.high && band(maximum(low, a.low)) == b &&
                        (all ? !(e.flags & segs.data[a.id].flags & ~fresh)
                             : ((e.flags | segs.data[a.id].flags) & fresh) != 0))
                        meet(i, a.id);
                }
            }
            for (u32 into = 0; into < (!all && (e.flags & fresh) ? 2u : 1u); ++into)
                for (u32 b = first; b <= last; ++b)
                    if (Active* slot = vec_push(s, active)) {
                        *slot = {e.x1, low, high, i, lists[into][b]};
                        lists[into][b] = active.count - 1;
                    }
        }
        return !s.failed;
    }
    bool apply_splits() noexcept {
        if (!vec_reserve(s, other, segs.count + splits.count))
            return false;
        Seg* out = other.data;
        u32 count = 0;
        for (u32 i = 0; i < segs.count; ++i) {
            Seg e = segs.data[i];
            e.flags &= ~fresh;
            if (heads.data[i] == ~0u) {
                out[count++] = e;
                continue;
            }
            points.count = 0;
            for (u32 at = heads.data[i]; at != ~0u; at = splits.data[at].next) {
                Node* slot = vec_push(s, points);
                if (!slot)
                    return false;
                const Node p{splits.data[at].x, splits.data[at].y};
                u32 k = points.count - 1;
                for (; k && before(p.x, p.y, points.data[k - 1].x, points.data[k - 1].y); --k)
                    points.data[k] = points.data[k - 1];
                points.data[k] = p;
            }
            const double dx = static_cast<double>(e.x1) - e.x0, dy = static_cast<double>(e.y1) - e.y0;
            Node previous{e.x0, e.y0};
            float t_previous = e.t0;
            for (u32 k = 0; k <= points.count; ++k) {
                Node next{e.x1, e.y1};
                float t_next = e.t1;
                if (k < points.count) {
                    next = points.data[k];
                    const double fraction = absolute(dx) >= absolute(dy) ? (static_cast<double>(next.x) - e.x0) / dx
                                                                         : (static_cast<double>(next.y) - e.y0) / dy;
                    t_next = static_cast<float>(static_cast<double>(e.t0) +
                                                (static_cast<double>(e.t1) - e.t0) * clamp_value(fraction, 0., 1.));
                }
                if (!same(previous, next))
                    out[count++] = {previous.x, previous.y, next.x, next.y, e.wa, e.wb, e.curve, t_previous, t_next, 1};
                previous = next;
                t_previous = t_next;
            }
        }
        other.count = count;
        const Vec<Seg> swap = segs;
        segs = other;
        other = swap;
        return true;
    }
    bool node_segments() noexcept {
        for (u32 pass = 0;; ++pass) {
            if (!normalize())
                return false;
            if (pass == passes)
                return true;
            if (!find_splits(pass == 0))
                return false;
            if (!splits.count)
                return true;
            if (!apply_splits())
                return false;
        }
    }
};

constexpr bool filled(CombineMode mode, bool a, bool b) noexcept {
    return mode == combine_union ? (a || b) : mode == combine_intersect ? (a && b) : mode == combine_xor ? a != b : (a && !b);
}

// The sweep is over a plane sheared so that lexicographic order of points is the order along its axis (x * 2^32 + y):
// a vertical edge leans forward. The shear has a positive determinant, so the side of a line a point is on there is the
// sign of the plain cross product.
// Positive when (x, y) lies above the line of the edge.
inline i64 above(const Seg& e, i32 x, i32 y) noexcept {
    return cross(e.x0, e.y0, e.x1, e.y1, x, y);
}
// Whether edge a leaves its first point below edge b (both start at the same point).
inline bool lower_slope(const Seg& a, const Seg& b) noexcept {
    return static_cast<i64>(a.y1 - a.y0) * (b.x1 - b.x0) < static_cast<i64>(b.y1 - b.y0) * (a.x1 - a.x0);
}

// +1: the edge is a boundary with the filled region above it (traversed first to second point keeps the region on the
// left); -1: filled below (traversed second to first); 0: not a boundary.
bool classify(Overlay& o, CombineMode mode, Vec<i8>& direction) noexcept {
    const u32 n = o.segs.count;
    Scratch& s = o.s;
    // The sweep's edges from below to above at the current point, with the keys of their last points.
    struct Entry {
        u64 end;
        u32 id;
    };
    Vec<i32> above_a{}, above_b{};
    Vec<Entry> status{};
    Vec<u32> group{};
    if (!vec_reserve(s, direction, n) || !vec_reserve(s, above_a, n) || !vec_reserve(s, above_b, n) ||
        !vec_reserve(s, status, n) || !vec_reserve(s, group, n))
        return false;
    direction.count = n;
    const Seg* e = o.segs.data;
    u32 next_start = 0;
    while (next_start < n) {
        // The next point where edges start; the edges that end before or at it leave the sweep first.
        const Node node{e[next_start].x0, e[next_start].y0};
        const u64 node_key = key_of(node.x, node.y);
        u32 kept = 0;
        for (u32 k = 0; k < status.count; ++k)
            if (status.data[k].end > node_key)
                status.data[kept++] = status.data[k];
        status.count = kept;
        const u32 first = next_start;
        while (next_start < n && e[next_start].x0 == node.x && e[next_start].y0 == node.y)
            ++next_start;
        const u32 added = next_start - first;
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
            if (above(e[status.data[mid].id], node.x, node.y) > 0)
                low = mid + 1;
            else
                high = mid;
        }
        for (u32 k = status.count; k > low; --k)
            status.data[k - 1 + added] = status.data[k - 1];
        status.count += added;
        i32 below_a = low ? above_a.data[status.data[low - 1].id] : 0,
            below_b = low ? above_b.data[status.data[low - 1].id] : 0;
        for (u32 k = 0; k < added; ++k) {
            const u32 id = group.data[k];
            status.data[low + k] = {key_of(e[id].x1, e[id].y1), id};
            above_a.data[id] = below_a + e[id].wa;
            above_b.data[id] = below_b + e[id].wb;
            // An edge that starts before the other operand's span is one of a reused boundary with nothing of the
            // other operand around it (see load): the sweep there lacks the edges left outside, and its windings are
            // its own.
            if (e[id].x0 < o.left) {
                above_a.data[id] = e[id].wa > 0;
                above_b.data[id] = e[id].wb > 0;
                below_a = above_a.data[id] - e[id].wa;
                below_b = above_b.data[id] - e[id].wb;
            }
            const bool below_filled = filled(mode, below_a != 0, below_b != 0),
                       above_filled = filled(mode, above_a.data[id] != 0, above_b.data[id] != 0);
            direction.data[id] = below_filled == above_filled ? i8{0} : above_filled ? i8{1} : i8{-1};
            below_a = above_a.data[id];
            below_b = above_b.data[id];
        }
    }
    return true;
}

// The bounds of the control points of `g` under `m`, which hold its outline; false when a point is not finite.
bool hull(const Geometry* g, const Matrix* m, double (&box)[4]) noexcept {
    box[0] = box[1] = 1e300;
    box[2] = box[3] = -1e300;
    for (u32 i = 0; g && i < g->point_count; ++i) {
        const Point p = transform_point(m, g->points[i]);
        if (!is_finite(p.x) || !is_finite(p.y))
            return false;
        box[0] = minimum<double>(box[0], p.x);
        box[1] = minimum<double>(box[1], p.y);
        box[2] = maximum<double>(box[2], p.x);
        box[3] = maximum<double>(box[3], p.y);
    }
    return true;
}

// The region to classify: the operands (null: none; `m` places the first) snapped to one grid, edged, split and
// merged. An operand that is the last combination's result, on the same grid and tolerance, is its boundary, less
// the edges outside the other operand's span of x (see load).
bool prepare(Overlay& o, const Geometry* a, const Matrix* m, const double (&box_a)[4], const Geometry* b,
             const double (&box_b)[4], float tolerance, CombineMode mode, Factory* factory) noexcept {
    double largest = 0;
    for (const double v : box_a)
        largest = a && a->point_count ? maximum(largest, absolute(v)) : largest;
    for (const double v : box_b)
        largest = b && b->point_count ? maximum(largest, absolute(v)) : largest;
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
    o.left = -0x7fffffff - 1;
    const Geometry* operands[]{a, b};
    const double* boxes[]{box_a, box_b};
    for (u32 k = 0; k < 2; ++k) {
        const Geometry* g = operands[k];
        if (!g)
            continue;
        Reuse* found = nullptr;
        for (u32 slot = 0; factory && slot < 2; ++slot) {
            Reuse& r = factory->reuse[slot];
            if (r.geometry == g && r.scale == scale && r.tolerance == o.tolerance)
                found = &r;
        }
        if (found) {
            found->stamp = ++factory->clock;
            const Geometry* other = operands[1 - k];
            const double* span = boxes[1 - k];
            const bool present = other && other->point_count;
            const i32 left = present ? static_cast<i32>(span[0] * scale) - 2 : 0x7fffffff,
                      right = present ? static_cast<i32>(span[2] * scale) + 2 : -0x7fffffff - 1;
            o.left = maximum(o.left, left);
            if (!o.load(*found, k, left, right, mode != combine_intersect && !(mode == combine_exclude && k)))
                return false;
        } else if (!o.add_figures(*g, k ? nullptr : m, k)) {
            return false;
        }
    }
    return o.node_segments();
}

// The edges of a reused boundary left outside the other operand's span join the boundary as they were.
bool keep_outside(Overlay& o, Vec<i8>& direction) noexcept {
    const u32 n = o.segs.count + o.outside.count;
    if (!vec_reserve(o.s, o.segs, n) || !vec_reserve(o.s, direction, n))
        return false;
    for (u32 i = 0; i < o.outside.count; ++i) {
        const Seg& e = o.outside.data[i];
        o.segs.data[o.segs.count] = e;
        direction.data[o.segs.count++] = e.wa + e.wb > 0 ? i8{1} : i8{-1};
    }
    direction.count = n;
    return true;
}

struct Ends {
    Node start, finish;
};
inline Ends ends_of(const Seg& e, i8 direction) noexcept {
    return direction > 0 ? Ends{{e.x0, e.y0}, {e.x1, e.y1}} : Ends{{e.x1, e.y1}, {e.x0, e.y0}};
}

// The curve of `curve` between parameters low < high, as a cubic.
void sub_cubic(const double (&p)[8], double low, double high, double (&out)[8]) noexcept {
    double head[8], rest[8];
    copy_bytes(out, p, sizeof(out));
    if (high < 1) {
        split_cubic(out, high, head, rest);
        copy_bytes(out, head, sizeof(out));
    }
    if (low > 0) {
        split_cubic(out, low / high, head, rest);
        copy_bytes(out, rest, sizeof(out));
    }
}

// One ring of boundary edges, in traversal order, as one closed figure of `out` (a straight run as a cubic with its
// handles on its ends).
bool emit_ring(Overlay& o, Geometry* out, const i8* direction, const u32* ring, u32 count) noexcept {
    const double inverse = 1 / o.scale;
    const auto at = [inverse](Node n) noexcept {
        return Point{static_cast<float>(n.x * inverse), static_cast<float>(n.y * inverse)};
    };
    const auto piece = [&](u32 k, float& ts, float& te) noexcept {
        const Seg& e = o.segs.data[ring[k]];
        const bool forward = direction[ring[k]] > 0;
        ts = forward ? e.t0 : e.t1;
        te = forward ? e.t1 : e.t0;
        return e.curve;
    };
    Vec<Point>& p = o.output;
    p.count = 0;
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
        const Node from = ends_of(o.segs.data[ring[i]], direction[ring[i]]).start,
                   to = ends_of(o.segs.data[ring[j]], direction[ring[j]]).finish;
        i = j + 1;
        if (((curve & straight) && same(from, to)) || !vec_reserve(o.s, p, p.count + 3))
            continue;
        const Point a = at(from), d = at(to);
        Point b = a, e = d;
        if (!(curve & straight)) {
            const double low = minimum<double>(first_t, last_t), high = maximum<double>(first_t, last_t);
            double part[8];
            sub_cubic(o.curves.data[curve].p, low, high, part);
            const u32 near = first_t > last_t ? 4 : 2, far = 6 - near;
            b = {static_cast<float>(part[near]), static_cast<float>(part[near + 1])};
            e = {static_cast<float>(part[far]), static_cast<float>(part[far + 1])};
            // A run that stays on its chord is a line (in squares, times the chord's squared length).
            const double cx = static_cast<double>(d.x) - a.x, cy = static_cast<double>(d.y) - a.y,
                         square = cx * cx + cy * cy, limit = o.tolerance * .01;
            bool flat = square > 0;
            const Point controls[]{b, e};
            for (const Point control : controls) {
                const double px = static_cast<double>(control.x) - a.x, py = static_cast<double>(control.y) - a.y,
                             along = px * cx + py * cy, across = px * cy - py * cx;
                flat &= along >= 0 && along <= square && across * across <= limit * limit * square;
            }
            if (flat) {
                b = a;
                e = d;
            }
        }
        p.data[p.count++] = b;
        p.data[p.count++] = e;
        p.data[p.count++] = d;
    }
    if (!p.count)
        return !o.s.failed;
    if (!geometry_begin(out, p.data[p.count - 1], false) || !geometry_append(out, p.data, p.count))
        return false;
    geometry_end(out, true);
    return true;
}

bool emit_rings(Overlay& o, const i8* d, Geometry* out) noexcept {
    Scratch& s = o.s;
    const u32 n = o.segs.count;
    const Seg* e = o.segs.data;
    // The boundary edges, in lists by the point they leave from, the lists in an open-addressed table.
    u32 count = 0, bits = 4;
    for (u32 i = 0; i < n; ++i)
        count += d[i] != 0;
    while ((1u << bits) < count * 2)
        ++bits;
    const u32 mask = (1u << bits) - 1;
    Vec<u32> ring{}, table{};
    if (!o.reserve(n) || !vec_reserve(s, ring, n) || !vec_reserve(s, table, mask + 1))
        return false;
    u8* used = arena_allocate_zero<u8>(s.arena, count ? count : 1);
    if (!used)
        return false;
    u64* keys = o.keys.data;
    u32 *edge = o.order.data, *next = o.spare_order.data, *slots = table.data;
    memset(slots, 0xff, (mask + 1) * sizeof(u32));
    const auto slot_of = [&](u64 key) noexcept {
        u32 h = static_cast<u32>(key * 0x9E3779B97F4A7C15ull >> (64 - bits));
        while (slots[h] != ~0u && keys[slots[h]] != key)
            h = (h + 1) & mask;
        return h;
    };
    for (u32 i = 0, at = 0; i < n; ++i)
        if (d[i]) {
            const Node start = ends_of(e[i], d[i]).start;
            keys[at] = key_of(start.x, start.y);
            edge[at] = i;
            const u32 h = slot_of(keys[at]);
            next[at] = slots[h];
            slots[h] = at++;
        }
    for (u32 first = 0; first < count; ++first) {
        if (used[first])
            continue;
        ring.count = 0;
        for (u32 at = first;;) {
            used[at] = 1;
            ring.data[ring.count++] = edge[at];
            const Node finish = ends_of(e[edge[at]], d[edge[at]]).finish;
            const u64 key = key_of(finish.x, finish.y);
            if (key == keys[first])
                break;
            // An unused edge leaving `finish`.
            at = slots[slot_of(key)];
            while (at != ~0u && used[at])
                at = next[at];
            if (at == ~0u)
                break;
        }
        if (ring.count >= 3 && !emit_ring(o, out, d, ring.data, ring.count))
            return false;
    }
    return !s.failed;
}

// Keeps the boundary of `result` (its edges oriented as the winding of one operand) and the curves it follows for the
// next combinations, in place of the boundary used or kept least recently; nothing is kept when the arena is full.
void remember(Factory* factory, const Overlay& o, const i8* direction, const Geometry* result) noexcept {
    const u32 slot = factory->reuse[1].stamp < factory->reuse[0].stamp;
    Reuse& r = factory->reuse[slot];
    Arena* arena = &factory->reused[slot];
    r.geometry = nullptr;
    arena_reset(arena);
    // The curves the boundary follows, numbered anew.
    const u32 n = o.curves.count;
    u32* remap = arena_allocate<u32>(&factory->scratch, n ? n : 1);
    if (!remap)
        return;
    memset(remap, 0xff, n * sizeof(u32));
    u32 count = 0, kept = 0;
    for (u32 i = 0; i < o.segs.count; ++i)
        if (direction[i]) {
            ++count;
            const u32 curve = o.segs.data[i].curve;
            if (!(curve & straight) && remap[curve] == ~0u)
                remap[curve] = kept++;
        }
    Seg* edges = arena_allocate<Seg>(arena, count ? count : 1);
    Curve* curves = arena_allocate<Curve>(arena, kept ? kept : 1);
    if (!edges || !curves)
        return;
    for (u32 i = 0; i < n; ++i)
        if (remap[i] != ~0u)
            curves[remap[i]] = o.curves.data[i];
    for (u32 i = 0, at = 0; i < o.segs.count; ++i)
        if (direction[i]) {
            Seg& e = edges[at++] = o.segs.data[i];
            e.wa = direction[i];
            e.wb = 0;
            e.flags = 0;
            if (!(e.curve & straight))
                e.curve = remap[e.curve];
        }
    r = {result, edges, curves, count, kept, o.lines, ++factory->clock, o.scale, o.tolerance};
}
} // namespace

Geometry* geometry_combine(const Geometry* a, const Geometry* b, CombineMode mode, float tolerance) noexcept {
    Factory* factory = a->factory;
    double box_a[4], box_b[4];
    Geometry* result = geometry_create(factory);
    if (!result || !hull(a, nullptr, box_a) || !hull(b, nullptr, box_b))
        return nullptr;
    // Operands whose outlines lie apart: an intersection is empty, and a difference is the first alone.
    if (box_a[2] < box_b[0] || box_b[2] < box_a[0] || box_a[3] < box_b[1] || box_b[3] < box_a[1]) {
        if (mode == combine_intersect)
            return result;
        if (mode == combine_exclude)
            b = nullptr;
    }
    Overlay o{};
    o.s = {&factory->scratch, false};
    const ArenaMark mark = arena_mark(&factory->scratch);
    Vec<i8> direction{};
    if (prepare(o, a, nullptr, box_a, b, box_b, tolerance, mode, factory) && classify(o, mode, direction) &&
        keep_outside(o, direction) && emit_rings(o, direction.data, result))
        remember(factory, o, direction.data, result);
    else
        result = nullptr;
    arena_rewind(&factory->scratch, mark);
    return result;
}

bool geometry_area(const Geometry* geometry, const Matrix* transform, float tolerance, double* area) noexcept {
    *area = 0;
    double box[4];
    if (!hull(geometry, transform, box))
        return false;
    Factory* factory = geometry->factory;
    Overlay o{};
    o.s = {&factory->scratch, false};
    const ArenaMark mark = arena_mark(&factory->scratch);
    Vec<i8> direction{};
    const bool ok = prepare(o, geometry, transform, box, nullptr, box, tolerance, combine_union, nullptr) &&
                    classify(o, combine_union, direction);
    if (ok) {
        i128 twice = 0;
        for (u32 i = 0; i < o.segs.count; ++i) {
            if (!direction.data[i])
                continue;
            const Ends ends = ends_of(o.segs.data[i], direction.data[i]);
            twice += static_cast<i128>(ends.start.x) * ends.finish.y - static_cast<i128>(ends.finish.x) * ends.start.y;
        }
        const double value = to_double(twice) * .5 / (o.scale * o.scale);
        *area = value < 0 ? -value : value;
    }
    arena_rewind(&factory->scratch, mark);
    return ok;
}
} // namespace maicord::geometry
