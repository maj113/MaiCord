#pragma once

#include "maicord/core/base.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/net/network.hpp"

// Plain sockets, for the code that does not run on a NetworkLoop: the loopback listeners of the browser sign-in and
// the captcha bridge (which wait on events next to their own), and the updater's blocking HTTPS transport. Windows
// calls Winsock (WSAEventSelect for the watches, closesocket to interrupt), Linux the system calls (an epoll
// descriptor per watch, shutdown to interrupt). Errors are the Win32/Winsock numbers, as in network.hpp.

namespace maicord {

// A socket by token (Windows SOCKET + 1, Linux descriptor + 1), so zero-initialized storage holds none. The
// high half of a Linux token remembers the watch the socket is attached to.
struct Socket {
    u64 token;
};

inline bool socket_valid(const Socket& socket) noexcept {
    return socket.token != 0;
}

// Winsock needs WSAStartup before the first socket and WSACleanup after the last; Linux needs nothing. Startup
// returns 0, or the error code.
i32 socket_startup() noexcept;
void socket_cleanup() noexcept;

enum class SocketListenError : u8 {
    none,
    failed,
    // Every port of the range was in use.
    ports_busy,
};

struct SocketListenResult {
    SocketListenError error;
    i32 native_error;
    // The port that was bound (the one of the range, or the one the system picked for port 0).
    u16 port;

    constexpr bool ok() const noexcept {
        return error == SocketListenError::none;
    }
};

// A listening TCP socket on 127.0.0.1 that no other process can share the port with (Windows
// SO_EXCLUSIVEADDRUSE; Linux SO_REUSEADDR, which still refuses a second listener). It binds the first free port of
// first_port .. first_port + port_count - 1; with port_count 0 it binds first_port alone, which may be 0 for any port.
// A port taken by another process (WSAEADDRINUSE, WSAEACCES) moves on to the next; any other error fails. A bound
// port below 1024 is refused. On failure the socket is closed and `*listener` is none.
SocketListenResult socket_listen_loopback(Socket* listener, u16 first_port, u16 port_count, i32 backlog) noexcept;

// Accepts one pending connection without blocking. False when none is pending (*native_error is WSAEWOULDBLOCK) or
// on failure. `*from_loopback` tells whether the peer is IPv4 127.0.0.1. The accepted socket is non-blocking.
bool socket_accept(Socket* listener, Socket* accepted, bool* from_loopback, i32* native_error) noexcept;

// A blocking TCP socket of the family of `address`, with `timeout_ms` as its send and receive timeout; and its
// connection to `address` (it blocks for at most the timeout). The two are separate so that a thread that may be
// asked to interrupt can publish the socket between them.
bool socket_create_stream(Socket* socket, const NetworkAddress& address, u32 timeout_ms, i32* native_error) noexcept;
bool socket_connect(const Socket& socket, const NetworkAddress& address, i32* native_error) noexcept;

// Bytes sent or received (0 from socket_receive is an orderly close), or -1 with network_last_error() set. The
// calls of a non-blocking socket fail with WSAEWOULDBLOCK instead of waiting; a receive that fills its buffer, and
// an accept, leave the socket's watch signaled again if more is pending, as Windows event selection does.
i32 socket_send(const Socket& socket, const void* data, u32 bytes) noexcept;
i32 socket_receive(const Socket& socket, void* data, u32 bytes) noexcept;
// Ends sending: the peer reads to the end of the response (SD_SEND / SHUT_WR).
void socket_shutdown_send(const Socket& socket) noexcept;
// Makes a thread blocked in this socket's calls return, from any thread; the owner still closes the socket. (On
// Windows closing wakes the call, so this does nothing there; on Linux close does not.)
void socket_interrupt(const Socket& socket) noexcept;
// Closes the socket and leaves none.
void socket_close(Socket* socket) noexcept;

// The events a watch reports: the FD_* values of Winsock.
inline constexpr u32 kSocketRead = 1u << 0;
inline constexpr u32 kSocketWrite = 1u << 1;
inline constexpr u32 kSocketAccept = 1u << 3;
inline constexpr u32 kSocketClose = 1u << 5;

// A waitable that is signaled while the socket attached to it has events to report. One watch serves one socket at
// a time; attaching another replaces it (the owner closes the old one).
struct SocketWatch {
    Waitable waitable;
    // Linux: the epoll descriptor + 1 that `waitable` borrows; Windows: unused.
    u32 epoll;
};
bool socket_watch_open(SocketWatch* watch) noexcept;
// Starts reporting events of `socket` (a listener asks for kSocketAccept, a connection for the others, as with
// WSAEventSelect). A socket from socket_listen_loopback or socket_accept is already non-blocking, as event
// selection makes it on Windows. Linux may also report events that were not in `mask`; callers act on the ones they
// asked for.
bool socket_watch_attach(SocketWatch* watch, Socket* socket, u32 mask) noexcept;
inline Waitable socket_watch_waitable(const SocketWatch& watch) noexcept {
    return watch.waitable;
}
// The events that happened since the last call; false when the system cannot say (treat the socket as lost). It
// consumes them: the waitable is signaled again only by new events.
bool socket_watch_poll(SocketWatch* watch, const Socket& socket, u32* events) noexcept;
// Clears a signaled watch that has no socket.
void socket_watch_reset(SocketWatch* watch) noexcept;
void socket_watch_close(SocketWatch* watch) noexcept;

// getaddrinfo on the calling thread, for a host name and a service (both UTF-16, ASCII): the first
// kNetworkResolverMaximumAddresses IPv4 and IPv6 answers. False with *native_error when the lookup fails or has no
// usable answer.
bool network_resolve_blocking(const wchar_t* host, const wchar_t* service, NetworkAddress* addresses, u32* count,
                              i32* native_error) noexcept;

// A UDP socket bound to 127.0.0.1 and a port the system picks, for the peer a connected NetworkDatagram talks to.
// Attach it to a watch before reading: its calls do not block.
bool socket_bind_datagram_loopback(Socket* socket, u16* port, i32* native_error) noexcept;
// Receives one datagram and says where it came from (0 bytes is an empty datagram), or sends one to `to`; -1 with
// network_last_error() set on failure, WSAEWOULDBLOCK when there is nothing to read.
i32 socket_receive_from(const Socket& socket, void* data, u32 bytes, NetworkAddress* from) noexcept;
i32 socket_send_to(const Socket& socket, const void* data, u32 bytes, const NetworkAddress& to) noexcept;

} // namespace maicord
