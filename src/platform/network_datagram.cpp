#include "maicord/core/win32_process.hpp"
#include "network_native.hpp"

namespace maicord {
namespace {
NetworkResult fail(NetworkError error, i32 native = WSAEINVAL) noexcept {
    return {native, error};
}
void operation(NetworkOperation* output, NetworkDatagram* owner, NetworkOperationKind kind) noexcept {
    *output = {};
    output->owner = owner;
    output->generation = owner->generation;
    output->kind = kind;
}
} // namespace
NetworkResult network_datagram_open(NetworkLoop* loop, NetworkDatagram* datagram, const sockaddr* peer,
                                    i32 peer_bytes) noexcept {
    if (!loop || !loop->completion_port || !datagram || datagram->open || !network_datagram_idle(*datagram) ||
        datagram->generation == ~u32{0} || !peer ||
        peer_bytes < sizeof(ADDRESS_FAMILY))
        return fail(NetworkError::invalid_argument);
    const bool v4 = peer->sa_family == AF_INET && peer_bytes == sizeof(sockaddr_in) &&
                    reinterpret_cast<const sockaddr_in*>(peer)->sin_port;
    const bool v6 = peer->sa_family == AF_INET6 && peer_bytes == sizeof(sockaddr_in6) &&
                    reinterpret_cast<const sockaddr_in6*>(peer)->sin6_port;
    if (!v4 && !v6)
        return fail(NetworkError::invalid_argument);
    const SOCKET socket = WSASocketW(peer->sa_family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                                     WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket == INVALID_SOCKET)
        return fail(NetworkError::socket_create, WSAGetLastError());
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0 || connect(socket, peer, peer_bytes) != 0) {
        const auto error = WSAGetLastError();
        closesocket(socket);
        return fail(NetworkError::connect_start, error);
    }
    if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket), loop->completion_port,
                                reinterpret_cast<ULONG_PTR>(datagram), 0)) {
        const auto error = thread_last_error();
        closesocket(socket);
        return fail(NetworkError::completion_port_associate, static_cast<i32>(error));
    }
    const u32 generation = datagram->generation + 1;
    *datagram = {};
    datagram->socket = socket;
    datagram->generation = generation;
    datagram->open = true;
    return {};
}
NetworkResult network_datagram_receive(NetworkDatagram* datagram, Slice<u8> buffer) noexcept {
    return network_datagram_receive_slot(datagram, 0, buffer);
}
NetworkResult network_datagram_receive_slot(NetworkDatagram* datagram, u32 slot, Slice<u8> buffer) noexcept {
    if (!datagram || !datagram->open || slot >= kNetworkDatagramReceiveSlots || !buffer.data || !buffer.count)
        return fail(NetworkError::invalid_argument);
    const u8 bit = slot ? static_cast<u8>(1u << (slot - 1)) : 0;
    if (slot ? (datagram->extra_receive_pending & bit) != 0 : datagram->receive_pending)
        return fail(NetworkError::invalid_argument);
    auto& receive = slot ? datagram->extra_receive_operations[slot - 1] : datagram->receive_operation;
    operation(&receive, datagram, NetworkOperationKind::datagram_receive);
    receive.buffer = {buffer.count, reinterpret_cast<char*>(buffer.data)};
    if (slot)
        datagram->extra_receive_pending = static_cast<u8>(datagram->extra_receive_pending | bit);
    else
        datagram->receive_pending = true;
    if (WSARecv(datagram->socket, native(&receive.buffer), 1, nullptr, &receive.receive_flags, native(&receive.overlapped),
                nullptr) == SOCKET_ERROR) {
        const auto error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            if (slot)
                datagram->extra_receive_pending = static_cast<u8>(datagram->extra_receive_pending & ~bit);
            else
                datagram->receive_pending = false;
            return fail(NetworkError::receive_start, error);
        }
    }
    return {};
}
bool network_datagram_set_receive_buffer(NetworkDatagram* datagram, u32 bytes) noexcept {
    if (!datagram || !datagram->open || !bytes || bytes > 0x7fffffff)
        return false;
    const int value = static_cast<int>(bytes);
    return setsockopt(datagram->socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&value),
                      sizeof(value)) == 0;
}
NetworkResult network_datagram_send(NetworkDatagram* datagram, ConstSlice<u8> buffer) noexcept {
    if (!datagram || !datagram->open || datagram->send_pending || (!buffer.data && buffer.count))
        return fail(NetworkError::invalid_argument);
    auto& send = datagram->send_operation;
    operation(&send, datagram, NetworkOperationKind::datagram_send);
    send.buffer = {buffer.count, reinterpret_cast<char*>(const_cast<u8*>(buffer.data))};
    datagram->send_pending = true;
    if (WSASend(datagram->socket, native(&send.buffer), 1, nullptr, 0, native(&send.overlapped), nullptr) == SOCKET_ERROR) {
        const auto error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            datagram->send_pending = false;
            return fail(NetworkError::send_start, error);
        }
    }
    return {};
}
void network_datagram_close(NetworkDatagram* datagram) noexcept {
    if (!datagram || !datagram->open)
        return;
    closesocket(datagram->socket);
    datagram->socket = INVALID_SOCKET;
    datagram->open = false;
}
} // namespace maicord
