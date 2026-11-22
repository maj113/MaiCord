#pragma once

#include "maicord/ui/account/login_view.hpp"
#include "maicord/ui/text/presentation.hpp"

namespace maicord {

inline constexpr u32 kMaximumLoginTextCommands = 18;

// `view` comes from application_login_view_state and `scene` was derived from it by login_scene_apply_view and laid out.
bool login_text_presentation_build(
    const LoginScene& scene, const LoginView& view, const Theme& theme,
    UiTextCommandStream* stream) noexcept;

} // namespace maicord
