#pragma once
// Internal-consistency checks over painted scenes: concentric corners of
// nested rounded rectangles, highlights cut by their clip, and per-component
// padding outliers in the layout tree. Header-only so any UI test can use it.
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/layout_engine.hpp"
#include "maicord/ui/paint.hpp"
#include "test_output.hpp"

namespace maicord {

enum class LayoutConsistencyKind : u8 { radius, clip, padding };

struct LayoutConsistencyIssue {
    LayoutConsistencyKind kind;
    const char* scene;
    UiRect outer, inner;
    float outer_radius, actual, expected_low, expected_high;
    Utf8View component;
    UiInsets padding, expected_padding;
};

inline constexpr u32 kLayoutConsistencyIssueCapacity = 256;
inline constexpr float kLayoutConsistencyTolerance = .5f;

struct LayoutConsistencyReport {
    LayoutConsistencyIssue issues[kLayoutConsistencyIssueCapacity];
    u32 count, dropped;
    u32 radius, clip, padding;
};

inline void layout_consistency_add(LayoutConsistencyReport* report, const LayoutConsistencyIssue& issue) noexcept {
    if (issue.kind == LayoutConsistencyKind::radius)
        ++report->radius;
    else if (issue.kind == LayoutConsistencyKind::clip)
        ++report->clip;
    else
        ++report->padding;
    if (report->count < kLayoutConsistencyIssueCapacity)
        report->issues[report->count++] = issue;
    else
        ++report->dropped;
}

// Plain rounded rectangles, gradients, surface materials and canvases carry a
// corner radius; lines, icons and picker fields do not.
inline bool layout_consistency_shape(const PaintQuad& quad) noexcept {
    const float mode = quad.border_width;
    const bool shape = mode >= 0 || mode == -2 || mode == -6 || mode == -7 || mode == -8;
    if (!shape || ui_icon_quad(quad) || !(quad.right > quad.left) || !(quad.bottom > quad.top))
        return false;
    const bool border = mode > 0 && quad.border.alpha > 0;
    return quad.fill.alpha > 0 || border || mode < 0;
}

struct LayoutConsistencyShape {
    UiRect rectangle, clip;
    float radius;
};

inline bool layout_consistency_contains(UiRect outer, UiRect inner) noexcept {
    constexpr float slack = .01f;
    return inner.left >= outer.left - slack && inner.top >= outer.top - slack &&
           inner.right <= outer.right + slack && inner.bottom <= outer.bottom + slack;
}

inline bool layout_consistency_point_inside(UiRect clip, float x, float y) noexcept {
    return x >= clip.left - .01f && x <= clip.right + .01f && y >= clip.top - .01f && y <= clip.bottom + .01f;
}

// Paint pass. For each visible rounded shape, the innermost shape containing
// it (ties: the earlier one is outer) constrains every corner whose insets are
// both below the outer radius: there the inner radius must lie in
// [R - max(dx, dy), R - min(dx, dy)], clamped at 0, within the tolerance.
// Whole circles are exempt from "too round" (avatars, status dots), and circles
// inside circles are glyph constructions (presence, radios), not nested
// surfaces. A clip that cuts into a rounded shape by less than its radius
// leaves a half-cut corner; the window edge (the root clip) is not a clip.
inline void layout_consistency_check_paint(const PaintStream& stream, u32 first, u32 end, const char* scene,
                                           LayoutConsistencyReport* report) noexcept {
    constexpr u32 capacity = kMaxPaintQuads;
    static LayoutConsistencyShape shapes[capacity];
    static u32 indices[capacity];
    end = minimum(end, stream.count);
    u32 count = 0;
    for (u32 b = 0; b < stream.batch_count; ++b) {
        const auto& batch = stream.batches[b];
        const auto& clip = stream.clips[batch.clip_index];
        for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
            if (q < first || q >= end || count >= capacity || !layout_consistency_shape(stream.quads[q]))
                continue;
            const auto& quad = stream.quads[q];
            const UiRect rectangle{quad.left, quad.top, quad.right, quad.bottom};
            const UiRect visible{clip.left, clip.top, clip.right, clip.bottom};
            if (!ui_rect_has_area(ui_rect_intersection(rectangle, visible)))
                continue;
            shapes[count] = {rectangle, visible, quad.radius};
            indices[count++] = q;
        }
    }
    const auto& root = stream.clips[0];
    const auto circle = [](const LayoutConsistencyShape& shape) noexcept {
        const float w = ui_rect_width(shape.rectangle), h = ui_rect_height(shape.rectangle);
        return w - h < .01f && h - w < .01f && shape.radius >= w * .5f - .01f;
    };
    for (u32 i = 0; i < count; ++i) {
        const auto& inner = shapes[i];
        const UiRect r = inner.rectangle;
        if (inner.radius > kLayoutConsistencyTolerance) {
            const auto& c = inner.clip;
            const float cut = maximum(maximum(c.left == root.left ? 0.0f : c.left - r.left,
                                              c.top == root.top ? 0.0f : c.top - r.top),
                                      maximum(c.right == root.right ? 0.0f : r.right - c.right,
                                              c.bottom == root.bottom ? 0.0f : r.bottom - c.bottom));
            // A cut at or past the radius removes the whole corner (a square edge).
            if (cut > kLayoutConsistencyTolerance && cut < inner.radius - kLayoutConsistencyTolerance)
                layout_consistency_add(report, {LayoutConsistencyKind::clip, scene, inner.clip, r, 0, inner.radius,
                                                0, cut});
        }
        u32 outer = count;
        float area = 0;
        for (u32 j = 0; j < count; ++j) {
            if (j == i || !layout_consistency_contains(shapes[j].rectangle, r))
                continue;
            const bool same = ui_rect_equal(shapes[j].rectangle, r);
            if (same && indices[j] > indices[i])
                continue;
            const float candidate = ui_rect_width(shapes[j].rectangle) * ui_rect_height(shapes[j].rectangle);
            if (outer == count || candidate < area || (candidate == area && indices[j] > indices[outer])) {
                outer = j;
                area = candidate;
            }
        }
        if (outer == count)
            continue;
        const auto& o = shapes[outer];
        const float big_r = o.radius;
        const bool round = circle(inner);
        if (big_r <= kLayoutConsistencyTolerance || (round && circle(o)))
            continue;
        float worst = 0, low = 0, high = 0;
        bool found = false;
        const float xs[2][2]{{r.left - o.rectangle.left, r.left}, {o.rectangle.right - r.right, r.right}};
        const float ys[2][2]{{r.top - o.rectangle.top, r.top}, {o.rectangle.bottom - r.bottom, r.bottom}};
        for (const auto& x : xs) {
            for (const auto& y : ys) {
                const float dx = maximum(0.0f, x[0]), dy = maximum(0.0f, y[0]);
                if (dx >= big_r - .01f || dy >= big_r - .01f)
                    continue;
                // A corner the clip removes, on either shape, is not seen as rounded.
                if (!layout_consistency_point_inside(inner.clip, x[1], y[1]) ||
                    !layout_consistency_point_inside(o.clip, x[1], y[1]))
                    continue;
                const float corner_low = maximum(0.0f, big_r - maximum(dx, dy));
                const float corner_high = maximum(0.0f, big_r - minimum(dx, dy));
                const float error = inner.radius < corner_low   ? corner_low - inner.radius
                                    : inner.radius > corner_high ? (round ? 0 : inner.radius - corner_high)
                                                                 : 0;
                if (error > worst) {
                    worst = error;
                    low = corner_low;
                    high = corner_high;
                    found = true;
                }
            }
        }
        if (found && worst > kLayoutConsistencyTolerance)
            layout_consistency_add(report, {LayoutConsistencyKind::radius, scene, o.rectangle, r, big_r,
                                            inner.radius, low, high});
    }
}

