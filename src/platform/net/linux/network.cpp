// maicord/platform/net/network.hpp on Linux: the Windows completion-port model on epoll.
//
// A connection's socket is non-blocking and registered once with the loop's epoll for edge-triggered readiness. An
// operation first tries the system call itself (a receive that finds data, a send that fits, a connect that is
// refused at once) and, if the call would block, waits in the owner until the next readiness edge. Either way the
// outcome is not returned to the caller but queued in the loop's ready list, so that every operation, like a Windows
// one, finishes as exactly one completion from network_loop_poll. Closing an owner delivers one cancelled
// completion for each operation still waiting, after bumping its generation so the consumers see them as stale.
//
// Edge-triggered readiness is safe because every operation is attempted when it is started and again at each edge
// until the call would block, so no data waits unnoticed in a socket without a pending operation.
#include "internal.hpp"
#include "maicord/sys/linux/libc.hpp"

namespace maicord {
namespace {

using namespace net_detail;

constexpr u8 kWaitConnect = 1;
constexpr u8 kWaitReceive = 2;
constexpr u8 kWaitSend = 4;
constexpr int kEventCapacity = 16;
constexpr i64 kNanosecondsPerMillisecond = 1'000'000;
constexpr u32 kInfinite = 0xFFFFFFFFu;

i64 monotonic_nanoseconds() noexcept {
    TimeSpec time{};
    clock_gettime(kClockMonotonic, &time);
    return time.seconds * 1'000'000'000 + time.nanoseconds;
}

void initialize_operation(NetworkOperation* operation, void* owner, NetworkOperationKind kind, u32 generation) noexcept {
    reset_value<NetworkOperation>(operation);
    operation->owner = owner;
    operation->generation = generation;
    operation->kind = kind;
}

void ready_append(NetworkLoop* loop, NetworkOperation* operation) noexcept {
    operation->next = nullptr;
    if (loop->ready_tail) {
        loop->ready_tail->next = operation;
    } else {
        loop->ready_head = operation;
    }
    loop->ready_tail = operation;
}

NetworkOperation* ready_pop(NetworkLoop* loop) noexcept {
    NetworkOperation* operation = loop->ready_head;
    if (operation) {
        loop->ready_head = operation->next;
        if (!loop->ready_head) {
            loop->ready_tail = nullptr;
        }
        operation->next = nullptr;
    }
    return operation;
}

// Moves what other threads posted to the ready list, oldest first.
void drain_posted(NetworkLoop* loop) noexcept {
    NetworkOperation* stack = __atomic_exchange_n(&loop->posted, nullptr, __ATOMIC_ACQUIRE);
    NetworkOperation* reversed = nullptr;
    while (stack) {
        NetworkOperation* next = stack->next;
        stack->next = reversed;
        reversed = stack;
        stack = next;
    }
    while (reversed) {
        NetworkOperation* next = reversed->next;
        ready_append(loop, reversed);
        reversed = next;
    }
}

void drain_wake(NetworkLoop* loop) noexcept {
    u64 counter = 0;
    sys_read(loop->wake, &counter, sizeof(counter));
}

// Whether `operation` was started by the current incarnation of its owner (Windows: the packet's key and the
// operation's generation both match).
bool owned(const NetworkOperation* operation, const void* owner, u32 owner_generation) noexcept {
    return owner && operation->generation == owner_generation;
}

void close_socket(NetworkConnection* connection) noexcept {
    if (connection->socket != 0) {
        sys_close(descriptor(connection->socket));
        connection->socket = 0;
    }
}

void finish_connect(NetworkConnection* connection) noexcept {
    int error = 0;
    u32 bytes = sizeof(error);
    const long result =
        sys_getsockopt(descriptor(connection->socket), kSolSocket, kSocketError, &error, &bytes);
    connection->waiting = static_cast<u8>(connection->waiting & ~kWaitConnect);
    if (syscall_failed(result)) {
        error = static_cast<int>(-result);
    }
    complete(connection->loop, &connection->connect_operation, error == 0, 0, error ? map_errno(error) : 0);
}

void try_receive(NetworkConnection* connection) noexcept {
    NetworkOperation& operation = connection->receive_operation;
    for (;;) {
        const long received = sys_recv(descriptor(connection->socket), operation.buffer.buf, operation.buffer.len, 0);
        if (received == -kErrorInterrupted) {
            continue;
        }
        if (received == -kErrorAgain) {
            connection->waiting |= kWaitReceive;
            return;
        }
        connection->waiting = static_cast<u8>(connection->waiting & ~kWaitReceive);
        if (syscall_failed(received)) {
            complete(connection->loop, &operation, false, 0, map_errno(static_cast<int>(-received)));
        } else {
            // Zero is the peer closing; poll turns it into peer_closed.
            complete(connection->loop, &operation, true, static_cast<u32>(received), 0);
        }
        return;
    }
}

void try_send(NetworkConnection* connection) noexcept {
    NetworkOperation& operation = connection->send_operation;
    u32 sent = operation.transferred;
    while (sent < operation.buffer.len) {
        const long written = sys_send(descriptor(connection->socket), operation.buffer.buf + sent,
                                      operation.buffer.len - sent, kMessageNoSignal);
        if (written == -kErrorInterrupted) {
            continue;
        }
        if (written == -kErrorAgain) {
            operation.transferred = sent;
            connection->waiting |= kWaitSend;
            return;
        }
        if (syscall_failed(written)) {
            connection->waiting = static_cast<u8>(connection->waiting & ~kWaitSend);
            complete(connection->loop, &operation, false, sent, map_errno(static_cast<int>(-written)));
            return;
        }
        sent += static_cast<u32>(written);
    }
    connection->waiting = static_cast<u8>(connection->waiting & ~kWaitSend);
    complete(connection->loop, &operation, true, sent, 0);
}

// The completion poll returns for a connection's operation.
void translate_connection(NetworkOperation* operation, NetworkPollResult* result) noexcept {
    auto* connection = static_cast<NetworkConnection*>(operation->owner);
    NetworkCompletion& completion = result->completion;
    completion.transferred = operation->transferred;
    completion.native_error = operation->native_error;
    completion.kind = operation->kind;
    completion.succeeded = operation->succeeded;
    completion.connection = connection;
    completion.stale = !owned(operation, connection, connection ? connection->generation : 0);
    if (completion.stale) {
        return;
    }
    switch (operation->kind) {
    case NetworkOperationKind::connect:
        connection->connect_pending = false;
        connection->state = operation->succeeded ? NetworkConnectionState::connected : NetworkConnectionState::failed;
        break;
    case NetworkOperationKind::receive:
        connection->receive_pending = false;
        if (!operation->succeeded) {
            connection->state = NetworkConnectionState::failed;
        } else if (operation->transferred == 0) {
            connection->state = NetworkConnectionState::peer_closed;
        }
        break;
    case NetworkOperationKind::send:
        connection->send_pending = false;
        if (!operation->succeeded) {
            connection->state = NetworkConnectionState::failed;
        }
        break;
    default:
        break;
    }
}

NetworkPollResult translate(NetworkOperation* operation) noexcept {
    NetworkPollResult result{};
    result.state = NetworkPollState::completion;
    switch (operation->kind) {
    case NetworkOperationKind::signal: {
        auto* signal = static_cast<NetworkSignal*>(operation->owner);
        result.completion.signal = signal;
        result.completion.signal_value = signal ? signal->value : 0;
        result.completion.kind = NetworkOperationKind::signal;
        result.completion.succeeded = signal != nullptr;
        result.completion.stale = !signal;
        if (signal) {
            InterlockedExchange(&signal->pending, 0);
        }
        break;
    }
    case NetworkOperationKind::resolve:
        translate_resolver(operation, &result);
        break;
    case NetworkOperationKind::file_read:
    case NetworkOperationKind::file_write:
        translate_file(operation, &result);
        break;
    case NetworkOperationKind::datagram_receive:
    case NetworkOperationKind::datagram_send:
        translate_datagram(operation, &result);
        break;
    default:
        translate_connection(operation, &result);
        break;
    }
    return result;
}

// Dispatches one epoll event to its owner: a socket's readiness runs that owner's waiting operations.
void dispatch(NetworkLoop* loop, const EpollEvent& event) noexcept {
    const u64 data = event.data;
    const u64 kind = data & 3;
    if (kind == kOwnerWake) {
        drain_wake(loop);
        drain_posted(loop);
    } else if (kind == kOwnerConnection) {
        service_connection(reinterpret_cast<NetworkConnection*>(data & ~u64{3}), event.events);
    } else if (kind == kOwnerDatagram) {
        service_datagram(reinterpret_cast<NetworkDatagram*>(data & ~u64{3}), event.events);
    }
}

} // namespace

namespace net_detail {

bool watch_socket(NetworkLoop* loop, int fd, const void* owner, u64 kind, u32 events) noexcept {
    EpollEvent event{events, reinterpret_cast<u64>(owner) | kind};
    const long result = sys_epoll_ctl(loop->epoll, kEpollAdd, fd, &event);
    if (syscall_failed(result)) {
        map_result(result);
        return false;
    }
    return true;
}

void complete(NetworkLoop* loop, NetworkOperation* operation, bool succeeded, u32 transferred,
              i32 native_error) noexcept {
    operation->succeeded = succeeded;
    operation->transferred = transferred;
    operation->native_error = native_error;
    ready_append(loop, operation);
}

void post(NetworkLoop* loop, NetworkOperation* operation, bool succeeded, u32 transferred, i32 native_error) noexcept {
    operation->succeeded = succeeded;
    operation->transferred = transferred;
    operation->native_error = native_error;
    NetworkOperation* head = __atomic_load_n(&loop->posted, __ATOMIC_RELAXED);
    do {
        operation->next = head;
    } while (!__atomic_compare_exchange_n(&loop->posted, &head, operation, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    const u64 one = 1;
    sys_write(loop->wake, &one, sizeof(one));
}

void service_connection(NetworkConnection* connection, u32 events) noexcept {
    if (connection->socket == 0) {
        return;
    }
    if ((connection->waiting & kWaitConnect) && (events & (kEpollOut | kEpollError | kEpollHangUp))) {
        finish_connect(connection);
    }
    if ((connection->waiting & kWaitReceive) &&
        (events & (kEpollIn | kEpollReadHangUp | kEpollHangUp | kEpollError))) {
        try_receive(connection);
    }
    if ((connection->waiting & kWaitSend) && (events & (kEpollOut | kEpollError | kEpollHangUp))) {
        try_send(connection);
    }
}

} // namespace net_detail

NetworkResult network_loop_initialize(NetworkLoop* loop) noexcept {
    if (!loop) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }
    reset_value<NetworkLoop>(loop);
    const long epoll = sys_epoll_create(kEpollCloseOnExec);
    if (syscall_failed(epoll)) {
        return failure(NetworkError::completion_port_create, map_result(epoll));
    }
    const long wake = sys_eventfd(0, kEventfdNonBlock | kEventfdCloseOnExec);
    if (syscall_failed(wake)) {
        sys_close(static_cast<int>(epoll));
        return failure(NetworkError::completion_port_create, map_result(wake));
    }
    loop->epoll = static_cast<int>(epoll);
    loop->wake = static_cast<int>(wake);
    EpollEvent event{kEpollIn, kOwnerWake};
    const long added = sys_epoll_ctl(loop->epoll, kEpollAdd, loop->wake, &event);
    if (syscall_failed(added)) {
        sys_close(loop->wake);
        sys_close(loop->epoll);
        reset_value<NetworkLoop>(loop);
        return failure(NetworkError::completion_port_create, map_result(added));
    }
    loop->ready = true;
    return success();
}

void network_loop_release(NetworkLoop* loop) noexcept {
    if (!loop) {
        return;
    }
    if (loop->ready) {
        resolver_forget_loop(loop);
        sys_close(loop->wake);
        sys_close(loop->epoll);
    }
    reset_value<NetworkLoop>(loop);
}

NetworkResult network_connection_connect(NetworkLoop* loop, NetworkConnection* connection,
                                         const NetworkAddress& address) noexcept {
    SockAddrStorage target;
    const u32 address_bytes = to_socket_address(address, &target);
    if (!loop || !loop->ready || !connection || address_bytes == 0) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }
    reset_value<NetworkConnection>(connection);
    connection->generation = 1;
    connection->loop = loop;
    const int family = address.family == 4 ? kAfInet : kAfInet6;
    const long opened = sys_socket(family, kSockStream | kSockNonBlock | kSockCloseOnExec, 0);
    if (syscall_failed(opened)) {
        return failure(NetworkError::socket_create, map_result(opened));
    }
    const int fd = static_cast<int>(opened);
    connection->socket = token(fd);
    const int no_delay = 1;
    const long option = sys_setsockopt(fd, kSolTcp, kTcpNoDelay, &no_delay, sizeof(no_delay));
    if (syscall_failed(option)) {
        const i32 native_error = map_result(option);
        network_connection_close(connection);
        return failure(NetworkError::socket_create, native_error);
    }
    if (!watch_socket(loop, fd, connection, kOwnerConnection,
                      kEpollIn | kEpollOut | kEpollReadHangUp | kEpollEdgeTriggered)) {
        const i32 native_error = network_last_error();
        network_connection_close(connection);
        return failure(NetworkError::completion_port_associate, native_error);
    }

