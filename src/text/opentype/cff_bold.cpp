#include "cff_internal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {

#pragma float_control(precise, on, push)
#pragma clang fp contract(off)
namespace {
// A point of an outline, in em units with Y up.
struct BoldPoint {double x, y;};
// The helpers that run for each point are inline: the build inlines only functions marked inline (/Ob1), and their
// calls took a third of the bold time.
inline bool same(BoldPoint a, BoldPoint b) noexcept { return a.x == b.x && a.y == b.y; }
inline u32 point_count(GlyphPathCommandKind kind) noexcept {
    return kind == GlyphPathCommandKind::cubic ? 3u :
           kind == GlyphPathCommandKind::end_open || kind == GlyphPathCommandKind::end_closed ? 0u : 1u;
}
inline BoldPoint load(const GlyphPathCommand& c, u32 slot) noexcept {
    const float x = slot == 0 ? c.x1 : slot == 1 ? c.x2 : c.x3, y = slot == 0 ? c.y1 : slot == 1 ? c.y2 : c.y3;
    return {x, -static_cast<double>(y)};
}
float stored(double value) noexcept { const float result = static_cast<float>(value); return result == 0 ? 0.f : result; }
void store(GlyphPathCommand& c, u32 slot, BoldPoint p) noexcept {
    const float x = stored(p.x), y = stored(-p.y);
    if (slot == 0) { c.x1 = x; c.y1 = y; } else if (slot == 1) { c.x2 = x; c.y2 = y; } else { c.x3 = x; c.y3 = y; }
}
// A point of the contour whose commands run from `begin` to the one before `end`. step() goes to the next point, and
// from the last point back to the first.
struct Cursor {u32 command, slot;};
inline void step(const GlyphPathCommand* c, u32 begin, u32 end, Cursor& at) noexcept {
    if (++at.slot < point_count(c[at.command].kind)) return;
    at.slot = 0;
    if (++at.command == end) at.command = begin;
}
// The command after the last of the contour that starts at `begin`, and the contour's point count.
u32 contour_end(const GlyphPathCommand* c, u32 count, u32 begin, u32* points) noexcept {
    u32 end = begin + 1;
    *points = point_count(c[begin].kind);
    while (end < count && point_count(c[end].kind)) *points += point_count(c[end++].kind);
    return end;
}
double sign(double value) noexcept { return value > 0 ? 1 : value < 0 ? -1 : 0; }
// DirectWrite widens a CFF face by 1310/65536 em on both axes: 2% of an em, truncated to 16.16.
constexpr double bold_strength = 1310.0 / 65536.0;
// The corner of the pen square that the edge from `from` to `to` moves to: on each axis the side its outward normal
// points to, or the middle when the normal has no part on that axis.
BoldPoint pen(BoldPoint from, BoldPoint to, double orientation) noexcept {
    constexpr double half = bold_strength / 2;
    return {half * (1 + orientation * sign(to.y - from.y)), half * (1 - orientation * sign(to.x - from.x))};
}
// A NaN from a degenerate meeting becomes 0.
double clamped(double value) noexcept { return value > 0 ? (value < bold_strength ? value : bold_strength) : 0; }
} // namespace

void cff_bold(GlyphOutline* outline) noexcept {
    GlyphPathCommand* c = outline->commands;
    const u32 count = outline->count;
    // The sign of the area of the point polygons gives the outward side of every edge, as in FreeType. A CFF face runs
    // its outer contours counterclockwise and its holes clockwise, so the holes shrink while the outer contours grow.
    // DirectWrite picks the other side for some glyphs whose contours cross themselves.
    double area = 0;
    for (u32 begin = 0, points = 0; begin < count;) {
        const u32 end = contour_end(c, count, begin, &points);
        Cursor at{begin, 0};
        BoldPoint p = load(c[begin], 0);
        for (u32 i = 0; i < points; ++i) {
            step(c, begin, end, at);
            const BoldPoint q = load(c[at.command], at.slot);
            area += p.x * q.y - q.x * p.y;
            p = q;
        }
        begin = end + 1;
    }
    const double orientation = area > 0 ? 1 : -1;
    for (u32 begin = 0, points = 0; begin < count;) {
        const u32 end = contour_end(c, count, begin, &points);
        // The points move in place. The wrap-around needs the original of the first point and of the first point
        // that differs from it. `previous` starts as the last point that differs from the first.
        const BoldPoint first = load(c[begin], 0);
        BoldPoint previous = first, second = first;
        Cursor at{begin, 0};
        for (u32 i = 1; i < points; ++i) {
            step(c, begin, end, at);
            const BoldPoint p = load(c[at.command], at.slot);
            if (same(p, first)) continue;
            if (same(second, first)) second = p;
            previous = p;
        }
        const u32 contour = begin;
        begin = end + 1;
        // A contour of one repeated point does not move.
        if (same(second, first)) continue;
        BoldPoint last = first, moved = first;
        at = {contour, 0};
        for (u32 i = 0; i < points; ++i, step(c, contour, end, at)) {
            const BoldPoint p = load(c[at.command], at.slot);
            // A repeated point moves with the point it repeats: both have the same neighbours.
            if (i && same(p, last)) { store(c[at.command], at.slot, moved); continue; }
            if (i) previous = last;
            BoldPoint next = p;
            Cursor scan = at;
            for (u32 j = i + 1; same(next, p); ++j) {
                if (j == points) { next = same(p, first) ? second : first; break; }
                step(c, contour, end, scan);
                next = load(c[scan.command], scan.slot);
            }
            // Each edge moves to its corner of the pen. The point moves to where its two moved edges meet, kept
            // inside the pen square. Between parallel edges (a spike) it moves with the edge after it.
            const BoldPoint a = pen(previous, p, orientation), b = pen(p, next, orientation);
            BoldPoint offset = a;
            if (!same(a, b)) {
                const double ax = p.x - previous.x, ay = p.y - previous.y, bx = next.x - p.x, by = next.y - p.y;
                const double denominator = ax * by - ay * bx;
                if (denominator == 0) offset = b;
                else {
                    const double t = ((b.x - a.x) * by - (b.y - a.y) * bx) / denominator;
                    offset = {a.x + t * ax, a.y + t * ay};
                }
            }
            moved = {p.x + clamped(offset.x), p.y + clamped(offset.y)};
            store(c[at.command], at.slot, moved);
            last = p;
        }
    }
    // The commands hold no negative zero (positive_zero, store()): their floats have the bounds of the points.
    outline->has_bounds = false;
    cff_detail::outline_bounds(outline);
}
#pragma float_control(pop)
} // namespace maicord
