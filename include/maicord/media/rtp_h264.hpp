#pragma once
#include "maicord/media/rtp_video_assembly.hpp"

namespace maicord {
inline constexpr u8 kH264NalIdr = 5, kH264NalSps = 7, kH264NalPps = 8;
// RFC 6184 packetization-mode 1 depacketizer: single NAL unit packets (types
// 1-23), STAP-A (24) and FU-A (28). STAP-B, MTAP and FU-B are rejected as
// damaged. `gap_before` reports that the packet preceding this payload in the
// access unit was lost: any partial FU-A is dropped and the unit is damaged.
// Damaged/overflow results keep no bytes of the payload; previous NAL units
// stay in the output so callers can inspect, but must not decode, them.
RtpDepacketize rtp_h264_push(RtpVideoAssembly* assembly, ConstSlice<u8> payload, bool gap_before) noexcept;
// True when the payload begins a new access unit: a parameter set/AUD/SEI, or
// a VCL slice (start of a FU-A or a complete NAL) with first_mb_in_slice == 0.
// Used only when the preceding packet is unknown (stream start or loss).
bool rtp_h264_starts_access_unit(ConstSlice<u8> payload) noexcept;
constexpr bool rtp_h264_keyframe(const RtpVideoAssembly& assembly) noexcept {
    return (assembly.nal_types >> kH264NalIdr) & 1;
}
} // namespace maicord