// Layout pass: padding samples keyed by the node's debug name (the component
// type). Names that appear once carry no consistency signal.
struct LayoutConsistencyPaddingSample {
    Utf8View component;
    UiInsets padding;
    const char* scene;
};
inline constexpr u32 kLayoutConsistencyPaddingCapacity = 2048;
struct LayoutConsistencyPadding {
    LayoutConsistencyPaddingSample samples[kLayoutConsistencyPaddingCapacity];
    u32 count;
};

inline void layout_consistency_record_layout(const UiLayoutEngine& engine, const char* scene,
                                             LayoutConsistencyPadding* table) noexcept {
    for (u32 i = 0; i < engine.count && table->count < kLayoutConsistencyPaddingCapacity; ++i) {
        const auto& node = engine.storage.data[i];
        if (!node.generation || !node.visible || !node.spec.debug_name.bytes)
            continue;
        table->samples[table->count++] = {node.spec.debug_name, node.spec.style.padding, scene};
    }
}

inline bool layout_consistency_same_insets(const UiInsets& a, const UiInsets& b) noexcept {
    const auto within = [](float x, float y) noexcept {
        return x - y <= kLayoutConsistencyTolerance && y - x <= kLayoutConsistencyTolerance;
    };
    return within(a.left, b.left) && within(a.right, b.right);
}