    initialize_operation(&connection->connect_operation, connection, NetworkOperationKind::connect,
                         connection->generation);
    connection->state = NetworkConnectionState::connecting;
    connection->connect_pending = true;
    const long started = sys_connect(fd, &target, address_bytes);
    if (!syscall_failed(started)) {
        complete(loop, &connection->connect_operation, true, 0, 0);
    } else if (started == -kErrorInProgress) {
        connection->waiting |= kWaitConnect;
    } else if (started == -kErrorInvalid || started == -kErrorAddressFamily) {
        const i32 native_error = map_result(started);
        connection->connect_pending = false;
        connection->state = NetworkConnectionState::failed;
        network_connection_close(connection);
        return failure(NetworkError::connect_start, native_error);
    } else {
        // Refused, unreachable, timed out: Windows reports those as the connect's completion, and so does this.
        complete(loop, &connection->connect_operation, false, 0, map_errno(static_cast<int>(-started)));
    }
    return success();
}

NetworkResult network_connection_receive(NetworkConnection* connection, Slice<u8> buffer) noexcept {
    if (!connection || connection->state != NetworkConnectionState::connected || connection->receive_pending ||
        !buffer.data || buffer.count == 0) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }
    initialize_operation(&connection->receive_operation, connection, NetworkOperationKind::receive,
                         connection->generation);
    connection->receive_operation.buffer.buf = reinterpret_cast<char*>(buffer.data);
    connection->receive_operation.buffer.len = buffer.count;
    connection->receive_pending = true;
    try_receive(connection);
    return success();
}

