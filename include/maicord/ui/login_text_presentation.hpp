#pragma once

#include "maicord/ui/login_view.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {

inline constexpr u32 kMaximumLoginTextCommands = 18;

bool login_text_presentation_build(
    const LoginScene& scene, const LoginView& view, const Theme& theme,
    UiTextCommandStream* stream) noexcept;

} // namespace maicord
