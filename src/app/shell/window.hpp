#pragma once

#include "maicord/core/base.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

void application_apply_minimum_window_track(HWND window, MINMAXINFO* limits) noexcept;
bool application_create_main_window(WNDPROC window_proc) noexcept;
// Borderless main window covering its monitor; off restores the saved
// placement and style. Returns false when the state did not change.
bool application_window_set_fullscreen(bool on) noexcept;
bool application_window_fullscreen() noexcept;

} // namespace maicord::app_detail
