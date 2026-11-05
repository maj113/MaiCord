#pragma once

#include "maicord/core/base.hpp"
#include "maicord/platform/atspi/map.hpp"
#include "maicord/platform/dbus/wire.hpp"

// The objects the application exports on the accessibility bus (Linux): the answers to the method calls a screen reader
// makes on the application root and on each snapshot node, over org.a11y.atspi.Accessible, Component, Action, Text,
// EditableText, Value, Selection and, on the root, Application, and org.freedesktop.DBus.Properties. Nothing here touches
// a connection: answer_call turns one call message into a reply body, so the connection (server.hpp) and the tests share
// it. Every function belongs to the UI thread, which the snapshot's owner serves.
//
// Coordinates are the snapshot's logical pixels (DIPs) with the window's own origin: Wayland does not tell a client where
// its window lies on the screen, so screen coordinates equal window coordinates.

namespace maicord::atspi {

struct HostCallbacks {
    void* context;
    // The current finished snapshot; the owner rebuilds a stale one here.
    const UiAccessTree* (*tree)(void* context) noexcept;
    // Requests an action for the node with this key; number carries a range value or a text selection
    // (ui_access_text_selection_pack), text a new value. Must not block (it is deferred to the UI loop).
    bool (*act)(void* context, u64 key, UiAccessAction action, double number, Utf8View text) noexcept;
    // The height of the caption above the snapshot's origin, in snapshot units.
    float (*content_top)(void* context) noexcept;
    // The window is the active one (it reports ACTIVE, and its focused node FOCUSED and the focus events follow it).
    bool (*window_active)(void* context) noexcept;
    // Optional: rectangles (snapshot units, one per line) covering bytes [start, end) of a text node; the count written.
    u32 (*text_bounds)(void* context, u64 key, u32 start, u32 end, UiRect* rectangles, u32 capacity) noexcept;
};

inline constexpr u32 kNameBytes = 64;

struct Endpoint {
    HostCallbacks host;
    char bus_name[kNameBytes];       // this connection's unique name on the accessibility bus
    char parent_name[kNameBytes];    // where the registry placed the application (empty before Socket.Embed answers)
    char parent_path[96];
    char registry_sender[kNameBytes];  // the registry's unique name: its calls do not count as a screen reader's
    i32 id;                          // Application.Id, set by the registry
    bool queried;                    // a client other than the registry has called
};

struct Failure {
    const char* name;
    const char* text;
};

enum class Outcome : u8 {
    unhandled,  // not an interface or member this object has: the connection answers UnknownMethod
    replied,    // `reply` is the method return
    failed,     // `failure` names the error
};

// Answers one method call addressed to an exported path. `reply` must be a fresh builder (the caller releases it).
Outcome answer_call(Endpoint* endpoint, const dbus::Message& call, dbus::Builder* reply, Failure* failure) noexcept;

// An object reference, the (so) of the AT-SPI wire: a bus name and a path.
void append_reference(dbus::Builder* builder, const char* name, const char* path) noexcept;
// The application's own reference to a snapshot node (key 0 is a real key; the root has its own path, kRootPath).
void append_node_reference(dbus::Builder* builder, const Endpoint& endpoint, u64 key) noexcept;

} // namespace maicord::atspi
