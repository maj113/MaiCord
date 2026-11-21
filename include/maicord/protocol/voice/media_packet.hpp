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
// The checks of media_packet_plaintext after the clear header. `layout` is the media_packet_header result for the
// same clear header and count: voice_packet_open parses the ciphertext and checks the plaintext, which has the same
// clear header. Sets the payload fields. A failure can leave some of them changed.
bool media_packet_payload(ConstSlice<u8> packet, MediaPacketLayout* layout) noexcept;
} // namespace maicord
