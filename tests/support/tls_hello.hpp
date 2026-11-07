#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// The data of extension `type` in the ClientHello record `hello`, or an empty slice.
inline ConstSlice<u8> hello_extension(ConstSlice<u8> hello, u16 type) noexcept {
    const auto u16_at = [&](u32 at) noexcept { return static_cast<u32>(hello.data[at] << 8 | hello.data[at + 1]); };
    // Record header (5), handshake header (4), version (2), random (32).
    u32 at = 5 + 4 + 2 + 32;
    if (hello.count < at + 1 || hello.data[0] != 22 || hello.data[5] != 1)
        return {};
    at += 1 + hello.data[at]; // session id
    if (hello.count < at + 2)
        return {};
    at += 2 + u16_at(at); // cipher suites
    if (hello.count < at + 1)
        return {};
    at += 1 + hello.data[at]; // compression methods
    if (hello.count < at + 2)
        return {};
    const u32 end = at + 2 + u16_at(at);
    for (at += 2; end <= hello.count && at + 4 <= end;) {
        const u32 bytes = u16_at(at + 2);
        if (at + 4 + bytes > end)
            return {};
        if (u16_at(at) == type)
            return {hello.data + at + 4, bytes};
        at += 4 + bytes;
    }
    return {};
}

// The cipher suites of the ClientHello record `hello` (two bytes each, in offer order), or an empty slice.
inline ConstSlice<u8> hello_cipher_suites(ConstSlice<u8> hello) noexcept {
    u32 at = 5 + 4 + 2 + 32;
    if (hello.count < at + 1 || hello.data[0] != 22 || hello.data[5] != 1)
        return {};
    at += 1 + hello.data[at]; // session id
    if (hello.count < at + 2)
        return {};
    const u32 bytes = static_cast<u32>(hello.data[at] << 8 | hello.data[at + 1]);
    return hello.count < at + 2 + bytes ? ConstSlice<u8>{} : ConstSlice<u8>{hello.data + at + 2, bytes};
}

} // namespace maicord
