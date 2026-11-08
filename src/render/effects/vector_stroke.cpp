#include "vector_internal.hpp"

// geometry_stroke: the outline of a stroke as polygons.
//
// A figure is flattened to a polyline (vertices that were inside a curve are smooth; the others are corners, which take
// the stroke's join). The stroke is then one outline: the left offset forward, the end cap, the right offset back and
// the start cap, or two rings for a closed figure. Inner joins use the offset curves' intersection while it lies on
// both segments; where the joins at a segment's two ends cut more of its offset than it has, they go through the vertex
// instead, which keeps the nonzero fill of the outline the stroke even where the offset folds over itself.
namespace maicord::geometry {
namespace {
// A point or a direction, as two doubles in one register.
using V = double __attribute__((ext_vector_type(2)));
double dot(V a, V b) noexcept { return a.x * b.x + a.y * b.y; }
double cross(V a, V b) noexcept { return a.x * b.y - a.y * b.x; }
V left_of(V d) noexcept { return V{-d.y, d.x}; }

// Cross products of unit vectors below this are straight lines.
constexpr double straight = 1e-12;

// A vertex of a polyline: its position, the directions the curves arrive and leave by (zero where there is none) and
// flags: `smooth`, and `joined` where a zero-length segment was taken out.
struct Vertex {
    V p, in, out;
    u32 flags;
};
constexpr u32 smooth = 1, joined = 2;
struct Poly {
    const Vertex* v;
    u32 n;
    bool closed;
};

struct Stroker {
    Scratch s;
    Geometry* out;
    double h, tolerance, arc_tolerance, limit;
    CapStyle cap_style;
    LineJoin join;
    Vec<Vertex> vertices, reversed;
    Vec<V> ring;
    Vec<Point> polygon;
    Vec<Flat> flat;
    bool closed;

    void put(V p) noexcept {
        if (V* slot = vec_push(s, ring))
            *slot = p;
    }
    void push_vertex(V p, u32 flags) noexcept {
        if (Vertex* slot = vec_push(s, vertices))
            *slot = {p, {}, {}, flags};
    }
    // A cubic's interior points (the caller pushes its end), cut where the curve would leave `tolerance` of a chord.
    void flatten(V a, V b, V c, V d) noexcept {
        const double q[8]{a.x, a.y, b.x, b.y, c.x, c.y, d.x, d.y};
        // Chords of a curve that turn by more than this make the miter of a smooth join stray from the true offset
        // by more than the tolerance (h * turn^2 / 8), so the curve is cut finer where it bends.
        flat.count = 0;
        if (flatten_cubic(s, q, tolerance, clamp_value(1 - 4 * tolerance / h, .5, .99995), flat))
            for (u32 i = 0; i + 1 < flat.count; ++i)
                push_vertex({flat.data[i].x, flat.data[i].y}, smooth);
    }
    // Drops repeated points, and the last one of a closed figure when it is the first. The tangents go with them.
    void clean() noexcept {
        Vertex* v = vertices.data;
        double extent = 0;
        for (u32 i = 0; i < vertices.count; ++i)
            extent = maximum(extent, maximum(absolute(v[i].p.x), absolute(v[i].p.y)));
        // Only repeated points are one point: a hook smaller than the tolerance still turns the offset through an arc.
        const double eps = (1 + extent) * 1e-9;
        const auto equal = [eps](V a, V b) noexcept { return absolute(a.x - b.x) <= eps && absolute(a.y - b.y) <= eps; };
        u32 n = 0;
        for (u32 i = 0; i < vertices.count; ++i) {
            if (n && equal(v[n - 1].p, v[i].p)) {
                // The vertex arrives as the first did and leaves as the last does.
                v[n - 1].flags = (v[n - 1].flags & v[i].flags & smooth) | joined;
                v[n - 1].out = v[i].out;
                continue;
            }
            v[n++] = v[i];
        }
        if (closed && n > 1 && equal(v[0].p, v[n - 1].p)) {
            v[0].flags = (v[0].flags & v[n - 1].flags & smooth) | ((v[0].flags | v[n - 1].flags) & joined);
            v[0].in = v[n - 1].in;
            --n;
        }
        vertices.count = n;
    }

