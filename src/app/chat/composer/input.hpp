#pragma once

#include "maicord/core/base.hpp"

#include "maicord/core/win32_types.hpp"
#include "maicord/platform/window/event.hpp"

namespace maicord::app_detail {

void end_message_composer_pointer_selection() noexcept;
// The composer gives up keyboard focus: its typing indicator, caret and revision follow.
void release_message_composer_focus() noexcept;
bool handle_composer_click(const WindowEvent& event) noexcept;
bool handle_composer_double_click(const WindowEvent& event) noexcept;
bool handle_composer_drag(const WindowEvent& event) noexcept;
bool handle_composer_hover(const WindowEvent& event) noexcept;
bool handle_composer_key(u32 key) noexcept;
bool handle_composer_button_key(u32 key, bool down, bool repeated) noexcept;
bool handle_composer_character(u32 value) noexcept;

} // namespace maicord::app_detail
