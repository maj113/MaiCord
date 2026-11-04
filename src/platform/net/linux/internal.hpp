#pragma once
// The pieces of the Linux network layer that its source files share: the error mapping, the loop's ready list and
// post stack, and the hooks the poll loop calls for socket events and finished lookups and file operations.
//
// Everything that touches a NetworkLoop's ready list runs on the loop's own thread. Other threads (the name lookup
// and file pools, network_signal_post callers) reach it only through post(), which pushes onto the lock-free
// stack and wakes the epoll.
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

// How an epoll event names its owner: the owner's address in the high bits, its kind in the low two.
inline constexpr u64 kOwnerConnection = 0;
inline constexpr u64 kOwnerDatagram = 1;
inline constexpr u64 kOwnerWake = 2;

// Registers the socket for edge-triggered readiness of both directions and hangups; false with the error set.
bool watch_socket(NetworkLoop* loop, int fd, const void* owner, u64 kind, u32 events) noexcept;

// Records the outcome of an operation and puts it in the ready list: the next poll returns it. Loop thread only.
void complete(NetworkLoop* loop, NetworkOperation* operation, bool succeeded, u32 transferred, i32 native_error) noexcept;
// The same from any thread (it goes through the post stack); the operation must not be touched afterwards.
void post(NetworkLoop* loop, NetworkOperation* operation, bool succeeded, u32 transferred, i32 native_error) noexcept;

// What the poll loop calls when epoll reports `events` for a socket's owner.
void service_connection(NetworkConnection* connection, u32 events) noexcept;
void service_datagram(NetworkDatagram* datagram, u32 events) noexcept;

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
