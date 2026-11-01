#include "av1_dxva_parameters.hpp"

#include "maicord/core/memory.hpp"
// Picture parameters, once per frame and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

// Spec 7.11.3.7 Div_Lut: round(2^14 * 256 / (256 + i)).
struct DivisorTable {
    u16 values[257];
    constexpr DivisorTable() noexcept : values{} {
        for (u32 i = 0; i <= 256; ++i)
            values[i] = static_cast<u16>(((1u << 22) + (256u + i) / 2u) / (256u + i));
    }
};
constexpr DivisorTable kDivisors;

i64 round2_signed(i64 value, u32 bits) noexcept {
    const i64 magnitude = ((value < 0 ? -value : value) + (i64{1} << (bits - 1))) >> bits;
    return value < 0 ? -magnitude : magnitude;
}

i64 clip16(i64 value) noexcept {
    return value < -32768 ? -32768 : value > 32767 ? 32767 : value;
}

} // namespace

bool av1_warp_valid(const i32 (&p)[6]) noexcept {
    constexpr i64 one = i64{1} << 16;
    const i64 alpha0 = clip16(p[2] - one);
    const i64 beta0 = clip16(p[3]);
    // resolve_divisor(p[2])
    const i64 d = p[2];
    const u64 magnitude = static_cast<u64>(d < 0 ? -d : d);
    if (!magnitude)
        return false;
    u32 n = 63;
    while (!(magnitude >> n))
        --n;
    const u64 e = magnitude - (u64{1} << n);
    const u64 f = n > 8 ? (e + (u64{1} << (n - 9))) >> (n - 8) : e << (8 - n);
    const u32 shift = n + 14;
    const i64 factor = d < 0 ? -i64{kDivisors.values[f]} : i64{kDivisors.values[f]};
    const i64 v = i64{p[4]} << 16;
    const i64 gamma0 = clip16(round2_signed(v * factor, shift));
    const i64 w = i64{p[3]} * p[4];
    const i64 delta0 = clip16(p[5] - round2_signed(w * factor, shift) - one);
    const auto reduce = [](i64 value) noexcept { return round2_signed(value, 6) * 64; };
    const i64 alpha = reduce(alpha0), beta = reduce(beta0), gamma = reduce(gamma0), delta = reduce(delta0);
    const auto absolute = [](i64 value) noexcept { return value < 0 ? -value : value; };
    return 4 * absolute(alpha) + 7 * absolute(beta) < one &&
           4 * absolute(gamma) + 4 * absolute(delta) < one;
}

