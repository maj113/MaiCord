#pragma once
#include "application_settings.hpp"
#include "application_state.hpp"
#include "maicord/ui/profile_scene.hpp"

namespace maicord::app_detail {
// No settings page, text form, popup, media viewer, reaction viewer or profile card covers the host
// view of a docked panel (pins, inbox, thread browser).
__declspec(noinline) inline bool application_panel_uncovered() noexcept {
    return !application_settings_active() && !g_app.text_form && !g_app.popup_scene.active &&
           !g_app.image_viewer.scene.active && !g_app.reaction_viewer.active &&
           !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active);
}
} // namespace maicord::app_detail
