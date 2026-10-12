#pragma once

#include "maicord/media/h264_parameters.hpp"

namespace maicord {

enum class H264SliceType : u8 { p, b, i };
struct H264Marking {
    u16 difference_minus1;
    u8 long_term; // index for 2/3/6; max index plus one for operation 4.
    u8 operation;
};
struct H264Slice {
    H264Marking marking[32];
    i32 delta_bottom, delta_poc0, delta_poc1;
    u32 first_mb;
    u16 frame_num, idr_id, poc_lsb;
    u8 pps_id, reference, references_l0, references_l1, marking_count;
    H264SliceType type;
    bool idr, all_same_type, discard_prior, long_term, adaptive_marking;
};

// Frame-coded Baseline/Main/High slices. Weighted prediction and list changes
// are validated but not duplicated into CPU tables: DXVA short-slice mode
// consumes them from the original compressed bytes. SPS/PPS must be validated.
// Only success writes output. Field pictures, redundant slices and SP/SI are
// unsupported; reference semantics are never silently approximated.
H264Error h264_parse_slice(ConstSlice<u8> nal, const H264Sps& sps,
    const H264Pps& pps, H264Slice* output) noexcept;
H264Error h264_slice_parameter_id(ConstSlice<u8> nal, u8* output) noexcept;
bool h264_same_picture(const H264Slice& a, const H264Slice& b) noexcept;
struct H264Picture {
    H264Slice first;
    u32 slice_count, bitstream_bytes;
    bool all_intra;
};
// One length-prefixed AVC access unit, keeping no compressed pointers.
H264Error h264_parse_picture(ConstSlice<u8> bytes, u8 length_bytes, const H264Sps& sps, const H264Pps& pps,
                             H264Picture* output, ConstSlice<u8> avcc = {}) noexcept;

static_assert(sizeof(H264Marking) == 4);
static_assert(sizeof(H264Slice) <= 164);

} // namespace maicord
