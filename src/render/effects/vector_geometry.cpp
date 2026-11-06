#include "vector_internal.hpp"

namespace maicord::geometry {
namespace {
// Each arena is only address space until used; a document that needs more than this fails. Commits come in steps
// of the arena's default.
constexpr usize permanent_reserve = usize{256} << 20, scratch_reserve = usize{128} << 20, reused_reserve = usize{64} << 20;
// Four cubics of this handle length trace a circle to within a fraction of a percent of its radius.
constexpr float kappa = .5522847498307936f;

// Whether a cubic stays within `tolerance` of its chord: a curve strays from it by at most three quarters of how far
// its handles do (and a handle beyond the chord's ends counts by how far past them it is).
bool cubic_flat(const double (&c)[8], double tolerance) noexcept {
    // In squares, times the chord's squared length: a handle's distance is its cross product with the chord, and past
    // an end the excess of its dot product over the chord's span.
    const double vx = c[6] - c[0], vy = c[7] - c[1], square = vx * vx + vy * vy, reach = tolerance * (4. / 3);
    for (u32 k = 1; k <= 2; ++k) {
        const double px = c[k * 2] - c[0], py = c[k * 2 + 1] - c[1];
        if (!(square > 1e-18)) {
            if (px * px + py * py > reach * reach)
                return false;
            continue;
        }
        const double along = px * vx + py * vy, across = px * vy - py * vx,
                     excess = along < 0 ? along : along > square ? along - square : 0;
        if (across * across + excess * excess > reach * reach * square)
            return false;
    }
    return true;
}

// Whether a cubic's tangent turns by no more than the angle whose cosine is `min_cosine` between its two ends (a value
// of -1 or less allows any turn).
bool cubic_turn_ok(const double (&c)[8], double min_cosine) noexcept {
    if (min_cosine <= -1)
        return true;
    // The start tangent is the first handle that is not on its end point, the end tangent likewise from the other side.
    double sx = 0, sy = 0, ex = 0, ey = 0;
    for (u32 k = 1; k <= 3; ++k)
        if (absolute(c[k * 2] - c[0]) + absolute(c[k * 2 + 1] - c[1]) > 1e-12) {
            sx = c[k * 2] - c[0];
            sy = c[k * 2 + 1] - c[1];
            break;
        }
    for (u32 k = 3; k-- > 0;)
        if (absolute(c[6] - c[k * 2]) + absolute(c[7] - c[k * 2 + 1]) > 1e-12) {
            ex = c[6] - c[k * 2];
            ey = c[7] - c[k * 2 + 1];
            break;
        }
    const double lengths = root(sx * sx + sy * sy) * root(ex * ex + ey * ey);
    return !(lengths > 1e-24) || sx * ex + sy * ey >= min_cosine * lengths;
}

// Makes room for `needed` elements of `size` bytes in an array of an arena, in place when it is the arena's last block.
// Arrays are aligned for elements of two doubles in a register.
bool grow(Arena* arena, void** data, u32* capacity, u32 count, u32 needed, u32 size) noexcept {
    if (needed <= *capacity)
        return true;
    u32 next = *capacity * 2;
    if (next < needed)
        next = needed;
    if (next < 8)
        next = 8;
    u8* const at = static_cast<u8*>(*data);
    u8* const end = at + usize{*capacity} * size;
    if (at && arena->base + arena->offset == end) {
        const ArenaMark mark = arena_mark(arena);
        void* more = arena_allocate(arena, usize{next - *capacity} * size, 16);
        if (more == end) {
            *capacity = next;
            return true;
        }
        if (more)
            arena_rewind(arena, mark);
    }
    void* fresh = arena_allocate(arena, usize{next} * size, 16);
    if (!fresh)
        return false;
    if (count)
        copy_bytes(fresh, at, count * size);
    *data = fresh;
    *capacity = next;
    return true;
}

bool reserve(Geometry* g, u32 points, u32 figures) noexcept {
    Arena* arena = &g->factory->permanent;
    return grow(arena, reinterpret_cast<void**>(&g->points), &g->point_capacity, g->point_count, points, sizeof(Point)) &&
           grow(arena, reinterpret_cast<void**>(&g->figures), &g->figure_capacity, g->figure_count, figures,
                sizeof(Figure));
}
} // namespace

bool vec_grow(Scratch& s, void** data, u32* capacity, u32 count, u32 needed, u32 size) noexcept {
    if (grow(s.arena, data, capacity, count, needed, size))
        return true;
    s.failed = true;
    return false;
}

bool flatten_cubic(Scratch& s, const double (&curve)[8], double tolerance, double min_cosine, Vec<Flat>& out) noexcept {
    // A stack of pieces still to cut, the next on top; a piece cut in two leaves its second half in place.
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
        Piece& piece = stack[top - 1];
        if (piece.depth >= 16 || top + 1 >= 48 || (cubic_flat(piece.c, tolerance) && cubic_turn_ok(piece.c, min_cosine))) {
            Flat* end = vec_push(s, out);
            if (!end)
                return false;
            *end = {piece.c[6], piece.c[7], static_cast<float>(piece.t1)};
            --top;
            continue;
        }
        Piece& first = stack[top++];
        double second[8];
        split_cubic(piece.c, .5, first.c, second);
        copy_bytes(piece.c, second, sizeof(second));
        first.t0 = piece.t0;
        first.t1 = piece.t0 = (piece.t0 + piece.t1) * .5;
        first.depth = ++piece.depth;
    }
    return true;
}

