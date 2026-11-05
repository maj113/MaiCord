#pragma once

#include "maicord/core/base.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/dbus/connection.hpp"

// The process's shared session bus connection, owned by the UI thread: the window loop waits on it
// (session_bus_waitable) and processes it (session_bus_process), and the desktop services (settings, notifications,
// open-uri, portal probes) make their calls and subscriptions on it. A connection that ends is dropped and, when someone
// still wants a bus, opened again after a pause; the open hooks then re-establish each service's subscriptions.
//
// Not thread-safe: every function here belongs to the UI thread. (secret_service.hpp opens connections of its own and
// may be called from any thread.)

namespace maicord::dbus {

// Opens the connection when there is none and the pause after a failure has passed; true when a live connection exists.
// After the first call the loop keeps trying to have one (session_bus_process).
bool session_bus_open() noexcept;
// The live connection or null; never connects.
Connection* session_bus() noexcept;
// Incremented on every new connection: a service compares it with the one it subscribed on.
u32 session_bus_generation() noexcept;

// Called with each new connection, right after it opens (and again after a reconnect), on the UI thread. At most 12.
using SessionBusHook = void (*)(Connection* connection) noexcept;
bool session_bus_add_hook(SessionBusHook hook) noexcept;

// For the window loop: a waitable for the socket (null without a connection), the milliseconds until the bus needs
// attention without traffic (kNoTimeout when none), and the dispatch of what arrived.
Waitable session_bus_waitable() noexcept;
u32 session_bus_wait_ms(u64 now_ms) noexcept;
void session_bus_process(u64 now_ms) noexcept;

// A further connection the window loop serves along with the session bus (the accessibility bus): its waitable wakes the
// loop, its next due time bounds the sleep (session_bus_wait_ms includes it), and it is processed with the session bus.
// At most kSessionBusExtraMaximum; they live as long as the process.
inline constexpr u32 kSessionBusExtraMaximum = 4;
struct SessionBusExtra {
    Waitable (*waitable)() noexcept;  // null while there is no connection
    u32 (*wait_ms)(u64 now_ms) noexcept;  // kNoTimeout when nothing is due
    void (*process)(u64 now_ms) noexcept;
};
bool session_bus_add_extra(const SessionBusExtra& extra) noexcept;
// The extras' waitables (those that have one), at most `capacity`; the count.
u32 session_bus_extra_waitables(Waitable* out, u32 capacity) noexcept;

// Pumps the bus (waiting on it) until `*done` or the time passes; for the start of a service that wants an answer
// before the first frame. True when `*done` became true.
bool session_bus_wait_for(const volatile bool* done, u32 timeout_ms) noexcept;

// Closes the connection and forgets that a bus is wanted (tests, shutdown).
void session_bus_close() noexcept;

} // namespace maicord::dbus
