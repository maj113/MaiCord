#include "maicord/render/d3d11/layer_presentation.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "renderer_interaction_state.hpp"

namespace maicord {

namespace {

// Damages `rectangle` grown by `fringe` DIPs, clipped to the viewport.
void damage_with_fringe(D3D11Renderer* renderer, const UiRect& rectangle, float fringe) noexcept {
    if (renderer->dpi && rectangle.left < rectangle.right && rectangle.top < rectangle.bottom) {
        const float scale = 96.f / static_cast<float>(renderer->dpi);
        presentation_damage_add(&renderer->presentation_damage,
                                {rectangle.left - fringe, rectangle.top - fringe, rectangle.right + fringe,
                                 rectangle.bottom + fringe},
                                {0, 0, static_cast<float>(renderer->width) * scale,
                                 static_cast<float>(renderer->height) * scale});
    }
}

// The settled rectangle a surface draws in (empty without its scene).
UiRect surface_rectangle(const D3D11Renderer& renderer, RendererLayerSurface surface) noexcept {
    switch (surface) {
    case RendererLayerSurface::profile:
        return renderer.profile_scene ? renderer.profile_scene->rectangle : UiRect{};
    case RendererLayerSurface::gallery:
        return renderer.media_picker_scene ? renderer.media_picker_scene->panel : UiRect{};
    case RendererLayerSurface::popup:
        if (d3d11_renderer_shortcuts_visible(renderer))
            return renderer.shortcuts_scene->viewport;
        if (d3d11_renderer_download_visible(renderer))
            return renderer.download_scene->viewport;
        if (d3d11_renderer_go_live_visible(renderer))
            return renderer.go_live_scene->viewport;
        if (d3d11_renderer_message_forward_visible(renderer))
            return renderer.message_forward_scene->viewport;
        if (d3d11_renderer_quick_switcher_visible(renderer))
            return renderer.quick_switcher_scene->viewport;
        if (d3d11_renderer_inbox_visible(renderer))
            return renderer.inbox_scene->panel;
        return renderer.popup_scene ? renderer.popup_scene->rectangle : UiRect{};
    case RendererLayerSurface::settings_choice:
        return renderer.popup_scene ? renderer.popup_scene->rectangle : UiRect{};
    case RendererLayerSurface::theme_editor:
        return renderer.theme_editor_scene ? renderer.theme_editor_scene->panel : UiRect{};
    case RendererLayerSurface::forum_form:
        return renderer.forum_form && renderer.forum_form->scene ? renderer.forum_form->scene->viewport : UiRect{};
    default:
        return {};
    }
}

} // namespace

bool renderer_set_layer_opacity(D3D11Renderer* renderer, RendererLayerSurface surface,
                                float opacity) noexcept {
    if (!renderer)
        return false;
    float* value = nullptr;
    switch (surface) {
    case RendererLayerSurface::profile:
        value = &renderer->layer_opacity.profile;
        break;
    case RendererLayerSurface::gallery:
        value = &renderer->layer_opacity.gallery;
        break;
    case RendererLayerSurface::popup:
        value = &renderer->layer_opacity.popup;
        break;
    case RendererLayerSurface::settings_choice:
        value = &renderer->layer_opacity.settings_choice;
        break;
    case RendererLayerSurface::theme_editor:
        value = &renderer->layer_opacity.theme_editor;
        break;
    case RendererLayerSurface::forum_form:
        value = &renderer->layer_opacity.forum_form;
        break;
    default:
        return false;
    }
    const UiRect rectangle = surface_rectangle(*renderer, surface);
    opacity = opacity >= 0 && opacity <= 1 ? opacity : 1;
    if (*value == opacity)
        return false;
    *value = opacity;
    // Cover rounded antialiasing and the popup's existing exterior shadow.
    damage_with_fringe(renderer, rectangle, 12);
    return true;
}

bool renderer_set_layer_transform(D3D11Renderer* renderer, RendererLayerSurface surface, float scale,
                                  float origin_x, float origin_y, float shift_x, float shift_y) noexcept {
    if (!renderer || static_cast<u32>(surface) >= static_cast<u32>(RendererLayerSurface::count))
        return false;
    if (!(scale > 0 && scale <= 1) || !ui_extent_valid(origin_x) || !ui_extent_valid(origin_y) ||
        !(shift_x >= -kRendererMaximumLayerShift && shift_x <= kRendererMaximumLayerShift) ||
        !(shift_y >= -kRendererMaximumLayerShift && shift_y <= kRendererMaximumLayerShift))
        scale = 1, origin_x = origin_y = shift_x = shift_y = 0;
    if (scale == 1)
        origin_x = origin_y = 0;
    auto& transform = renderer->layer_transform[static_cast<u32>(surface)];
    if (transform.scale == scale && transform.origin_x == origin_x && transform.origin_y == origin_y &&
        transform.shift_x == shift_x && transform.shift_y == shift_y)
        return false;
    // A shrinking surface stays inside its settled rectangle (and its shadow); a shifted one moves at most this far.
    const float moved = maximum(maximum(transform.shift_x < 0 ? -transform.shift_x : transform.shift_x,
                                        transform.shift_y < 0 ? -transform.shift_y : transform.shift_y),
                                maximum(shift_x < 0 ? -shift_x : shift_x, shift_y < 0 ? -shift_y : shift_y));
    transform = {scale, origin_x, origin_y, shift_x, shift_y};
    damage_with_fringe(renderer, surface_rectangle(*renderer, surface), 12 + moved);
    return true;
}

bool renderer_set_settings_modal_progress(D3D11Renderer* renderer, float progress) noexcept {
    if (!renderer)
        return false;
    progress = progress >= 0 && progress <= 1 ? progress : 1;
    if (renderer->settings_modal_progress == progress)
        return false;
    renderer->settings_modal_progress = progress;
    presentation_damage_invalidate_full(&renderer->presentation_damage);
    return true;
}

bool renderer_set_toast_opacity(D3D11Renderer* renderer, float opacity) noexcept {
    if (!renderer)
        return false;
    opacity = opacity >= 0 && opacity <= 1 ? opacity : 1;
    if (renderer->toast_opacity == opacity)
        return false;
    renderer->toast_opacity = opacity;
    // The popover shadow below it reaches 12 DIP out and 6 DIP down.
    damage_with_fringe(renderer, renderer->toast_scene ? renderer->toast_scene->rectangle : UiRect{}, 20);
    return true;
}

bool renderer_set_tooltip_entrance(D3D11Renderer* renderer, float remaining) noexcept {
    if (!renderer)
        return false;
    remaining = remaining >= 0 && remaining <= 1 ? remaining : 0;
    if (renderer->tooltip_entrance_remaining == remaining)
        return false;
    renderer->tooltip_entrance_remaining = remaining;
    damage_with_fringe(renderer, renderer->tooltip_scene ? renderer->tooltip_scene->rectangle : UiRect{}, 12);
    return true;
}

} // namespace maicord
