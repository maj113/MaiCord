#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/platform/file_identity.hpp"
#include "maicord/sys/winsock.hpp"

struct addrinfoexW;
struct sockaddr;

namespace maicord {

enum class NetworkError : u8 {
    none,
    invalid_argument,
    winsock_startup,
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
    poll,
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

// OVERLAPPED, WSABUF and ADDRINFOEXW layouts, so that this header needs no
// Winsock; network_native.hpp checks them against the SDK.
struct NetworkOverlapped {
    usize Internal;
    usize InternalHigh;
    DWORD Offset;
    DWORD OffsetHigh;
    HANDLE hEvent;
};
struct NetworkBuffer {
    unsigned long len;
    char* buf;
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
    SOCKET socket;
    NetworkOperation connect_operation;
    NetworkOperation receive_operation;
    NetworkOperation send_operation;
    u32 generation;
    NetworkConnectionState state;
    bool connect_pending;
    bool receive_pending;
    bool send_pending;
};

// Connected UDP: the kernel filters inbound packets to this peer. One send and
// receive may overlap; packet boundaries and zero-length packets are preserved.
// Mutations and completion dispatch belong to the same owning worker.
// Zero-initialize before first use. Close cancels I/O but owner/buffers stay live
// until every pending flag clears through network_loop_poll. Reopen only then.
// Receive slot 0 is receive_operation/receive_pending; slots 1.. are optional
// extra concurrent reads (video bursts) tracked in extra_receive_pending bits.
inline constexpr u32 kNetworkDatagramReceiveSlots = 8;
struct NetworkDatagram {
    SOCKET socket;
    NetworkOperation receive_operation, send_operation;
    NetworkOperation extra_receive_operations[kNetworkDatagramReceiveSlots - 1];
    u32 generation;
    bool open, receive_pending, send_pending;
    u8 extra_receive_pending;
};
inline bool network_datagram_idle(const NetworkDatagram& datagram) noexcept {
    return !datagram.receive_pending && !datagram.send_pending && !datagram.extra_receive_pending;
}
NetworkResult network_datagram_open(NetworkLoop* loop, NetworkDatagram* datagram,
    const sockaddr* peer, i32 peer_bytes) noexcept;
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

// The hostname and service strings passed to network_resolver_start must remain
// valid until the resolve completion is removed from the NetworkLoop. Resolver
// storage has the same lifetime requirement and may only be released afterward.
struct NetworkResolver {
    NetworkOperation operation;
    NetworkAddressHints hints;
    addrinfoexW* results;
    HANDLE cancel_handle;
    HANDLE completion_port;
    u32 generation;
    i32 native_error;
    NetworkResolverState state;
    bool pending;
};

// Embedded cross-thread IOCP packet. Only one post may be outstanding per
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

// The first successful initialization also pins one Winsock reference for the
// process lifetime: a cancelled lookup outlives its completion, so releasing
// the last loop must not unload Winsock.
NetworkResult network_loop_initialize(NetworkLoop* loop) noexcept;
void network_loop_release(NetworkLoop* loop) noexcept;

NetworkResult network_connection_connect(NetworkLoop* loop, NetworkConnection* connection,
                                         const sockaddr* address, i32 address_bytes) noexcept;
NetworkResult network_connection_receive(NetworkConnection* connection, Slice<u8> buffer) noexcept;
NetworkResult network_connection_send(NetworkConnection* connection, ConstSlice<u8> buffer) noexcept;
void network_connection_close(NetworkConnection* connection) noexcept;

NetworkResult network_resolver_start(NetworkLoop* loop, NetworkResolver* resolver,
                                     const wchar_t* hostname, const wchar_t* service) noexcept;
NetworkResult network_resolver_cancel(NetworkResolver* resolver) noexcept;
bool network_resolver_release(NetworkResolver* resolver) noexcept;

NetworkResult network_signal_post(NetworkLoop* loop, NetworkSignal* signal, u32 value) noexcept;

NetworkPollResult network_loop_poll(NetworkLoop* loop, u32 timeout_milliseconds) noexcept;

} // namespace maicord
