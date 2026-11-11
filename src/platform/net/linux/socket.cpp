// maicord/platform/net/socket.hpp on Linux.
//
// A socket's token is its descriptor + 1 in the low half and, once it is attached to a watch, that watch's epoll
// descriptor + 1 in the high half. A watch is a private epoll descriptor holding its one socket edge-triggered; the
// epoll descriptor is itself pollable, so it is the watch's waitable, and socket_watch_poll consumes what it
// reports. Windows event selection re-arms an event when a recv leaves data behind or an accept leaves a connection
// pending, and the callers rely on it by reading once per event: socket_receive and socket_accept do the same by
// re-registering the socket, which makes epoll report its current readiness again.
#include "internal.hpp"
#include "maicord/platform/net/socket.hpp"

namespace maicord {
namespace {

using namespace net_detail;

constexpr int kShutdownWrite = 1;
constexpr u32 kWatchEvents = kEpollIn | kEpollOut | kEpollReadHangUp | kEpollEdgeTriggered;
constexpr u8 kLoopbackAddress[4] = {127, 0, 0, 1};

int descriptor_of(const Socket& socket) noexcept {
    return static_cast<int>(socket.token & 0xffffffffu) - 1;
}

int watch_of(const Socket& socket) noexcept {
    return static_cast<int>(socket.token >> 32) - 1;
}

Socket wrap(int fd) noexcept {
    return Socket{static_cast<u64>(fd) + 1};
}

SocketListenResult listen_failure(int fd, i32 native_error,
                                  SocketListenError error = SocketListenError::failed) noexcept {
    if (fd >= 0) {
        sys_close(fd);
    }
    set_error(native_error);
    return SocketListenResult{error, native_error, 0};
}

u16 swap16(u16 value) noexcept {
    return static_cast<u16>((value >> 8) | (value << 8));
}

// Makes epoll report the socket's readiness again, as a new edge.
void rearm(const Socket& socket) noexcept {
    const int watch = watch_of(socket);
    if (watch >= 0) {
        EpollEvent event{kWatchEvents, 0};
        sys_epoll_ctl(watch, kEpollModify, descriptor_of(socket), &event);
    }
}

} // namespace

i32 socket_startup() noexcept {
    return 0;
}

void socket_cleanup() noexcept {
}

SocketListenResult socket_listen_loopback(Socket* listener, u16 first_port, u16 port_count, i32 backlog) noexcept {
    *listener = {};
    const long opened = sys_socket(kAfInet, kSockStream | kSockNonBlock | kSockCloseOnExec, 0);
    if (syscall_failed(opened)) {
        return listen_failure(-1, map_result(opened));
    }
    const int fd = static_cast<int>(opened);
    const int reuse = 1;
    const long option = sys_setsockopt(fd, kSolSocket, kSocketReuseAddress, &reuse, sizeof(reuse));
    if (syscall_failed(option)) {
        return listen_failure(fd, map_result(option));
    }
    SockAddrIn address{};
    address.family = kAfInet;
    copy_bytes(address.address, kLoopbackAddress, 4);
    bool bound = false;
    const u16 attempts = port_count ? port_count : 1;
    for (u16 offset = 0; offset < attempts; ++offset) {
        address.port = swap16(static_cast<u16>(first_port + offset));
        const long result = sys_bind(fd, &address, sizeof(address));
        bound = !syscall_failed(result);
        if (bound) {
            break;
        }
        const i32 error = map_result(result);
        if (!port_count || (error != WSAEADDRINUSE && error != WSAEACCES)) {
            return listen_failure(fd, error);
        }
    }
    if (!bound) {
        return listen_failure(fd, WSAEADDRINUSE, SocketListenError::ports_busy);
    }
    u32 address_bytes = sizeof(address);
    const long named = sys_getsockname(fd, &address, &address_bytes);
    if (syscall_failed(named)) {
        return listen_failure(fd, map_result(named));
    }
    const u16 port = swap16(address.port);
    if (address_bytes != sizeof(address) || port < 1024) {
        return listen_failure(fd, WSAEINVAL);
    }
    const long listening = sys_listen(fd, backlog);
    if (syscall_failed(listening)) {
        return listen_failure(fd, map_result(listening));
    }
    *listener = wrap(fd);
    return SocketListenResult{SocketListenError::none, 0, port};
}

bool socket_accept(Socket* listener, Socket* accepted, bool* from_loopback, i32* native_error) noexcept {
    SockAddrIn peer{};
    u32 peer_bytes = sizeof(peer);
    long result;
    do {
        result = sys_accept(descriptor_of(*listener), &peer, &peer_bytes, kSockNonBlock | kSockCloseOnExec);
    } while (result == -kErrorInterrupted);
    // Another connection may be waiting behind this one: report it again.
    rearm(*listener);
    if (syscall_failed(result)) {
        *native_error = map_result(result);
        return false;
    }
    *from_loopback = peer_bytes == sizeof(peer) && peer.family == kAfInet &&
                     bytes_equal(peer.address, kLoopbackAddress, 4);
    *accepted = wrap(static_cast<int>(result));
    return true;
}

i32 socket_send(const Socket& socket, const void* data, u32 bytes) noexcept {
    long result;
    do {
        result = sys_send(descriptor_of(socket), data, bytes, kMessageNoSignal);
    } while (result == -kErrorInterrupted);
    if (syscall_failed(result)) {
        map_result(result);
        return -1;
    }
    return static_cast<i32>(result);
}

i32 socket_receive(const Socket& socket, void* data, u32 bytes) noexcept {
    long result;
    do {
        result = sys_recv(descriptor_of(socket), data, bytes, 0);
    } while (result == -kErrorInterrupted);
    if (syscall_failed(result)) {
        map_result(result);
        return -1;
    }
    // A buffer filled to the brim may have left more behind.
    if (static_cast<u32>(result) == bytes) {
        rearm(socket);
    }
    return static_cast<i32>(result);
}

void socket_shutdown_send(const Socket& socket) noexcept {
    sys_shutdown(descriptor_of(socket), kShutdownWrite);
}

void socket_close(Socket* socket) noexcept {
    if (socket_valid(*socket)) {
        sys_close(descriptor_of(*socket));
    }
    *socket = {};
}

bool socket_watch_open(SocketWatch* watch) noexcept {
    *watch = {};
    const long epoll = sys_epoll_create(kEpollCloseOnExec);
    if (syscall_failed(epoll)) {
        map_result(epoll);
        return false;
    }
    watch->epoll = token(static_cast<int>(epoll));
    watch->waitable = waitable_borrow_fd(static_cast<int>(epoll));
    return true;
}

bool socket_watch_attach(SocketWatch* watch, Socket* socket, u32 mask) noexcept {
    EpollEvent event{kWatchEvents, mask};
    const long result = sys_epoll_ctl(descriptor(watch->epoll), kEpollAdd, descriptor_of(*socket), &event);
    if (syscall_failed(result)) {
        map_result(result);
        return false;
    }
    socket->token = (socket->token & 0xffffffffu) | (static_cast<u64>(watch->epoll) << 32);
    return true;
}

bool socket_watch_poll(SocketWatch* watch, const Socket&, u32* events) noexcept {
    *events = 0;
    EpollEvent reported[4];
    const long count = sys_epoll_wait(descriptor(watch->epoll), reported, 4, 0);
    if (syscall_failed(count)) {
        map_result(count);
        return false;
    }
    // Readability is both a read and, on a listener, an accept: the caller acts on the events it asked for.
    for (long index = 0; index < count; ++index) {
        const u32 raised = reported[index].events;
        if (raised & kEpollIn) {
            *events |= kSocketRead | kSocketAccept;
        }
        if (raised & kEpollOut) {
            *events |= kSocketWrite;
        }
        if (raised & (kEpollReadHangUp | kEpollHangUp | kEpollError)) {
            *events |= kSocketClose;
        }
    }
    return true;
}

void socket_watch_reset(SocketWatch* watch) noexcept {
    EpollEvent reported[4];
    while (sys_epoll_wait(descriptor(watch->epoll), reported, 4, 0) > 0) {
    }
}

void socket_watch_close(SocketWatch* watch) noexcept {
    if (watch->epoll != 0) {
        waitable_close(watch->waitable);
        sys_close(descriptor(watch->epoll));
    }
    *watch = {};
}

bool socket_bind_datagram_loopback(Socket* result, u16* port, i32* native_error) noexcept {
    *result = {};
    const long opened = sys_socket(kAfInet, kSockDatagram | kSockNonBlock | kSockCloseOnExec, 0);
    if (syscall_failed(opened)) {
        *native_error = map_result(opened);
        return false;
    }
    const int fd = static_cast<int>(opened);
    SockAddrIn address{};
    address.family = kAfInet;
    copy_bytes(address.address, kLoopbackAddress, 4);
    u32 address_bytes = sizeof(address);
    long status = sys_bind(fd, &address, sizeof(address));
    if (!syscall_failed(status)) {
        status = sys_getsockname(fd, &address, &address_bytes);
    }
    if (syscall_failed(status)) {
        *native_error = map_result(status);
        sys_close(fd);
        return false;
    }
    *port = swap16(address.port);
    *result = wrap(fd);
    return true;
}

i32 socket_receive_from(const Socket& socket, void* data, u32 bytes, NetworkAddress* from) noexcept {
    SockAddrStorage peer;
    u32 peer_bytes = sizeof(peer);
    long result;
    do {
        result = sys_recv_from(descriptor_of(socket), data, bytes, 0, &peer, &peer_bytes);
    } while (result == -kErrorInterrupted);
    if (syscall_failed(result)) {
        map_result(result);
        return -1;
    }
    if (from) {
        from_socket_address(&peer, peer_bytes, from);
    }
    return static_cast<i32>(result);
}

i32 socket_send_to(const Socket& socket, const void* data, u32 bytes, const NetworkAddress& to) noexcept {
    SockAddrStorage target;
    const u32 target_bytes = to_socket_address(to, &target);
    if (target_bytes == 0) {
        set_error(WSAEINVAL);
        return -1;
    }
    long result;
    do {
        result = sys_send_to(descriptor_of(socket), data, bytes, kMessageNoSignal, &target, target_bytes);
    } while (result == -kErrorInterrupted);
    if (syscall_failed(result)) {
        map_result(result);
        return -1;
    }
    return static_cast<i32>(result);
}

} // namespace maicord
