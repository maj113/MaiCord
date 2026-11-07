#pragma once

#include "maicord/platform/atspi/server.hpp"
#include "maicord/platform/dbus/connection.hpp"

// The server's state, shared by server.cpp (the connection) and events.cpp (the signals); private to them.

namespace maicord::atspi {

struct Server {
    Endpoint endpoint;
    dbus::Connection* session;  // the session connection the hook last saw (never owned)
    dbus::Connection* bus;      // the accessibility bus connection, owned
    u32 name_watch;
    u32 status_subscription;
    u32 request;  // identifies the GetAddress call a reply belongs to
    bool started;
    bool address_pending;
    bool embedded;
    // Events: what the last publish left.
    u64 focus_key;
    bool active;
    u64 text_key;
    u32 text_bytes;
    char text[kUiAccessValueMaximumBytes];
};

extern Server g_server;

// Forgets what the event stream remembers (a new connection starts from the current state, silently).
void events_reset() noexcept;

} // namespace maicord::atspi
