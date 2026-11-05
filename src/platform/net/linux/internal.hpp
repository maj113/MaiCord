#pragma once
// The pieces of the Linux network layer that its source files share: the error mapping, the loop's ready list and
// post stack, and the ring helpers every kind of operation starts and is cancelled through.
//
// Everything that touches a NetworkLoop's ring or ready list runs on the loop's own thread (the ring is made with
// IORING_SETUP_SINGLE_ISSUER). Other threads (the name lookup pool, network_signal_post callers) reach it only
// through post(), which pushes onto the lock-free stack and wakes the ring through its eventfd.
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/linux/error.hpp"
#include "maicord/platform/net/network.hpp"
#include "maicord/sys/linux/calls.hpp"

namespace maicord::net_detail {

// Winsock error numbers the shared code does not name (maicord/core/win32_error.hpp has the ones it does).
inline constexpr i32 kWsaNoBuffers = 10055;
inline constexpr i32 kWsaTimedOut = 10060;
inline constexpr i32 kWsaConnectionRefused = 10061;
inline constexpr i32 kWsaNotConnected = 10057;
inline constexpr i32 kWsaNetworkUnreachable = 10051;
inline constexpr i32 kWsaAddressNotAvailable = 10049;
inline constexpr i32 kWsaConnectionAborted = 10053;
inline constexpr i32 kWsaHostNotFound = 11001;
inline constexpr i32 kWsaTryAgain = 11002;
inline constexpr i32 kWsaNoRecovery = 11003;

// The Win32/Winsock number of a positive errno, the way shared code compares them (see the header of network.hpp).
i32 map_errno(int error) noexcept;
// The same for a failed raw system call result (a negated errno); also stored as the thread's network error.
i32 map_result(long result) noexcept;
void set_error(i32 error) noexcept;

inline NetworkResult success() noexcept {
    return NetworkResult{0, NetworkError::none};
}

inline NetworkResult failure(NetworkError error, i32 native_error) noexcept {
    set_error(native_error);
    return NetworkResult{native_error, error};
}

inline int descriptor(u32 socket) noexcept {
    return static_cast<int>(socket) - 1;
}

inline u32 token(int fd) noexcept {
    return static_cast<u32>(fd) + 1;
}

// Queues the kernel request that `operation` stands for, built from its kind, its buffer, its transferred count and
// its owner (a connection or datagram socket, or a file and its offset). The next poll hands it to the kernel.
void start(NetworkLoop* loop, NetworkOperation* operation) noexcept;
// Hands every queued entry to the kernel now; false when the system call failed (the entries stay queued).
bool submit(NetworkLoop* loop) noexcept;
// Cancels every request on the socket `fd` and submits at once, so that the descriptor can be closed behind it: each
// request still finishes with a completion of its own, cancelled.
void cancel_socket(NetworkLoop* loop, int fd) noexcept;
// Queues the cancellation of one operation (a file operation that may sit in a slow file system).
void cancel_operation(NetworkLoop* loop, NetworkOperation* operation) noexcept;

// Records the outcome of an operation and puts it in the ready list: the next poll returns it. Loop thread only.
void complete(NetworkLoop* loop, NetworkOperation* operation, bool succeeded, u32 transferred, i32 native_error) noexcept;
// The same from any thread (it goes through the post stack); the operation must not be touched afterwards.
void post(NetworkLoop* loop, NetworkOperation* operation, bool succeeded, u32 transferred, i32 native_error) noexcept;

// The error of a datagram socket's failed call: a refused connected UDP socket reports the ICMP error on the next
// call, which Windows calls a connection reset.
i32 datagram_error(long result) noexcept;
// A datagram's read finished (completed or cancelled): the next posted slot's read starts.
void receive_finished(NetworkDatagram* datagram) noexcept;

// The operation of a resolve, signal, file or socket completion that poll returns: fills the rest of
// NetworkPollResult. `operation` came off the ready list.
void translate_resolver(NetworkOperation* operation, NetworkPollResult* result) noexcept;
void translate_file(NetworkOperation* operation, NetworkPollResult* result) noexcept;
void translate_datagram(NetworkOperation* operation, NetworkPollResult* result) noexcept;

// The ASCII text of a UTF-16 host or service name (null is the empty string); false when it holds anything else or
// does not fit `capacity` with its terminator.
bool narrow_text(const wchar_t* text, char* output, u32 capacity) noexcept;
// getaddrinfo for TCP on the calling thread: up to kNetworkResolverMaximumAddresses answers in `addresses`, or the
// Winsock error in *error (0 on success).
void lookup(const char* host, const char* service, NetworkAddress* addresses, u32* count, i32* error) noexcept;

// A loop is going away: the lookups that would complete on it are abandoned.
void resolver_forget_loop(NetworkLoop* loop) noexcept;

// The readable sockaddr of a NetworkAddress for the kernel, and back; the byte count, or 0.
u32 to_socket_address(const NetworkAddress& address, SockAddrStorage* storage) noexcept;
bool from_socket_address(const void* socket_address, u32 bytes, NetworkAddress* address) noexcept;

} // namespace maicord::net_detail
