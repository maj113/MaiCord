#include "maicord/render/colr_program.hpp"
#include "maicord/render/glyph_raster.hpp"
#include "colr_segment_bounds.hpp"

namespace maicord {
namespace {
constexpr u32 none=~0u;
constexpr ColrMatrix identity{1,0,0,1,0,0};
struct Point { float x,y; };
struct Segment { Point a,b; };
static_assert(sizeof(Segment)==16 && alignof(Segment)==4);
struct MaskRow {
    u32 offset, count;
};
struct Builder {
    ColrPaintProgram* p;
    const ColrPaintSource* source;
    float scale;
    u32 mask_buckets[256]{};
};
Point transform(const ColrMatrix& m, Point p) noexcept {
    return {p.x * m.m11 + p.y * m.m21 + m.dx, p.x * m.m12 + p.y * m.m22 + m.dy};
}
ColrMatrix multiply(ColrMatrix a, ColrMatrix b) noexcept {
    const auto offset = transform(b, {a.dx, a.dy});
    return {a.m11 * b.m11 + a.m12 * b.m21,
            a.m11 * b.m12 + a.m12 * b.m22,
            a.m21 * b.m11 + a.m22 * b.m21,
            a.m21 * b.m12 + a.m22 * b.m22,
            offset.x,
            offset.y};
}
bool inverse(const ColrMatrix& m, float* output) noexcept {
    const float det = m.m11 * m.m22 - m.m12 * m.m21;
    if (!(det > 1e-12f || det < -1e-12f))
        return false;
    output[0] = m.m22 / det;
    output[1] = -m.m12 / det;
    output[2] = -m.m21 / det;
    output[3] = m.m11 / det;
    output[4] = -(m.dx * output[0] + m.dy * output[2]);
    output[5] = -(m.dx * output[1] + m.dy * output[3]);
    return true;
}
ColrPaintNode* emit(Builder& b, u32 kind, const ColrMatrix* m = nullptr) noexcept {
    if (b.p->nodes >= 8192)
        return nullptr;
    auto* n = arena_allocate_zero<ColrPaintNode>(&b.p->storage);
    if (!n)
        return nullptr;
    const u32 offset = static_cast<u32>(reinterpret_cast<u8*>(n) - b.p->storage.base);
    if (b.p->last != none)
        reinterpret_cast<ColrPaintNode*>(b.p->storage.base + b.p->last)->next = offset;
    else
        b.p->first = offset;
    b.p->last = offset;
    ++b.p->nodes;
    n->next = none;
    n->kind = kind;
    if (m) {
        if (!inverse(*m, n->inverse))
            n->mode = ~0u; // Singular transforms paint nothing.
    } else {
        // Preserve the exact identity inverse, including its signed zeros.
        constexpr float unit_inverse[6]{1, -0.f, -0.f, 1, 0, 0};
        copy_bytes(n->inverse, unit_inverse, sizeof(unit_inverse));
    }
    if (kind <= 3 || kind == 7)
        ++b.p->stack;
    if (kind == 6) {
        if (b.p->stack < 2)
            return nullptr;
        --b.p->stack;
    }
    b.p->peak_stack = maximum(b.p->peak_stack, b.p->stack);
    return b.p->stack <= 64 ? n : nullptr;
}
void bounds_point(GlyphOutline& r, Point p) noexcept {
    if (!r.has_bounds) {
        r.minimum_x = r.maximum_x = p.x;
        r.minimum_y = r.maximum_y = p.y;
        r.has_bounds = true;
    } else {
        r.minimum_x = minimum(r.minimum_x, p.x);
        r.minimum_y = minimum(r.minimum_y, p.y);
        r.maximum_x = maximum(r.maximum_x, p.x);
        r.maximum_y = maximum(r.maximum_y, p.y);
    }
}
struct SegmentWriter {
    Arena& storage;
    u8* next;
    u8* last;
    float scale;
    ~SegmentWriter() noexcept {
        storage.offset=static_cast<usize>(next-storage.base);
        storage.peak=maximum(storage.peak,storage.offset);
    }
};
bool line(SegmentWriter& writer, Point a, Point c) noexcept {
    auto& storage=writer.storage;Segment* segment;
    // All program records and segment sizes preserve four-byte alignment.
    // During this scope no callback or other record allocator can intervene.
    if(writer.next<=writer.last) {
        segment=reinterpret_cast<Segment*>(writer.next);
        writer.next+=sizeof(Segment);
    } else {
        storage.offset=static_cast<usize>(writer.next-storage.base);
        segment=arena_allocate<Segment>(&storage);
        if(!segment)return false;
        writer.next=storage.base+storage.offset;
        writer.last=storage.base+storage.committed-sizeof(Segment);
    }
    *segment={a,c};return true;
}
Point midpoint(Point a, Point b) noexcept {
    return {(a.x + b.x) * .5f, (a.y + b.y) * .5f};
}
float absf(float x) noexcept {
    return x < 0 ? -x : x;
}
bool curve(SegmentWriter& b, Point a, Point c1, Point c2, Point z, u32 depth = 0) noexcept {
    // Flatten in device space, so scale/shear/DPI cannot magnify curve error.
    const float dx = z.x - a.x, dy = z.y - a.y;
    const float error =
        absf((c1.x - z.x) * dy - (c1.y - z.y) * dx) + absf((c2.x - z.x) * dy - (c2.y - z.y) * dx);
    const float extent =
        maximum(maximum(absf(c1.x - a.x), absf(c1.y - a.y)),
                maximum(maximum(absf(c2.x - a.x), absf(c2.y - a.y)), maximum(absf(dx), absf(dy))));
    if (depth == 12 || extent * b.scale <= .125f ||
        (error * error * b.scale * b.scale <= .015625f * (dx * dx + dy * dy) &&
         (dx * dx + dy * dy > 1e-16f)))
        return line(b, a, z);
    const auto ab = midpoint(a, c1), bc = midpoint(c1, c2), cd = midpoint(c2, z);
    const auto abc = midpoint(ab, bc), bcd = midpoint(bc, cd), center = midpoint(abc, bcd);
    return curve(b, a, ab, abc, center, depth + 1) && curve(b, center, bcd, cd, z, depth + 1);
}
bool emit_mask(Builder& b, u32 job) noexcept {
    if (b.p->last != none) {
        auto* paint = reinterpret_cast<ColrPaintNode*>(b.p->storage.base + b.p->last);
        if (paint->kind <= 3) {
            // A leaf paint clipped by a shape is one operation. Most pixels
            // miss that shape and need no gradient evaluation at all.
            paint->kind += 8;
            paint->reserved = job;
            return true;
        }
    }
    auto* node = emit(b, 4);
    if (!node)
        return false;
    node->data = job;
    return true;
}
// Keep the full-capacity outline frame behind the mask-cache miss.
__declspec(noinline) bool build_mask(Builder& b, u32 glyph, const ColrMatrix& m) noexcept {
    const u32 bound=glyph<=65535&&b.source->outline_capacity?b.source->outline_capacity(b.source->context,static_cast<u16>(glyph)):0;
    const u32 capacity=bound?minimum<u32>(bound,2048):2048;
    auto* commands=static_cast<GlyphPathCommand*>(_alloca(static_cast<usize>(capacity)*sizeof(GlyphPathCommand)));
    GlyphOutline outline{commands,capacity};
    if (glyph > 65535 || !b.source->outline(b.source->context,static_cast<u16>(glyph),&outline))
        return false;
    auto* job = arena_allocate_zero<ColrMask>(&b.p->storage);
    if (!job)
        return false;
    const u32 at = static_cast<u32>(reinterpret_cast<u8*>(job) - b.p->storage.base);
    if (!emit_mask(b, at))
        return false;
    ++b.p->mask_count;
    job->cache_next = b.mask_buckets[glyph & 255];
    b.mask_buckets[glyph & 255] = at;
    job->glyph = glyph;
    job->transform = m;
    job->fill = outline.fill_rule == GlyphFillRule::winding ? 1u : 0u;
    job->segments = static_cast<u32>(b.p->storage.offset);
    Point current{}, start{};
    bool filled = false;
    GlyphOutline bounds{};
    {
        SegmentWriter writer{b.p->storage,b.p->storage.base+b.p->storage.offset,
            b.p->storage.base+b.p->storage.committed-sizeof(Segment),b.scale};
        const u8* figure = nullptr;
        for (u32 i = 0; i < outline.count; ++i) {
            const auto& c = commands[i];
            const u32 kind = static_cast<u32>(c.kind);
            // Close/end commands use the figure start, never their unused points.
            if (kind > 3) {
                if (kind == 5) {
                    if (filled && (writer.next == figure || current.x != start.x || current.y != start.y) &&
                        !line(writer, current, start))
                        return false;
                    current = start;
                }
                continue;
            }
            const auto p1 = transform(m, {c.x1, c.y1});
            if (kind < 2) {
                current = start = p1;
                filled = kind == 0;
                figure = writer.next;
            } else if (kind == 2) {
                if (filled && !line(writer, current, p1))
                    return false;
                current = p1;
            } else {
                const auto p2 = transform(m, {c.x2, c.y2}), p3 = transform(m, {c.x3, c.y3});
                if (filled && !curve(writer, current, p1, p2, p3))
                    return false;
                current = p3;
            }
        }
    }
    job->segment_count = static_cast<u32>((b.p->storage.offset - job->segments) / sizeof(Segment));
    const auto* segments = reinterpret_cast<const Segment*>(b.p->storage.base + job->segments);
    colr_detail::segment_bounds(segments, job->segment_count, bounds);
    if (!bounds.has_bounds)
        return true;
    job->bounds[0] = bounds.minimum_x;
    job->bounds[1] = bounds.minimum_y;
    job->bounds[2] = bounds.maximum_x;
    job->bounds[3] = bounds.maximum_y;
    bounds_point(b.p->bounds, {bounds.minimum_x, bounds.minimum_y});
    bounds_point(b.p->bounds, {bounds.maximum_x, bounds.maximum_y});
    GlyphRasterBounds rect{};
    if (!glyph_raster_bounds(bounds.minimum_x, bounds.minimum_y, bounds.maximum_x, bounds.maximum_y,
                             b.scale, 1, &rect))
        return false;
    job->left = rect.left;
    job->top = rect.top;
    job->width = rect.width;
    job->height = rect.height;
    const u64 pixels = static_cast<u64>(b.p->mask_pixels) + static_cast<u64>(job->width) * job->height;
    if (pixels > 0x3fffffffu)
        return false;
    b.p->mask_pixels = static_cast<u32>(pixels);
    auto* rows = arena_allocate_zero<MaskRow>(&b.p->storage, job->height);
    if (!rows)
        return false;
    job->rows = static_cast<u32>(reinterpret_cast<u8*>(rows) - b.p->storage.base);
    // Bin edges by scanline, including an AA gutter. Edges further away cannot
    // affect either winding or the saturated half-pixel distance coverage.
    for (u32 i = 0; i < job->segment_count; ++i) {
        const auto& e = segments[i];
        const i32 first = maximum(0, static_cast<i32>(minimum(e.a.y, e.b.y) * b.scale - job->top - 1.5f));
        const i32 last = minimum(static_cast<i32>(job->height) - 1,
                                 static_cast<i32>(maximum(e.a.y, e.b.y) * b.scale - job->top + 1.5f));
        if (first <= last) {
            // Until offsets are assigned, the two zeroed row words hold
            // interval starts and ends. Count each edge with two updates.
            ++rows[first].count;
            if (static_cast<u32>(last) + 1 < job->height)
                ++rows[last + 1].offset;
        }
    }
    u32 references = 0, count = 0;
    for (u32 y = 0; y < job->height; ++y) {
        const bool shared = y && !rows[y].count && !rows[y].offset;
        count += rows[y].count;
        count -= rows[y].offset;
        if (shared) rows[y] = {rows[y-1].offset, none};
        else {
            rows[y] = {references * 4, 0};
            if (count > b.p->storage.reserved / sizeof(u32) - references)
                return false;
            references += count;
        }
    }
    auto* indices = arena_allocate<u32>(&b.p->storage, references ? references : 1);
    if (!indices)
        return false;
    const u32 first_index = static_cast<u32>(reinterpret_cast<u8*>(indices) - b.p->storage.base);
    for (u32 y = 0; y < job->height; ++y) rows[y].offset += first_index;
    for (u32 i = 0; i < job->segment_count; ++i) {
        const auto& e = segments[i];
        const i32 first = maximum(0, static_cast<i32>(minimum(e.a.y, e.b.y) * b.scale - job->top - 1.5f));
        const i32 last = minimum(static_cast<i32>(job->height) - 1,
                                 static_cast<i32>(maximum(e.a.y, e.b.y) * b.scale - job->top + 1.5f));
        for (i32 y = first; y <= last; ++y)
            if (rows[y].count != none)
                reinterpret_cast<u32*>(b.p->storage.base + rows[y].offset)[rows[y].count++] = i;
    }
    for (u32 y = 1; y < job->height; ++y)
        if (rows[y].count == none) rows[y].count = rows[y-1].count;
    return true;
}

bool mask(Builder& b, u32 glyph, const ColrMatrix& m) noexcept {
    // Common COLRv1 artwork fills the same shape many times. Retain one mask
    // within this bake, not another cross-frame cache, and avoid repeated outline decoding.
    for (u32 at = b.mask_buckets[glyph & 255]; at;) {
        const auto* job = reinterpret_cast<const ColrMask*>(b.p->storage.base + at);
        const auto& a = job->transform;
        if (job->glyph == glyph && a.m11 == m.m11 && a.m12 == m.m12 && a.m21 == m.m21 && a.m22 == m.m22 &&
            a.dx == m.dx && a.dy == m.dy) {
            return emit_mask(b, at);
        }
        at = job->cache_next;
    }
    return build_mask(b, glyph, m);
}

bool clip(Builder& b, ColrRect r, ColrMatrix m) noexcept {
    if (r.right <= r.left || r.bottom <= r.top)
        return true;
    auto* n = emit(b, 5, &m);
    if (!n)
        return false;
    n->values[0] = r.left;
    n->values[1] = r.top;
    n->values[2] = r.right;
    n->values[3] = r.bottom;
    return true;
}
bool tree(Builder& b, const ColrPaintElement& e, ColrMatrix m, u32 depth) noexcept;
bool children(Builder& b, u32 count, ColrMatrix m, u32 depth, bool layers) noexcept {
    ColrPaintElement child{};
    if (!count)
        return emit(b, 7) != nullptr;
    if (!b.source->first_child(b.source->context,&child))
        return false;
    for (u32 i = 0; i < count; ++i) {
        if (!tree(b, child, m, depth + 1))
            return false;
        if (layers && i) {
            auto* n = emit(b, 6);
            if (!n)
                return false;
            n->mode = 3;
            n->count = 1;
        }
        if (i + 1 < count && !b.source->next_sibling(b.source->context,&child))
            return false;
    }
    return b.source->parent(b.source->context);
}
bool tree(Builder& b, const ColrPaintElement& e, ColrMatrix m, u32 depth) noexcept {
    if (depth > 64)
        return false;
    b.p->paint_types |= 1u << static_cast<u32>(e.kind);
    const auto& p = e.paint;
    switch (e.kind) {
    case ColrPaintKind::none:
        return emit(b, 7) != nullptr;
    case ColrPaintKind::layers:
        return children(b, p.layers.childCount, m, depth, true);
    case ColrPaintKind::transform: {
        const auto transformed = multiply(p.transform, m);
        // Traversal only needs the singularity decision, not six inverse values.
        const float det = transformed.m11 * transformed.m22 - transformed.m12 * transformed.m21;
        if (!(det > 1e-12f || det < -1e-12f))
            return emit(b, 7) != nullptr;
        return children(b, 1, transformed, depth, false);
    }
    case ColrPaintKind::glyph:
        return children(b, 1, m, depth, false) && mask(b, p.glyph.glyphIndex, m);
    case ColrPaintKind::color_glyph:
        return children(b, 1, m, depth, false) && clip(b, p.colorGlyph.clipBox, m);
    case ColrPaintKind::composite: {
        b.p->general_paints |= p.composite.mode != 3u;
        if (!children(b, 2, m, depth, false))
            return false;
        auto* n = emit(b, 6);
        if (!n)
            return false;
        n->mode = static_cast<u32>(p.composite.mode);
        return true;
    }
    case ColrPaintKind::solid_glyph:
    case ColrPaintKind::solid: {
        auto* n = emit(b, 0, &m);
        if (!n)
            return false;
        const auto c = e.kind == ColrPaintKind::solid ? p.solid.value : p.solidGlyph.color.value;
        n->values[0] = c.r;
        n->values[1] = c.g;
        n->values[2] = c.b;
        n->values[3] = c.a;
        return e.kind == ColrPaintKind::solid || mask(b, p.solidGlyph.glyphIndex, m);
    }
    case ColrPaintKind::linear_gradient:
    case ColrPaintKind::radial_gradient:
    case ColrPaintKind::sweep_gradient: {
        b.p->general_paints |= e.kind == ColrPaintKind::sweep_gradient;
        auto* n = emit(b, static_cast<u32>(e.kind) - 3, &m);
        if (!n)
            return false;
        if (e.kind == ColrPaintKind::linear_gradient) {
            const auto& g = p.linearGradient;
            n->mode = g.extendMode;
            n->count = g.gradientStopCount;
            n->values[0] = g.x0;
            n->values[1] = g.y0;
            n->values[2] = g.x1;
            n->values[3] = g.y1;
            n->values[4] = g.x2;
            n->values[5] = g.y2;
        } else if (e.kind == ColrPaintKind::radial_gradient) {
            const auto& g = p.radialGradient;
            n->mode = g.extendMode;
            n->count = g.gradientStopCount;
            n->values[0] = g.x0;
            n->values[1] = g.y0;
            n->values[2] = g.x1;
            n->values[3] = g.y1;
            n->values[4] = g.radius0;
            n->values[5] = g.radius1;
        } else {
            const auto& g = p.sweepGradient;
            n->mode = g.extendMode;
            n->count = g.gradientStopCount;
            n->values[0] = g.centerX;
            n->values[1] = g.centerY;
            n->values[2] = g.startAngle;
            n->values[3] = g.endAngle;
        }
        if (!n->count || n->count > 65536)
            return false;
        auto* stops = arena_allocate<ColrGradientStop>(&b.p->storage, n->count);
        if (!stops)
            return false;
        n->data = static_cast<u32>(reinterpret_cast<u8*>(stops) - b.p->storage.base);
        return b.source->stops(b.source->context,{stops,n->count});
    }
    default:
        return false;
    }
}
} // namespace

bool colr_program_build(ColrPaintProgram* p, const ColrPaintSource& source,
                      const ColrPaintElement& root, ColrRect bounds, float scale) noexcept {
    if (!p || !source.first_child || !source.next_sibling || !source.parent || !source.stops || !source.outline || !(scale > 0))
        return false;
    if (!p->storage.base && !arena_initialize(&p->storage, 8 * 1024 * 1024, 0, 4096))
        return false;
    arena_reset(&p->storage);
    p->first = p->last = none;
    p->nodes = p->stack = p->peak_stack = p->paint_types = 0;
    p->general_paints = false;
    p->bounds = {};
    p->mask_count = p->mask_pixels = 0;
    Builder b{p, &source, scale};
    if (!tree(b, root, identity, 0) || !clip(b, bounds, identity) || p->stack != 1)
        return false;
    if (bounds.right > bounds.left && bounds.bottom > bounds.top) {
        p->bounds.minimum_x = bounds.left;
        p->bounds.minimum_y = bounds.top;
        p->bounds.maximum_x = bounds.right;
        p->bounds.maximum_y = bounds.bottom;
        p->bounds.has_bounds = true;
    }
    p->bounds.count = 1;
    return true;
}

} // namespace maicord
