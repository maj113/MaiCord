#pragma once

#include "maicord/core/base.hpp"
#include "maicord/platform/window/window.hpp"
#include "maicord/os/wait.hpp"

namespace maicord::app_detail {

// The smallest client (the caption bar's rows included) the window may be sized to, in pixels at its DPI.
void application_minimum_client_size(u32* width, u32* height) noexcept;
// Tells the window system the minimum again (window_set_min_size) once the layout that sets it changed: the sign-in
// form and the main layout, the panes and the caption bar each need a different one. Wayland has no query like
// WM_GETMINMAXINFO: the compositor knows only what xdg_toplevel.set_min_size last said, so the shell calls this
// every turn (the window dedups). Windows asks on demand and has nothing to do.
#if defined(_WIN32)
inline void application_window_refresh_minimum_size() noexcept {}
#else
void application_window_refresh_minimum_size() noexcept;
#endif
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
// The platform's request to quit that does not come as a window event: on Linux SIGTERM, SIGINT and SIGHUP (read from a
// descriptor the main loop waits on; src/app/linux/shutdown_signal.cpp). `application_platform_quit_signal` is the
// waitable (null when there is none) and `application_platform_quit_requested` consumes what arrived. Windows has
// none: a closing session reaches the window as messages.
#if defined(_WIN32)
inline Waitable application_platform_quit_signal() noexcept {
    return nullptr;
}
inline bool application_platform_quit_requested() noexcept {
    return false;
}
#else
Waitable application_platform_quit_signal() noexcept;
bool application_platform_quit_requested() noexcept;
#endif
// The desktop's own interface and monospace fonts (the settings portal, read when the window was created) to the text
// system, before the first text resolves the default families. Nothing on Windows.
#if defined(_WIN32)
inline void application_platform_desktop_fonts() noexcept {}
#else
void application_platform_desktop_fonts() noexcept;
#endif
// Borderless main window covering its monitor; off restores the saved
// placement and style. Returns false when the state did not change.
bool application_window_set_fullscreen(bool on) noexcept;
bool application_window_fullscreen() noexcept;

} // namespace maicord::app_detail
