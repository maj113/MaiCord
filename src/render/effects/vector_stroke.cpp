#include "vector_internal.hpp"

// Geometry::Widen: the outline of a stroke as polygons.
//
// A figure is flattened to a polyline (vertices that were inside a curve are smooth; the others are corners, which take
// the stroke's join). The stroke is then one outline: the left offset forward, the end cap, the right offset back and
// the start cap, or two rings for a closed figure. Where the inner side of a turn is tighter than a segment is long the
// offset curves fold over themselves, and that figure is built from separate pieces instead (a quadrilateral per
// segment, a wedge per join, a cap each), which cannot cancel each other. Every polygon winds the same way, so the
// nonzero fill of whatever overlaps is the stroke.
namespace maicord::geometry {
namespace {
struct V {
    double x, y;
};
V operator+(V a, V b) noexcept { return {a.x + b.x, a.y + b.y}; }
V operator-(V a, V b) noexcept { return {a.x - b.x, a.y - b.y}; }
V operator*(V a, double k) noexcept { return {a.x * k, a.y * k}; }
double dot(V a, V b) noexcept { return a.x * b.x + a.y * b.y; }
double cross(V a, V b) noexcept { return a.x * b.y - a.y * b.x; }
V left_of(V d) noexcept { return {-d.y, d.x}; }

// Cross products of unit vectors below this are straight lines.
constexpr double straight = 1e-12;

struct Poly {
    const V* p;
    const u8* smooth;
    u32 n;
    bool closed;
};

struct Stroker {
    Scratch s;
    SimplifiedGeometrySink* sink;
    double h, tolerance, arc_tolerance, limit;
    CapStyle start_cap, end_cap;
    LineJoin join;
    Vec<V> points, reversed, ring;
    Vec<u8> smooth, reversed_smooth;
    bool closed;

    void put(Vec<V>& list, V p) noexcept {
        if (V* slot = vec_push(s, list))
            *slot = p;
    }
    void put_flag(Vec<u8>& list, u8 flag) noexcept {
        if (u8* slot = vec_push(s, list))
            *slot = flag;
    }
    void push_vertex(V p, u8 flag) noexcept {
        put(points, p);
        put_flag(smooth, flag);
    }
    // A cubic's interior points (the caller pushes its end), cut where the curve would leave `tolerance` of a chord.
    void flatten(V a, V b, V c, V d) noexcept {
        const double q[8]{a.x, a.y, b.x, b.y, c.x, c.y, d.x, d.y};
        flatten_cubic(q, tolerance, [&](float t, double x, double y) noexcept {
            if (t < 1)
                push_vertex({x, y}, 1);
        });
    }
    // Drops repeated points, and the last one of a closed figure when it is the first.
    void clean() noexcept {
        double extent = 0;
        for (u32 i = 0; i < points.count; ++i)
            extent = maximum(extent, maximum(absolute(points.data[i].x), absolute(points.data[i].y)));
        // Points closer than a quarter of the tolerance are one point: the polyline's turns there are noise, and they
        // would make every join on a tiny segment look folded.
        const double eps = maximum((1 + extent) * 1e-9, tolerance * .25);
        const auto equal = [eps](V a, V b) noexcept { return absolute(a.x - b.x) <= eps && absolute(a.y - b.y) <= eps; };
        u32 out = 0;
        for (u32 i = 0; i < points.count; ++i) {
            if (out && equal(points.data[out - 1], points.data[i])) {
                smooth.data[out - 1] &= smooth.data[i];
                continue;
            }
            points.data[out] = points.data[i];
            smooth.data[out++] = smooth.data[i];
        }
        if (closed && out > 1 && equal(points.data[0], points.data[out - 1])) {
            smooth.data[0] &= smooth.data[out - 1];
            --out;
        }
        points.count = smooth.count = out;
    }

