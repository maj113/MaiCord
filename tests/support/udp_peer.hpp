#pragma once
// The peer a connected NetworkDatagram talks to in the network tests: a UDP socket bound to 127.0.0.1 and a port the
// system picks, as a Socket the platform layer can watch (platform/net/socket.hpp: a Windows SOCKET + 1, a Linux
// descriptor + 1 in the token's low half). Attach it to a watch before reading: its calls do not block. Receive (`from`
// may be null) and send return the bytes (0 is an empty datagram), or -1 with network_last_error() set, WSAEWOULDBLOCK
// when there is nothing to read.
#include "maicord/platform/net/socket.hpp"
#if defined(_WIN32)
#include "../../src/platform/net/native.hpp"
#else
#include "../../src/platform/net/linux/internal.hpp"
#include "maicord/core/intrinsics.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"
#endif

namespace maicord {

#if defined(_WIN32)
inline bool udp_peer_bind(Socket* result, u16* port, i32* native_error) noexcept {
    *result = {};
    const SOCKET value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (value == INVALID_SOCKET) {
        *native_error = WSAGetLastError();
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_bytes = sizeof(address);
    if (bind(value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        getsockname(value, reinterpret_cast<sockaddr*>(&address), &address_bytes) == SOCKET_ERROR) {
        *native_error = WSAGetLastError();
        closesocket(value);
        return false;
    }
    *port = ntohs(address.sin_port);
    *result = Socket{static_cast<u64>(value) + 1};
    return true;
}

inline i32 udp_peer_receive_from(const Socket& peer, void* data, u32 bytes, NetworkAddress* from) noexcept {
    sockaddr_storage source;
    int source_bytes = sizeof(source);
    const int received = recvfrom(static_cast<SOCKET>(peer.token - 1), static_cast<char*>(data), static_cast<int>(bytes),
                                  0, reinterpret_cast<sockaddr*>(&source), &source_bytes);
    if (received >= 0 && from)
        network_address_from_socket(reinterpret_cast<const sockaddr*>(&source), static_cast<usize>(source_bytes), from);
    return received;
}

inline i32 udp_peer_send_to(const Socket& peer, const void* data, u32 bytes, const NetworkAddress& to) noexcept {
    sockaddr_storage target;
    const i32 target_bytes = socket_address(to, &target);
    if (target_bytes == 0) {
        WSASetLastError(WSAEINVAL);
        return -1;
    }
    return sendto(static_cast<SOCKET>(peer.token - 1), static_cast<const char*>(data), static_cast<int>(bytes), 0,
                  reinterpret_cast<const sockaddr*>(&target), target_bytes);
}
#else
inline int udp_peer_descriptor(const Socket& peer) noexcept {
    return static_cast<int>(peer.token & 0xffffffffu) - 1;
}

inline bool udp_peer_bind(Socket* result, u16* port, i32* native_error) noexcept {
    *result = {};
    const long opened = sys_socket(kAfInet, kSockDatagram | kSockNonBlock | kSockCloseOnExec, 0);
    if (syscall_failed(opened)) {
        *native_error = net_detail::map_result(opened);
        return false;
    }
    const int fd = static_cast<int>(opened);
    SockAddrIn address{};
    address.family = kAfInet;
    constexpr u8 kLoopback[4] = {127, 0, 0, 1};
    copy_bytes(address.address, kLoopback, 4);
    u32 address_bytes = sizeof(address);
    long status = sys_bind(fd, &address, sizeof(address));
    if (!syscall_failed(status))
        status = sys_getsockname(fd, &address, &address_bytes);
    if (syscall_failed(status)) {
        *native_error = net_detail::map_result(status);
        sys_close(fd);
        return false;
    }
    *port = byte_swap(address.port);
    *result = Socket{static_cast<u64>(fd) + 1};
    return true;
}

inline i32 udp_peer_receive_from(const Socket& peer, void* data, u32 bytes, NetworkAddress* from) noexcept {
    SockAddrStorage source;
    u32 source_bytes = sizeof(source);
    long result;
    do {
        result = sys_recv_from(udp_peer_descriptor(peer), data, bytes, 0, &source, &source_bytes);
    } while (result == -kErrorInterrupted);
    if (syscall_failed(result)) {
        net_detail::map_result(result);
        return -1;
    }
    if (from)
        net_detail::from_socket_address(&source, source_bytes, from);
    return static_cast<i32>(result);
}

inline i32 udp_peer_send_to(const Socket& peer, const void* data, u32 bytes, const NetworkAddress& to) noexcept {
    SockAddrStorage target;
    const u32 target_bytes = net_detail::to_socket_address(to, &target);
    if (target_bytes == 0) {
        net_detail::set_error(WSAEINVAL);
        return -1;
    }
    long result;
    do {
        result = sys_send_to(udp_peer_descriptor(peer), data, bytes, kMessageNoSignal, &target, target_bytes);
    } while (result == -kErrorInterrupted);
    if (syscall_failed(result)) {
        net_detail::map_result(result);
        return -1;
    }
    return static_cast<i32>(result);
}
#endif

} // namespace maicord
