#include "maicord/render/text_form_surface.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
bool text_form_surface_update(TextFormSurface* surface, const TextSystem& text) noexcept {
    if (!surface) return false;
    reset_value(&surface->geometry);
    surface->composition_complete = true;
    if (!surface->scene || !surface->scene->active) {
        text_edit_layout_invalidate(&surface->layout);
        return true;
    }
    const auto& scene = *surface->scene;
    if (scene.content.confirmation_only) {
        text_edit_layout_invalidate(&surface->layout);
        return true;
    }
    const auto* field = text_form_node(scene, TextFormElement::edit);
    if (!field || !ui_rect_valid(field->content_rectangle)) return false;
    const auto rectangle = field->content_rectangle;
    surface->layout.selections = surface->metrics;
    surface->layout.selection_capacity = kTextEditSelectionCapacity;
    const u8 options = text_edit_layout_scroll_to_caret |
        (scene.content.single_line ? text_edit_layout_center_vertically : text_edit_layout_wrap);
    if (!text_edit_layout_measure(&surface->layout, {scene.content.text, scene.text_signature,
        scene.content.anchor_utf16, scene.content.caret_utf16, rectangle.left, rectangle.top,
        ui_rect_width(rectangle), ui_rect_height(rectangle), kMaximumUiCoordinate,
        options}, text)) return false;
    const auto& layout = surface->layout;
    for (u32 i = 0; i < layout.selection_count; ++i) {
        const auto& metric = layout.selections[i];
        surface->selections[i] = {metric.left, metric.top, metric.left + metric.width, metric.top + metric.height};
    }
    surface->geometry = {{layout.origin_x, layout.origin_y,
        scene.content.single_line ? maximum(rectangle.right, layout.origin_x + maximum(layout.width, 1.f))
                                  : layout.origin_x + layout.width,
        layout.origin_y + layout.layout_height}, {surface->selections, layout.selection_count},
        {layout.caret.left, layout.caret.top, layout.caret.left + layout.caret.width, layout.caret.top + layout.caret.height}};
    u32 marks = 0;
    for (u32 i = 0; i < scene.content.composition.count; ++i) {
        const auto& span = scene.content.composition[i];
        TextEditMetric metrics[kTextEditSelectionCapacity]{};
        u32 count = 0;
        if (!text_edit_layout_range_geometry(layout, span.start, span.end, {metrics, kTextEditSelectionCapacity}, &count) ||
            count > kTextCompositionSpanCapacity - marks) { surface->composition_complete = false; break; }
        for (u32 j = 0; j < count; ++j) {
            const auto& metric = metrics[j];
            surface->composition[marks++] = {{metric.left, metric.top,
                metric.left + metric.width, metric.top + metric.height}, span};
        }
    }
    surface->geometry.composition = {surface->composition, marks};
    return true;
}
void text_form_surface_release(TextFormSurface* surface) noexcept {
    if (!surface) return;
    text_edit_layout_release(&surface->layout);
    reset_value(surface);
}
} // namespace maicord
