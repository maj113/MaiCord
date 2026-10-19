#include "maicord/render/layer_presentation.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "d3d11_renderer_interaction_state.hpp"

namespace maicord {

bool renderer_set_layer_opacity(D3D11Renderer* renderer, RendererLayerSurface surface,
                                float opacity) noexcept {
    if (!renderer)
        return false;
    float* value = nullptr;
    UiRect rectangle{};
    switch (surface) {
    case RendererLayerSurface::profile:
        value = &renderer->layer_opacity.profile;
        if (renderer->profile_scene)
            rectangle = renderer->profile_scene->rectangle;
        break;
    case RendererLayerSurface::gallery:
        value = &renderer->layer_opacity.gallery;
        if (renderer->media_picker_scene)
            rectangle = renderer->media_picker_scene->panel;
        break;
    case RendererLayerSurface::popup:
        value = &renderer->layer_opacity.popup;
        if (d3d11_renderer_shortcuts_visible(*renderer))
            rectangle = renderer->shortcuts_scene->viewport;
        else if (d3d11_renderer_download_visible(*renderer))
            rectangle = renderer->download_scene->viewport;
        else if (d3d11_renderer_message_forward_visible(*renderer))
            rectangle = renderer->message_forward_scene->viewport;
        else if (d3d11_renderer_quick_switcher_visible(*renderer))
            rectangle = renderer->quick_switcher_scene->viewport;
        else if (d3d11_renderer_inbox_visible(*renderer))
            rectangle = renderer->inbox_scene->panel;
        else if (renderer->popup_scene)
            rectangle = renderer->popup_scene->rectangle;
        break;
    case RendererLayerSurface::settings_choice:
        value = &renderer->layer_opacity.settings_choice;
        if (renderer->popup_scene)
            rectangle = renderer->popup_scene->rectangle;
        break;
    case RendererLayerSurface::settings_page:
        value = &renderer->layer_opacity.settings_page;
        if (renderer->settings_scene) {
            const auto& scene = *renderer->settings_scene;
            const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.body);
            if (node)
                rectangle = {node->rectangle.left, scene.viewport.top, scene.viewport.right,
                             scene.viewport.bottom};
        }
        break;
    case RendererLayerSurface::theme_editor:
        value = &renderer->layer_opacity.theme_editor;
        if (renderer->theme_editor_scene)
            rectangle = renderer->theme_editor_scene->panel;
        break;
    case RendererLayerSurface::forum_form:
        value = &renderer->layer_opacity.forum_form;
        if (renderer->forum_form && renderer->forum_form->scene)
            rectangle = renderer->forum_form->scene->viewport;
        break;
    default:
        return false;
    }
    opacity = opacity >= 0 && opacity <= 1 ? opacity : 1;
    if (*value == opacity)
        return false;
    *value = opacity;
    if (renderer->dpi && rectangle.left < rectangle.right && rectangle.top < rectangle.bottom) {
        // Cover rounded antialiasing and the popup's existing exterior shadow.
        constexpr float fringe = 12;
        const float scale = 96.f / static_cast<float>(renderer->dpi);
        presentation_damage_add(&renderer->presentation_damage,
                                {rectangle.left - fringe, rectangle.top - fringe, rectangle.right + fringe,
                                 rectangle.bottom + fringe},
                                {0, 0, static_cast<float>(renderer->width) * scale,
                                 static_cast<float>(renderer->height) * scale});
    }
    return true;
}

bool renderer_set_popup_scale(D3D11Renderer* renderer, float scale, float origin_x, float origin_y) noexcept {
    if (!renderer)
        return false;
    if (!(scale > 0 && scale <= 1) || !ui_extent_valid(origin_x) || !ui_extent_valid(origin_y))
        scale = 1, origin_x = origin_y = 0;
    if (scale == 1)
        origin_x = origin_y = 0;
    if (renderer->popup_layer_scale == scale && renderer->popup_layer_origin_x == origin_x &&
        renderer->popup_layer_origin_y == origin_y)
        return false;
    renderer->popup_layer_scale = scale;
    renderer->popup_layer_origin_x = origin_x;
    renderer->popup_layer_origin_y = origin_y;
    // A shrinking popup stays inside its settled rectangle (and its shadow).
    const UiRect rectangle = renderer->popup_scene ? renderer->popup_scene->rectangle : UiRect{};
    if (renderer->dpi && rectangle.left < rectangle.right && rectangle.top < rectangle.bottom) {
        constexpr float fringe = 12;
        const float pixels = 96.f / static_cast<float>(renderer->dpi);
        presentation_damage_add(&renderer->presentation_damage,
                                {rectangle.left - fringe, rectangle.top - fringe, rectangle.right + fringe,
                                 rectangle.bottom + fringe},
                                {0, 0, static_cast<float>(renderer->width) * pixels,
                                 static_cast<float>(renderer->height) * pixels});
    }
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
    const UiRect rectangle = renderer->toast_scene ? renderer->toast_scene->rectangle : UiRect{};
    if (renderer->dpi && rectangle.left < rectangle.right && rectangle.top < rectangle.bottom) {
        // The popover shadow below it reaches 12 DIP out and 6 DIP down.
        constexpr float fringe = 20;
        const float pixels = 96.f / static_cast<float>(renderer->dpi);
        presentation_damage_add(&renderer->presentation_damage,
                                {rectangle.left - fringe, rectangle.top - fringe, rectangle.right + fringe,
                                 rectangle.bottom + fringe},
                                {0, 0, static_cast<float>(renderer->width) * pixels,
                                 static_cast<float>(renderer->height) * pixels});
    }
    return true;
}

} // namespace maicord
