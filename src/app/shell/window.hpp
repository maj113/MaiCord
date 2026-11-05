#pragma once

#include "maicord/core/base.hpp"
#include "maicord/platform/window/window.hpp"

namespace maicord::app_detail {

// The smallest client (the caption bar's rows included) the window may be sized to, in pixels at its DPI.
void application_minimum_client_size(u32* width, u32* height) noexcept;
// Creates the main window; its events reach application_window_event (window_runtime.hpp).
bool application_create_main_window() noexcept;
// Before the renderer starts: what the window system must have ready for the GPU device (the Wayland connection the
// device is made on). Nothing on Windows.
void application_platform_window_prepare() noexcept;
// How the platform makes the window and its frame: the Windows build's frame glue and hooks
// (src/app/windows/platform_window.cpp). On failure `*failure` names the step for the log.
bool application_platform_window_create(Window* out, const char** failure) noexcept;
// Shows the main window the first time (the probe builds show it without taking the focus).
void application_platform_window_show(ShowMode mode) noexcept;
// Borderless main window covering its monitor; off restores the saved
// placement and style. Returns false when the state did not change.
bool application_window_set_fullscreen(bool on) noexcept;
bool application_window_fullscreen() noexcept;

} // namespace maicord::app_detail