NetworkResult network_connection_send(NetworkConnection* connection, ConstSlice<u8> buffer) noexcept {
    if (!connection || connection->state != NetworkConnectionState::connected || connection->send_pending ||
        !buffer.data || buffer.count == 0) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }
    initialize_operation(&connection->send_operation, connection, NetworkOperationKind::send, connection->generation);
    connection->send_operation.buffer.buf = reinterpret_cast<char*>(const_cast<u8*>(buffer.data));
    connection->send_operation.buffer.len = buffer.count;
    connection->send_pending = true;
    try_send(connection);
    return success();
}

void network_connection_close(NetworkConnection* connection) noexcept {
    if (!connection) {
        return;
    }
    close_socket(connection);
    ++connection->generation;
    connection->state = NetworkConnectionState::closed;
    // The operations that were waiting for the socket will never run: each finishes now, cancelled.
    const u8 waiting = connection->waiting;
    connection->waiting = 0;
    if (waiting & kWaitConnect) {
        complete(connection->loop, &connection->connect_operation, false, 0, ERROR_OPERATION_ABORTED);
    }
    if (waiting & kWaitReceive) {
        complete(connection->loop, &connection->receive_operation, false, 0, ERROR_OPERATION_ABORTED);
    }
    if (waiting & kWaitSend) {
        complete(connection->loop, &connection->send_operation, false, 0, ERROR_OPERATION_ABORTED);
    }
}

