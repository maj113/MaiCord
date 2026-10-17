#pragma once

#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
struct MessageActionBar;

bool message_action_bar_presentation_build(
    const MessageActionBar& bar, const Theme& theme,
    PaintStream* paint) noexcept;
bool message_action_bar_text_presentation_build_output(
    const MessageActionBar& bar, const Theme& theme,
    UiTextOutput* text) noexcept;
} // namespace maicord
