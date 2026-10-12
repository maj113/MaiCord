#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

enum class H264Error : u8 { none, end, invalid_input, truncated, malformed, unsupported };

// AVC length-prefixed NALs stay in caller storage. No Annex-B copy.
struct H264NalCursor {
    ConstSlice<u8> bytes;
    u32 offset;
    u8 length_bytes;
};
struct H264Nal {
    ConstSlice<u8> bytes; // Includes the one-byte NAL header, excludes length.
    H264Error error;
    u8 type;
    u8 reference;
};
H264Nal h264_next_nal(H264NalCursor* cursor) noexcept;

// Scaling lists in bitstream scan order, also used by DXVA_Qmatrix_H264.
struct H264Scaling {
    u8 list4x4[6][16];
    u8 list8x8[2][64];
};

// One active SPS is decoded; do not allocate a full table of these per stream.
// All fields are values, so parsing never keeps the input buffer.
struct H264Sps {
    H264Scaling scaling;
    i32 cycle_offsets[255];
    i64 cycle_delta;
    u32 coded_width, coded_height, crop_x, crop_y, width, height;
    u32 units_in_tick, time_scale;
    i32 offset_non_reference, offset_top_to_bottom;
    u16 sar_width, sar_height;
    u8 id, profile, constraints, level;
    u8 frame_num_bits, poc_type, poc_bits, max_references, cycle_count;
    u8 max_reorder, max_buffering;
    u8 primaries, transfer, matrix;
    bool frame_only, mbaff, direct_8x8, delta_always_zero, gaps;
    bool transform_bypass, scaling_present, full_range, fixed_rate, restriction;
};
struct H264Pps {
    H264Scaling scaling;
    u8 id, sps_id, references_l0, references_l1, weighted_bipred;
    signed char initial_qp, initial_qs, chroma_qp, second_chroma_qp;
    bool cabac, bottom_poc_present, weighted_pred, deblocking_control;
    bool constrained_intra, redundant_count, transform_8x8, scaling_present;
};

// Baseline/Main/High 8-bit 4:2:0 parameter syntax. Unsupported coding tools
// report unsupported instead of being silently approximated. Output is only
// assigned after complete validation, including RBSP trailing bits.
H264Error h264_parse_sps(ConstSlice<u8> nal, H264Sps* output) noexcept;
H264Error h264_parse_pps(ConstSlice<u8> nal, const H264Sps& sps, H264Pps* output) noexcept;
// Resolve one active avcC pair without allocating a 288-entry parameter table.
H264Error h264_select_parameters(ConstSlice<u8> avcc, u8 pps_id,
    H264Sps* sps, H264Pps* pps) noexcept;

// Decoded picture buffer frames a conforming stream may hold: the signalled
// max_dec_frame_buffering, else the level limit MaxDpbMbs / frame size (Table
// A-1, at least max_num_ref_frames). 16 when the level is unknown or the SPS
// contradicts it. Decoder surface pools size from this, not the 16-frame
// syntax maximum.
u8 h264_sps_dpb_frames(const H264Sps& sps) noexcept;
// Output (bumping) delay: max_num_reorder_frames when signalled, else the DPB.
u8 h264_sps_output_reorder(const H264Sps& sps) noexcept;

static_assert(sizeof(H264Scaling) == 224);
static_assert(sizeof(H264Sps) <= 1'328);
static_assert(sizeof(H264Pps) <= 244);

} // namespace maicord
