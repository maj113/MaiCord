#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kVoiceDiscoveryBytes = 74;
struct VoiceDiscoveryAddress {
    char address[64];
    u32 address_bytes;
    u16 port;
};
bool voice_discovery_request(u32 ssrc, Slice<u8> output) noexcept;
// Validate exact packet size/type/length, matching SSRC, a bounded terminated
// address and nonzero port. Numeric IP validation belongs to the socket layer.
// The response owns its text; failure preserves output.
bool voice_discovery_response(ConstSlice<u8> packet, u32 ssrc, VoiceDiscoveryAddress* output) noexcept;
} // namespace maicord
