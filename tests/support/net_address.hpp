#pragma once

#include "maicord/core/memory.hpp"
#include "maicord/platform/net/network.hpp"
#include "maicord/sys/winsock.hpp"

// The Windows tests that talk to a Winsock peer directly name its address as a sockaddr; the network layer takes a
// NetworkAddress.

namespace maicord {

// The NetworkAddress of an IPv4 or IPv6 sockaddr (`bytes` long); an empty one for anything else.
inline NetworkAddress test_network_address(const void* socket_address, u32 bytes) noexcept {
    NetworkAddress address{};
    const auto* generic = static_cast<const sockaddr*>(socket_address);
    if (generic->sa_family == AF_INET && bytes >= sizeof(sockaddr_in)) {
        const auto* v4 = static_cast<const sockaddr_in*>(socket_address);
        address.family = 4;
        address.port = ntohs(v4->sin_port);
        copy_bytes(address.ip, &v4->sin_addr, 4);
    } else if (generic->sa_family == AF_INET6 && bytes >= sizeof(sockaddr_in6)) {
        const auto* v6 = static_cast<const sockaddr_in6*>(socket_address);
        address.family = 6;
        address.port = ntohs(v6->sin6_port);
        copy_bytes(address.ip, &v6->sin6_addr, 16);
        address.scope_id = v6->sin6_scope_id;
    }
    return address;
}

} // namespace maicord