    // Appends the points on the outline of a stroke between the offsets h u and h v around `centre`.
    void arc(V centre, V u, V v, V hint, u32 depth) noexcept {
        const V sum = u + v;
        const double length = root(dot(sum, sum));
        if (depth >= 20 || h * (1 - length * .5) <= arc_tolerance)
            return;
        const V mid = length > 1e-9 ? sum * (1 / length) : hint;
        arc(centre, u, mid, hint, depth + 1);
        put(centre + mid * h);
        arc(centre, mid, v, hint, depth + 1);
    }
    // The outer side of a join at p from direction d0 to d1: the offset points, and what the join puts between them
    // (an exact miter point replaces them).
    void outer_join(V p, V d0, V d1, bool is_smooth) noexcept {
        const V n0 = left_of(d0), n1 = left_of(d1), a = p + n0 * h, b = p + n1 * h;
        const double turn = 1 + dot(d0, d1);
        if (is_smooth || join == join_miter) {
            if (is_smooth ? (turn > 1e-6 && h * (root(2 / turn) - 1) <= tolerance)
                          : (turn > 1e-12 && turn >= 2 / (limit * limit))) {
                put(p + (n0 + n1) * (h / turn));
                return;
            }
            put(a);
            if (is_smooth) {
                // A miter that strays from the offset of the curve by more than the tolerance is the arc it stands for.
                arc(p, n0, n1, d0, 0);
            } else {
                // Past the miter limit the tip is clipped at limit * h from the vertex, across its bisector.
                const V sum = n0 + n1;
                const double length = root(dot(sum, sum));
                const V bisector = length > 1e-9 ? sum * (1 / length) : d0;
                const double clip = limit * h;
                put(a + d0 * ((clip - dot(a - p, bisector)) / dot(d0, bisector)));
                put(b - d1 * ((dot(b - p, bisector) - clip) / dot(d1, bisector)));
            }
            put(b);
            return;
        }
        put(a);
        if (join == join_round)
            arc(p, n0, n1, d0, 0);
        put(b);
    }
    // Between P + h u and P - h u around the tip direction d, where u is d turned left.
    void cap(V p, V d) noexcept {
        const V u = left_of(d);
        if (cap_style == cap_square) {
            put(p + u * h + d * h);
            put(p - u * h + d * h);
        } else if (cap_style == cap_round) {
            arc(p, u, u * -1, d, 0);
        }
    }

