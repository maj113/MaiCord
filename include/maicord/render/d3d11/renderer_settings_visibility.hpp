#pragma once
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/settings/theme_editor.hpp"

namespace maicord {

// Rendering visibility only: Settings authority/read barriers remain active.
inline bool renderer_theme_editor_visible(const D3D11Renderer& renderer) noexcept {
    return !renderer.login_visible && renderer.theme_editor_scene && renderer.theme_editor_scene->active &&
           ui_theme_editor_current(*renderer.theme_editor_scene);
}
inline bool renderer_settings_page_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.settings_scene && renderer.settings_scene->active &&
           !renderer_theme_editor_visible(renderer);
}
// Only a full-window settings layer hides the shell. Beneath a modal or a
// dialog the shell stays live: its scenes, media and animations keep their
// own cadence and the backdrop redraws when they change.
inline bool renderer_settings_hides_shell(const D3D11Renderer& renderer) noexcept {
    return renderer_settings_page_visible(renderer) &&
           renderer.settings_scene->presentation == UiSettingsPresentation::layer;
}

} // namespace maicord