namespace {
// The factory released last, parked for the next factory_create (null while none is): each frame of a sticker would
// otherwise reserve its four arenas and fault their first pages in again, which costs a small document more than its
// geometry. Parked, its arenas keep at most their first kept_bytes committed and nothing it made.
Factory* g_parked = nullptr;
constexpr usize kept_bytes = 16 * 1024;

void destroy(Factory* factory) noexcept {
    Arena memory = factory->permanent;
    arena_release(&factory->scratch);
    arena_release(&factory->reused[0]);
    arena_release(&factory->reused[1]);
    arena_release(&memory);
}
} // namespace

bool factory_create(Factory** out) noexcept {
    if (Factory* parked = __atomic_exchange_n(&g_parked, static_cast<Factory*>(nullptr), __ATOMIC_ACQUIRE)) {
        *out = parked;
        return true;
    }
    Arena permanent{};
    if (!arena_initialize(&permanent, permanent_reserve, 0, 64 * 1024, ArenaTag::render))
        return false;
    auto* factory = arena_allocate_zero<Factory>(&permanent);
    if (!factory) {
        arena_release(&permanent);
        return false;
    }
    factory->permanent = permanent;
    if (!arena_initialize(&factory->scratch, scratch_reserve, 0, 64 * 1024, ArenaTag::render) ||
        !arena_initialize(&factory->reused[0], reused_reserve, 0, 64 * 1024, ArenaTag::render) ||
        !arena_initialize(&factory->reused[1], reused_reserve, 0, 64 * 1024, ArenaTag::render)) {
        destroy(factory);
        return false;
    }
    *out = factory;
    return true;
}

void factory_release(Factory* factory) noexcept {
    // Everything after the factory itself goes; the kept results with it.
    Arena* const arenas[]{&factory->permanent, &factory->scratch, &factory->reused[0], &factory->reused[1]};
    for (Arena* arena : arenas) {
        const usize start = arena == arenas[0] ? sizeof(Factory) : 0;
        arena_trim(arena, maximum(start, minimum(arena->offset, kept_bytes)));
        arena_rewind(arena, {start});
    }
    factory->reuse[0] = factory->reuse[1] = {};
    Factory* none = nullptr;
    if (!__atomic_compare_exchange_n(&g_parked, &none, factory, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        destroy(factory);
}

Geometry* geometry_create(Factory* factory) noexcept {
    auto* geometry = arena_allocate_zero<Geometry>(&factory->permanent);
    if (geometry)
        geometry->factory = factory;
    return geometry;
}

bool geometry_begin(Geometry* g, Point start, bool polyline) noexcept {
    if (!reserve(g, g->point_count + 1, g->figure_count + 1))
        return false;
    g->figures[g->figure_count++] = {g->point_count, g->point_count + 1, false, polyline};
    g->points[g->point_count++] = start;
    return true;
}

bool geometry_append(Geometry* g, const Point* points, u32 count) noexcept {
    if (!g->figure_count || !reserve(g, g->point_count + count, g->figure_count))
        return false;
    copy_bytes(g->points + g->point_count, points, count * static_cast<u32>(sizeof(Point)));
    g->point_count += count;
    g->figures[g->figure_count - 1].end = g->point_count;
    return true;
}

void geometry_end(Geometry* g, bool closed) noexcept {
    if (g->figure_count)
        g->figures[g->figure_count - 1].closed = closed;
}

Geometry* geometry_rounded_rectangle(Factory* factory, float l, float t, float r, float b, float rx, float ry) noexcept {
    rx = rx > 0 ? minimum(rx, (r - l) * .5f) : 0;
    ry = ry > 0 ? minimum(ry, (b - t) * .5f) : 0;
    Geometry* g = geometry_create(factory);
    Point p[24];
    u32 n = 0;
    const bool round = rx > 0 && ry > 0;
    if (!round) {
        p[n++] = {r, t};
        p[n++] = {r, b};
        p[n++] = {l, b};
    } else {
        // Clockwise from the top: before each corner the side (when the radii leave one) as a cubic with its handles
        // on its ends, then the corner from the direction of dx[k], dy[k] from its centre to that of dx[k + 1].
        static constexpr float dx[]{0, 1, 0, -1, 0}, dy[]{-1, 0, 1, 0, -1};
        const float kx = rx * kappa, ky = ry * kappa;
        Point at{l + rx, t};
        for (u32 k = 0; k < 4; ++k) {
            const float cx = k < 2 ? r - rx : l + rx, cy = k == 1 || k == 2 ? b - ry : t + ry;
            const Point start{cx + dx[k] * rx, cy + dy[k] * ry}, end{cx + dx[k + 1] * rx, cy + dy[k + 1] * ry};
            if (k & 1 ? ry * 2 < b - t : rx * 2 < r - l) {
                p[n++] = at;
                p[n++] = start;
                p[n++] = start;
            }
            p[n++] = {start.x + dx[k + 1] * kx, start.y + dy[k + 1] * ky};
            p[n++] = {end.x + dx[k] * kx, end.y + dy[k] * ky};
            p[n++] = at = end;
        }
    }
    if (!g || !geometry_begin(g, round ? Point{l + rx, t} : Point{l, t}, !round) || !geometry_append(g, p, n))
        return nullptr;
    geometry_end(g, true);
    return g;
}

Geometry* geometry_transform(const Geometry* source, const Matrix& m) noexcept {
    Geometry* g = geometry_create(source->factory);
    if (!g || !reserve(g, source->point_count, source->figure_count))
        return nullptr;
    for (u32 i = 0; i < source->point_count; ++i)
        g->points[i] = transform_point(&m, source->points[i]);
    copy_bytes(g->figures, source->figures, source->figure_count * static_cast<u32>(sizeof(Figure)));
    g->point_count = source->point_count;
    g->figure_count = source->figure_count;
    return g;
}
} // namespace maicord::geometry
