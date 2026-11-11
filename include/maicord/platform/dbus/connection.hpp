#pragma once

#include "maicord/core/base.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/dbus/wire.hpp"

// An in-house D-Bus client over the bus's Unix socket (Linux): the session bus from DBUS_SESSION_BUS_ADDRESS (unix:path
// and unix:abstract forms; $XDG_RUNTIME_DIR/bus when unset), SASL EXTERNAL with the uid, Hello, NEGOTIATE_UNIX_FD,
// method calls with replies and timeouts, signals through AddMatch, exported method handling, and UNIX_FD passing
// (SCM_RIGHTS).
//
// A connection is single-threaded: every call on one connection comes from one thread at a time, and the handlers run
// inside connection_process (or a synchronous call) on the calling thread. To wake a loop on bus traffic wait on
// connection_waitable() (it is signaled while bytes can be read) with connection_wait_ms() as the timeout bound, then
// call connection_process().

namespace maicord::dbus {

struct Connection;

enum class Status : u8 {
    ok,            // a method return arrived
    error_reply,   // an error message arrived (the Message names it)
    timeout,       // no reply within the time
    disconnected,  // the connection ended first
    failed,        // the call could not be made (bad arguments, a full table, a failed body)
};

inline constexpr u32 kNoTimeout = 0xFFFFFFFFu;  // for connection_wait_ms: nothing is due

// Opening. A failed open logs why and returns null.
Connection* connection_open_session() noexcept;
// `address` is a D-Bus address string ("unix:path=/run/user/1000/bus,guid=..."; several separated by ';').
Connection* connection_open_address(const char* address) noexcept;
void connection_close(Connection* connection) noexcept;  // pending calls complete with Status::disconnected

bool connection_alive(const Connection* connection) noexcept;
const char* connection_unique_name(const Connection* connection) noexcept;  // ":1.42"
bool connection_unix_fds(const Connection* connection) noexcept;           // whether the bus agreed to pass descriptors
int connection_fd(const Connection* connection) noexcept;
// A level-triggered waitable over the socket (owned by the connection).
Waitable connection_waitable(Connection* connection) noexcept;
// Milliseconds until the connection needs processing without incoming bytes (the earliest call timeout, or a short
// wait while output is queued); kNoTimeout when nothing is due.
u32 connection_wait_ms(const Connection* connection, u64 now_ms) noexcept;
// Reads what has arrived, dispatches replies, signals and method calls to their handlers, expires timed-out calls and
// flushes queued output. `readable` false (the caller's wait has just seen the socket not ready) skips the read: bytes
// that arrive later keep the socket readable for the next wait.
void connection_process(Connection* connection, u64 now_ms, bool readable = true) noexcept;

// Method calls -------------------------------------------------------------------------------------------------------

struct CallTarget {
    const char* destination;  // a bus name; null for a peer connection without a bus
    const char* path;
    const char* interface;  // may be null
    const char* member;
};

// `reply` is the method return or error message (null for timeout, disconnected and failed); valid only during the call.
using ReplyHandler = void (*)(void* context, Status status, const Message* reply) noexcept;

// Sends a method call. With a handler the call is pending until its reply or `timeout_ms`; without one the call is sent
// with no-reply-expected. `body` may be null; the caller releases it either way. False when nothing was sent.
// `flags` are MessageFlag bits added to the call (flag_no_auto_start: do not start the service to answer it).
bool connection_call(Connection* connection, const CallTarget& target, const Builder* body, u32 timeout_ms,
                     ReplyHandler handler, void* context, u8 flags = 0) noexcept;
// Sends a call and waits for the reply, dispatching other traffic meanwhile. On Status::ok or Status::error_reply
// `*reply` holds the message (the caller releases it with message_release).
Status connection_call_sync(Connection* connection, const CallTarget& target, const Builder* body, u32 timeout_ms,
                            Message* reply) noexcept;
// What a failed call reports: the error name of an error reply, else `fallback` (the handler's `reply` may be null).
const char* reply_error_name(Status status, const Message* reply, const char* fallback) noexcept;

// Signals and exported methods ---------------------------------------------------------------------------------------

struct SignalMatch {
    const char* sender;     // a bus name for the bus's filter; not compared locally; may be null
    const char* path;       // compared exactly; may be null
    const char* interface;  // may be null
    const char* member;     // may be null
    const char* arg0;       // the bus's filter on the first string argument; may be null
};

using SignalHandler = void (*)(void* context, const Message& signal) noexcept;

// AddMatch on the bus (not waited for: later calls on this connection are ordered after it) and a local handler for the
// signals that match path, interface, member and arg0. Returns an id (nonzero) for connection_unsubscribe; 0 when the
// table is full or the call could not be sent. Local matching ignores `sender`: a well-known name is not the signal's
// unique sender.
u32 connection_subscribe(Connection* connection, const SignalMatch& match, SignalHandler handler, void* context) noexcept;
// RemoveMatch and forgets the handler; the id may be reused by a later subscription.
void connection_unsubscribe(Connection* connection, u32 id) noexcept;

// Called for each method call addressed to this connection; return true after replying (connection_reply or
// connection_reply_error, now or later); false answers org.freedesktop.DBus.Error.UnknownMethod.
using MethodHandler = bool (*)(void* context, Connection* connection, const Message& call) noexcept;
// The handler of calls to no exported object.
void connection_set_method_handler(Connection* connection, MethodHandler handler, void* context) noexcept;
// Exports an object: calls whose path is exactly `path` go to `handler` (at most 16 objects); a handler that returns
// false falls through to the connection's default handler, then to UnknownMethod.
bool connection_export_object(Connection* connection, const char* path, MethodHandler handler, void* context) noexcept;
void connection_unexport_object(Connection* connection, const char* path) noexcept;

bool connection_reply(Connection* connection, const Message& call, const Builder* body) noexcept;
bool connection_reply_error(Connection* connection, const Message& call, const char* name, const char* text) noexcept;
bool connection_emit(Connection* connection, const char* path, const char* interface, const char* member,
                     const Builder* body) noexcept;
// RequestName: true when this connection became the primary owner.
bool connection_request_name(Connection* connection, const char* name, u32 timeout_ms) noexcept;

// Blocking socket helpers, for the sockets of this directory that are not bus connections too -------------------------

// Waits for `fd` (poll `events`) until `deadline` (monotonic ms); false on timeout or error.
bool poll_until(int fd, i16 events, u64 deadline) noexcept;
// Sends all `count` bytes by `deadline`, waiting while the socket is full; false on error or timeout.
bool send_all_blocking(int fd, const u8* bytes, u32 count, u64 deadline) noexcept;

// Addresses ----------------------------------------------------------------------------------------------------------

struct SocketAddress {
    u8 data[112];  // a sockaddr_un
    u32 length;
};

// The usable unix: entries of a D-Bus address string, in order (other transports are skipped); returns how many were
// written to `out` (at most `capacity`).
u32 address_parse(const char* address, SocketAddress* out, u32 capacity) noexcept;

} // namespace maicord::dbus
