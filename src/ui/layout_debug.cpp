#include "maicord/ui/layout_debug.hpp"

namespace maicord {
namespace {

struct TextBuilder {
    char* data;
    u32 capacity;
    u32 length;
};

void append_character(TextBuilder* builder, char value) noexcept {
    if (builder->length + 1 >= builder->capacity) {
        return;
    }
    builder->data[builder->length++] = value;
    builder->data[builder->length] = 0;
}

void append_text(TextBuilder* builder, const char* value) noexcept {
    if (!value) {
        return;
    }
    while (*value != 0 && builder->length + 1 < builder->capacity) {
        builder->data[builder->length++] = *value++;
    }
    builder->data[builder->length] = 0;
}

void append_view(TextBuilder* builder, ConstSlice<char> value) noexcept {
    for (u32 index = 0;
         index < value.count && builder->length + 1 < builder->capacity;
         ++index) {
        builder->data[builder->length++] = value[index];
    }
    builder->data[builder->length] = 0;
}

void append_unsigned(TextBuilder* builder, u32 value) noexcept {
    char digits[10]{};
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < 10);
    while (count != 0) {
        append_character(builder, digits[--count]);
    }
}

void append_coordinate(TextBuilder* builder, float value) noexcept {
    if (!ui_coordinate_valid(value)) {
        append_text(builder, "invalid");
        return;
    }
    if (value < 0.0f) {
        append_character(builder, '-');
        value = -value;
    }
    const u32 tenths = static_cast<u32>(value * 10.0f + 0.5f);
    append_unsigned(builder, tenths / 10);
    append_character(builder, '.');
    append_character(builder, static_cast<char>('0' + tenths % 10));
}

void append_rectangle(TextBuilder* builder, const UiRect& rectangle) noexcept {
    append_character(builder, '(');
    append_coordinate(builder, rectangle.left);
    append_character(builder, ',');
    append_coordinate(builder, rectangle.top);
    append_character(builder, ' ');
    append_coordinate(builder, ui_rect_width(rectangle));
    append_character(builder, 'x');
    append_coordinate(builder, ui_rect_height(rectangle));
    append_character(builder, ')');
}

const char* diagnostic_text(UiLayoutDiagnosticKind kind) noexcept {
    switch (kind) {
    case UiLayoutDiagnosticKind::invalid_hierarchy:
        return "invalid hierarchy";
    case UiLayoutDiagnosticKind::invalid_extent:
        return "negative or invalid extent";
    case UiLayoutDiagnosticKind::unexpected_zero_size:
        return "unexpected zero-size element";
    case UiLayoutDiagnosticKind::child_overflow:
        return "child bbox exceeds parent content bbox";
    case UiLayoutDiagnosticKind::sibling_collision:
        return "prohibited sibling collision";
    case UiLayoutDiagnosticKind::invalid_clip:
        return "invalid or missing clip";
    case UiLayoutDiagnosticKind::text_overflow:
        return "text exceeds allocated content bbox";
    case UiLayoutDiagnosticKind::invalid_alignment:
        return "incorrect alignment";
    case UiLayoutDiagnosticKind::invalid_anchor:
        return "invalid parent-relative anchors or anchored bounds";
    case UiLayoutDiagnosticKind::invalid_baseline:
        return "incorrect baseline alignment";
    case UiLayoutDiagnosticKind::unstable_measurement:
        return "content measurement did not converge";
    case UiLayoutDiagnosticKind::invalid_content_owner:
        return "render content has no live widget owner";
    case UiLayoutDiagnosticKind::content_overflow:
        return "render content exceeds widget content bbox";
    case UiLayoutDiagnosticKind::content_collision:
        return "render content overlaps another widget";
    }
    return "unknown layout diagnostic";
}

UiWidgetHandle widget_handle_for_layout(const UiWidgetManager& manager,
                                        UiLayoutNodeHandle layout) noexcept {
    if (layout.generation == 0 || layout.slot >= manager.count) {
        return ui_widget_invalid_handle();
    }
    const UiWidget& widget = manager.storage[layout.slot];
    return UiWidgetHandle{layout.slot, widget.generation};
}

void append_path(TextBuilder* builder, const UiWidgetManager& manager,
                 UiLayoutNodeHandle layout) noexcept {
    char path[256]{};
    const u32 bytes = ui_widget_manager_format_path(
        manager, widget_handle_for_layout(manager, layout),
        Slice<char>{path, static_cast<u32>(sizeof(path))});
    append_view(builder, ConstSlice<char>{path, bytes});
}

bool push_outline(PaintStream* stream, const UiRect& rectangle,
                  const Color& color, float width) noexcept {
    return paint_stream_push_rounded_border(
        stream, rectangle.left, rectangle.top, rectangle.right,
        rectangle.bottom, Color{0.0f, 0.0f, 0.0f, 0.0f}, color, 0.0f,
        width);
}

} // namespace

const UiLayoutDebugPalette kDefaultUiLayoutDebugPalette{
    {{0.20f, 0.78f, 1.00f, 0.92f}, {0.38f, 0.92f, 0.46f, 0.92f},
     {0.78f, 0.48f, 1.00f, 0.92f}, {1.00f, 0.72f, 0.22f, 0.92f}},
    {0.18f, 1.00f, 0.72f, 0.88f},
    {1.00f, 0.72f, 0.20f, 0.82f},
    {0.20f, 0.62f, 1.00f, 0.82f},
    {0.94f, 0.40f, 1.00f, 0.88f},
    {1.00f, 0.20f, 0.24f, 1.00f},
    {1.00f, 0.08f, 0.12f, 0.25f},
};

