#pragma once

#include "maicord/media/video/h264_slice.hpp"

namespace maicord {

using i8 = signed char;

// Everything a decoder that takes slice parameters (VA-API; DXVA short slices leave it to the driver) needs from a
// slice header beyond what h264_parse_slice keeps: the reference list changes, the explicit prediction weights, the
// deblocking and CABAC parameters and the position where slice_data() starts. Frame-coded Baseline/Main/High slices
// only, as h264_parse_slice.
struct H264ListChange {
    u8 operation;  // 0 and 1: abs_diff_pic_num_minus1 follows (subtract, add); 2: long_term_pic_num
    u32 value;
};
inline constexpr u32 kH264MaximumListChanges = 33;
struct H264SliceDetail {
    u32 first_mb;
    // Bits from the start of the NAL unit (its header byte included) to slice_data(), counted after the removal of
    // emulation prevention bytes and, for CABAC, past the cabac_alignment_one_bits.
    u32 data_bit_offset;
    H264ListChange changes[2][kH264MaximumListChanges];
    u8 change_count[2];
    u8 active[2]; // num_ref_idx_l0_active, l1 (the PPS defaults unless overridden); l1 is 0 outside B slices
    H264SliceType type;
    bool direct_spatial_mv_pred;
    // pred_weight_table(): present for explicitly weighted P and B slices. Entries without a coded weight hold the
    // default weight (1 << denominator) and offset 0, so they can be used as they are.
    bool weighted;
    u8 luma_log2_weight_denom, chroma_log2_weight_denom;
    bool luma_flag[2], chroma_flag[2]; // some entry of the list has a coded luma / chroma weight
    i16 luma_weight[2][16], luma_offset[2][16];
    i16 chroma_weight[2][16][2], chroma_offset[2][16][2];
    u8 cabac_init_idc;
    i8 qp_delta;
    u8 disable_deblocking_filter_idc;
    i8 alpha_c0_offset_div2, beta_offset_div2;
};

// Parses the whole header of `nal` (header byte included, no start code) against its active parameter sets. Only
// success writes output.
H264Error h264_parse_slice_detail(ConstSlice<u8> nal, const H264Sps& sps, const H264Pps& pps,
                                  H264SliceDetail* output) noexcept;

} // namespace maicord
