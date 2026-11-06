// maicord/platform/net/network.hpp on Linux: the Windows completion-port model on io_uring.
//
// A loop owns one ring (Linux 6.1: IORING_SETUP_SINGLE_ISSUER | COOP_TASKRUN, so only the loop's own thread submits and
// completions are posted without interrupting it). Starting an operation queues a submission entry; network_loop_poll hands
// the queued entries to the kernel and waits for completions in one system call, which is how a Windows loop learns
// of overlapped I/O. A request that can finish at once (data waiting, room to send, a file read from the page cache)
// finishes inside that call. The outcome is never returned to the caller but put in the loop's ready list, so that
// every operation, like a Windows one, finishes as exactly one completion from network_loop_poll. Closing an owner
// bumps its generation, so that the completions of its requests read as stale, and cancels them: each still finishes
// once, cancelled.
//
// Other threads reach the loop through post(): the poster pushes the operation on a stack and writes an eventfd that a
// read request of the ring waits on.
#include "internal.hpp"
#include "maicord/sys/linux/io_uring.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

using namespace net_detail;

constexpr u32 kEntries = 128;
constexpr u32 kRingFlags = kRingSetupSubmitAll | kRingSetupSingleIssuer | kRingSetupCoopTaskRun;
constexpr i64 kNanosecondsPerSecond = 1'000'000'000;
constexpr u32 kInfinite = 0xFFFFFFFFu;
constexpr int kErrorNoSystemCall = 38;
constexpr int kMapPopulate = 0x8000;
constexpr int kErrorTimerExpired = 62; // ETIME: the wait of io_uring_enter ran out
constexpr int kErrorBusy = 16;
// The user data of a completion that is not an operation: a cancel (0) and the read of the wake eventfd (1).
constexpr u64 kNoOperation = 0;
constexpr u64 kWakeRead = 1;

i64 monotonic_nanoseconds() noexcept {
    TimeSpec time{};
    clock_gettime(kClockMonotonic, &time);
    return time.seconds * kNanosecondsPerSecond + time.nanoseconds;
}