bool ui_widget_manager_debug_paint(
    const UiWidgetManager& manager,
    ConstSlice<UiLayoutDiagnostic> diagnostics, u32 flags,
    const UiLayoutDebugPalette& palette, PaintStream* stream) noexcept {
    if (!stream || (!diagnostics.data && diagnostics.count != 0)) {
        return false;
    }
    for (u32 index = 0; index < manager.layout.count; ++index) {
        const UiLayoutNode& node = manager.layout.storage[index];
        if (!node.visible) {
            continue;
        }
        if ((flags & ui_layout_debug_margins) != 0) {
            const UiInsets& margin = node.spec.style.margin;
            const UiRect outer{node.rectangle.left - margin.left,
                               node.rectangle.top - margin.top,
                               node.rectangle.right + margin.right,
                               node.rectangle.bottom + margin.bottom};
            if (!push_outline(stream, outer, palette.margin, 1.0f)) {
                return false;
            }
        }
        if ((flags & ui_layout_debug_bounds) != 0 &&
            !push_outline(stream, node.rectangle,
                          palette.bounds[node.depth & 3u], 1.0f)) {
            return false;
        }
        if ((flags & ui_layout_debug_content) != 0 &&
            !push_outline(stream, node.content_rectangle, palette.content,
                          1.0f)) {
            return false;
        }
        if ((flags & ui_layout_debug_clips) != 0 &&
            !push_outline(stream, node.clip_rectangle, palette.clip, 1.0f)) {
            return false;
        }
        if ((flags & ui_layout_debug_baselines) != 0 &&
            node.measured_baseline > 0.0f) {
            const float baseline =
                node.rectangle.top + node.measured_baseline;
            if (!paint_stream_push(stream, node.content_rectangle.left,
                                   baseline, node.content_rectangle.right,
                                   baseline + 1.0f, palette.baseline)) {
                return false;
            }
        }
        if ((flags & ui_layout_debug_anchors) != 0 &&
            (node.spec.style.flags & ui_layout_node_anchored) != 0 && node.parent < index) {
            const auto* region = ui_layout_engine_anchor_region(manager.layout,
                {static_cast<u16>(index), node.generation});
            if (!region) continue;
            const auto guide = ui_layout_anchor_rectangle(manager.layout.storage[node.parent], *region);
            if (!ui_rect_valid_or_empty(guide)) continue;
            if (ui_rect_has_area(guide) && !push_outline(stream, guide, palette.baseline, 1.0f)) return false;
            const UiPoint points[]{{guide.left, guide.top}, {guide.right, guide.bottom}};
            for (const auto& point : points) {
                if (!paint_stream_push(stream, point.x - 3.0f, point.y, point.x + 3.0f,
                                       point.y + 1.0f, palette.baseline) ||
                    !paint_stream_push(stream, point.x, point.y - 3.0f, point.x + 1.0f,
                                       point.y + 3.0f, palette.baseline)) return false;
            }
        }
    }
    if ((flags & ui_layout_debug_diagnostics) != 0) {
        for (u32 index = 0; index < diagnostics.count; ++index) {
            const UiLayoutDiagnostic& diagnostic = diagnostics[index];
            if (ui_rect_has_area(diagnostic.actual) &&
                !paint_stream_push(stream, diagnostic.actual.left,
                                   diagnostic.actual.top,
                                   diagnostic.actual.right,
                                   diagnostic.actual.bottom,
                                   palette.warning_fill)) {
                return false;
            }
            if (ui_rect_has_area(diagnostic.expected) &&
                !push_outline(stream, diagnostic.expected, palette.warning,
                              2.0f)) {
                return false;
            }
        }
    }
    return true;
}

u32 ui_widget_manager_format_diagnostic(
    const UiWidgetManager& manager, const UiLayoutDiagnostic& diagnostic,
    Slice<char> output) noexcept {
    if (!output.data || output.count == 0) {
        return 0;
    }
    output[0] = 0;
    TextBuilder builder{output.data, output.count, 0};
    append_text(&builder, "Layout warning: ");
    append_path(&builder, manager, diagnostic.node);
    append_text(&builder, " | ");
    append_text(&builder, diagnostic_text(diagnostic.kind));
    if (diagnostic.related.generation != 0) {
        append_text(&builder, " | related=");
        append_path(&builder, manager, diagnostic.related);
    }
    if (ui_rect_has_area(diagnostic.expected)) {
        append_text(&builder, " | expected=");
        append_rectangle(&builder, diagnostic.expected);
    }
    if (ui_rect_has_area(diagnostic.actual)) {
        append_text(&builder, " | actual=");
        append_rectangle(&builder, diagnostic.actual);
    }
    if (diagnostic.overflow.left > 0.0f ||
        diagnostic.overflow.top > 0.0f ||
        diagnostic.overflow.right > 0.0f ||
        diagnostic.overflow.bottom > 0.0f) {
        append_text(&builder, " | overflow=");
        append_coordinate(&builder, diagnostic.overflow.left);
        append_character(&builder, ',');
        append_coordinate(&builder, diagnostic.overflow.top);
        append_character(&builder, ',');
        append_coordinate(&builder, diagnostic.overflow.right);
        append_character(&builder, ',');
        append_coordinate(&builder, diagnostic.overflow.bottom);
    }
    return builder.length;
}

} // namespace maicord
