#include "va_parameters.hpp"

#include "maicord/core/memory.hpp"
#include "../av1_dxva_parameters.hpp"
// Picture parameters, once per frame and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord::vaapi {

void av1_picture_parameters(const Av1SequenceHeader& s, const Av1FrameHeader& h,
                            const u8 (&slot_surface)[kAv1RefFrames], const SurfaceId* surfaces, u8 surface,
                            PictureParameterAv1* output) noexcept {
    PictureParameterAv1& p = *output;
    clear_bytes(&p, sizeof(p));
    p.profile = s.profile;
    p.order_hint_bits_minus_1 = s.order_hint_bits ? s.order_hint_bits - 1u : 0;
    p.bit_depth_idx = s.bit_depth == 8 ? 0 : s.bit_depth == 10 ? 1 : 2;
    p.matrix_coefficients = s.matrix_coefficients;
    auto& q = p.seq_info_fields.fields;
    q.still_picture = s.reduced_still_picture_header;
    q.use_128x128_superblock = s.use_128x128_superblock;
    q.enable_filter_intra = s.enable_filter_intra;
    q.enable_intra_edge_filter = s.enable_intra_edge_filter;
    q.enable_interintra_compound = s.enable_interintra_compound;
    q.enable_masked_compound = s.enable_masked_compound;
    q.enable_dual_filter = s.enable_dual_filter;
    q.enable_order_hint = s.enable_order_hint;
    q.enable_jnt_comp = s.enable_jnt_comp;
    q.enable_cdef = s.enable_cdef;
    q.mono_chrome = s.mono_chrome;
    q.color_range = s.color_range;
    q.subsampling_x = s.subsampling_x;
    q.subsampling_y = s.subsampling_y;
    q.film_grain_params_present = s.film_grain_params_present;
    p.current_frame = surfaces[surface];
    p.current_display_picture = surfaces[surface];
    p.frame_width_minus1 = static_cast<u16>(h.upscaled_width - 1u);
    p.frame_height_minus1 = static_cast<u16>(h.frame_height - 1u);
    for (u32 i = 0; i < kAv1RefFrames; ++i)
        p.ref_frame_map[i] = slot_surface[i] == 255 ? kInvalidId : surfaces[slot_surface[i]];
    for (u32 i = 0; i < kAv1RefsPerFrame; ++i)
        p.ref_frame_idx[i] = h.ref_frame_idx[i];
    p.primary_ref_frame = h.primary_ref_frame;
    p.order_hint = h.order_hint;
    auto& segmentation = p.seg_info;
    segmentation.segment_info_fields.bits.enabled = h.segmentation_enabled;
    segmentation.segment_info_fields.bits.update_map = h.segmentation_update_map;
    segmentation.segment_info_fields.bits.temporal_update = h.segmentation_temporal_update;
    segmentation.segment_info_fields.bits.update_data = h.segmentation_update_data;
    static_assert(sizeof(segmentation.feature_data) == sizeof(h.feature_data) &&
                  sizeof(segmentation.feature_mask) == sizeof(h.feature_enabled));
    copy_bytes(segmentation.feature_data, h.feature_data, sizeof(segmentation.feature_data));
    copy_bytes(segmentation.feature_mask, h.feature_enabled, sizeof(segmentation.feature_mask));
    // Film grain is never requested: a stream that applies it goes to the software decoder, so the grain-free
    // picture a reference slot holds is also the one shown.
    p.tile_cols = h.tile_columns;
    p.tile_rows = h.tile_rows;
    for (u32 i = 0; i < 63; ++i) {
        p.width_in_sbs_minus_1[i] = i + 1u < h.tile_columns && h.tile_widths[i] ? h.tile_widths[i] - 1u : 0;
        p.height_in_sbs_minus_1[i] = i + 1u < h.tile_rows && h.tile_heights[i] ? h.tile_heights[i] - 1u : 0;
    }
    p.context_update_tile_id = h.context_update_tile_id;
    auto& info = p.pic_info_fields.bits;
    info.frame_type = h.frame_type;
    info.show_frame = h.show_frame;
    info.showable_frame = h.showable_frame;
    info.error_resilient_mode = h.error_resilient_mode;
    info.disable_cdf_update = h.disable_cdf_update;
    info.allow_screen_content_tools = h.allow_screen_content_tools;
    info.force_integer_mv = h.force_integer_mv;
    info.allow_intrabc = h.allow_intrabc;
    info.use_superres = h.use_superres;
    info.allow_high_precision_mv = h.allow_high_precision_mv;
    info.is_motion_mode_switchable = h.is_motion_mode_switchable;
    info.use_ref_frame_mvs = h.use_ref_frame_mvs;
    info.disable_frame_end_update_cdf = h.disable_frame_end_update_cdf;
    info.allow_warped_motion = h.allow_warped_motion;
    p.superres_scale_denominator = h.superres_denom;
    p.interp_filter = h.interpolation_filter;
    p.filter_level[0] = h.loop_filter_level[0];
    p.filter_level[1] = h.loop_filter_level[1];
    p.filter_level_u = h.loop_filter_level[2];
    p.filter_level_v = h.loop_filter_level[3];
    p.loop_filter_info_fields.bits.sharpness_level = h.loop_filter_sharpness;
    p.loop_filter_info_fields.bits.mode_ref_delta_enabled = h.loop_filter_delta_enabled;
    p.loop_filter_info_fields.bits.mode_ref_delta_update = h.loop_filter_delta_update;
    copy_bytes(p.ref_deltas, h.loop_filter_ref_deltas, sizeof(p.ref_deltas));
    copy_bytes(p.mode_deltas, h.loop_filter_mode_deltas, sizeof(p.mode_deltas));
    p.base_qindex = h.base_q_idx;
    p.y_dc_delta_q = h.delta_q_y_dc;
    p.u_dc_delta_q = h.delta_q_u_dc;
    p.u_ac_delta_q = h.delta_q_u_ac;
    p.v_dc_delta_q = h.delta_q_v_dc;
    p.v_ac_delta_q = h.delta_q_v_ac;
    // qm_* are 0xff without quantizer matrices.
    if (h.qm_y != 0xff) {
        p.qmatrix_fields.bits.using_qmatrix = 1;
        p.qmatrix_fields.bits.qm_y = h.qm_y;
        p.qmatrix_fields.bits.qm_u = h.qm_u;
        p.qmatrix_fields.bits.qm_v = h.qm_v;
    }
    auto& m = p.mode_control_fields.bits;
    m.delta_q_present_flag = h.delta_q_present;
    m.log2_delta_q_res = h.delta_q_res;
    m.delta_lf_present_flag = h.delta_lf_present;
    m.log2_delta_lf_res = h.delta_lf_res;
    m.delta_lf_multi = h.delta_lf_multi;
    m.tx_mode = h.tx_mode;
    m.reference_select = h.reference_select;
    m.reduced_tx_set_used = h.reduced_tx_set;
    m.skip_mode_present = h.skip_mode_present;
    p.cdef_damping_minus_3 = h.cdef_damping_minus_3;
    p.cdef_bits = h.cdef_bits;
    // Secondary strength in the lower two bits (as coded), primary in the next four.
    for (u32 k = 0; k < 8; ++k) {
        p.cdef_y_strengths[k] = static_cast<u8>((h.cdef_y_primary[k] << 2) | (h.cdef_y_secondary[k] & 3u));
        p.cdef_uv_strengths[k] = static_cast<u8>((h.cdef_uv_primary[k] << 2) | (h.cdef_uv_secondary[k] & 3u));
    }
    auto& r = p.loop_restoration_fields.bits;
    r.yframe_restoration_type = h.restoration_type[0];
    r.cbframe_restoration_type = h.restoration_type[1];
    r.crframe_restoration_type = h.restoration_type[2];
    if (h.restoration_type[0] || h.restoration_type[1] || h.restoration_type[2]) {
        const u32 luma = h.log2_restoration_unit_size[0], chroma = h.log2_restoration_unit_size[1];
        r.lr_unit_shift = luma >= 6 ? luma - 6u : 0;
        r.lr_uv_shift = chroma < luma ? luma - chroma : 0;
    }
    const bool intra = av1_frame_is_intra(h);
    for (u32 k = 0; k < kAv1RefsPerFrame; ++k) {
        WarpedMotionAv1& warp = p.wm[k];
        if (intra)
            continue;
        warp.wmtype = h.gm_type[k];
        for (u32 j = 0; j < 6; ++j)
            warp.wmmat[j] = h.gm_params[k][j];
        warp.invalid = !av1_warp_valid(h.gm_params[k]);
    }
}

void av1_tile_parameters(const Av1Tile& tile, u32 offset, u32 group_start, u32 group_end,
                         SliceParameterAv1* output) noexcept {
    SliceParameterAv1& s = *output;
    clear_bytes(&s, sizeof(s));
    s.slice_data_size = tile.size;
    s.slice_data_offset = offset;
    s.slice_data_flag = kSliceDataFlagAll;
    s.tile_row = tile.row;
    s.tile_column = tile.column;
    s.tg_start = static_cast<u16>(group_start);
    s.tg_end = static_cast<u16>(group_end);
}

} // namespace maicord::vaapi
#pragma clang attribute pop
