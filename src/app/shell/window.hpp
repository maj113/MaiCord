#pragma once

#include "maicord/core/base.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

// The smallest outer size (frame included) the window may be sized to, in pixels at its DPI.
void application_minimum_window_frame(HWND window, u32* width, u32* height) noexcept;
// Creates the main window; its messages reach application_window_event (window_runtime.hpp).
bool application_create_main_window() noexcept;
// Borderless main window covering its monitor; off restores the saved
// placement and style. Returns false when the state did not change.
bool application_window_set_fullscreen(bool on) noexcept;
bool application_window_fullscreen() noexcept;

} // namespace maicord::app_detail
