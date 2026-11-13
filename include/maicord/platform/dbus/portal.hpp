#pragma once

#include "maicord/core/base.hpp"

// Portal facts (org.freedesktop.portal.Desktop on the session bus, UI thread): whether this process is sandboxed, and
// which portal interfaces the desktop provides.

namespace maicord::dbus {

struct Connection;

// Whether the process runs under Flatpak (/.flatpak-info exists) or Snap (SNAP is set): the cases where an application
// reaches the desktop through portals only.
bool portal_sandboxed() noexcept;

// A u property of a portal interface ("version" of "org.freedesktop.portal.Inhibit"); 0 when the portal, the interface
// or the property is missing. A synchronous call on `connection`, at most `timeout_ms` long.
u32 portal_property(Connection* connection, const char* interface, const char* name, u32 timeout_ms) noexcept;
// The `version` property of a portal interface on the session bus; 0 also when the bus is missing.
u32 portal_interface_version(const char* interface, u32 timeout_ms) noexcept;

// The window the portals' dialogs belong to, as the portals name it ("wayland:<handle>"; the window backend sets it once
// the compositor has given the handle and clears it with the window). The FileChooser, OpenURI, GlobalShortcuts,
// Screenshot and ScreenCast requests send it as parent_window. "" when there is none.
void portal_set_parent_window(const char* identifier) noexcept;
const char* portal_parent_window() noexcept;

} // namespace maicord::dbus