// Each component's most frequent horizontal padding is its convention; every
// sample that differs from it is an outlier. Vertical padding varies by design
// (grouped messages, first rows), horizontal padding sets column edges.
inline void layout_consistency_check_padding(const LayoutConsistencyPadding& table,
                                             LayoutConsistencyReport* report) noexcept {
    for (u32 i = 0; i < table.count; ++i) {
        const auto& sample = table.samples[i];
        bool first = true;
        for (u32 j = 0; j < i && first; ++j)
            first = !utf8_equal(table.samples[j].component, sample.component);
        if (!first)
            continue;
        u32 total = 0, best = 0, mode = i;
        for (u32 j = i; j < table.count; ++j) {
            if (!utf8_equal(table.samples[j].component, sample.component))
                continue;
            ++total;
            u32 votes = 0;
            for (u32 k = i; k < table.count; ++k)
                votes += utf8_equal(table.samples[k].component, sample.component) &&
                         layout_consistency_same_insets(table.samples[k].padding, table.samples[j].padding);
            if (votes > best) {
                best = votes;
                mode = j;
            }
        }
        if (total < 2)
            continue;
        for (u32 j = i; j < table.count; ++j) {
            const auto& other = table.samples[j];
            if (utf8_equal(other.component, sample.component) &&
                !layout_consistency_same_insets(other.padding, table.samples[mode].padding)) {
                LayoutConsistencyIssue issue{LayoutConsistencyKind::padding, other.scene};
                issue.component = other.component;
                issue.padding = other.padding;
                issue.expected_padding = table.samples[mode].padding;
                layout_consistency_add(report, issue);
            }
        }
    }
}

inline void layout_consistency_write_number(float value) noexcept {
    if (value < 0) {
        test_write_text("-");
        value = -value;
    }
    const u64 scaled = static_cast<u64>(value * 10 + .5f);
    test_write_u64(scaled / 10);
    if (scaled % 10) {
        test_write_text(".");
        test_write_u64(scaled % 10);
    }
}

inline void layout_consistency_write_rect(UiRect r) noexcept {
    test_write_text("[");
    layout_consistency_write_number(r.left);
    test_write_text(",");
    layout_consistency_write_number(r.top);
    test_write_text(" ");
    layout_consistency_write_number(ui_rect_width(r));
    test_write_text("x");
    layout_consistency_write_number(ui_rect_height(r));
    test_write_text("]");
}

inline void layout_consistency_print(const LayoutConsistencyReport& report) noexcept {
    for (u32 i = 0; i < report.count; ++i) {
        const auto& issue = report.issues[i];
        test_write_text(issue.scene);
        if (issue.kind == LayoutConsistencyKind::padding) {
            char name[96]{};
            const u32 bytes = minimum<u32>(issue.component.bytes, sizeof(name) - 1);
            copy_bytes(name, issue.component.data, bytes);
            test_write_text(": padding ");
            test_write_text(name);
            test_write_text(" ");
            layout_consistency_write_rect({issue.padding.left, issue.padding.top, issue.padding.left + issue.padding.right,
                                           issue.padding.top + issue.padding.bottom});
            test_write_text(" expected ");
            layout_consistency_write_rect({issue.expected_padding.left, issue.expected_padding.top,
                                           issue.expected_padding.left + issue.expected_padding.right,
                                           issue.expected_padding.top + issue.expected_padding.bottom});
            test_write_line("");
            continue;
        }
        test_write_text(issue.kind == LayoutConsistencyKind::radius ? ": radius " : ": clip ");
        layout_consistency_write_rect(issue.inner);
        test_write_text(" r=");
        layout_consistency_write_number(issue.actual);
        if (issue.kind == LayoutConsistencyKind::radius) {
            test_write_text(" in ");
            layout_consistency_write_rect(issue.outer);
            test_write_text(" R=");
            layout_consistency_write_number(issue.outer_radius);
            test_write_text(" expected ");
            layout_consistency_write_number(issue.expected_low);
            if (issue.expected_high - issue.expected_low > .05f) {
                test_write_text("..");
                layout_consistency_write_number(issue.expected_high);
            }
        } else {
            test_write_text(" cut ");
            layout_consistency_write_number(issue.expected_high);
            test_write_text(" by clip ");
            layout_consistency_write_rect(issue.outer);
        }
        test_write_line("");
    }
}

} // namespace maicord
