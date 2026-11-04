#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/platform/desktop/file_identity.hpp"

// The network layer: stream connections, connected datagram sockets, name resolution, cross-thread signals and
// overlapped file reads and writes, all completed through one NetworkLoop per worker thread.
//
// The model is the Windows completion port's, and Linux emulates it on epoll (src/platform/net/linux): an
// operation is started on a caller-owned object, finishes later as one NetworkCompletion from network_loop_poll
// (an operation that finishes at once is still delivered by the next poll), and closing an object delivers one
// completion for every operation it had pending. The members that differ per system are under #if _WIN32; shared
// code touches only the portable ones and the functions below.
//
// Errors are the Win32/Winsock numbers (WSAEINVAL, WSAEMSGSIZE, WSAECONNRESET, ...; on Linux the layer maps
// errno onto them, maicord/os/linux/error.hpp) in NetworkResult::native_error, NetworkCompletion::native_error
// and network_last_error().

#if defined(_WIN32)
struct addrinfoexW;
struct sockaddr;
#endif

namespace maicord {

enum class NetworkError : u8 {
    none,
    invalid_argument,
    winsock_startup,
    // The completion mechanism of the loop: the completion port on Windows, the epoll descriptor on Linux.
    completion_port_create,
    socket_create,
    completion_port_associate,
    connect_extension,
    bind_local,
    connect_start,
    receive_start,
    send_start,
    resolve_start,
    resolve_cancel,
    signal_post,
    file_open,
    file_read,
    file_write,
};

struct NetworkResult {
    i32 native_error;
    NetworkError error;

