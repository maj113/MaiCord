// The connected UDP sockets of maicord/platform/net/network.hpp on Linux, on the same ring as the streams.
//
// Each of the eight receive slots is a posted buffer; a datagram goes to the slot that was posted first, which is how
// overlapped reads on a Windows socket are served. The kernel is asked for one read at a time (the oldest slot's):
// several reads waiting on one socket are woken in no promised order. A datagram larger than its buffer completes as
// a failure with WSAEMSGSIZE and the bytes that fit (the voice code drops it); a zero-length datagram is a valid
// receive. Closing delivers one cancelled completion for every read that was still waiting.
#include "internal.hpp"
#include "maicord/core/win32_error.hpp"

namespace maicord {
namespace {

using namespace net_detail;

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

// Starts the read of the slot posted first, when none is under way.
void next_receive(NetworkDatagram* datagram) noexcept {
    if (datagram->receive_running || datagram->receive_queued == 0 || datagram->socket == 0) {
        return;
    }
    const u32 slot = datagram->receive_queue[0];
    for (u32 index = 1; index < datagram->receive_queued; ++index) {
        datagram->receive_queue[index - 1] = datagram->receive_queue[index];
    }
    --datagram->receive_queued;
    datagram->receive_running = true;
    start(datagram->loop, &receive_operation(datagram, slot));
}

} // namespace

namespace net_detail {

i32 datagram_error(long result) noexcept {
    const int error = static_cast<int>(-result);
    return error == kErrorConnectionRefused ? WSAECONNRESET : map_errno(error);
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

void receive_finished(NetworkDatagram* datagram) noexcept {
    datagram->receive_running = false;
    next_receive(datagram);
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
    const u32 generation = datagram->generation + 1;
    reset_value(datagram);
    datagram->socket = token(fd);
    datagram->loop = loop;
    datagram->generation = generation;
    datagram->open = true;
    return success();
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
    datagram->receive_queue[datagram->receive_queued++] = static_cast<u8>(slot);
    next_receive(datagram);
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
    start(datagram->loop, &send);
    return success();
}

void network_datagram_close(NetworkDatagram* datagram) noexcept {
    if (!datagram || !datagram->open) {
        return;
    }
    // The read under way and the send are cancelled with the descriptor; reads that never started finish at once.
    if (datagram->receive_running || datagram->send_pending) {
        cancel_socket(datagram->loop, descriptor(datagram->socket));
    }
    for (u32 index = 0; index < datagram->receive_queued; ++index) {
        complete(datagram->loop, &receive_operation(datagram, datagram->receive_queue[index]), false, 0,
                 ERROR_OPERATION_ABORTED);
    }
    datagram->receive_queued = 0;
    sys_close(descriptor(datagram->socket));
    datagram->socket = 0;
    datagram->open = false;
}

} // namespace maicord
