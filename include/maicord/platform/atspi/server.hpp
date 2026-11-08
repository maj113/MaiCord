#pragma once

#include "maicord/core/base.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/atspi/accessible.hpp"

// The AT-SPI server (Linux): the application's presence on the accessibility bus, for screen readers, magnifiers and the
// other assistive technologies that speak org.a11y.atspi, over the in-house D-Bus client (src/platform/dbus).
//
// Nothing is connected until it is wanted. The server watches the session bus for org.a11y.Bus and reads
// org.a11y.Status: when IsEnabled or ScreenReaderEnabled is true (a screen reader started, or the desktop's
// accessibility switch is on) it asks org.a11y.Bus.GetAddress, connects to the accessibility bus, and registers the
// application with the registry (org.a11y.atspi.Socket.Embed). The exported objects answer from the snapshot's owner
// (HostCallbacks) on the UI thread; events (focus, state, text, children, announcements) go out as signals once a client
// has called. When both flags go false, or the bus goes away, the connection is closed.
//
// UI thread only. The window loop waits on server_waitable() and calls server_process(); session_bus_add_extra
// (session.hpp) is the loop's hook for that.

namespace maicord::atspi {

// Starts watching the session bus. The callbacks must outlive the server. Returns false when already started.
bool server_start(const HostCallbacks& host) noexcept;
// Disconnects and forgets the session bus watches.
void server_stop() noexcept;

// Registered with the registry: a screen reader can see the application.
bool server_connected() noexcept;
// A client has called an exported object since the connection: it is worth building snapshots and sending events.
bool server_queried() noexcept;

Waitable server_waitable() noexcept;
u32 server_wait_ms(u64 now_ms) noexcept;
// `readable` as dbus::connection_process's.
void server_process(u64 now_ms, bool readable = true) noexcept;

// The events the difference between two snapshots calls for (the plan of ui_access_plan_events over `tree`), as signals.
void server_publish(const UiAccessTree& tree, const UiAccessEventPlan& plan) noexcept;
// Speech that belongs to no node (status changes, new messages): an Announcement on the node with `key`, or on the window
// when the key is 0.
void server_announce(u64 key, Utf8View text, bool assertive) noexcept;

} // namespace maicord::atspi