u32 acquire(const u32* counter) noexcept {
    return __atomic_load_n(counter, __ATOMIC_ACQUIRE);
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

// The next free submission entry, zeroed; null when the ring is full and cannot be emptied.
RingSubmission* take_entry(NetworkLoop* loop) noexcept {
    if (loop->queued_tail - acquire(loop->submission_head) > loop->submission_mask && !submit(loop) &&
        loop->queued_tail - acquire(loop->submission_head) > loop->submission_mask) {
        return nullptr;
    }
    auto* entry = &static_cast<RingSubmission*>(loop->submissions)[loop->queued_tail++ & loop->submission_mask];
    reset_value<RingSubmission>(entry);
    return entry;
}

// Arms the read that tells the loop another thread posted something.
void arm_wake(NetworkLoop* loop) noexcept {
    if (RingSubmission* entry = take_entry(loop)) {
        entry->opcode = kRingOpRead;
        entry->descriptor = loop->wake;
        entry->address = reinterpret_cast<u64>(&loop->wake_counter);
        entry->length = sizeof(loop->wake_counter);
        entry->user_data = kWakeRead;
    }
}

int descriptor_of(const NetworkOperation* operation) noexcept {
    switch (operation->kind) {
    case NetworkOperationKind::datagram_receive:
    case NetworkOperationKind::datagram_send:
        return descriptor(static_cast<const NetworkDatagram*>(operation->owner)->socket);
    case NetworkOperationKind::file_read:
    case NetworkOperationKind::file_write:
        return static_cast<int>(reinterpret_cast<u64>(static_cast<const NetworkFile*>(operation->owner)->job_handle)) - 1;
    default:
        return descriptor(static_cast<const NetworkConnection*>(operation->owner)->socket);
    }
}

// Whether the rest of a partly moved operation may be requested: its owner is still the one that started it.
bool resumable(const NetworkOperation* operation) noexcept {
    switch (operation->kind) {
    case NetworkOperationKind::send: {
        const auto* connection = static_cast<const NetworkConnection*>(operation->owner);
        return connection->socket != 0 && connection->generation == operation->generation;
    }
    case NetworkOperationKind::file_read:
    case NetworkOperationKind::file_write:
        return static_cast<const NetworkFile*>(operation->owner)->handle != nullptr;
    default:
        return false;
    }
}

// What a completion entry of the ring means for the operation it names (`result` is the call's value or a negated
// errno). A send or file operation that moved only part of its buffer asks for the rest instead of finishing.
void finish_result(NetworkLoop* loop, NetworkOperation* operation, i32 result) noexcept {
    const NetworkOperationKind kind = operation->kind;
    const bool datagram = kind == NetworkOperationKind::datagram_receive || kind == NetworkOperationKind::datagram_send;
    if (result < 0) {
        const i32 error = -result == kErrorCanceled   ? ERROR_OPERATION_ABORTED
                          : datagram                  ? datagram_error(result)
                                                      : map_errno(-result);
        complete(loop, operation, false, operation->transferred, error);
        return;
    }
    const u32 bytes = static_cast<u32>(result);
    switch (kind) {
    case NetworkOperationKind::datagram_receive:
        // A datagram larger than its buffer: the bytes that fit, as the error that Windows reports.
        if (bytes > operation->buffer.len) {
            complete(loop, operation, false, operation->buffer.len, WSAEMSGSIZE);
            return;
        }
        break;
    case NetworkOperationKind::send:
    case NetworkOperationKind::file_read:
    case NetworkOperationKind::file_write: {
        const u32 done = operation->transferred + bytes;
        if (bytes > 0 && done < operation->buffer.len && resumable(operation)) {
            operation->transferred = done;
            start(loop, operation);
            return;
        }
        complete(loop, operation, true, done, 0);
        return;
    }
    default:
        break;
    }
    // Zero from a receive is the peer closing; poll turns it into peer_closed.
    complete(loop, operation, true, bytes, 0);
}

void finish(NetworkLoop* loop, NetworkOperation* operation, i32 result) noexcept {
    finish_result(loop, operation, result);
    if (operation->kind == NetworkOperationKind::datagram_receive) {
        receive_finished(static_cast<NetworkDatagram*>(operation->owner));
    }
}

// Reaps the completion entries the kernel has posted; the count.
u32 reap(NetworkLoop* loop) noexcept {
    u32 head = *loop->completion_head;
    const u32 tail = acquire(loop->completion_tail);
    const u32 count = tail - head;
    for (; head != tail; ++head) {
        const RingCompletion entry = static_cast<const RingCompletion*>(loop->completions)[head & loop->completion_mask];
        if (entry.user_data == kWakeRead) {
            drain_posted(loop);
            arm_wake(loop);
        } else if (entry.user_data != kNoOperation) {
            finish(loop, reinterpret_cast<NetworkOperation*>(entry.user_data), entry.result);
        }
    }
    __atomic_store_n(loop->completion_head, head, __ATOMIC_RELEASE);
    return count;
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
    // Windows: the packet's key and the operation's generation both match the owner's current incarnation.
    completion.stale = !connection || operation->generation != connection->generation;
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

void* map_ring(int ring, u32 bytes, u64 offset) noexcept {
    const long address = syscall6(kSysMmap, 0, static_cast<long>(bytes), kProtectRead | kProtectWrite,
                                  kMapShared | kMapPopulate, ring, static_cast<long>(offset));
    return syscall_failed(address) ? nullptr : reinterpret_cast<void*>(address);
}

// Makes the loop's ring and maps it; the negated errno of the step that failed, or 0.
long open_ring(NetworkLoop* loop) noexcept {
    RingParameters parameters{};
    parameters.flags = kRingFlags;
    const long ring = sys_io_uring_setup(kEntries, &parameters);
    if (syscall_failed(ring)) {
        return ring;
    }
    const u32 submission_bytes = parameters.submission.array + parameters.submission_entries * sizeof(u32);
    const u32 completion_bytes = parameters.completion.completions + parameters.completion_entries * sizeof(RingCompletion);
    loop->ring_bytes = maximum(submission_bytes, completion_bytes);
    loop->entry_bytes = parameters.submission_entries * sizeof(RingSubmission);
    auto* rings = static_cast<u8*>(map_ring(static_cast<int>(ring), loop->ring_bytes, 0));
    loop->submissions = map_ring(static_cast<int>(ring), loop->entry_bytes, kRingOffsetSubmissionEntries);
    if (!rings || !loop->submissions) {
        if (rings) {
            sys_munmap(rings, loop->ring_bytes);
        }
        if (loop->submissions) {
            sys_munmap(loop->submissions, loop->entry_bytes);
        }
        sys_close(static_cast<int>(ring));
        return -kErrorNoMemory;
    }
    loop->ring = static_cast<int>(ring);
    loop->ring_memory = rings;
    loop->completions = rings + parameters.completion.completions;
    loop->submission_head = reinterpret_cast<u32*>(rings + parameters.submission.head);
    loop->submission_tail = reinterpret_cast<u32*>(rings + parameters.submission.tail);
    loop->submission_mask = *reinterpret_cast<u32*>(rings + parameters.submission.mask);
    loop->completion_head = reinterpret_cast<u32*>(rings + parameters.completion.head);
    loop->completion_tail = reinterpret_cast<u32*>(rings + parameters.completion.tail);
    loop->completion_mask = *reinterpret_cast<u32*>(rings + parameters.completion.mask);
    // Entry number i is slot i: the array indirection of the ring is not used.
    auto* array = reinterpret_cast<u32*>(rings + parameters.submission.array);
    for (u32 slot = 0; slot < parameters.submission_entries; ++slot) {
        array[slot] = slot;
    }
    return 0;
}

void close_ring(NetworkLoop* loop) noexcept {
    sys_munmap(loop->ring_memory, loop->ring_bytes);
    sys_munmap(loop->submissions, loop->entry_bytes);
    sys_close(loop->ring);
}

} // namespace

namespace net_detail {

bool submit(NetworkLoop* loop) noexcept {
    for (;;) {
        __atomic_store_n(loop->submission_tail, loop->queued_tail, __ATOMIC_RELEASE);
        const u32 pending = loop->queued_tail - acquire(loop->submission_head);
        if (pending == 0) {
            return true;
        }
        const long entered = sys_io_uring_enter(loop->ring, pending, 0, 0, nullptr, 0);
        if (entered != -kErrorInterrupted) {
            return !syscall_failed(entered);
        }
    }
}

void start(NetworkLoop* loop, NetworkOperation* operation) noexcept {
    RingSubmission* entry = take_entry(loop);
    if (!entry) {
        complete(loop, operation, false, operation->transferred, kWsaNoBuffers);
        return;
    }
    entry->descriptor = descriptor_of(operation);
    entry->address = reinterpret_cast<u64>(operation->buffer.buf) + operation->transferred;
    entry->length = operation->buffer.len - operation->transferred;
    entry->user_data = reinterpret_cast<u64>(operation);
    switch (operation->kind) {
    case NetworkOperationKind::receive:
        entry->opcode = kRingOpRecv;
        break;
    case NetworkOperationKind::datagram_receive:
        entry->opcode = kRingOpRecv;
        entry->flags_word = kMessageTruncate;
        break;
    case NetworkOperationKind::file_read:
    case NetworkOperationKind::file_write:
        entry->opcode = operation->kind == NetworkOperationKind::file_read ? kRingOpRead : kRingOpWrite;
        entry->offset = static_cast<const NetworkFile*>(operation->owner)->offset + operation->transferred;
        break;
    default:
        entry->opcode = kRingOpSend;
        entry->flags_word = kMessageNoSignal;
        break;
    }
}

void cancel_socket(NetworkLoop* loop, int fd) noexcept {
    if (RingSubmission* entry = take_entry(loop)) {
        entry->opcode = kRingOpAsyncCancel;
        entry->flags = kRingEntrySkipSuccess;
        entry->descriptor = fd;
        entry->flags_word = kRingCancelAll | kRingCancelDescriptor;
        entry->user_data = kNoOperation;
    }
    submit(loop);
}

void cancel_operation(NetworkLoop* loop, NetworkOperation* operation) noexcept {
    if (RingSubmission* entry = take_entry(loop)) {
        entry->opcode = kRingOpAsyncCancel;
        entry->flags = kRingEntrySkipSuccess;
        entry->address = reinterpret_cast<u64>(operation);
        entry->user_data = kNoOperation;
    }
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

} // namespace net_detail

NetworkResult network_loop_initialize(NetworkLoop* loop) noexcept {
    if (!loop) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }
    reset_value<NetworkLoop>(loop);
    const long opened = open_ring(loop);
    if (opened) {
        return failure(NetworkError::completion_port_create, map_result(opened));
    }
    // A blocking eventfd: a kernel that cannot poll it for the ring's read serves the read on a worker instead.
    const long wake = sys_eventfd(0, kEventfdCloseOnExec);
    if (syscall_failed(wake)) {
        close_ring(loop);
        reset_value<NetworkLoop>(loop);
        return failure(NetworkError::completion_port_create, map_result(wake));
    }
    loop->wake = static_cast<int>(wake);
    arm_wake(loop);
    loop->ready = true;
    return success();
}

void network_loop_release(NetworkLoop* loop) noexcept {
    if (!loop) {
        return;
    }
    if (loop->ready) {
        resolver_forget_loop(loop);
        close_ring(loop);
        sys_close(loop->wake);
    }
    reset_value<NetworkLoop>(loop);
}

const char* network_unavailable_reason() noexcept {
    RingParameters parameters{};
    parameters.flags = kRingFlags;
    const long ring = sys_io_uring_setup(2, &parameters);
    if (!syscall_failed(ring)) {
        sys_close(static_cast<int>(ring));
        return nullptr;
    }
    switch (static_cast<int>(-ring)) {
    case kErrorPermission:
        return "io_uring is not allowed here: the kernel.io_uring_disabled sysctl forbids it, or a seccomp profile "
               "(Docker's and Podman's defaults, a Flatpak or Snap sandbox) refuses io_uring_setup. Allow io_uring "
               "for MaiCord, or run it outside that sandbox.";
    case kErrorNoSystemCall:
        return "io_uring is not available: the kernel is older than Linux 6.1, which MaiCord needs, or a seccomp "
               "profile hides the system call (Podman's default profile answers \"not implemented\").";
    case kErrorInvalid:
        return "this kernel is too old: MaiCord needs Linux 6.1 or newer (io_uring with IORING_SETUP_SINGLE_ISSUER).";
    case kErrorNoMemory:
        return "the kernel has no memory for an io_uring (a locked-memory or cgroup limit).";
    default:
        return "the kernel refused to create an io_uring.";
    }
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
    // The request reads the address when it is submitted, so it lives in the connection. A connect is submitted at
    // once, as a Windows one starts: a peer may accept it before the loop is polled. Other requests wait for the poll.
    copy_bytes(connection->connect_address, &target, address_bytes);
    initialize_operation(&connection->connect_operation, connection, NetworkOperationKind::connect,
                         connection->generation);
    connection->state = NetworkConnectionState::connecting;
    connection->connect_pending = true;
    if (RingSubmission* entry = take_entry(loop)) {
        entry->opcode = kRingOpConnect;
        entry->descriptor = fd;
        entry->address = reinterpret_cast<u64>(connection->connect_address);
        entry->offset = address_bytes;
        entry->user_data = reinterpret_cast<u64>(&connection->connect_operation);
    } else {
        connection->connect_pending = false;
        connection->state = NetworkConnectionState::failed;
        network_connection_close(connection);
        return failure(NetworkError::connect_start, kWsaNoBuffers);
    }
    submit(loop);
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
    start(connection->loop, &connection->receive_operation);
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
    start(connection->loop, &connection->send_operation);
    return success();
}

void network_connection_close(NetworkConnection* connection) noexcept {
    if (!connection) {
        return;
    }
    if (connection->socket != 0) {
        const int fd = descriptor(connection->socket);
        // The requests under way are cancelled before the descriptor goes: each finishes once, as a stale completion.
        if (connection->connect_pending || connection->receive_pending || connection->send_pending) {
            cancel_socket(connection->loop, fd);
        }
        sys_close(fd);
        connection->socket = 0;
    }
    ++connection->generation;
    connection->state = NetworkConnectionState::closed;
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
    const bool finite = timeout_milliseconds != kInfinite;
    const i64 deadline = finite ? monotonic_nanoseconds() + timeout_milliseconds * i64{1'000'000} : 0;
    for (;;) {
        if (!loop->ready_head) {
            drain_posted(loop);
        }
        if (NetworkOperation* operation = ready_pop(loop)) {
            return translate(operation);
        }
        if (reap(loop)) {
            continue;
        }
        // One call hands over what was queued and waits for the next completion, or for the deadline.
        TimeSpec limit{};
        RingEnterArgument argument{};
        u32 flags = kRingEnterGetEvents;
        if (finite) {
            // Even a poll that must not wait runs the ring's pending work: the zero timeout expires after it.
            const i64 remaining = maximum<i64>(deadline - monotonic_nanoseconds(), 0);
            limit = TimeSpec{remaining / kNanosecondsPerSecond, remaining % kNanosecondsPerSecond};
            argument.timeout = reinterpret_cast<u64>(&limit);
            flags |= kRingEnterExtendedArgument;
        }
        __atomic_store_n(loop->submission_tail, loop->queued_tail, __ATOMIC_RELEASE);
        const long entered =
            sys_io_uring_enter(loop->ring, loop->queued_tail - acquire(loop->submission_head), 1, flags,
                               finite ? &argument : nullptr, finite ? sizeof(argument) : 0);
        if (entered == -kErrorInterrupted) {
            continue;
        }
        // A timeout and a busy completion ring (overflowed completions to reap) are not failures.
        if (syscall_failed(entered) && entered != -kErrorTimerExpired && entered != -kErrorBusy &&
            entered != -kErrorAgain) {
            result.completion.native_error = map_result(entered);
            return result;
        }
        reap(loop);
        if (!loop->ready_head && !loop->posted && finite && monotonic_nanoseconds() >= deadline) {
            result.state = NetworkPollState::timeout;
            return result;
        }
    }
}

} // namespace maicord
