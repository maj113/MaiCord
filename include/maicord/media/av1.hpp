#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// AV1 bitstream (AOM 1.0.0 with errata) header parsing for hardware decode:
// OBU framing, the sequence header, the uncompressed frame header, tile group
// layout and the per-slot reference state the frame header syntax inherits
// from. Entropy-coded tile data is never read. Every read is bounds checked;
// a failed parse leaves the reference state untouched.
enum class Av1Status : u8 {
    ok,
    truncated,  // Ran past the OBU.
    malformed,  // Violates a syntax or conformance constraint.
    unsupported // Valid but outside what hardware decode handles here.
};

inline constexpr u32 kAv1RefFrames = 8, kAv1RefsPerFrame = 7, kAv1MaxTileColumns = 64, kAv1MaxTileRows = 64;
inline constexpr u8 kAv1PrimaryRefNone = 7;
inline constexpr u8 kAv1ObuSequenceHeader = 1, kAv1ObuTemporalDelimiter = 2, kAv1ObuFrameHeader = 3,
                    kAv1ObuTileGroup = 4, kAv1ObuFrame = 6;
enum Av1FrameType : u8 {
    av1_key_frame,
    av1_inter_frame,
    av1_intra_only_frame,
    av1_switch_frame
};

struct Av1Obu {
    ConstSlice<u8> payload;
    u8 type, temporal_id, spatial_id;
    bool extension;
};
// Reads the OBU at *offset and advances it. An OBU without obu_size runs to
// the end of data (the last OBU of an RTP frame).
Av1Status av1_next_obu(ConstSlice<u8> data, u32* offset, Av1Obu* obu) noexcept;

struct Av1SequenceHeader {
    u32 max_width, max_height;
    u16 operating_point_idc; // Of operating point 0, the one decoded.
    u16 op_idc[32];
    u32 decoder_model_ops; // Bit i: decoder_model_present_for_this_op[i].
    u8 operating_points;
    u8 profile, bit_depth, frame_width_bits, frame_height_bits;
    u8 frame_id_bits, delta_frame_id_bits;           // Zero without frame ids.
    u8 order_hint_bits;                              // Zero when order hints are disabled.
    u8 force_screen_content_tools, force_integer_mv; // 2: chosen per frame.
    u8 buffer_removal_time_bits, frame_presentation_time_bits;
    u8 matrix_coefficients;
    bool reduced_still_picture_header, decoder_model_info_present, equal_picture_interval;
    bool use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter, enable_interintra_compound;
    bool enable_masked_compound, enable_warped_motion, enable_dual_filter, enable_order_hint,
        enable_jnt_comp;
    bool enable_ref_frame_mvs, enable_superres, enable_cdef, enable_restoration;
    bool mono_chrome, subsampling_x, subsampling_y, color_range, separate_uv_delta_q;
    bool film_grain_params_present;
};
Av1Status av1_parse_sequence_header(ConstSlice<u8> payload, Av1SequenceHeader* output) noexcept;

// film_grain_params() as coded; ar_coeffs keep their +128 bias.
struct Av1FilmGrain {
    u16 grain_seed, cb_offset, cr_offset;
    u8 points_y[14][2], points_cb[10][2], points_cr[10][2];
    u8 num_y_points, num_cb_points, num_cr_points;
    u8 ar_coeffs_y[24], ar_coeffs_cb[25], ar_coeffs_cr[25];
    u8 grain_scaling_minus_8, ar_coeff_lag, ar_coeff_shift_minus_6, grain_scale_shift;
    u8 cb_mult, cb_luma_mult, cr_mult, cr_luma_mult;
    bool apply_grain, chroma_scaling_from_luma, overlap_flag, clip_to_restricted_range;
};