void av1_dxva_picture_parameters(const Av1SequenceHeader& s, const Av1FrameHeader& h,
                                 const u8 (&slot_surface)[kAv1RefFrames], const Av1References& references,
                                 u8 surface, DXVA_PicParams_AV1* output) noexcept {
    DXVA_PicParams_AV1& p = *output;
    static_assert(sizeof(p.tiles.widths) == sizeof(h.tile_widths) &&
                  sizeof(p.frame_refs[0].wmmat) == sizeof(h.gm_params[0]) &&
                  sizeof(p.loop_filter.ref_deltas) == sizeof(h.loop_filter_ref_deltas) &&
                  sizeof(p.segmentation.feature_mask) == sizeof(h.feature_enabled) &&
                  sizeof(p.segmentation.feature_data) == sizeof(h.feature_data));
    clear_bytes(&p, sizeof(p));
    p.width = h.upscaled_width;
    p.height = h.frame_height;
    p.max_width = s.max_width;
    p.max_height = s.max_height;
    p.CurrPicTextureIndex = surface;
    p.superres_denom = h.superres_denom;
    p.bitdepth = s.bit_depth;
    p.seq_profile = s.profile;
    p.tiles.cols = h.tile_columns;
    p.tiles.rows = h.tile_rows;
    p.tiles.context_update_id = h.context_update_tile_id;
    copy_bytes(p.tiles.widths, h.tile_widths, sizeof(p.tiles.widths));
    copy_bytes(p.tiles.heights, h.tile_heights, sizeof(p.tiles.heights));
    auto& c = p.coding;
    c.use_128x128_superblock = s.use_128x128_superblock;
    c.intra_edge_filter = s.enable_intra_edge_filter;
    c.interintra_compound = s.enable_interintra_compound;
    c.masked_compound = s.enable_masked_compound;
    c.warped_motion = h.allow_warped_motion;
    c.dual_filter = s.enable_dual_filter;
    c.jnt_comp = s.enable_jnt_comp;
    c.screen_content_tools = h.allow_screen_content_tools;
    c.integer_mv = h.force_integer_mv;
    c.cdef = s.enable_cdef;
    c.restoration = s.enable_restoration;
    // The driver applies film grain to the output and keeps clean references.
    c.film_grain = s.film_grain_params_present;
    c.intrabc = h.allow_intrabc;
    c.high_precision_mv = h.allow_high_precision_mv;
    c.switchable_motion_mode = h.is_motion_mode_switchable;
    c.filter_intra = s.enable_filter_intra;
    c.disable_frame_end_update_cdf = h.disable_frame_end_update_cdf;
    c.disable_cdf_update = h.disable_cdf_update;
    c.reference_mode = h.reference_select;
    c.skip_mode = h.skip_mode_present;
    c.reduced_tx_set = h.reduced_tx_set;
    c.superres = h.use_superres;
    c.tx_mode = h.tx_mode;
    c.use_ref_frame_mvs = h.use_ref_frame_mvs;
    c.enable_ref_frame_mvs = s.enable_ref_frame_mvs;
    c.reference_frame_update = 1;
    p.format.frame_type = h.frame_type;
    p.format.show_frame = h.show_frame;
    p.format.showable_frame = h.showable_frame;
    p.format.subsampling_x = s.subsampling_x;
    p.format.subsampling_y = s.subsampling_y;
    p.format.mono_chrome = s.mono_chrome;
    p.primary_ref_frame = h.primary_ref_frame;
    p.order_hint = h.order_hint;
    p.order_hint_bits = s.order_hint_bits;
    const bool intra = av1_frame_is_intra(h);
    for (u32 i = 0; i < kAv1RefsPerFrame; ++i) {
        DXVA_PicEntry_AV1& entry = p.frame_refs[i];
        entry.Index = 0xff;
        if (intra)
            continue;
        const u8 slot = h.ref_frame_idx[i];
        entry.Index = slot;
        entry.width = references[slot].upscaled_width;
        entry.height = references[slot].frame_height;
        copy_bytes(entry.wmmat, h.gm_params[i], sizeof(entry.wmmat));
        entry.wmtype = h.gm_type[i];
        entry.wminvalid = !av1_warp_valid(h.gm_params[i]);
    }
    copy_bytes(p.RefFrameMapTextureIndex, slot_surface, sizeof(slot_surface));
    auto& lf = p.loop_filter;
    lf.filter_level[0] = h.loop_filter_level[0];
    lf.filter_level[1] = h.loop_filter_level[1];
    lf.filter_level_u = h.loop_filter_level[2];
    lf.filter_level_v = h.loop_filter_level[3];
    lf.sharpness_level = h.loop_filter_sharpness;
    lf.mode_ref_delta_enabled = h.loop_filter_delta_enabled;
    lf.mode_ref_delta_update = h.loop_filter_delta_update;
    lf.delta_lf_multi = h.delta_lf_multi;
    lf.delta_lf_present = h.delta_lf_present;
    copy_bytes(lf.ref_deltas, h.loop_filter_ref_deltas, sizeof(lf.ref_deltas));
    copy_bytes(lf.mode_deltas, h.loop_filter_mode_deltas, sizeof(lf.mode_deltas));
    lf.delta_lf_res = h.delta_lf_res;
    copy_bytes(lf.frame_restoration_type, h.restoration_type, sizeof(lf.frame_restoration_type));
    for (u32 i = 0; i < 3; ++i)
        lf.log2_restoration_unit_size[i] = h.log2_restoration_unit_size[i];
    auto& q = p.quantization;
    q.delta_q_present = h.delta_q_present;
    q.delta_q_res = h.delta_q_res;
    q.base_qindex = h.base_q_idx;
    q.y_dc_delta_q = h.delta_q_y_dc;
    q.u_dc_delta_q = h.delta_q_u_dc;
    q.v_dc_delta_q = h.delta_q_v_dc;
    q.u_ac_delta_q = h.delta_q_u_ac;
    q.v_ac_delta_q = h.delta_q_v_ac;
    q.qm_y = h.qm_y;
    q.qm_u = h.qm_u;
    q.qm_v = h.qm_v;
    p.cdef.damping = h.cdef_damping_minus_3;
    p.cdef.bits = h.cdef_bits;
    for (u32 i = 0; i < 8; ++i) {
        p.cdef.y_strengths[i].primary = h.cdef_y_primary[i];
        p.cdef.y_strengths[i].secondary = h.cdef_y_secondary[i];
        p.cdef.uv_strengths[i].primary = h.cdef_uv_primary[i];
        p.cdef.uv_strengths[i].secondary = h.cdef_uv_secondary[i];
    }
    p.interp_filter = h.interpolation_filter;
    auto& seg = p.segmentation;
    seg.enabled = h.segmentation_enabled;
    seg.update_map = h.segmentation_update_map;
    seg.update_data = h.segmentation_update_data;
    seg.temporal_update = h.segmentation_temporal_update;
    copy_bytes(seg.feature_mask, h.feature_enabled, sizeof(seg.feature_mask));
    copy_bytes(seg.feature_data, h.feature_data, sizeof(seg.feature_data));
    const Av1FilmGrain& g = h.film_grain;
    if (s.film_grain_params_present && g.apply_grain) {
        auto& fg = p.film_grain;
        fg.apply_grain = 1;
        fg.scaling_shift_minus8 = g.grain_scaling_minus_8;
        fg.chroma_scaling_from_luma = g.chroma_scaling_from_luma;
        fg.ar_coeff_lag = g.ar_coeff_lag;
        fg.ar_coeff_shift_minus6 = g.ar_coeff_shift_minus_6;
        fg.grain_scale_shift = g.grain_scale_shift;
        fg.overlap_flag = g.overlap_flag;
        fg.clip_to_restricted_range = g.clip_to_restricted_range;
        fg.matrix_coeff_is_identity = s.matrix_coefficients == 0;
        fg.grain_seed = g.grain_seed;
        copy_bytes(fg.scaling_points_y, g.points_y, sizeof(g.points_y));
        fg.num_y_points = g.num_y_points;
        copy_bytes(fg.scaling_points_cb, g.points_cb, sizeof(g.points_cb));
        fg.num_cb_points = g.num_cb_points;
        copy_bytes(fg.scaling_points_cr, g.points_cr, sizeof(g.points_cr));
        fg.num_cr_points = g.num_cr_points;
        copy_bytes(fg.ar_coeffs_y, g.ar_coeffs_y, sizeof(g.ar_coeffs_y));
        copy_bytes(fg.ar_coeffs_cb, g.ar_coeffs_cb, sizeof(g.ar_coeffs_cb));
        copy_bytes(fg.ar_coeffs_cr, g.ar_coeffs_cr, sizeof(g.ar_coeffs_cr));
        fg.cb_mult = g.cb_mult;
        fg.cb_luma_mult = g.cb_luma_mult;
        fg.cr_mult = g.cr_mult;
        fg.cr_luma_mult = g.cr_luma_mult;
        fg.cb_offset = static_cast<SHORT>(g.cb_offset);
        fg.cr_offset = static_cast<SHORT>(g.cr_offset);
    }
}

} // namespace maicord
#pragma clang attribute pop
