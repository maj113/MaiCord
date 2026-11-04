#pragma once

#include "maicord/core/base.hpp"

// Portal facts (org.freedesktop.portal.Desktop on the session bus, UI thread): whether this process is sandboxed, and
// which portal interfaces the desktop provides.

namespace maicord::dbus {

// Whether the process runs under Flatpak (/.flatpak-info exists) or Snap (SNAP is set): the cases where an application
// reaches the desktop through portals only.
bool portal_sandboxed() noexcept;

// The `version` property of a portal interface ("org.freedesktop.portal.Inhibit"); 0 when the portal, the interface or
// the bus is missing. A synchronous call on the session bus, at most `timeout_ms` long.
u32 portal_interface_version(const char* interface, u32 timeout_ms) noexcept;

// A probe only (nothing is inhibited): whether the Inhibit portal is there, which the idle-inhibit feature may use
// where the compositor lacks the Wayland protocol. Returns its version, 0 when absent.
u32 inhibit_portal_version(u32 timeout_ms) noexcept;

} // namespace maicord::dbus
