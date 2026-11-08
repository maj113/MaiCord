#pragma once

#include "maicord/core/base.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/dbus/connection.hpp"

// Single instance and protocol links, the Linux twin of the Windows named mutex and WM_COPYDATA in
// platform/desktop/integration.hpp: the first process owns the profile; a later ordinary launch hands its request (a
// discord:// or maicord:// link, or just "show yourself") to the first one and exits.
//
// The channel is org.freedesktop.Application (Activate and Open) on a well-known session bus name, the application id
// ("io.github.maj113.MaiCord", which is also the .desktop file's basename), so launchers and `gio open` reach it too;
// without a session bus an abstract Unix socket carries the same request (the peer's uid is checked). The name and the
// socket are per profile when a profile key is given, so a test profile never reaches the user's client.
//
// UI thread. The primary's requests arrive inside the session bus processing, or in instance_process() for the socket.

namespace maicord::dbus {

// 2048 bytes of package URL plus the scheme and path around it, as DesktopLaunch::url.
inline constexpr u32 kLaunchUrlCapacity = 2200;
inline constexpr u32 kLaunchTokenCapacity = 128;

struct LaunchRequest {
    bool startup;      // --startup: started by the session's autostart
    bool url_present;
    u32 url_bytes;
    char url[kLaunchUrlCapacity];  // UTF-8, kept verbatim; the link parser validates it
    char activation_token[kLaunchTokenCapacity];  // an xdg-activation token for focus, "" when none
};

struct InstanceConfig {
    const char* application_id;  // "io.github.maj113.MaiCord": a valid D-Bus name
    const char* profile_key;     // null or "" for the default profile; any other string names another profile
};

enum class InstanceResult : u8 {
    primary,    // this process owns the profile: go on and start
    forwarded,  // the running instance has the request: exit
    refused,    // another process owns the profile and did not take the request: exit, never start a second client
    unguarded,  // neither a session bus nor a socket could be used: start without the guard (logged)
};

// Takes the profile or hands `request` to its owner (waiting up to `timeout_ms` for that owner's reply).
// MAICORD_INSTANCE_BACKEND=bus|socket forces a channel (the default tries the bus first).
InstanceResult instance_acquire(const InstanceConfig& config, const LaunchRequest& request, u32 timeout_ms) noexcept;

// Called for each request that reaches the primary: a forwarded launch, or Activate/Open from a launcher. Requests that
// arrive before a handler is set wait (the last one is kept).
using InstanceHandler = void (*)(void* context, const LaunchRequest& request) noexcept;
void instance_set_handler(InstanceHandler handler, void* context) noexcept;

// The socket backend's waitable (null on the bus backend, where the session bus waitable covers it) and its processing.
Waitable instance_waitable() noexcept;
void instance_process() noexcept;

void instance_release() noexcept;

// "io.github.maj113.MaiCord" or "io.github.maj113.MaiCord.p<16 hex>" for a profile key, and the object path of that name
// ("/io/github/maj113/MaiCord"). False when `out` is too small or the id is not a valid D-Bus name.
bool instance_bus_name(const char* application_id, const char* profile_key, char* name, u32 name_capacity, char* path,
                       u32 path_capacity) noexcept;

// The abstract Unix socket the socket backend binds for this id and profile (a sockaddr_un, see SocketAddress): for
// tests that talk to it directly.
void instance_socket_address(const char* application_id, const char* profile_key, SocketAddress* address) noexcept;

} // namespace maicord::dbus