    // The left offset of a polyline, forward.
    void left_path(const Poly& poly) noexcept {
        const Vertex* vx = poly.v;
        const u32 n = poly.n, segments = poly.closed ? n : n - 1;
        // Each segment's unit direction and length, and what the joins that leave this side on the inside cut from its
        // offset: where the two ends cut more than the segment has, the offset runs backwards there and its joins go
        // through the vertex instead.
        struct Segment {
            V d;
            double length, cut;
        };
        Segment* segment = arena_allocate<Segment>(s.arena, segments);
        if (!segment) {
            s.failed = true;
            return;
        }
        for (u32 i = 0; i < segments; ++i) {
            const V d = vx[i + 1 == n ? 0 : i + 1].p - vx[i].p;
            const double length = root(dot(d, d));
            segment[i] = {d * (1 / length), length, 0};
        }
        for (u32 v = poly.closed ? 0 : 1, previous = poly.closed ? n - 1 : 0; v < segments; previous = v++) {
            const V d0 = segment[previous].d, d1 = segment[v].d;
            const double turn = cross(d0, d1), half = 1 + dot(d0, d1);
            if (turn > straight && half > 1e-9) {
                const double cut = h * turn / half;
                segment[(v + n - 1) % n].cut += cut;
                segment[v].cut += cut;
            }
        }
        const auto offset = [&](V p, V d) noexcept { return p + left_of(d) * h; };
        // Whether two directions differ enough for the points offset along each to be two points.
        const auto apart = [](V a, V b) noexcept { return dot(a, b) < .99999; };
        double length0 = 0, length1 = 0;
        V d0{}, d1{};
        if (!poly.closed) {
            d0 = segment[0].d;
            length0 = segment[0].length;
            const V t = unit_or(vx[0].out, d0);
            put(offset(vx[0].p, t));
            if (apart(t, d0))
                put(offset(vx[0].p, d0));
        } else {
            d0 = segment[n - 1].d;
            length0 = segment[n - 1].length;
        }
        for (u32 v = poly.closed ? 0 : 1; v < segments; ++v) {
            d1 = segment[v].d;
            length1 = segment[v].length;
            const V p = vx[v].p;
            // At a corner the join is between the tangents the curves arrive and leave by; the flattened chords may
            // turn quite differently there (a hook smaller than the tolerance), and Direct2D offsets the curves.
            const bool corner = !(vx[v].flags & smooth);
            const V t0 = corner ? unit_or(vx[v].in, d0) : d0, t1 = corner ? unit_or(vx[v].out, d1) : d1;
            const double turn = cross(t0, t1), straightness = dot(t0, t1);
            if (turn > straight) {
                // The left side is the inner one.
                const double half = 1 + dot(d0, d1);
                if (cross(d0, d1) > straight && half > 1e-9 && segment[(v + n - 1) % n].cut <= length0 &&
                    segment[v].cut <= length1) {
                    put(p + (left_of(d0) + left_of(d1)) * (h / half));
                } else {
                    put(offset(p, d0));
                    put(p);
                    put(offset(p, d1));
                }
            } else if (turn < -straight || straightness < 0) {
                if (corner && apart(t0, d0))
                    put(offset(p, d0));
                if ((vx[v].flags & joined) && straightness < 0) {
                    // A zero-length segment between the two directions: Direct2D turns through it by a right angle on
                    // either side (a reversal there ends square, however far the miter limit would let it reach).
                    const V mid = left_of(t0) * -1;
                    outer_join(p, t0, mid, false);
                    outer_join(p, mid, t1, false);
                } else {
                    outer_join(p, t0, t1, !corner);
                }
                if (corner && apart(t1, d1))
                    put(offset(p, d1));
            } else {
                put(offset(p, d0));
                if (apart(d0, d1))
                    put(offset(p, d1));
            }
            d0 = d1;
            length0 = length1;
        }
        if (!poly.closed) {
            const V last = vx[n - 1].p, t = unit_or(vx[n - 1].in, d0);
            if (apart(t, d0))
                put(offset(last, d0));
            put(offset(last, t));
        }
    }

    // The ring's points as one closed polygon of the outline.
    void emit() noexcept {
        const V* list = ring.data;
        u32 n = 0;
        for (u32 i = 0; i < ring.count; ++i)
            if (!n || list[i].x != list[n - 1].x || list[i].y != list[n - 1].y)
                ring.data[n++] = list[i];
        while (n > 1 && list[0].x == list[n - 1].x && list[0].y == list[n - 1].y)
            --n;
        ring.count = 0;
        if (n < 3 || !vec_reserve(s, polygon, n))
            return;
        for (u32 i = 0; i < n; ++i)
            polygon.data[i] = {static_cast<float>(list[i].x), static_cast<float>(list[i].y)};
        if (geometry_begin(out, polygon.data[0], true) && geometry_append(out, polygon.data + 1, n - 1))
            geometry_end(out, true);
        else
            s.failed = true;
    }
    // `t` as a unit vector, or `fallback` when it is none.
    static V unit_or(V t, V fallback) noexcept {
        const double length = root(dot(t, t));
        return length > 1e-12 ? t * (1 / length) : fallback;
    }

