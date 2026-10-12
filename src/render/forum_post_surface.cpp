#include "maicord/render/forum_post_surface.hpp"

namespace maicord {
bool forum_post_surface_update(ForumPostSurface* surface, const TextSystem& text) noexcept {
    if (!surface)
        return false;
    for (u32 f = 0; f < 2; ++f) {
        surface->geometry[f] = {};
        surface->composition_complete[f] = true;
        if (!surface->scene || !surface->scene->active) {
            text_edit_layout_invalidate(&surface->layouts[f]);
            continue;
        }
        const auto& scene = *surface->scene;
        const auto& content = scene.content.fields[f];
        const auto* field = forum_post_node(scene, f ? ForumPostElement::body : ForumPostElement::title);
        if (!field || !ui_rect_valid(field->content_rectangle))
            return false;
        const auto rectangle = field->content_rectangle;
        auto& layout = surface->layouts[f];
        layout.selections = surface->metrics[f];
        layout.selection_capacity = kTextEditSelectionCapacity;
        const u8 options = text_edit_layout_scroll_to_caret |
                           (f ? text_edit_layout_wrap : text_edit_layout_center_vertically);
        if (!text_edit_layout_measure(&layout,
                                        {content.text, scene.text_signature[f], content.anchor_utf16,
                                         content.caret_utf16, rectangle.left, rectangle.top,
                                         ui_rect_width(rectangle), ui_rect_height(rectangle),
                                         kMaximumUiCoordinate, options},
                                        text))
            return false;
        for (u32 i = 0; i < layout.selection_count; ++i) {
            const auto& metric = layout.selections[i];
            surface->selections[f][i] = {metric.left, metric.top, metric.left + metric.width,
                                         metric.top + metric.height};
        }
        surface->geometry[f] = {{layout.origin_x, layout.origin_y,
                                 f ? layout.origin_x + layout.width
                                   : maximum(rectangle.right, layout.origin_x + maximum(layout.width, 1.f)),
                                 layout.origin_y + layout.layout_height},
                                {surface->selections[f], layout.selection_count},
                                {layout.caret.left, layout.caret.top,
                                 layout.caret.left + layout.caret.width,
                                 layout.caret.top + layout.caret.height}};
        u32 marks = 0;
        for (u32 i = 0; i < content.composition.count; ++i) {
            const auto& span = content.composition[i];
            TextEditMetric metrics[kTextEditSelectionCapacity]{};
            u32 count = 0;
            if (!text_edit_layout_range_geometry(layout, span.start, span.end,
                                                   {metrics, kTextEditSelectionCapacity}, &count) ||
                count > kTextCompositionSpanCapacity - marks) {
                surface->composition_complete[f] = false;
                break;
            }
            for (u32 j = 0; j < count; ++j) {
                const auto& metric = metrics[j];
                surface->composition[f][marks++] = {
                    {metric.left, metric.top, metric.left + metric.width, metric.top + metric.height},
                    span};
            }
        }
        surface->geometry[f].composition = {surface->composition[f], marks};
    }
    return true;
}
void forum_post_surface_release(ForumPostSurface* surface) noexcept {
    if (!surface)
        return;
    for (auto& layout : surface->layouts)
        text_edit_layout_release(&layout);
    *surface = {};
}
} // namespace maicord
