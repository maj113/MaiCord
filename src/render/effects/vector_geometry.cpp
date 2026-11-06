#include "vector_internal.hpp"
#include "maicord/sys/crt.hpp"

namespace maicord::geometry {
namespace {
// Each arena is only address space until used; a document that needs more than this fails.
constexpr usize permanent_reserve = usize{256} << 20, scratch_reserve = usize{128} << 20, commit_step = 256 * 1024;
// Four cubics of this handle length trace a circle to within a fraction of a percent of its radius.
constexpr double kappa = .5522847498307936;

Geometry* new_geometry(Factory* factory) noexcept {
    auto* geometry = arena_allocate_zero<Geometry>(&factory->permanent);
    if (geometry)
        geometry->factory = factory;
    return geometry;
}

// Appends room for `extra` elements to an array of the geometry's own, in place when it is the arena's last block.
template <typename T> bool grow(Arena* arena, T*& data, u32& capacity, u32 count, u32 extra) noexcept {
    if (count + extra <= capacity)
        return true;
    u32 next = capacity * 2;
    if (next < count + extra)
        next = count + extra;
    if (next < 8)
        next = 8;
    if (data && arena->base + arena->offset == reinterpret_cast<u8*>(data + capacity)) {
        const ArenaMark mark = arena_mark(arena);
        void* more = arena_allocate(arena, static_cast<usize>(next - capacity) * sizeof(T), alignof(T));
        if (more == data + capacity) {
            capacity = next;
            return true;
        }
        if (more)
            arena_rewind(arena, mark);
    }
    T* fresh = arena_allocate<T>(arena, next);
    if (!fresh)
        return false;
    if (count)
        copy_bytes(fresh, data, static_cast<u32>(count * sizeof(T)));
    data = fresh;
    capacity = next;
    return true;
}

// Appends one figure, segment or point at a time; every call reports whether the arena still had room.
struct Builder {
    Geometry& g;
    Arena* arena() const noexcept { return &g.factory->permanent; }
    bool point(Point p) noexcept {
        if (!grow(arena(), g.points, g.point_capacity, g.point_count, 1))
            return false;
        g.points[g.point_count++] = p;
        return true;
    }
    bool kind(u8 k) noexcept {
        if (!grow(arena(), g.kinds, g.kind_capacity, g.kind_count, 1))
            return false;
        g.kinds[g.kind_count++] = k;
        ++g.figures[g.figure_count - 1].kind_count;
        return true;
    }
    bool begin(Point p) noexcept {
        if (!grow(arena(), g.figures, g.figure_capacity, g.figure_count, 1))
            return false;
        g.figures[g.figure_count++] = {g.kind_count, 0, g.point_count, 0};
        return point(p);
    }
    bool line(Point p) noexcept { return g.figure_count && kind(kind_line) && point(p); }
    bool cubic(Point a, Point b, Point c) noexcept {
        return g.figure_count && kind(kind_cubic) && point(a) && point(b) && point(c);
    }
    void end(bool closed) noexcept {
        if (g.figure_count)
            g.figures[g.figure_count - 1].closed = closed;
    }
};

// The sink Geometry::Open returns.
struct PathSink final : GeometrySink {
    Builder builder;
    Point current;
    bool failed;
    explicit PathSink(Geometry& g) noexcept : builder{g}, current{}, failed(false) {}
    void BeginFigure(Point p, FigureBegin) noexcept override {
        failed |= !builder.begin(p);
        current = p;
    }
    void AddLines(const Point* p, u32 n) noexcept override {
        for (u32 i = 0; i < n; ++i)
            failed |= !builder.line(p[i]);
        if (n)
            current = p[n - 1];
    }
    void AddBezier(const BezierSegment& b) noexcept override {
        failed |= !builder.cubic(b.point1, b.point2, b.point3);
        current = b.point3;
    }
    void AddBeziers(const BezierSegment* b, u32 n) noexcept override {
        for (u32 i = 0; i < n; ++i)
            AddBezier(b[i]);
    }
    void EndFigure(FigureEnd end) noexcept override { builder.end(end == figure_closed); }
    Result Close() noexcept override {
        builder.g.building = false;
        return failed ? result_out_of_memory : result_ok;
    }
};

// Forwards every figure it is given, mapped through a matrix, to a Builder.
void copy_transformed(Builder& to, const Geometry& from, const Matrix* m, bool& ok) noexcept {
    for (u32 f = 0; f < from.figure_count; ++f) {
        const Figure& figure = from.figures[f];
        const Point* p = from.points + figure.point_begin;
        ok &= to.begin(transform_point(m, *p++));
        for (u32 k = 0; k < figure.kind_count; ++k) {
            if (from.kinds[figure.kind_begin + k] == kind_line) {
                ok &= to.line(transform_point(m, *p++));
            } else {
                ok &= to.cubic(transform_point(m, p[0]), transform_point(m, p[1]), transform_point(m, p[2]));
                p += 3;
            }
        }
        to.end(figure.closed != 0);
    }
}

struct Bounds {
    double left, top, right, bottom;
    bool any;
    void add(double x, double y) noexcept {
        if (!any) {
            left = right = x;
            top = bottom = y;
            any = true;
            return;
        }
        left = minimum(left, x);
        right = maximum(right, x);
        top = minimum(top, y);
        bottom = maximum(bottom, y);
    }
};
double cubic_at(double a, double b, double c, double d, double t) noexcept {
    const double u = 1 - t;
    return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d;
}
// The extremes of one coordinate of a cubic between its end points: the roots of the derivative.
void add_extremes(Bounds& bounds, const double (&x)[4], const double (&y)[4]) noexcept {
    for (u32 axis = 0; axis < 2; ++axis) {
        const double* v = axis ? y : x;
        const double a = v[3] - 3 * v[2] + 3 * v[1] - v[0], b = 2 * (v[2] - 2 * v[1] + v[0]), c = v[1] - v[0];
        double roots[2];
        u32 count = 0;
        if (absolute(a) < 1e-12) {
            if (absolute(b) > 1e-12)
                roots[count++] = -c / b;
        } else {
            const double discriminant = b * b - 4 * a * c;
            if (discriminant >= 0) {
                const double square = root(discriminant);
                roots[count++] = (-b + square) / (2 * a);
                roots[count++] = (-b - square) / (2 * a);
            }
        }
        for (u32 i = 0; i < count; ++i)
            if (roots[i] > 0 && roots[i] < 1)
                bounds.add(cubic_at(x[0], x[1], x[2], x[3], roots[i]), cubic_at(y[0], y[1], y[2], y[3], roots[i]));
    }
}
} // namespace

bool factory_create(Factory** out) noexcept {
    Arena permanent{};
    if (!arena_initialize(&permanent, permanent_reserve, 0, commit_step, ArenaTag::render))
        return false;
    auto* factory = arena_allocate_zero<Factory>(&permanent);
    if (!factory || !arena_initialize(&factory->scratch, scratch_reserve, 0, commit_step, ArenaTag::render)) {
        arena_release(&permanent);
        return false;
    }
    factory->permanent = permanent;
    *out = factory;
    return true;
}

void Factory::Release() noexcept {
    Arena memory = permanent;
    arena_release(&scratch);
    arena_release(&memory);
}

Result Factory::CreateRectangleGeometry(const Rect* r, Geometry** out) noexcept {
    Geometry* g = new_geometry(this);
    if (!g)
        return result_out_of_memory;
    Builder b{*g};
    if (!b.begin({r->left, r->top}) || !b.line({r->right, r->top}) || !b.line({r->right, r->bottom}) ||
        !b.line({r->left, r->bottom}))
        return result_out_of_memory;
    b.end(true);
    *out = g;
    return result_ok;
}

Result Factory::CreateEllipseGeometry(const Ellipse& e, Geometry** out) noexcept {
    Geometry* g = new_geometry(this);
    if (!g)
        return result_out_of_memory;
    Builder b{*g};
    const float cx = e.point.x, cy = e.point.y, rx = e.radiusX < 0 ? -e.radiusX : e.radiusX,
                ry = e.radiusY < 0 ? -e.radiusY : e.radiusY, kx = rx * static_cast<float>(kappa),
                ky = ry * static_cast<float>(kappa);
    if (!b.begin({cx - rx, cy}) || !b.cubic({cx - rx, cy - ky}, {cx - kx, cy - ry}, {cx, cy - ry}) ||
        !b.cubic({cx + kx, cy - ry}, {cx + rx, cy - ky}, {cx + rx, cy}) ||
        !b.cubic({cx + rx, cy + ky}, {cx + kx, cy + ry}, {cx, cy + ry}) ||
        !b.cubic({cx - kx, cy + ry}, {cx - rx, cy + ky}, {cx - rx, cy}))
        return result_out_of_memory;
    b.end(true);
    *out = g;
    return result_ok;
}

Result Factory::CreateRoundedRectangleGeometry(const RoundedRect& r, Geometry** out) noexcept {
    const float l = r.rect.left, t = r.rect.top, rt = r.rect.right, bt = r.rect.bottom;
    const float half_width = (rt - l) * .5f, half_height = (bt - t) * .5f;
    float rx = r.radiusX < 0 ? 0 : r.radiusX, ry = r.radiusY < 0 ? 0 : r.radiusY;
    rx = rx > half_width ? half_width : rx;
    ry = ry > half_height ? half_height : ry;
    if (!(rx > 0 && ry > 0)) {
        const Rect plain = r.rect;
        return CreateRectangleGeometry(&plain, out);
    }
    Geometry* g = new_geometry(this);
    if (!g)
        return result_out_of_memory;
    Builder b{*g};
    const float kx = rx * static_cast<float>(kappa), ky = ry * static_cast<float>(kappa);
    bool ok = b.begin({l + rx, t});
    // Straight sides are left out when the corner radii meet.
    if (ok && l + rx < rt - rx)
        ok = b.line({rt - rx, t});
    ok = ok && b.cubic({rt - rx + kx, t}, {rt, t + ry - ky}, {rt, t + ry});
    if (ok && t + ry < bt - ry)
        ok = b.line({rt, bt - ry});
    ok = ok && b.cubic({rt, bt - ry + ky}, {rt - rx + kx, bt}, {rt - rx, bt});
    if (ok && l + rx < rt - rx)
        ok = b.line({l + rx, bt});
    ok = ok && b.cubic({l + rx - kx, bt}, {l, bt - ry + ky}, {l, bt - ry});
    if (ok && t + ry < bt - ry)
        ok = b.line({l, t + ry});
    ok = ok && b.cubic({l, t + ry - ky}, {l + rx - kx, t}, {l + rx, t});
    if (!ok)
        return result_out_of_memory;
    b.end(true);
    *out = g;
    return result_ok;
}

Result Factory::CreateTransformedGeometry(const Geometry* source, const Matrix* m, Geometry** out) noexcept {
    Geometry* g = new_geometry(this);
    if (!g)
        return result_out_of_memory;
    Builder b{*g};
    bool ok = true;
    copy_transformed(b, *source, m, ok);
    if (!ok)
        return result_out_of_memory;
    *out = g;
    return result_ok;
}

Result Factory::CreatePathGeometry(Geometry** out) noexcept {
    Geometry* g = new_geometry(this);
    if (!g)
        return result_out_of_memory;
    *out = g;
    return result_ok;
}

Result Factory::CreateStrokeStyle(const StrokeStyleProperties& properties, const float*, u32 dash_count,
                                  StrokeStyle** out) noexcept {
    auto* style = arena_allocate_zero<StrokeStyle>(&permanent);
    if (!style)
        return result_out_of_memory;
    if (dash_count)
        return result_invalid_argument;
    style->properties = properties;
    *out = style;
    return result_ok;
}

Result Geometry::Open(GeometrySink** sink) noexcept {
    if (building || figure_count)
        return result_invalid_argument;
    auto* path = arena_allocate<PathSink>(&factory->permanent);
    if (!path)
        return result_out_of_memory;
    new (path) PathSink(*this);
    building = true;
    *sink = path;
    return result_ok;
}

Result Geometry::GetBounds(const Matrix* m, Rect* out) const noexcept {
    Bounds bounds{};
    for (u32 f = 0; f < figure_count; ++f) {
        const Figure& figure = figures[f];
        const Point* p = points + figure.point_begin;
        Point current = transform_point(m, *p++);
        bounds.add(current.x, current.y);
        for (u32 k = 0; k < figure.kind_count; ++k) {
            if (kinds[figure.kind_begin + k] == kind_line) {
                current = transform_point(m, *p++);
                bounds.add(current.x, current.y);
            } else {
                const Point c1 = transform_point(m, p[0]), c2 = transform_point(m, p[1]), c3 = transform_point(m, p[2]);
                p += 3;
                const double x[4]{current.x, c1.x, c2.x, c3.x}, y[4]{current.y, c1.y, c2.y, c3.y};
                bounds.add(c3.x, c3.y);
                add_extremes(bounds, x, y);
                current = c3;
            }
        }
    }
    if (!bounds.any) {
        // An empty geometry reports an inverted rectangle, as Direct2D does.
        constexpr float most = 3.4028234663852886e+38f;
        *out = {__builtin_inff(), __builtin_inff(), most, most};
        return result_ok;
    }
    *out = {static_cast<float>(bounds.left), static_cast<float>(bounds.top), static_cast<float>(bounds.right),
            static_cast<float>(bounds.bottom)};
    return result_ok;
}

Result Geometry::Simplify(SimplificationOption, const Matrix* m, float, SimplifiedGeometrySink* sink) const noexcept {
    sink->SetFillMode(fill_winding);
    for (u32 f = 0; f < figure_count; ++f) {
        const Figure& figure = figures[f];
        const Point* p = points + figure.point_begin;
        sink->BeginFigure(transform_point(m, *p++), figure_filled);
        // Runs of one kind go out together.
        Point lines[32];
        BezierSegment cubics[16];
        u32 line_count = 0, cubic_count = 0;
        for (u32 k = 0; k < figure.kind_count; ++k) {
            if (kinds[figure.kind_begin + k] == kind_line) {
                if (cubic_count) {
                    sink->AddBeziers(cubics, cubic_count);
                    cubic_count = 0;
                }
                lines[line_count++] = transform_point(m, *p++);
                if (line_count == 32) {
                    sink->AddLines(lines, line_count);
                    line_count = 0;
                }
            } else {
                if (line_count) {
                    sink->AddLines(lines, line_count);
                    line_count = 0;
                }
                cubics[cubic_count++] = {transform_point(m, p[0]), transform_point(m, p[1]),
                                         transform_point(m, p[2])};
                p += 3;
                if (cubic_count == 16) {
                    sink->AddBeziers(cubics, cubic_count);
                    cubic_count = 0;
                }
            }
        }
        if (line_count)
            sink->AddLines(lines, line_count);
        if (cubic_count)
            sink->AddBeziers(cubics, cubic_count);
        sink->EndFigure(figure.closed ? figure_closed : figure_open);
    }
    return result_ok;
}

Result Geometry::ComputeArea(const Matrix* m, float tolerance, float* area) const noexcept {
    double value = 0;
    const Result result = overlay_area(factory, this, m, tolerance, &value);
    *area = static_cast<float>(value);
    return result;
}

Result Geometry::CombineWithGeometry(const Geometry* other, CombineMode mode, const Matrix* other_transform,
                                     float tolerance, SimplifiedGeometrySink* sink) const noexcept {
    return overlay_combine(factory, this, nullptr, other, other_transform, mode, tolerance, sink);
}

Result Geometry::Widen(float width, const StrokeStyle* style, const Matrix* m, float tolerance,
                       SimplifiedGeometrySink* sink) const noexcept {
    if (m || !style)
        return result_invalid_argument;
    return stroke_outline(factory, this, width, style->properties, tolerance, sink);
}
} // namespace maicord::geometry