    constexpr bool ok() const noexcept {
        return error == NetworkError::none;
    }
};

// An IP address and port, whichever system's socket calls take it: the IPv4 address in the first four bytes of `ip`,
// everything in network byte order except `port`. A zero-initialized address is "none".
struct NetworkAddress {
    // 0 none, 4 or 6.
    u8 family;
    u8 reserved;
    // Host byte order.
    u16 port;
    u8 ip[16];
    u32 scope_id;
};
static_assert(sizeof(NetworkAddress) == 24);

// An IPv4 or IPv6 address literal ("192.0.2.1", "2001:db8::1") with `port`. False for anything else (no host
// names, no brackets, no scope suffix).
bool network_address_parse(const char* text, u16 port, NetworkAddress* address) noexcept;
// Whether `text` (UTF-16, as shared code holds host names) is an IPv6 address literal.
bool network_address_is_ipv6_literal(const wchar_t* text) noexcept;

enum class NetworkConnectionState : u8 {
    empty,
    connecting,
    connected,
    peer_closed,
    failed,
    closed,
};

enum class NetworkOperationKind : u8 {
    connect,
    receive,
    send,
    resolve,
    signal,
    file_read,
    datagram_receive,
    datagram_send,
    file_write,
};

struct NetworkConnection;
struct NetworkResolver;
struct NetworkSignal;
struct NetworkFile;
struct NetworkDatagram;

// A buffer as WSABUF lays it out (a 32-bit length, then the pointer).
struct NetworkBuffer {
    u32 len;
    char* buf;
};

#if defined(_WIN32)
// OVERLAPPED and ADDRINFOEXW layouts, so that this header needs no Winsock; native.hpp checks them against the SDK.
struct NetworkOverlapped {
    usize Internal;
    usize InternalHigh;
    DWORD Offset;
    DWORD OffsetHigh;
    HANDLE hEvent;
};
struct NetworkAddressHints {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    usize ai_addrlen;
    wchar_t* ai_canonname;
    sockaddr* ai_addr;
    void* ai_blob;
    usize ai_bloblen;
    void* ai_provider;
    addrinfoexW* ai_next;
};

struct NetworkOperation {
    NetworkOverlapped overlapped;
    void* owner;
    NetworkBuffer buffer;
    u32 generation;
    DWORD receive_flags;
    NetworkOperationKind kind;
};

struct NetworkLoop {
    HANDLE completion_port;
    bool winsock_started;
};
#else
// An operation waits in its owner until the socket is ready, then in the loop's ready list until a poll returns it
// with the outcome recorded here.
struct NetworkOperation {
    // The loop's ready list and post stack.
    NetworkOperation* next;
    void* owner;
    NetworkBuffer buffer;
    u32 generation;
    NetworkOperationKind kind;
    bool succeeded;
    // Bytes moved; for a send, the bytes sent so far while it waits.
    u32 transferred;
    i32 native_error;
};

// One epoll descriptor and one eventfd per loop. Completions go to `ready` in the order they happen; operations
// that other threads finish (signals, name lookups, file reads) arrive on the `posted` stack and wake the epoll.
struct NetworkLoop {
    int epoll;
    int wake;
    bool ready;
    NetworkOperation* ready_head;
    NetworkOperation* ready_tail;
    NetworkOperation* volatile posted;
};
#endif

// Whether network_loop_initialize succeeded and the loop was not released since.
#if defined(_WIN32)
inline bool network_loop_ready(const NetworkLoop& loop) noexcept {
    return loop.completion_port != nullptr;
}
#else
inline bool network_loop_ready(const NetworkLoop& loop) noexcept {
    return loop.ready;
}
#endif

// One overlapped file read at a time on the shared completion loop. The file
// is opened without write/delete sharing, retaining the selected bytes until
// close. Owner and read buffer stay alive until pending becomes false, even
// after close cancels the read. Open/metadata work belongs on a worker thread.
struct NetworkFile {
    NetworkOperation operation;
    HANDLE handle;
    u64 bytes;
    u32 generation;
    bool pending;
    FileIdentity identity;
#if !defined(_WIN32)
    // The loop the pool posts completions to, and the file offset of the operation the pool serves. The pool works
    // on `job_handle`, not `handle`: a close while the operation runs clears `handle` but leaves the descriptor
    // open (`close_when_done`) for the completion to close, since closing under a running call would let the number
    // be reused. `cancelled` tells the pool to skip an operation that has not started.
    NetworkLoop* loop;
    u64 offset;
    HANDLE job_handle;
    volatile u32 cancelled;
    bool close_when_done;
#endif
};
NetworkResult network_file_open(NetworkLoop* loop, NetworkFile* file, const wchar_t* path) noexcept;
NetworkResult network_file_read(NetworkFile* file, u64 offset, Slice<u8> buffer) noexcept;
// Creates a new staging file without replacing an existing path. Writes use
// the same one-operation owner/completion lifetime as network_file_read. If
// creation succeeds but association fails, handle remains owned for cleanup.
NetworkResult network_file_create(NetworkLoop* loop, NetworkFile* file, const wchar_t* path) noexcept;
NetworkResult network_file_write(NetworkFile* file, u64 offset, ConstSlice<u8> buffer) noexcept;
void network_file_close(NetworkFile* file) noexcept;

struct NetworkConnection {
#if defined(_WIN32)
    SOCKET socket;
#else
    // The descriptor + 1; 0 when there is none.
    u32 socket;
    NetworkLoop* loop;
    // Which operations wait for the socket (bit 0 connect, 1 receive, 2 send).
    u8 waiting;
#endif
    NetworkOperation connect_operation;
    NetworkOperation receive_operation;
    NetworkOperation send_operation;
    u32 generation;
    NetworkConnectionState state;
    bool connect_pending;
    bool receive_pending;
    bool send_pending;
};
// Forgets the socket of a connection that was never opened or whose socket is gone (zero-initialized storage
// counts as none); it does not close anything.
void network_connection_reset(NetworkConnection* connection) noexcept;
// Whether the connection holds an open socket.
bool network_connection_has_socket(const NetworkConnection& connection) noexcept;

// Connected UDP: the kernel filters inbound packets to this peer. One send and
// receive may overlap; packet boundaries and zero-length packets are preserved.
// Mutations and completion dispatch belong to the same owning worker.
// Zero-initialize before first use. Close cancels I/O but owner/buffers stay live
// until every pending flag clears through network_loop_poll. Reopen only then.
// Receive slot 0 is receive_operation/receive_pending; slots 1.. are optional
// extra concurrent reads (video bursts) tracked in extra_receive_pending bits.
inline constexpr u32 kNetworkDatagramReceiveSlots = 8;
struct NetworkDatagram {
#if defined(_WIN32)
    SOCKET socket;
#else
    // The descriptor + 1; 0 when there is none.
    u32 socket;
    NetworkLoop* loop;
    // Which operations wait for the socket: bit 0 send, bits 1.. the receive slots 0...
    u16 waiting;
    // The order the waiting receives were posted in: a read is served to the oldest.
    u32 receive_serial;
    u32 receive_order[kNetworkDatagramReceiveSlots];
#endif
    NetworkOperation receive_operation, send_operation;
    NetworkOperation extra_receive_operations[kNetworkDatagramReceiveSlots - 1];
    u32 generation;
    bool open, receive_pending, send_pending;
    u8 extra_receive_pending;
};
inline bool network_datagram_idle(const NetworkDatagram& datagram) noexcept {
    return !datagram.receive_pending && !datagram.send_pending && !datagram.extra_receive_pending;
}
// `peer` is an IPv4 or IPv6 address with a nonzero port.
NetworkResult network_datagram_open(NetworkLoop* loop, NetworkDatagram* datagram, const NetworkAddress& peer) noexcept;
NetworkResult network_datagram_receive(NetworkDatagram* datagram, Slice<u8> buffer) noexcept;
// Posts a read on slot (0 is network_datagram_receive). Completions report it
// in NetworkCompletion::datagram_slot.
NetworkResult network_datagram_receive_slot(NetworkDatagram* datagram, u32 slot, Slice<u8> buffer) noexcept;
// Best-effort SO_RCVBUF so kernel buffering absorbs video packet bursts.
bool network_datagram_set_receive_buffer(NetworkDatagram* datagram, u32 bytes) noexcept;
NetworkResult network_datagram_send(NetworkDatagram* datagram, ConstSlice<u8> buffer) noexcept;
void network_datagram_close(NetworkDatagram* datagram) noexcept;

enum class NetworkResolverState : u8 {
    empty,
    resolving,
    resolved,
    cancelled,
    failed,
};

// The most addresses one lookup keeps (the first ones the system returns, in its order).
inline constexpr u32 kNetworkResolverMaximumAddresses = 16;

// The hostname and service strings passed to network_resolver_start must remain
// valid until the resolve completion is removed from the NetworkLoop. Resolver
// storage has the same lifetime requirement and may only be released afterward.
// A successful completion leaves the answers in addresses[0, address_count).
struct NetworkResolver {
    NetworkOperation operation;
    NetworkAddress addresses[kNetworkResolverMaximumAddresses];
    u32 address_count;
#if defined(_WIN32)
    NetworkAddressHints hints;
    addrinfoexW* results;
    HANDLE cancel_handle;
    HANDLE completion_port;
#else
    // The lookup in the process's table of them, and the loop it completes on.
    u32 job;
    u32 job_generation;
    NetworkLoop* loop;
#endif
    u32 generation;
    i32 native_error;
    NetworkResolverState state;
    bool pending;
};

// Embedded cross-thread completion. Only one post may be outstanding per
// signal object; completion returns the caller-supplied value.
struct NetworkSignal {
    NetworkOperation operation;
    u32 value;
    volatile LONG pending;
};

enum class NetworkPollState : u8 {
    completion,
    timeout,
    error,
};

struct NetworkCompletion {
    NetworkConnection* connection;
    NetworkResolver* resolver;
    NetworkSignal* signal;
    NetworkFile* file;
    u32 transferred;
    u32 signal_value;
    i32 native_error;
    NetworkOperationKind kind;
    bool succeeded;
    bool stale;
    NetworkDatagram* datagram;
    u8 datagram_slot; // Receive slot of a datagram_receive completion.
};

struct NetworkPollResult {
    NetworkCompletion completion;
    NetworkPollState state;
};

// Windows: the first successful initialization also pins one Winsock reference for the
// process lifetime: a cancelled lookup outlives its completion, so releasing
// the last loop must not unload Winsock.
NetworkResult network_loop_initialize(NetworkLoop* loop) noexcept;
void network_loop_release(NetworkLoop* loop) noexcept;

NetworkResult network_connection_connect(NetworkLoop* loop, NetworkConnection* connection,
                                         const NetworkAddress& address) noexcept;
NetworkResult network_connection_receive(NetworkConnection* connection, Slice<u8> buffer) noexcept;
NetworkResult network_connection_send(NetworkConnection* connection, ConstSlice<u8> buffer) noexcept;
void network_connection_close(NetworkConnection* connection) noexcept;

NetworkResult network_resolver_start(NetworkLoop* loop, NetworkResolver* resolver,
                                     const wchar_t* hostname, const wchar_t* service) noexcept;
NetworkResult network_resolver_cancel(NetworkResolver* resolver) noexcept;
bool network_resolver_release(NetworkResolver* resolver) noexcept;

NetworkResult network_signal_post(NetworkLoop* loop, NetworkSignal* signal, u32 value) noexcept;

NetworkPollResult network_loop_poll(NetworkLoop* loop, u32 timeout_milliseconds) noexcept;

// The calling thread's last network error as a Win32 number: WSAGetLastError() on Windows, the last error the
// network layer mapped from errno on Linux. Read it right after the failing call, as WSAGetLastError() is.
i32 network_last_error() noexcept;

} // namespace maicord
