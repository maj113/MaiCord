#pragma once

#include "maicord/platform/dbus/connection.hpp"

// Helpers over a connection for the services that own names and export objects: name ownership and watching, the
// org.freedesktop.DBus.Properties and Introspectable interfaces of an exported object, and portal Request handles.

namespace maicord::dbus {

// Names ----------------------------------------------------------------------------------------------------------------

// Gives up a name this connection owns. True when the bus says it was released.
bool connection_release_name(Connection* connection, const char* name, u32 timeout_ms) noexcept;

// Called with the name's owner state: once after the watch starts (the answer to GetNameOwner) and on each change.
using NameWatchHandler = void (*)(void* context, const char* name, bool owned) noexcept;
// Watches who owns `name` (NameOwnerChanged filtered on it). Returns an id for connection_unwatch_name, 0 when the table
// (8 watches) is full. The first call of the handler comes from the connection's processing, never from this call.
u32 connection_watch_name(Connection* connection, const char* name, NameWatchHandler handler, void* context) noexcept;
void connection_unwatch_name(Connection* connection, u32 id) noexcept;

// Properties and introspection -----------------------------------------------------------------------------------------

// Writes the variant (open_variant, the value, close_variant) of the property at `index` of the object's list to `body`.
using PropertyWriter = void (*)(u32 index, Builder* body) noexcept;

// `properties` names those of `interface`, NUL-terminated end to end with an empty name after the last (the string literal
// "A\0B\0" is that), in the order `write` takes its index.
struct ExportedObject {
    const char* interface;
    const char* properties;  // null for none
    PropertyWriter write;
    const char* introspection;  // the object's own interfaces as XML (the node and standard interfaces are added); null for none
};

// Answers org.freedesktop.DBus.Properties (Get, GetAll; Set is refused as read-only), Introspectable.Introspect and
// nothing else. True when `call` was one of those and a reply (or error) was sent.
bool object_handle_standard(Connection* connection, const Message& call, const ExportedObject& object) noexcept;

// Portal requests ------------------------------------------------------------------------------------------------------

// A portal method that returns a Request handle answers later with Request.Response(u code, a{sv} results) on that handle.
// The handle's path is known from the caller's unique name and the handle_token passed in the call's options, so the
// listener is subscribed before the call is made and no response is missed.
//   code 0 success, 1 the user cancelled, 2 anything else.
using PortalResponseHandler = void (*)(void* context, u32 code, Reader* results) noexcept;

struct PortalRequest {
    Connection* connection;
    u32 subscription;
    PortalResponseHandler handler;
    void* context;
    char token[40];
    char path[200];
    bool active;
};

// Fills `token` (the handle_token to put in the call's options) and subscribes the Response listener; false when the
// subscription failed. Pair it with portal_request_end (also called by the listener when the response arrives).
bool portal_request_begin(PortalRequest* request, Connection* connection, PortalResponseHandler handler,
                          void* context) noexcept;
// Stops listening (the call failed, or the request is abandoned).
void portal_request_end(PortalRequest* request) noexcept;
// The method's return carried the handle path (its first value); when it differs from the predicted one (portals
// before version 0.9 ignore handle_token) the listener moves to it.
void portal_request_adopt_reply(PortalRequest* request, const Message& reply) noexcept;
// Appends `handle_token` to an options array being built (inside an open "{sv}" array).
void portal_request_append_token(Builder* options, const PortalRequest& request) noexcept;
// Request.Close on the live handle.
void portal_request_close(PortalRequest* request) noexcept;

} // namespace maicord::dbus
