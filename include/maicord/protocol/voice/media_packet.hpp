#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
enum class MediaPacketKind : u8 {
    rtp,
    rtcp
};
struct MediaPacketLayout {
    MediaPacketKind kind;
    u32 clear_bytes, extension_bytes, payload_offset, payload_bytes;
    u32 ssrc, timestamp;
    u16 sequence, extension_profile;
    u8 type, csrc_count, padding_bytes;
    bool marker, padded;
};
// RTP/RTCP-mux classification, version and clear-header bounds. Values are
// untrusted until transport authentication. Extension elements stay encrypted.
// Failures preserve output; output cannot overlap the input packet.
bool media_packet_header(ConstSlice<u8> packet, MediaPacketLayout* output) noexcept;
// Additionally validate RTP extension/padding bounds or compound RTCP packet
// lengths/padding. RTP payload excludes extension contents and trailing padding.
// This is framing validation, not negotiated codec or RTCP feedback decoding.
bool media_packet_plaintext(ConstSlice<u8> packet, MediaPacketLayout* output) noexcept;
} // namespace maicord