    // Appends the points on the outline of a stroke between the offsets on the outer side of a join.
    // `with_ends` adds the offset points themselves (A before, B after): an exact miter point replaces them.
    void arc(Vec<V>& dst, V centre, V u, V v, V hint, u32 depth) noexcept {
        const V sum = u + v;
        const double length = root(dot(sum, sum));
        if (depth >= 20 || h * (1 - length * .5) <= arc_tolerance)
            return;
        const V mid = length > 1e-9 ? sum * (1 / length) : hint;
        arc(dst, centre, u, mid, hint, depth + 1);
        put(dst, centre + mid * h);
        arc(dst, centre, mid, v, hint, depth + 1);
    }
    void outer_join(Vec<V>& dst, V p, V d0, V d1, double sigma, bool is_smooth, bool with_ends) noexcept {
        const V n0 = left_of(d0) * sigma, n1 = left_of(d1) * sigma, a = p + n0 * h, b = p + n1 * h;
        const double turn = 1 + dot(d0, d1);
        if (is_smooth || join == join_miter) {
            if (is_smooth ? turn > 1e-6 : (turn > 1e-12 && turn >= 2 / (limit * limit))) {
                put(dst, p + (n0 + n1) * (h / turn));
                return;
            }
            if (is_smooth) {
                if (with_ends) {
                    put(dst, a);
                    put(dst, b);
                }
                return;
            }
            // Past the miter limit the tip is clipped at limit * h from the vertex, across its bisector.
            const V sum = n0 + n1;
            const double length = root(dot(sum, sum));
            const V bisector = length > 1e-9 ? sum * (1 / length) : d0;
            const double clip = limit * h, s0 = (clip - dot(a - p, bisector)) / dot(d0, bisector),
                         s1 = (dot(b - p, bisector) - clip) / dot(d1, bisector);
            if (with_ends)
                put(dst, a);
            put(dst, a + d0 * s0);
            put(dst, b - d1 * s1);
            if (with_ends)
                put(dst, b);
            return;
        }
        if (with_ends)
            put(dst, a);
        if (join == join_round)
            arc(dst, p, n0, n1, d0, 0);
        if (with_ends)
            put(dst, b);
    }
    // Between P + h u and P - h u around the tip direction d, where u is d turned left.
    void cap(Vec<V>& dst, V p, V d, CapStyle style) noexcept {
        const V u = left_of(d);
        if (style == cap_square) {
            put(dst, p + u * h + d * h);
            put(dst, p - u * h + d * h);
        } else if (style == cap_round) {
            arc(dst, p, u, u * -1, d, 0);
        }
    }

    // The left offset of a polyline, forward.
    void left_path(Vec<V>& dst, const Poly& poly) noexcept {
        const u32 n = poly.n, segments = poly.closed ? n : n - 1;
        const auto direction = [&](u32 i, double& length) noexcept {
            const V d = poly.p[(i + 1) % n] - poly.p[i];
            length = root(dot(d, d));
            return d * (1 / length);
        };
        // What the joins that leave this side on the inside cut from each segment's offset: where the two ends cut
        // more than the segment has, the offset runs backwards there and its joins go through the vertex instead.
        double* cuts = arena_allocate_zero<double>(s.arena, segments);
        if (!cuts) {
            s.failed = true;
            return;
        }
        {
            double length0 = 0, length1 = 0;
            V d0 = direction(poly.closed ? n - 1 : 0, length0), d1{};
            for (u32 v = poly.closed ? 0 : 1; v < (poly.closed ? n : n - 1); ++v) {
                d1 = direction(v, length1);
                const double turn = cross(d0, d1), half = 1 + dot(d0, d1);
                if (turn > straight && half > 1e-9) {
                    const double cut = h * turn / half;
                    cuts[(v + n - 1) % n] += cut;
                    cuts[v] += cut;
                }
                d0 = d1;
            }
        }
        double length0 = 0, length1 = 0;
        V d0{}, d1{};
        if (!poly.closed) {
            d0 = direction(0, length0);
            put(dst, poly.p[0] + left_of(d0) * h);
        } else {
            d0 = direction(n - 1, length0);
        }
        for (u32 v = poly.closed ? 0 : 1; v < (poly.closed ? n : n - 1); ++v) {
            d1 = direction(v, length1);
            const V p = poly.p[v];
            const double turn = cross(d0, d1), straightness = dot(d0, d1);
            if (turn > straight) {
                // The left side is the inner one.
                const double half = 1 + straightness;
                if (half > 1e-9 && cuts[(v + n - 1) % n] <= length0 && cuts[v] <= length1) {
                    put(dst, p + (left_of(d0) + left_of(d1)) * (h / half));
                } else {
                    put(dst, p + left_of(d0) * h);
                    put(dst, p);
                    put(dst, p + left_of(d1) * h);
                }
            } else if (turn < -straight || straightness < 0) {
                outer_join(dst, p, d0, d1, 1, poly.smooth[v] != 0, true);
            } else {
                put(dst, p + left_of(d0) * h);
            }
            d0 = d1;
            length0 = length1;
        }
        if (!poly.closed)
            put(dst, poly.p[n - 1] + left_of(d0) * h);
    }

