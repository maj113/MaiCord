#pragma once
// Winsock views of network.hpp's layout-identical records.
#include "maicord/platform/network.hpp"

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

inline OVERLAPPED* native(NetworkOverlapped* value) noexcept {
    return reinterpret_cast<OVERLAPPED*>(value);
}

inline WSABUF* native(NetworkBuffer* value) noexcept {
    return reinterpret_cast<WSABUF*>(value);
}

inline ADDRINFOEXW* native(NetworkAddressHints* value) noexcept {
    return reinterpret_cast<ADDRINFOEXW*>(value);
}

} // namespace maicord
