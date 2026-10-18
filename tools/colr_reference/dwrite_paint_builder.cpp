// Test/tool-only DirectWrite COLRv1 paint-program builder: the reference the
// native COLR compiler (src/render/colr_source.cpp) is compared against.
// Never linked into MaiCord.exe or MaiCordAccount.dll.
#include "dwrite_paint_builder.hpp"
#include "maicord/render/colr_paint.hpp"
#include "../font_reference/dwrite_text_system.hpp"
#include <d2d1.h> // Gradient-stop POD only; no Direct2D renderer or dependency.

namespace maicord {
namespace {
constexpr u32 none = ~0u;
constexpr DWRITE_MATRIX identity{1, 0, 0, 1, 0, 0};
struct Point {
    float x, y;
};
struct Segment {
    Point a, b;
};
// Borrows the font's immutable tables for this bake. Simple static TrueType
// contours skip the DWrite scaler and outline cache; composite, variable,
// simulated and non-TrueType outlines use the face's DirectWrite outline provider.
struct FontOutlines {
    IDWriteFontFace* face;
    const u8 *head = nullptr, *loca = nullptr, *glyf = nullptr;
    void* contexts[5]{};
    const u8* hmtx = nullptr;
    u32 hmtx_bytes = 0;
    u16 horizontal_metrics = 0;
    u32 loca_bytes = 0, glyf_bytes = 0;
    u16 units = 0, location_format = 0;
    static u16 be16(const u8* p) noexcept {
        return static_cast<u16>((p[0] << 8) | p[1]);
    }
    static u32 be32(const u8* p) noexcept {
        return (static_cast<u32>(be16(p)) << 16) | be16(p + 2);
    }
    explicit FontOutlines(IDWriteFontFace* f) noexcept : face(f) {
        if (face->GetType() != DWRITE_FONT_FACE_TYPE_TRUETYPE ||
            face->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE)
            return;
        const void* variation = nullptr;
        void* variation_context = nullptr;
        u32 length = 0;
        BOOL exists = FALSE;
        const HRESULT result = face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('g', 'v', 'a', 'r'),
                                                     &variation, &length, &variation_context, &exists);
        if (variation_context)
            face->ReleaseFontTable(variation_context);
        if (FAILED(result) || exists)
            return;
        const u32 tags[] = {DWRITE_MAKE_OPENTYPE_TAG('h', 'e', 'a', 'd'),
                            DWRITE_MAKE_OPENTYPE_TAG('l', 'o', 'c', 'a'),
                            DWRITE_MAKE_OPENTYPE_TAG('g', 'l', 'y', 'f')};
        const u8** tables[] = {&head, &loca, &glyf};
        u32 lengths[3]{};
        for (u32 i = 0; i < 3; ++i) {
            if (FAILED(face->TryGetFontTable(tags[i], reinterpret_cast<const void**>(tables[i]),
                                             &lengths[i], &contexts[i], &exists)) ||
                !exists)
                return;
        }
        if (lengths[0] < 54)
            return;
        location_format = be16(head + 50);
        if (location_format > 1)
            return;
        const u16 em = be16(head + 18);
        if (em < 16 || em > 16384)
            return;
        loca_bytes = lengths[1];
        glyf_bytes = lengths[2];
        if (!(be16(head + 16) & 2)) {
            const u8* hhea = nullptr;
            u32 hhea_bytes = 0;
            if (FAILED(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('h', 'h', 'e', 'a'),
                                             reinterpret_cast<const void**>(&hhea), &hhea_bytes,
                                             &contexts[3], &exists)) ||
                !exists || hhea_bytes < 36)
                return;
            horizontal_metrics = be16(hhea + 34);
            if (!horizontal_metrics)
                return;
            if (FAILED(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('h', 'm', 't', 'x'),
                                             reinterpret_cast<const void**>(&hmtx), &hmtx_bytes,
                                             &contexts[4], &exists)) ||
                !exists)
                return;
        }
        units = em;
    }
    ~FontOutlines() noexcept {
        for (auto* context : contexts)
            if (context)
                face->ReleaseFontTable(context);
    }
    bool simple(u16 glyph, GlyphOutline* out) const noexcept {
        if (!units)
            return false;
        const u32 stride = location_format ? 4 : 2, index = glyph * stride;
        if (index + 2 * stride > loca_bytes)
            return false;
        const u32 begin = location_format ? be32(loca + index) : be16(loca + index) * 2;
        const u32 end = location_format ? be32(loca + index + stride) : be16(loca + index + stride) * 2;
        if (begin > end || end > glyf_bytes)
            return false;
        glyph_outline_reset(out);
        out->fill_rule = GlyphFillRule::winding;
        if (begin == end)
            return true;
        const u8* data = glyf + begin;
        const u32 bytes = end - begin;
        if (bytes < 10)
            return false;
        const i16 contours = static_cast<i16>(be16(data));
        if (contours < 0 || contours > 2048 || bytes < 12u + static_cast<u32>(contours) * 2)
            return false;
        if (!contours)
            return true;
        const u32 points_count = be16(data + 10 + (contours - 1) * 2) + 1;
        if (points_count > 2048)
            return false;
        u32 at = 12 + contours * 2 + be16(data + 10 + contours * 2);
        if (at > bytes)
            return false;
        u8 flags[2048];
        Point points[2048];
        for (u32 i = 0; i < points_count;) {
            if (at >= bytes)
                return false;
            const u8 flag = data[at++];
            u32 repeat = 1;
            if (flag & 8) {
                if (at >= bytes)
                    return false;
                repeat += data[at++];
            }
            if (repeat > points_count - i)
                return false;
            while (repeat--)
                flags[i++] = flag;
        }
        i32 x_origin = 0;
        if (hmtx) {
            const u32 bearing = glyph < horizontal_metrics
                                    ? glyph * 4 + 2
                                    : horizontal_metrics * 4 + (glyph - horizontal_metrics) * 2;
            if (bearing + 2 > hmtx_bytes)
                return false;
            x_origin = static_cast<i16>(be16(hmtx + bearing)) - static_cast<i16>(be16(data + 2));
        }
        const float scale = 1.f / units;
        for (u32 axis = 0; axis < 2; ++axis) {
            i32 coordinate = axis ? 0 : x_origin;
            const u8 short_flag = axis ? 4 : 2, same_flag = axis ? 32 : 16;
            for (u32 i = 0; i < points_count; ++i) {
                i32 delta = 0;
                const u8 flag = flags[i];
                if (flag & short_flag) {
                    if (at >= bytes)
                        return false;
                    delta = data[at++];
                    if (!(flag & same_flag))
                        delta = -delta;
                } else if (!(flag & same_flag)) {
                    if (at + 2 > bytes)
                        return false;
                    delta = static_cast<i16>(be16(data + at));
                    at += 2;
                }
                coordinate += delta;
                if (axis)
                    points[i].y = -coordinate * scale;
                else
                    points[i].x = coordinate * scale;
            }
        }
        auto append = [&](GlyphPathCommandKind kind, Point a = Point{}, Point b = Point{},
                          Point c = Point{}) noexcept {
            if (out->count >= out->capacity)
                return false;
            out->commands[out->count++] = {a.x, a.y, b.x, b.y, c.x, c.y, kind, {0, 0, 0}};
            return true;
        };
        auto middle = [](Point a, Point b) noexcept { return Point{(a.x + b.x) * .5f, (a.y + b.y) * .5f}; };
        u32 first = 0;
        for (i32 contour = 0; contour < contours; ++contour) {
            const u32 last = be16(data + 10 + contour * 2);
            if (last < first || last >= points_count)
                return false;
            Point start;
            u32 i = first, limit = last + 1;
            if (flags[first] & 1) {
                start = points[first];
                ++i;
            } else if (flags[last] & 1) {
                start = points[last];
                --limit;
            } else
                start = middle(points[last], points[first]);
            if (!append(GlyphPathCommandKind::begin_filled, start))
                return false;
            Point current = start;
            while (i < limit) {
                const Point point = points[i];
                if (flags[i] & 1) {
                    if (!append(GlyphPathCommandKind::line, point))
                        return false;
                    current = point;
                    ++i;
                } else {
                    Point next = start;
                    bool next_on = true;
                    if (i + 1 < limit) {
                        next = points[i + 1];
                        next_on = (flags[i + 1] & 1) != 0;
                    }
                    const Point target = next_on ? next : middle(point, next);
                    // Preserve DWrite's quadratic-to-cubic rounding: it uses
                    // endpoint * third + control * (1 - third), after scaling.
                    constexpr float third = 1.f / 3.f, two_thirds = 1.f - third;
                    const Point a{current.x * third + point.x * two_thirds,
                                  current.y * third + point.y * two_thirds};
                    const Point b{target.x * third + point.x * two_thirds,
                                  target.y * third + point.y * two_thirds};
                    if (!append(GlyphPathCommandKind::cubic, a, b, target))
                        return false;
                    current = target;
                    i += next_on ? 2 : 1;
                }
            }
            if (!append(GlyphPathCommandKind::end_closed))
                return false;
            first = last + 1;
        }
        return true;
    }
};
struct MaskRow {
    u32 offset, count;
};
struct Builder {
    ColrPaintProgram* p;
    IDWritePaintReader* reader;
    IDWriteFontFace* face;
    float scale;
    const FontOutlines* outlines;
    u32 mask_buckets[256]{};
};
Point transform(DWRITE_MATRIX m, Point p) noexcept {
    return {p.x * m.m11 + p.y * m.m21 + m.dx, p.x * m.m12 + p.y * m.m22 + m.dy};
}
DWRITE_MATRIX multiply(DWRITE_MATRIX a, DWRITE_MATRIX b) noexcept {
    const auto offset = transform(b, {a.dx, a.dy});
    return {a.m11 * b.m11 + a.m12 * b.m21,
            a.m11 * b.m12 + a.m12 * b.m22,
            a.m21 * b.m11 + a.m22 * b.m21,
            a.m21 * b.m12 + a.m22 * b.m22,
            offset.x,
            offset.y};
}
bool inverse(DWRITE_MATRIX m, float* output) noexcept {
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
ColrPaintNode* emit(Builder& b, u32 kind, DWRITE_MATRIX m = identity) noexcept {
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
    if (!inverse(m, n->inverse))
        n->mode = ~0u; // Singular transforms paint nothing.
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
bool line(Builder& b, Point a, Point c) noexcept {
    auto* segment = arena_allocate<Segment>(&b.p->storage);
    if (!segment)
        return false;
    *segment = {a, c};
    return true;
}
Point midpoint(Point a, Point b) noexcept {
    return {(a.x + b.x) * .5f, (a.y + b.y) * .5f};
}
float absf(float x) noexcept {
    return x < 0 ? -x : x;
}
bool curve(Builder& b, Point a, Point c1, Point c2, Point z, u32 depth = 0) noexcept {
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
            // A leaf paint clipped by a shape is one operation, so pixels
            // outside the shape skip gradient evaluation.
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
bool mask(Builder& b, u32 glyph, DWRITE_MATRIX m) noexcept {
    // Artwork often fills the same shape repeatedly: reuse one mask per bake
    // (no cross-frame cache) to avoid repeated DWrite calls.
    for (u32 at = b.mask_buckets[glyph & 255]; at;) {
        const auto* job = reinterpret_cast<const ColrMask*>(b.p->storage.base + at);
        const auto& a = job->transform;
        if (job->glyph == glyph && a.m11 == m.m11 && a.m12 == m.m12 && a.m21 == m.m21 && a.m22 == m.m22 &&
            a.dx == m.dx && a.dy == m.dy) {
            return emit_mask(b, at);
        }
        at = job->cache_next;
    }
    GlyphPathCommand commands[2048];
    GlyphOutline outline{};
    if (glyph > 65535 || !glyph_outline_initialize(&outline, {commands, 2048}) ||
        (!b.outlines->simple(static_cast<u16>(glyph), &outline) &&
         !text_outline_glyph(b.face, static_cast<u16>(glyph), &outline).ok()))
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
    job->transform = {m.m11,m.m12,m.m21,m.m22,m.dx,m.dy};
    job->fill = outline.fill_rule == GlyphFillRule::winding ? 1u : 0u;
    job->segments = static_cast<u32>(b.p->storage.offset);
    Point current{}, start{};
    bool filled = false;
    GlyphOutline bounds{};
    for (u32 i = 0; i < outline.count; ++i) {
        const auto& c = commands[i];
        const u32 kind = static_cast<u32>(c.kind);
        const auto p1 = transform(m, {c.x1, c.y1}), p2 = transform(m, {c.x2, c.y2}),
                   p3 = transform(m, {c.x3, c.y3});
        if (kind == 0 || kind == 1) {
            current = start = p1;
            filled = kind == 0;
        } else if (kind == 2) {
            if (filled && !line(b, current, p1))
                return false;
            current = p1;
        } else if (kind == 3) {
            if (filled && !curve(b, current, p1, p2, p3))
                return false;
            current = p3;
        } else if (kind == 5) {
            if (filled && !line(b, current, start))
                return false;
            current = start;
        }
    }
    job->segment_count = static_cast<u32>((b.p->storage.offset - job->segments) / sizeof(Segment));
    const auto* segments = reinterpret_cast<const Segment*>(b.p->storage.base + job->segments);
    for (u32 i = 0; i < job->segment_count; ++i) {
        bounds_point(bounds, segments[i].a);
        bounds_point(bounds, segments[i].b);
    }
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
    u64 references = 0;
    for (u32 i = 0; i < job->segment_count; ++i) {
        const auto& e = segments[i];
        const i32 first = maximum(0, static_cast<i32>(minimum(e.a.y, e.b.y) * b.scale - job->top - 1.5f));
        const i32 last = minimum(static_cast<i32>(job->height) - 1,
                                 static_cast<i32>(maximum(e.a.y, e.b.y) * b.scale - job->top + 1.5f));
        for (i32 y = first; y <= last; ++y) {
            ++rows[y].count;
            ++references;
        }
    }
    if (references > b.p->storage.reserved / sizeof(u32))
        return false;
    auto* indices = arena_allocate<u32>(&b.p->storage, references ? static_cast<u32>(references) : 1);
    if (!indices)
        return false;
    const u32 first_index = static_cast<u32>(reinterpret_cast<u8*>(indices) - b.p->storage.base);
    u32 cursor = 0;
    for (u32 y = 0; y < job->height; ++y) {
        const u32 count = rows[y].count;
        rows[y] = {first_index + cursor * 4, 0};
        cursor += count;
    }
    for (u32 i = 0; i < job->segment_count; ++i) {
        const auto& e = segments[i];
        const i32 first = maximum(0, static_cast<i32>(minimum(e.a.y, e.b.y) * b.scale - job->top - 1.5f));
        const i32 last = minimum(static_cast<i32>(job->height) - 1,
                                 static_cast<i32>(maximum(e.a.y, e.b.y) * b.scale - job->top + 1.5f));
        for (i32 y = first; y <= last; ++y)
            reinterpret_cast<u32*>(b.p->storage.base + rows[y].offset)[rows[y].count++] = i;
    }
    return true;
}

bool clip(Builder& b, D2D_RECT_F r, DWRITE_MATRIX m) noexcept {
    if (r.right <= r.left || r.bottom <= r.top)
        return true;
    auto* n = emit(b, 5, m);
    if (!n)
        return false;
    n->values[0] = r.left;
    n->values[1] = r.top;
    n->values[2] = r.right;
    n->values[3] = r.bottom;
    return true;
}
bool tree(Builder& b, const DWRITE_PAINT_ELEMENT& e, DWRITE_MATRIX m, u32 depth) noexcept;
bool children(Builder& b, u32 count, DWRITE_MATRIX m, u32 depth, bool layers) noexcept {
    DWRITE_PAINT_ELEMENT child{};
    if (!count)
        return emit(b, 7) != nullptr;
    if (FAILED(b.reader->MoveToFirstChild(&child)))
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
        if (i + 1 < count && FAILED(b.reader->MoveToNextSibling(&child)))
            return false;
    }
    return SUCCEEDED(b.reader->MoveToParent());
}
bool tree(Builder& b, const DWRITE_PAINT_ELEMENT& e, DWRITE_MATRIX m, u32 depth) noexcept {
    if (depth > 64)
        return false;
    b.p->paint_types |= 1u << static_cast<u32>(e.paintType);
    const auto& p = e.paint;
    switch (e.paintType) {
    case DWRITE_PAINT_TYPE_NONE:
        return emit(b, 7) != nullptr;
    case DWRITE_PAINT_TYPE_LAYERS:
        return children(b, p.layers.childCount, m, depth, true);
    case DWRITE_PAINT_TYPE_TRANSFORM: {
        const auto transformed = multiply(p.transform, m);
        float invert[6];
        if (!inverse(transformed, invert))
            return emit(b, 7) != nullptr;
        return children(b, 1, transformed, depth, false);
    }
    case DWRITE_PAINT_TYPE_GLYPH:
        return children(b, 1, m, depth, false) && mask(b, p.glyph.glyphIndex, m);
    case DWRITE_PAINT_TYPE_COLOR_GLYPH:
        return children(b, 1, m, depth, false) && clip(b, p.colorGlyph.clipBox, m);
    case DWRITE_PAINT_TYPE_COMPOSITE: {
        if (!children(b, 2, m, depth, false))
            return false;
        auto* n = emit(b, 6);
        if (!n)
            return false;
        n->mode = static_cast<u32>(p.composite.mode);
        return true;
    }
    case DWRITE_PAINT_TYPE_SOLID_GLYPH:
    case DWRITE_PAINT_TYPE_SOLID: {
        auto* n = emit(b, 0, m);
        if (!n)
            return false;
        const auto c = e.paintType == DWRITE_PAINT_TYPE_SOLID ? p.solid.value : p.solidGlyph.color.value;
        n->values[0] = c.r;
        n->values[1] = c.g;
        n->values[2] = c.b;
        n->values[3] = c.a;
        return e.paintType == DWRITE_PAINT_TYPE_SOLID || mask(b, p.solidGlyph.glyphIndex, m);
    }
    case DWRITE_PAINT_TYPE_LINEAR_GRADIENT:
    case DWRITE_PAINT_TYPE_RADIAL_GRADIENT:
    case DWRITE_PAINT_TYPE_SWEEP_GRADIENT: {
        auto* n = emit(b, static_cast<u32>(e.paintType) - 3, m);
        if (!n)
            return false;
        if (e.paintType == DWRITE_PAINT_TYPE_LINEAR_GRADIENT) {
            const auto& g = p.linearGradient;
            n->mode = g.extendMode;
            n->count = g.gradientStopCount;
            n->values[0] = g.x0;
            n->values[1] = g.y0;
            n->values[2] = g.x1;
            n->values[3] = g.y1;
            n->values[4] = g.x2;
            n->values[5] = g.y2;
        } else if (e.paintType == DWRITE_PAINT_TYPE_RADIAL_GRADIENT) {
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
        auto* stops = arena_allocate<D2D1_GRADIENT_STOP>(&b.p->storage, n->count);
        if (!stops)
            return false;
        n->data = static_cast<u32>(reinterpret_cast<u8*>(stops) - b.p->storage.base);
        return SUCCEEDED(b.reader->GetGradientStops(0, n->count, stops));
    }
    default:
        return false;
    }
}
} // namespace

bool colr_paint_build(ColrPaintProgram* p, IDWritePaintReader* reader, IDWriteFontFace* face,
                      const DWRITE_PAINT_ELEMENT& root, D2D_RECT_F bounds, float scale) noexcept {
    if (!p || !reader || !face || !(scale > 0))
        return false;
    if (!p->storage.base && !arena_initialize(&p->storage, 8 * 1024 * 1024, 0, 4096))
        return false;
    arena_reset(&p->storage);
    p->first = p->last = none;
    p->nodes = p->stack = p->peak_stack = p->paint_types = 0;
    p->bounds = {};
    p->mask_count = p->mask_pixels = 0;
    FontOutlines outlines(face);
    Builder b{p, reader, face, scale, &outlines};
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
