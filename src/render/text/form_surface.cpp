#include "maicord/render/text/form_surface.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
bool text_form_field_update(TextEditLayoutOwner* owner, TextEditMetric* metrics, UiRect* selections,
                            TextFormCompositionMark* composition, TextFormEditGeometry* geometry,
                            bool* composition_complete, const TextSystem& text, UiRect rectangle, Utf8View value,
                            u64 signature, u32 anchor_utf16, u32 caret_utf16,
                            ConstSlice<TextCompositionSpan> spans, bool single_line) noexcept {
    owner->selections = metrics;
    owner->selection_capacity = kTextEditSelectionCapacity;
    const u8 options = text_edit_layout_scroll_to_caret |
        (single_line ? text_edit_layout_center_vertically : text_edit_layout_wrap);
    if (!text_edit_layout_measure(owner, {value, signature, anchor_utf16, caret_utf16, rectangle.left, rectangle.top,
        ui_rect_width(rectangle), ui_rect_height(rectangle), kMaximumUiCoordinate, options}, text)) return false;
    const auto& layout = *owner;
    for (u32 i = 0; i < layout.selection_count; ++i) {
        const auto& metric = layout.selections[i];
        selections[i] = {metric.left, metric.top, metric.left + metric.width, metric.top + metric.height};
    }
    *geometry = {{layout.origin_x, layout.origin_y,
        single_line ? maximum(rectangle.right, layout.origin_x + maximum(layout.width, 1.f))
                    : layout.origin_x + layout.width,
        layout.origin_y + layout.layout_height}, {selections, layout.selection_count},
        {layout.caret.left, layout.caret.top, layout.caret.left + layout.caret.width, layout.caret.top + layout.caret.height}};
    u32 marks = 0;
    for (u32 i = 0; i < spans.count; ++i) {
        const auto& span = spans[i];
        TextEditMetric pieces[kTextEditSelectionCapacity]{};
        u32 count = 0;
        if (!text_edit_layout_range_geometry(layout, span.start, span.end, {pieces, kTextEditSelectionCapacity}, &count) ||
            count > kTextCompositionSpanCapacity - marks) { *composition_complete = false; break; }
        for (u32 j = 0; j < count; ++j) {
            const auto& metric = pieces[j];
            composition[marks++] = {{metric.left, metric.top, metric.left + metric.width, metric.top + metric.height}, span};
        }
    }
    geometry->composition = {composition, marks};
    return true;
}
bool text_form_surface_update(TextFormSurface* surface, const TextSystem& text) noexcept {
    reset_value(&surface->geometry);
    surface->composition_complete = true;
    if (!surface->scene || !surface->scene->active || surface->scene->content.confirmation_only) {
        text_edit_layout_invalidate(&surface->layout);
        return true;
    }
    const auto& scene = *surface->scene;
    const auto* field = text_form_node(scene, TextFormElement::edit);
    if (!field || !ui_rect_valid(field->content_rectangle)) return false;
    const auto& content = scene.content;
    return text_form_field_update(&surface->layout, surface->metrics, surface->selections, surface->composition,
                                  &surface->geometry, &surface->composition_complete, text, field->content_rectangle,
                                  content.text, scene.text_signature, content.anchor_utf16, content.caret_utf16,
                                  content.composition, content.single_line);
}
void text_form_surface_release(TextFormSurface* surface) noexcept {
    text_edit_layout_release(&surface->layout);
    reset_value(surface);
}
} // namespace maicord
