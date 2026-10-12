#pragma once
#include "maicord/media/rtp_video_assembly.hpp"

namespace maicord {
inline constexpr u8 kH265NalVps = 32, kH265NalSps = 33, kH265NalPps = 34;
// RFC 7798 depacketizer without DONL (sprop-max-don-diff = 0): single NAL unit
// packets (types 0-47), aggregation packets (48) and fragmentation units (49).
// PACI (50) and unassigned types are damaged. Same result contract as H.264.
RtpDepacketize rtp_h265_push(RtpVideoAssembly* assembly, ConstSlice<u8> payload, bool gap_before) noexcept;
// Parameter set/AUD/prefix SEI, or a VCL NAL start with
// first_slice_segment_in_pic_flag set.
bool rtp_h265_starts_access_unit(ConstSlice<u8> payload) noexcept;
// IRAP pictures: BLA_W_LP (16) through CRA_NUT (21).
constexpr bool rtp_h265_keyframe(const RtpVideoAssembly& assembly) noexcept {
    return (assembly.nal_types & (u64{0x3f} << 16)) != 0;
}
} // namespace maicord
