// The connected UDP sockets of maicord/platform/net/network.hpp on Linux, on the same epoll loop as the streams.
//
// Each of the eight receive slots is a posted buffer; a datagram goes to the slot that was posted first, which is
// how overlapped reads on a Windows socket are served. A datagram larger than its buffer completes as a failure
// with WSAEMSGSIZE and the bytes that fit (the voice code drops it); a zero-length datagram is a valid receive.
// Closing delivers one cancelled completion for every operation that was still waiting for the socket.
#include "internal.hpp"

namespace maicord {
namespace {

using namespace net_detail;

constexpr u16 kWaitSend = 1;

// Bit 0 of NetworkDatagram::waiting is the send, bit s + 1 the receive slot s.
constexpr u16 receive_bit(u32 slot) noexcept {
    return static_cast<u16>(2u << slot);
}

NetworkResult fail(NetworkError error, i32 native = WSAEINVAL) noexcept {
    return failure(error, native);
}

void initialize_operation(NetworkOperation* output, NetworkDatagram* owner, NetworkOperationKind kind) noexcept {
    reset_value(output);
    output->owner = owner;
    output->generation = owner->generation;
    output->kind = kind;
}

NetworkOperation& receive_operation(NetworkDatagram* datagram, u32 slot) noexcept {
    return slot ? datagram->extra_receive_operations[slot - 1] : datagram->receive_operation;
}

// A refused connected UDP socket reports the ICMP error on the next call; Windows calls it a connection reset.
i32 datagram_error(long result) noexcept {
    const int error = static_cast<int>(-result);
    return error == kErrorConnectionRefused ? WSAECONNRESET : map_errno(error);
}

// The receive slot that has waited longest, or kNetworkDatagramReceiveSlots when none waits.
u32 oldest_waiting(const NetworkDatagram& datagram) noexcept {
    u32 found = kNetworkDatagramReceiveSlots;
    for (u32 slot = 0; slot < kNetworkDatagramReceiveSlots; ++slot) {
        if ((datagram.waiting & receive_bit(slot)) &&
            (found == kNetworkDatagramReceiveSlots || datagram.receive_order[slot] < datagram.receive_order[found])) {
            found = slot;
        }
    }
    return found;
}

// Serves waiting receives, oldest first, until the socket has nothing more to read.
void serve_receives(NetworkDatagram* datagram) noexcept {
    for (;;) {
        const u32 slot = oldest_waiting(*datagram);
        if (slot == kNetworkDatagramReceiveSlots || datagram->socket == 0) {
            return;
        }
        NetworkOperation& operation = receive_operation(datagram, slot);
        const long received = sys_recv(descriptor(datagram->socket), operation.buffer.buf, operation.buffer.len,
                                       kMessageTruncate | kMessageDontWait);
        if (received == -kErrorInterrupted) {
            continue;
        }
        if (received == -kErrorAgain) {
            return;
        }
        datagram->waiting = static_cast<u16>(datagram->waiting & ~receive_bit(slot));
        if (syscall_failed(received)) {
            complete(datagram->loop, &operation, false, 0, datagram_error(received));
        } else if (static_cast<u64>(received) > operation.buffer.len) {
            complete(datagram->loop, &operation, false, operation.buffer.len, WSAEMSGSIZE);
        } else {
            complete(datagram->loop, &operation, true, static_cast<u32>(received), 0);
        }
    }
}

void try_send(NetworkDatagram* datagram) noexcept {
    NetworkOperation& operation = datagram->send_operation;
    for (;;) {
        const long sent =
            sys_send(descriptor(datagram->socket), operation.buffer.buf, operation.buffer.len, kMessageNoSignal);
        if (sent == -kErrorInterrupted) {
            continue;
        }
        if (sent == -kErrorAgain) {
            datagram->waiting |= kWaitSend;
            return;
        }
        datagram->waiting = static_cast<u16>(datagram->waiting & ~kWaitSend);
        if (syscall_failed(sent)) {
            complete(datagram->loop, &operation, false, 0, datagram_error(sent));
        } else {
            complete(datagram->loop, &operation, true, static_cast<u32>(sent), 0);
        }
        return;
    }
}

} // namespace

namespace net_detail {

void service_datagram(NetworkDatagram* datagram, u32 events) noexcept {
    if (datagram->socket == 0) {
        return;
    }
    if (events & (kEpollIn | kEpollError | kEpollHangUp)) {
        serve_receives(datagram);
    }
    if ((datagram->waiting & kWaitSend) && (events & (kEpollOut | kEpollError | kEpollHangUp))) {
        try_send(datagram);
    }
}

void translate_datagram(NetworkOperation* operation, NetworkPollResult* result) noexcept {
    auto* datagram = static_cast<NetworkDatagram*>(operation->owner);
    NetworkCompletion& completion = result->completion;
    completion.transferred = operation->transferred;
    completion.native_error = operation->native_error;
    completion.kind = operation->kind;
    completion.succeeded = operation->succeeded;
    completion.datagram = datagram;
    const bool is_owned = datagram && operation->generation == datagram->generation;
    completion.stale = !is_owned || !datagram->open;
    // Even a cancelled/closed owner's completion retires the corresponding I/O. A zero-length receive is a valid
    // datagram, never a peer-close signal.
    if (is_owned) {
        if (operation->kind == NetworkOperationKind::datagram_send) {
            datagram->send_pending = false;
        } else if (operation == &datagram->receive_operation) {
            datagram->receive_pending = false;
        } else {
            const auto slot = static_cast<u32>(operation - datagram->extra_receive_operations);
            completion.datagram_slot = static_cast<u8>(slot + 1);
            datagram->extra_receive_pending = static_cast<u8>(datagram->extra_receive_pending & ~(1u << slot));
        }
    }
}

} // namespace net_detail

NetworkResult network_datagram_open(NetworkLoop* loop, NetworkDatagram* datagram,
                                    const NetworkAddress& peer) noexcept {
    SockAddrStorage target;
    const u32 peer_bytes = to_socket_address(peer, &target);
    if (!loop || !loop->ready || !datagram || datagram->open || !network_datagram_idle(*datagram) ||
        datagram->generation == ~u32{0} || peer_bytes == 0 || !peer.port) {
        return fail(NetworkError::invalid_argument);
    }
    const long opened = sys_socket(peer.family == 4 ? kAfInet : kAfInet6,
                                   kSockDatagram | kSockNonBlock | kSockCloseOnExec, 0);
    if (syscall_failed(opened)) {
        return fail(NetworkError::socket_create, map_result(opened));
    }
    const int fd = static_cast<int>(opened);
    const long connected = sys_connect(fd, &target, peer_bytes);
    if (syscall_failed(connected)) {
        const i32 error = map_result(connected);
        sys_close(fd);
        return fail(NetworkError::connect_start, error);
    }
    if (!watch_socket(loop, fd, datagram, kOwnerDatagram, kEpollIn | kEpollOut | kEpollEdgeTriggered)) {
        const i32 error = network_last_error();
        sys_close(fd);
        return fail(NetworkError::completion_port_associate, error);
    }
    const u32 generation = datagram->generation + 1;
    reset_value(datagram);
    datagram->socket = token(fd);
    datagram->loop = loop;
    datagram->generation = generation;
    datagram->open = true;
    return success();
}

NetworkResult network_datagram_receive(NetworkDatagram* datagram, Slice<u8> buffer) noexcept {
    return network_datagram_receive_slot(datagram, 0, buffer);
}

NetworkResult network_datagram_receive_slot(NetworkDatagram* datagram, u32 slot, Slice<u8> buffer) noexcept {
    if (!datagram || !datagram->open || slot >= kNetworkDatagramReceiveSlots || !buffer.data || !buffer.count) {
        return fail(NetworkError::invalid_argument);
    }
    const u8 bit = slot ? static_cast<u8>(1u << (slot - 1)) : 0;
    if (slot ? (datagram->extra_receive_pending & bit) != 0 : datagram->receive_pending) {
        return fail(NetworkError::invalid_argument);
    }
    NetworkOperation& receive = receive_operation(datagram, slot);
    initialize_operation(&receive, datagram, NetworkOperationKind::datagram_receive);
    receive.buffer = {buffer.count, reinterpret_cast<char*>(buffer.data)};
    if (slot) {
        datagram->extra_receive_pending = static_cast<u8>(datagram->extra_receive_pending | bit);
    } else {
        datagram->receive_pending = true;
    }
    datagram->waiting |= receive_bit(slot);
    datagram->receive_order[slot] = ++datagram->receive_serial;
    serve_receives(datagram);
    return success();
}

bool network_datagram_set_receive_buffer(NetworkDatagram* datagram, u32 bytes) noexcept {
    if (!datagram || !datagram->open || !bytes || bytes > 0x7fffffff) {
        return false;
    }
    const int value = static_cast<int>(bytes);
    return !syscall_failed(
        sys_setsockopt(descriptor(datagram->socket), kSolSocket, kSocketReceiveBuffer, &value, sizeof(value)));
}

NetworkResult network_datagram_send(NetworkDatagram* datagram, ConstSlice<u8> buffer) noexcept {
    if (!datagram || !datagram->open || datagram->send_pending || (!buffer.data && buffer.count)) {
        return fail(NetworkError::invalid_argument);
    }
    NetworkOperation& send = datagram->send_operation;
    initialize_operation(&send, datagram, NetworkOperationKind::datagram_send);
    send.buffer = {buffer.count, reinterpret_cast<char*>(const_cast<u8*>(buffer.data))};
    datagram->send_pending = true;
    try_send(datagram);
    return success();
}

void network_datagram_close(NetworkDatagram* datagram) noexcept {
    if (!datagram || !datagram->open) {
        return;
    }
    sys_close(descriptor(datagram->socket));
    datagram->socket = 0;
    datagram->open = false;
    const u16 waiting = datagram->waiting;
    datagram->waiting = 0;
    if (waiting & kWaitSend) {
        complete(datagram->loop, &datagram->send_operation, false, 0, ERROR_OPERATION_ABORTED);
    }
    for (u32 slot = 0; slot < kNetworkDatagramReceiveSlots; ++slot) {
        if (waiting & receive_bit(slot)) {
            complete(datagram->loop, &receive_operation(datagram, slot), false, 0, ERROR_OPERATION_ABORTED);
        }
    }
}

} // namespace maicord
