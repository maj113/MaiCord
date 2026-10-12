#pragma once

#include "maicord/core/base.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

void end_message_composer_pointer_selection() noexcept;
bool handle_composer_click(LPARAM lparam) noexcept;
bool handle_composer_double_click(LPARAM lparam) noexcept;
bool handle_composer_drag(WPARAM buttons, LPARAM lparam) noexcept;
bool handle_composer_hover(LPARAM lparam) noexcept;
bool handle_composer_key(WPARAM key) noexcept;
bool handle_composer_button_key(u32 key, bool down, bool repeated) noexcept;
bool handle_composer_character(WPARAM value) noexcept;

} // namespace maicord::app_detail
