#pragma once

#include "maicord/media/video/hevc.hpp"

namespace maicord {

using i8 = signed char;

// Everything a decoder that takes slice parameters (VA-API; DXVA short slices leave it to the driver) needs from a slice
// segment header beyond what hevc_parse_slice keeps: the reference list changes, the explicit prediction weights, the
// slice-level coding flags and offsets, and the position where slice_segment_data() starts. Main profile (8-bit 4:2:0)
// only, as hevc_parse_slice.
struct HevcSliceDetail {
    u32 address; // slice_segment_address
    // Bytes from the start of the NAL unit (its two header bytes included) to slice_segment_data(), counted after
    // the removal of emulation prevention bytes; escape_bytes is how many of those the header holds.
    u32 data_byte_offset;
    u16 escape_bytes;
    u16 entry_points; // num_entry_point_offsets
    bool first, dependent;
    u8 slice_type; // 0 B, 1 P, 2 I
    bool sao_luma, sao_chroma, mvd_l1_zero, cabac_init, temporal_mvp, deblocking_disabled, collocated_from_l0,
        loop_filter_across_slices;
    u8 active[2]; // num_ref_idx_l0_active, l1 (the PPS defaults unless overridden); 0 for lists the slice type lacks
    bool modified[2];
    u8 list_entry[2][15]; // list_entry_l0 / l1 when modified
    u8 collocated_ref_idx;
    u8 five_minus_max_num_merge_cand;
    bool weighted;
    u8 luma_log2_weight_denom;
    i8 delta_chroma_log2_weight_denom;
    i8 delta_luma_weight[2][15], luma_offset[2][15];
    i8 delta_chroma_weight[2][15][2];
    i8 chroma_offset[2][15][2]; // ChromaOffsetL0 / L1, derived as the standard does (7-56)
    i8 qp_delta, cb_qp_offset, cr_qp_offset; // slice_qp_delta and the slice-level chroma offsets
    i8 beta_offset_div2, tc_offset_div2;     // in effect: the slice's override or the PPS's
};

// nal includes its two header bytes; c.pps must be the slice's PPS. `previous` is the detail of the previous slice
// segment of the picture, which a dependent segment inherits (null for the first). Only success writes output.
HevcError hevc_parse_slice_detail(const HevcNal& nal, const HevcConfiguration& c, const HevcSliceDetail* previous,
                                  HevcSliceDetail* output) noexcept;

} // namespace maicord