void network_connection_reset(NetworkConnection* connection) noexcept {
    connection->socket = 0;
}

bool network_connection_has_socket(const NetworkConnection& connection) noexcept {
    return connection.socket != 0;
}

NetworkResult network_signal_post(NetworkLoop* loop, NetworkSignal* signal, u32 value) noexcept {
    if (!loop || !loop->ready || !signal || InterlockedCompareExchange(&signal->pending, 1, 0) != 0) {
        return failure(NetworkError::invalid_argument, ERROR_INVALID_PARAMETER);
    }
    reset_value<NetworkOperation>(&signal->operation);
    signal->operation.owner = signal;
    signal->operation.kind = NetworkOperationKind::signal;
    signal->value = value;
    post(loop, &signal->operation, true, 0, 0);
    return success();
}

NetworkPollResult network_loop_poll(NetworkLoop* loop, u32 timeout_milliseconds) noexcept {
    NetworkPollResult result{};
    result.state = NetworkPollState::error;
    if (!loop || !loop->ready) {
        result.completion.native_error = ERROR_INVALID_PARAMETER;
        return result;
    }
    const i64 deadline =
        timeout_milliseconds == kInfinite ? 0 : monotonic_nanoseconds() + timeout_milliseconds * kNanosecondsPerMillisecond;
    for (;;) {
        if (!loop->ready_head) {
            drain_posted(loop);
        }
        if (NetworkOperation* operation = ready_pop(loop)) {
            return translate(operation);
        }
        int wait = -1;
        if (timeout_milliseconds != kInfinite) {
            const i64 remaining = deadline - monotonic_nanoseconds();
            // Round up so a wait never returns before its deadline and spins until it passes.
            wait = remaining <= 0 ? 0 : static_cast<int>(minimum<i64>((remaining + kNanosecondsPerMillisecond - 1) /
                                                                          kNanosecondsPerMillisecond,
                                                                      0x7fffffff));
        }
        EpollEvent events[kEventCapacity];
        const long count = sys_epoll_wait(loop->epoll, events, kEventCapacity, wait);
        if (count == -kErrorInterrupted) {
            continue;
        }
        if (syscall_failed(count)) {
            result.completion.native_error = map_result(count);
            return result;
        }
        // Every event of the batch becomes completions before anything is returned, so no event outlives the poll:
        // a handler may close the owner an event named between polls.
        for (long index = 0; index < count; ++index) {
            dispatch(loop, events[index]);
        }
        if (count == 0 && !loop->ready_head && timeout_milliseconds != kInfinite &&
            monotonic_nanoseconds() >= deadline) {
            result.state = NetworkPollState::timeout;
            return result;
        }
    }
}

} // namespace maicord
