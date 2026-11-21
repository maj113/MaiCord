#pragma once

#include "maicord/core/base.hpp"

// Whether the desktop is logging out, for the close of the main window (UI thread, on the session bus).
//
// KDE Plasma logs out in two steps. Its session manager, ksmserver, starts the logout. Then KWin closes every window
// with the same xdg_toplevel.close that the close button sends, and waits for each window to go. A window that hides
// to the tray then keeps MaiCord running into the end of the session, so the close quits instead.
//
// Other desktops do not close windows at logout: GNOME stops the applications with SIGTERM. The session monitor of the
// Inhibit portal reports a logout on GNOME, but Plasma does not send its states through that portal.

namespace maicord::dbus {

// ksmserver's isShuttingDown (org.kde.ksmserver, /KSMServer): true from the start of a logout until it ends or the user
// cancels it. False without the service (another desktop), without a session bus and on any error. A synchronous call
// of at most 500 ms that never starts the service.
bool logout_in_progress() noexcept;

} // namespace maicord::dbus
