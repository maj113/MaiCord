// maicord/platform/net/socket.hpp on Windows: Winsock, with the calls and their order the loopback listeners and the
// updater transport made themselves before they used it.
#include "maicord/platform/net/socket.hpp"
#include "native.hpp"

namespace maicord {
namespace {

SOCKET raw(const Socket& socket) noexcept {
    return static_cast<SOCKET>(socket.token - 1);
}

// SOCKET + 1: INVALID_SOCKET becomes zero, "none".
Socket wrap(SOCKET value) noexcept {
    return Socket{static_cast<u64>(value) + 1};
}

SocketListenResult listen_failure(SOCKET listener, i32 native_error,
                                  SocketListenError error = SocketListenError::failed) noexcept {
    if (listener != INVALID_SOCKET) {
        closesocket(listener);
    }
    WSASetLastError(native_error);
    return SocketListenResult{error, native_error, 0};
}

} // namespace

i32 socket_startup() noexcept {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data);
}

void socket_cleanup() noexcept {
    WSACleanup();
}

SocketListenResult socket_listen_loopback(Socket* listener, u16 first_port, u16 port_count, i32 backlog) noexcept {
    *listener = {};
    const SOCKET value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (value == INVALID_SOCKET) {
        return listen_failure(value, WSAGetLastError());
    }
    const BOOL exclusive = TRUE;
    if (setsockopt(value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                   sizeof(exclusive)) == SOCKET_ERROR) {
        return listen_failure(value, WSAGetLastError());
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool bound = false;
    const u16 attempts = port_count ? port_count : 1;
    for (u16 offset = 0; offset < attempts; ++offset) {
        address.sin_port = htons(static_cast<u16>(first_port + offset));
        bound = bind(value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
        if (bound) {
            break;
        }
        const i32 error = WSAGetLastError();
        if (!port_count || (error != WSAEADDRINUSE && error != WSAEACCES)) {
            return listen_failure(value, error);
        }
    }
    if (!bound) {
        return listen_failure(value, WSAEADDRINUSE, SocketListenError::ports_busy);
    }
    int address_bytes = sizeof(address);
    if (getsockname(value, reinterpret_cast<sockaddr*>(&address), &address_bytes) == SOCKET_ERROR ||
        address_bytes != sizeof(address) || ntohs(address.sin_port) < 1024 ||
        listen(value, backlog) == SOCKET_ERROR) {
        return listen_failure(value, WSAGetLastError());
    }
    *listener = wrap(value);
    return SocketListenResult{SocketListenError::none, 0, ntohs(address.sin_port)};
}

bool socket_accept(Socket* listener, Socket* accepted, bool* from_loopback, i32* native_error) noexcept {
    sockaddr_in peer{};
    int peer_bytes = sizeof(peer);
    const SOCKET value = accept(raw(*listener), reinterpret_cast<sockaddr*>(&peer), &peer_bytes);
    if (value == INVALID_SOCKET) {
        *native_error = WSAGetLastError();
        return false;
    }
    *from_loopback = peer_bytes == sizeof(peer) && peer.sin_family == AF_INET &&
                     peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    *accepted = wrap(value);
    return true;
}

i32 socket_send(const Socket& socket_value, const void* data, u32 bytes) noexcept {
    return send(raw(socket_value), static_cast<const char*>(data), static_cast<int>(bytes), 0);
}

i32 socket_receive(const Socket& socket_value, void* data, u32 bytes) noexcept {
    return recv(raw(socket_value), static_cast<char*>(data), static_cast<int>(bytes), 0);
}

void socket_shutdown_send(const Socket& socket_value) noexcept {
    shutdown(raw(socket_value), SD_SEND);
}

void socket_close(Socket* socket_value) noexcept {
    if (socket_valid(*socket_value)) {
        closesocket(raw(*socket_value));
    }
    *socket_value = {};
}

bool socket_watch_open(SocketWatch* watch) noexcept {
    *watch = {};
    watch->waitable = WSACreateEvent();
    return watch->waitable != WSA_INVALID_EVENT;
}

bool socket_watch_attach(SocketWatch* watch, Socket* socket_value, u32 mask) noexcept {
    return WSAEventSelect(raw(*socket_value), watch->waitable, static_cast<long>(mask)) != SOCKET_ERROR;
}

bool socket_watch_poll(SocketWatch* watch, const Socket& socket_value, u32* events) noexcept {
    WSANETWORKEVENTS reported{};
    if (WSAEnumNetworkEvents(raw(socket_value), watch->waitable, &reported) == SOCKET_ERROR) {
        return false;
    }
    *events = static_cast<u32>(reported.lNetworkEvents);
    return true;
}

void socket_watch_reset(SocketWatch* watch) noexcept {
    WSAResetEvent(watch->waitable);
}

void socket_watch_close(SocketWatch* watch) noexcept {
    if (watch->waitable != WSA_INVALID_EVENT) {
        WSACloseEvent(watch->waitable);
    }
    *watch = {};
}

} // namespace maicord
