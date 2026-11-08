#pragma once
// Winsock views of network.hpp's layout-identical records, and the conversions between NetworkAddress and sockaddr.
#include "maicord/platform/net/network.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/sys/winsock.hpp"
#include <stddef.h>

namespace maicord {

static_assert(sizeof(NetworkOverlapped) == sizeof(OVERLAPPED) &&
              offsetof(NetworkOverlapped, Offset) == offsetof(OVERLAPPED, Offset) &&
              offsetof(NetworkOverlapped, hEvent) == offsetof(OVERLAPPED, hEvent));
static_assert(sizeof(NetworkBuffer) == sizeof(WSABUF) && offsetof(NetworkBuffer, buf) == offsetof(WSABUF, buf));
static_assert(sizeof(NetworkAddressHints) == sizeof(ADDRINFOEXW) &&
              offsetof(NetworkAddressHints, ai_addrlen) == offsetof(ADDRINFOEXW, ai_addrlen) &&
              offsetof(NetworkAddressHints, ai_next) == offsetof(ADDRINFOEXW, ai_next));
static_assert(sizeof(NetworkCompletionEntry) == sizeof(OVERLAPPED_ENTRY) &&
              offsetof(NetworkCompletionEntry, lpOverlapped) == offsetof(OVERLAPPED_ENTRY, lpOverlapped) &&
              offsetof(NetworkCompletionEntry, dwNumberOfBytesTransferred) ==
                  offsetof(OVERLAPPED_ENTRY, dwNumberOfBytesTransferred));

inline OVERLAPPED* native(NetworkOverlapped* value) noexcept {
    return reinterpret_cast<OVERLAPPED*>(value);
}

inline OVERLAPPED_ENTRY* native(NetworkCompletionEntry* value) noexcept {
    return reinterpret_cast<OVERLAPPED_ENTRY*>(value);
}

inline WSABUF* native(NetworkBuffer* value) noexcept {
    return reinterpret_cast<WSABUF*>(value);
}

inline ADDRINFOEXW* native(NetworkAddressHints* value) noexcept {
    return reinterpret_cast<ADDRINFOEXW*>(value);
}

// The sockaddr of `address` in `storage` and its byte count; 0 for an address that is neither IPv4 nor IPv6.
inline i32 socket_address(const NetworkAddress& address, sockaddr_storage* storage) noexcept {
    clear_bytes(storage, sizeof(*storage));
    if (address.family == 4) {
        auto* v4 = reinterpret_cast<sockaddr_in*>(storage);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(address.port);
        copy_bytes(&v4->sin_addr, address.ip, 4);
        return sizeof(sockaddr_in);
    }
    if (address.family == 6) {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(storage);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(address.port);
        copy_bytes(&v6->sin6_addr, address.ip, 16);
        v6->sin6_scope_id = address.scope_id;
        return sizeof(sockaddr_in6);
    }
    return 0;
}

// The NetworkAddress of an IPv4 or IPv6 sockaddr; false for anything else.
inline bool network_address_from_socket(const sockaddr* source, usize bytes, NetworkAddress* address) noexcept {
    clear_bytes(address, sizeof(*address));
    if (!source || bytes < sizeof(ADDRESS_FAMILY)) {
        return false;
    }
    if (source->sa_family == AF_INET && bytes >= sizeof(sockaddr_in)) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(source);
        address->family = 4;
        address->port = ntohs(v4->sin_port);
        copy_bytes(address->ip, &v4->sin_addr, 4);
        return true;
    }
    if (source->sa_family == AF_INET6 && bytes >= sizeof(sockaddr_in6)) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(source);
        address->family = 6;
        address->port = ntohs(v6->sin6_port);
        copy_bytes(address->ip, &v6->sin6_addr, 16);
        address->scope_id = v6->sin6_scope_id;
        return true;
    }
    return false;
}

} // namespace maicord
