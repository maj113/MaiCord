#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// RTP video packetizers, the inverse of rtp_h264/rtp_h265/rtp_av1 (what the
// receive path depacketizes back into the same bytes, DAVE trailer included):
//   H.264 (RFC 6184, packetization mode 1): single NAL unit packets, STAP-A
//     for runs of small units, FU-A for units larger than a payload.
//   H.265 (RFC 7798): single NAL unit packets, APs, FUs.
//   AV1 (AOM RTP payload v1.0): one aggregation header per packet, OBU
//     elements without obu_size (each with a LEB128 length), Z/Y across
//     fragments, N on the first packet of a key frame.
// Input: Annex B with start codes of any length (H.264/H.265), AV1 OBUs with
// obu_size (the last may lack it and runs to the end). Payloads are cut to at
// most max_payload bytes, fragments of one unit evened out as WebRTC does.
enum class RtpVideoPacketCodec : u8 {
    h264,
    h265,
    av1
};
// RTP header (12), one-byte extensions (up to 16) and transport AEAD (20) stay under a 1200-byte datagram.
inline constexpr u32 kRtpVideoMaximumPayload = 1100;
struct RtpVideoPacketizer {
    ConstSlice<u8> frame;
    u32 max_payload;
    // Current NAL unit/OBU: header at unit, payload [payload, unit_end); next_unit follows it.
    u32 unit, payload, unit_end, next_unit;
    u32 sent;                // Fragmenting: bytes of the unit's payload (AV1: element) already sent.
    u32 fragments, fragment; // H.264/H.265 fragment count of the current unit and the next one's index.
    RtpVideoPacketCodec codec;
    bool keyframe, first, failed;
};
// False for a malformed frame (nothing to send).
bool rtp_packetizer_begin(RtpVideoPacketizer* packetizer, RtpVideoPacketCodec codec, ConstSlice<u8> frame,
                          bool keyframe, u32 max_payload) noexcept;
// Writes the next payload into output (at least max_payload bytes) and
// returns its size; 0 once the frame is done or on failure (failed set).
// *marker is set on the frame's last packet.
u32 rtp_packetizer_next(RtpVideoPacketizer* packetizer, Slice<u8> output, bool* marker) noexcept;
// Helpers the packetizers share with the receive path, DAVE's frame preparation and the encoder's unit rewriting.
// The first start code prefix (00 00 01) at or after from: *start is where its zeros begin (a fourth zero included),
// the result where the unit after it begins; both are data.count when there is none.
u32 annexb_next_unit(ConstSlice<u8> data, u32 from, u32* start) noexcept;
// The shortest LEB128 form of a value (an AV1 obu_size): its length, and writing it.
u32 leb128_bytes(u32 value) noexcept;
u32 leb128_write(u8* output, u32 value) noexcept;
} // namespace maicord
