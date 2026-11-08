#include "maicord/render/text/forum_post_surface.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/chat/threads/forum_post_scene.hpp"

namespace maicord {
bool forum_post_surface_update(ForumPostSurface* surface, const TextSystem& text) noexcept {
    if (!surface)
        return false;
    // Field 0 is the single-line title, field 1 the body.
    for (u32 f = 0; f < 2; ++f) {
        reset_value(&surface->geometry[f]);
        surface->composition_complete[f] = true;
        if (!surface->scene || !surface->scene->active) {
            text_edit_layout_invalidate(&surface->layouts[f]);
            continue;
        }
        const auto& scene = *surface->scene;
        const auto& content = scene.content.fields[f];
        const auto* field = forum_post_node(scene, f ? ForumPostElement::body : ForumPostElement::title);
        if (!field || !ui_rect_valid(field->content_rectangle) ||
            !text_form_field_update(&surface->layouts[f], surface->metrics[f], surface->selections[f],
                                    surface->composition[f], &surface->geometry[f], &surface->composition_complete[f],
                                    text, field->content_rectangle, content.text, scene.text_signature[f],
                                    content.anchor_utf16, content.caret_utf16, content.composition, f == 0))
            return false;
    }
    return true;
}
void forum_post_surface_release(ForumPostSurface* surface) noexcept {
    if (!surface)
        return;
    for (auto& layout : surface->layouts)
        text_edit_layout_release(&layout);
    reset_value(surface);
}
} // namespace maicord
