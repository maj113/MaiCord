#pragma once
#include "maicord/media/av1.hpp"
#include "maicord/media/rtp_video_assembly.hpp"

namespace maicord {
// Aggregation header N bit: the packet starts a coded video sequence.
inline constexpr u64 kRtpAv1NewSequence = u64{1} << 32;
// AOM AV1 RTP payload (v1.0) depacketizer: aggregation header Z/Y/W/N and
// OBU elements, fragments joined across packets. The output is the frame
// Discord's patched WebRTC depacketizer builds, which DAVE's unencrypted
// ranges are defined over: every OBU but the last gets obu_has_size_field
// and a minimal LEB128 obu_size; the last OBU keeps its received header and
// runs to the end of the frame (it carries the DAVE trailer). Elements are
// otherwise copied verbatim. Z must match the previous packet's Y; a
// mismatch, a gap or a malformed element damages the unit. nal_types gets
// bit n for every OBU of type n, plus kRtpAv1NewSequence.
RtpDepacketize rtp_av1_push(RtpVideoAssembly* assembly, ConstSlice<u8> payload, bool gap_before) noexcept;
// A payload that does not continue an OBU and begins with a sequence header,
// temporal delimiter or frame (header), or has N set.
bool rtp_av1_starts_temporal_unit(ConstSlice<u8> payload) noexcept;
// OBU headers stay unencrypted under DAVE, so a key temporal unit is told by
// N or its sequence header; the decoder checks frame_type after decryption.
constexpr bool rtp_av1_keyframe(const RtpVideoAssembly& assembly) noexcept {
    return (assembly.nal_types & (kRtpAv1NewSequence | u64{1} << kAv1ObuSequenceHeader)) != 0;
}
} // namespace maicord