    // One closed polygon to the sink.
    void emit(Vec<V>& list) noexcept {
        u32 n = 0;
        for (u32 i = 0; i < list.count; ++i)
            if (!n || list.data[i].x != list.data[n - 1].x || list.data[i].y != list.data[n - 1].y)
                list.data[n++] = list.data[i];
        while (n > 1 && list.data[0].x == list.data[n - 1].x && list.data[0].y == list.data[n - 1].y)
            --n;
        if (n < 3)
            return;
        const auto at = [&](u32 i) noexcept {
            return Point{static_cast<float>(list.data[i].x), static_cast<float>(list.data[i].y)};
        };
        sink->BeginFigure(at(0), figure_filled);
        Point batch[32];
        u32 count = 0;
        for (u32 i = 1; i < n; ++i) {
            batch[count++] = at(i);
            if (count == 32) {
                sink->AddLines(batch, count);
                count = 0;
            }
        }
        if (count)
            sink->AddLines(batch, count);
        sink->EndFigure(figure_closed);
    }
    void stroke_outline(const Poly& forward, const Poly& backward) noexcept {
        ring.count = 0;
        left_path(ring, forward);
        const u32 n = forward.n;
        if (!forward.closed) {
            V d = forward.p[n - 1] - forward.p[n - 2];
            cap(ring, forward.p[n - 1], d * (1 / root(dot(d, d))), end_cap);
        }
        if (forward.closed) {
            emit(ring);
            ring.count = 0;
        }
        left_path(ring, backward);
        if (!forward.closed) {
            V d = forward.p[0] - forward.p[1];
            cap(ring, forward.p[0], d * (1 / root(dot(d, d))), start_cap);
        }
        emit(ring);
    }

    void stroke_figure() noexcept {
        clean();
        const u32 n = points.count;
        if (s.failed || n < 2)
            return;
        // The reverse of the figure, whose left side is the right side of this one.
        reversed.count = reversed_smooth.count = 0;
        for (u32 i = n; i--;) {
            put(reversed, points.data[i]);
            put_flag(reversed_smooth, smooth.data[i]);
        }
        if (s.failed)
            return;
        stroke_outline({points.data, smooth.data, n, closed}, {reversed.data, reversed_smooth.data, n, closed});
    }
};
} // namespace

Result stroke_outline(Factory* factory, const Geometry* geometry, float width, const StrokeStyleProperties& style,
                      float tolerance, SimplifiedGeometrySink* sink) noexcept {
    if (!(width > 0) || !is_finite(width))
        return result_ok;
    const ArenaMark mark = arena_mark(&factory->scratch);
    Stroker st{};
    st.s = {&factory->scratch, false};
    st.sink = sink;
    st.h = width * .5;
    st.tolerance = tolerance > 1e-9f ? tolerance : 1e-9;
    // Arcs are short and few (round caps and joins only), and a round cap is what a small stroke is made of.
    st.arc_tolerance = st.tolerance * .25;
    st.limit = style.miterLimit > 1 ? style.miterLimit : 1;
    st.start_cap = style.startCap;
    st.end_cap = style.endCap;
    st.join = style.lineJoin;
    sink->SetFillMode(fill_winding);
    for (u32 f = 0; f < geometry->figure_count && !st.s.failed; ++f) {
        const Figure& figure = geometry->figures[f];
        const Point* p = geometry->points + figure.point_begin;
        st.points.count = st.smooth.count = 0;
        st.closed = figure.closed != 0;
        V current{p->x, p->y};
        ++p;
        st.push_vertex(current, 0);
        for (u32 k = 0; k < figure.kind_count; ++k) {
            if (geometry->kinds[figure.kind_begin + k] == kind_line) {
                current = {p->x, p->y};
                ++p;
            } else {
                const V b{p[0].x, p[0].y}, c{p[1].x, p[1].y}, d{p[2].x, p[2].y};
                p += 3;
                st.flatten(current, b, c, d);
                current = d;
            }
            st.push_vertex(current, 0);
        }
        st.stroke_figure();
    }
    const Result result = st.s.failed ? result_out_of_memory : result_ok;
    arena_rewind(&factory->scratch, mark);
    return result;
}
} // namespace maicord::geometry
