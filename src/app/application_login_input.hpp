#pragma once

#include "maicord/core/base.hpp"
#include "maicord/ui/login_scene.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

bool application_login_paste() noexcept;
bool application_login_prepare_identifier() noexcept;
void application_login_tick_caret(u64 now_ms) noexcept;
bool application_login_position_edit(LoginControl control, i32 client_x, i32 client_y,
                                     bool extend_selection) noexcept;
void application_login_end_pointer_selection() noexcept;
bool application_login_handle_drag(WPARAM buttons, LPARAM position) noexcept;
bool application_login_handle_hover(LPARAM position) noexcept;
bool application_login_handle_character(WPARAM character) noexcept;

} // namespace maicord::app_detail