// What a reference slot keeps for later frame headers (spec 7.20); a frame
// header carries the same state for the slots it refreshes.
struct Av1Reference {
    i32 gm_params[kAv1RefsPerFrame][6];
    i16 feature_data[8][8];
    Av1FilmGrain film_grain;
    u32 upscaled_width, frame_height, render_width, render_height, frame_id;
    signed char loop_filter_ref_deltas[8], loop_filter_mode_deltas[2];
    u8 feature_enabled[8]; // Bit j: segmentation feature j.
    u8 frame_type, order_hint;
    bool valid, showable_frame;
};
using Av1References = Av1Reference[kAv1RefFrames];

struct Av1FrameHeader : Av1Reference {
    u16 tile_widths[kAv1MaxTileColumns], tile_heights[kAv1MaxTileRows]; // Superblocks.
    u32 frame_width;  // Coded width; upscaled_width after superres.
    u32 header_bytes; // frame_header_obu() bytes, through byte alignment.
    u16 context_update_tile_id;
    u8 tile_columns, tile_rows, tile_columns_log2, tile_rows_log2, tile_size_bytes;
    u8 frame_to_show_map_idx, refresh_frame_flags, primary_ref_frame;
    u8 ref_frame_idx[kAv1RefsPerFrame];
    u8 gm_type[kAv1RefsPerFrame];
    u8 superres_denom, interpolation_filter, tx_mode;
    u8 base_q_idx, qm_y, qm_u, qm_v; // qm_* are 0xff without quantizer matrices.
    signed char delta_q_y_dc, delta_q_u_dc, delta_q_u_ac, delta_q_v_dc, delta_q_v_ac;
    u8 delta_q_res, delta_lf_res;
    u8 loop_filter_level[4], loop_filter_sharpness;
    u8 cdef_damping_minus_3, cdef_bits; // Strengths are coded values (secondary 0-3).
    u8 cdef_y_primary[8], cdef_y_secondary[8], cdef_uv_primary[8], cdef_uv_secondary[8];
    u8 restoration_type[3]; // 0 none, 1 Wiener, 2 self-guided, 3 switchable.
    u8 log2_restoration_unit_size[3];
    bool show_existing_frame, show_frame, error_resilient_mode, disable_cdf_update;
    bool allow_screen_content_tools, force_integer_mv, allow_intrabc, allow_high_precision_mv;
    bool is_motion_mode_switchable, use_ref_frame_mvs, disable_frame_end_update_cdf, use_superres;
    bool delta_q_present, delta_lf_present, delta_lf_multi;
    bool segmentation_enabled, segmentation_update_map, segmentation_temporal_update,
        segmentation_update_data;
    bool loop_filter_delta_enabled, loop_filter_delta_update;
    bool reference_select, skip_mode_present, allow_warped_motion, reduced_tx_set, coded_lossless;
};
constexpr bool av1_frame_is_intra(const Av1FrameHeader& header) noexcept {
    return header.frame_type == av1_key_frame || header.frame_type == av1_intra_only_frame;
}
// frame_header_obu() of a frame header or frame OBU. A show_existing_frame
// header is completed from its slot (spec 7.21), so references update alike.
// Inter frames must name valid reference slots.
Av1Status av1_parse_frame_header(const Av1SequenceHeader& sequence, const Av1References& references,
                                 ConstSlice<u8> payload, u8 temporal_id, u8 spatial_id,
                                 Av1FrameHeader* output) noexcept;
// Reference update process (spec 7.20) for refresh_frame_flags.
void av1_update_references(Av1References& references, const Av1FrameHeader& header) noexcept;

struct Av1Tile {
    u32 offset, size; // Tile data within the tile group payload.
    u16 row, column;
};
// tile_group_obu(): tiles tile_start..tile_end of the frame, written to
// tiles[0..]. payload starts at the tile group (after the frame header and
// its byte alignment for a frame OBU). Unsupported when tiles is too small.
Av1Status av1_parse_tile_group(const Av1FrameHeader& header, ConstSlice<u8> payload, Slice<Av1Tile> tiles,
                               u32* tile_start, u32* tile_end) noexcept;
} // namespace maicord