    void stroke_figure() noexcept {
        clean();
        const u32 n = vertices.count;
        if (s.failed || n < 2 || !vec_reserve(s, reversed, n))
            return;
        // The reverse of the figure, whose left side is the right side of this one.
        const Vertex* v = vertices.data;
        for (u32 i = 0; i < n; ++i)
            reversed.data[i] = {v[n - 1 - i].p, v[n - 1 - i].out * -1, v[n - 1 - i].in * -1, v[n - 1 - i].flags};
        const Poly forward{v, n, closed}, backward{reversed.data, n, closed};
        ring.count = 0;
        left_path(forward);
        // Caps face the way the curve itself leaves its ends.
        if (closed) {
            emit();
        } else {
            const V d = v[n - 1].p - v[n - 2].p;
            cap(v[n - 1].p, unit_or(v[n - 1].in, d * (1 / root(dot(d, d)))));
        }
        left_path(backward);
        if (!closed) {
            const V d = v[0].p - v[1].p;
            cap(v[0].p, unit_or(v[0].out * -1, d * (1 / root(dot(d, d)))));
        }
        emit();
    }
};
} // namespace

Geometry* geometry_stroke(const Geometry* geometry, const StrokeStyle& style, float tolerance) noexcept {
    Factory* factory = geometry->factory;
    Geometry* out = geometry_create(factory);
    if (!out || !(style.width > 0) || !is_finite(style.width))
        return out;
    const ArenaMark mark = arena_mark(&factory->scratch);
    Stroker st{};
    st.s = {&factory->scratch, false};
    st.out = out;
    st.h = style.width * .5;
    st.tolerance = tolerance > 1e-9f ? tolerance : 1e-9;
    // Arcs are short and few (round caps and joins only), and a round cap is what a small stroke is made of.
    st.arc_tolerance = st.tolerance * .25;
    st.limit = style.miter_limit > 1 ? style.miter_limit : 1;
    st.cap_style = style.cap;
    st.join = style.join;
    // Whether two tangents point the same way within five degrees: the vertex between them is smooth.
    const auto aligned = [](V a, V b) noexcept {
        const double lengths = root(dot(a, a)) * root(dot(b, b));
        return lengths > 1e-24 && dot(a, b) >= .9962 * lengths;
    };
    // The first of the points that is not at `from`, as a direction.
    const auto direction = [](V from, V a, V b, V c) noexcept {
        const V candidates[]{a, b, c};
        for (const V& v : candidates)
            if (absolute(v.x - from.x) + absolute(v.y - from.y) > 1e-12)
                return v - from;
        return V{};
    };
    const Point* points = geometry->points;
    for (u32 f = 0; f < geometry->figure_count && !st.s.failed; ++f) {
        const Figure& figure = geometry->figures[f];
        st.vertices.count = 0;
        st.closed = figure.closed;
        V current{points[figure.first].x, points[figure.first].y};
        st.push_vertex(current, 0);
        // A vertex whose arriving and leaving tangents agree is smooth however sharply the flattened curve may turn
        // there (a hook smaller than the tolerance): its join is the offset of the curve, not a corner's.
        V first_leave{}, arrive{};
        const u32 step = figure.polyline ? 1 : 3;
        for (u32 at = figure.first + step; at < figure.end && !st.s.failed; at += step) {
            const V to{points[at].x, points[at].y};
            V leave = to - current, next_arrive = leave, b{}, c{};
            if (step == 3) {
                b = {points[at - 2].x, points[at - 2].y};
                c = {points[at - 1].x, points[at - 1].y};
                leave = direction(current, b, c, to);
                next_arrive = direction(to, c, b, current) * -1;
            }
            Vertex& last = st.vertices.data[st.vertices.count - 1];
            if (at == figure.first + step)
                first_leave = leave;
            else if (aligned(arrive, leave))
                last.flags = smooth;
            last.out = leave;
            if (step == 3)
                st.flatten(current, b, c, to);
            current = to;
            st.push_vertex(current, 0);
            arrive = next_arrive;
            if (!st.s.failed)
                st.vertices.data[st.vertices.count - 1].in = next_arrive;
        }
        if (st.s.failed)
            break;
        if (st.closed && figure.end > figure.first + 1) {
            // A figure that ends away from its start closes with a line back to it.
            Vertex& first = st.vertices.data[0];
            Vertex& last = st.vertices.data[st.vertices.count - 1];
            const V gap = first.p - last.p;
            const bool line = gap.x != 0 || gap.y != 0;
            const V back = line ? gap : first_leave, into = line ? gap : arrive;
            if (aligned(arrive, back))
                last.flags = smooth;
            if (aligned(into, first_leave))
                first.flags = smooth;
            first.in = into;
            last.out = back;
        }
        st.stroke_figure();
    }
    arena_rewind(&factory->scratch, mark);
    return st.s.failed ? nullptr : out;
}
} // namespace maicord::geometry
