#pragma once

#include "maicord/core/base.hpp"
#include "maicord/platform/window/window.hpp"

// What the user's desktop does with a link or a file: ShellExecute on Windows (inline forwarders,
// maicord/platform/desktop/windows/shell_open.inl); the OpenURI portal, falling back to xdg-open, on Linux
// (src/platform/desktop/linux/shell_open.cpp over src/platform/dbus/open_uri.cpp).

namespace maicord {

// Opens `target`, a NUL-terminated URL or absolute path, with the program the desktop names for it, in front of
// `owner` (null: no owner). True when the request was handed to the desktop; a link the browser refuses afterwards
// is not reported.
bool desktop_open_target(Window owner, const wchar_t* target) noexcept;

// Shows the file at `path` (NUL-terminated, absolute) in the file manager, selected where the manager can select.
bool desktop_reveal_file(const wchar_t* path) noexcept;

// Opens the system's notification settings; false where the system has no such page to open.
bool desktop_open_notification_settings(Window owner) noexcept;

// The system's alert sound for an input the program refuses (a field at its limit). Nothing where the desktop has no
// such sound for a program to ask for (Wayland has no portable bell).
void desktop_alert_beep() noexcept;

} // namespace maicord

#if defined(_WIN32)
#include "maicord/platform/desktop/windows/shell_open.inl"
#endif
