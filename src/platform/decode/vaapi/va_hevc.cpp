#include "va_parameters.hpp"

#include "maicord/core/memory.hpp"
#include "../hevc_gpu_geometry.hpp"
// Picture and slice parameters, once per picture and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord::vaapi {

void hevc_picture_parameters(const HevcSps& a, const HevcPps& b, const HevcPictureHeader& picture, bool intra,
                             const HevcReferencePlan& plan, const HevcGpuReferences& references, u8 surface,
                             const SurfaceId* surfaces, PictureParameterHevc* output) noexcept {
    PictureParameterHevc& p = *output;
    clear_bytes(&p, sizeof(p));
    p.curr_pic.picture_id = surfaces[surface];
    p.curr_pic.pic_order_cnt = plan.poc;
    for (auto& entry : p.reference_frames) {
        entry.picture_id = kInvalidId;
        entry.flags = kPictureHevcInvalid;
    }
    for (u32 i = 0; i < references.count && i < 15; ++i) {
        const HevcReference& reference = references.pictures[i];
        p.reference_frames[i].picture_id = surfaces[reference.surface];
        p.reference_frames[i].pic_order_cnt = reference.poc;
        p.reference_frames[i].flags = reference.long_term ? kPictureHevcLongTermReference : 0;
    }
    for (u32 i = 0; i < references.before_count; ++i)
        p.reference_frames[references.before[i]].flags |= kPictureHevcRpsStCurrBefore;
    for (u32 i = 0; i < references.following_count; ++i)
        p.reference_frames[references.following[i]].flags |= kPictureHevcRpsStCurrAfter;
    for (u32 i = 0; i < references.long_count; ++i)
        p.reference_frames[references.long_term[i]].flags |= kPictureHevcRpsLtCurr | kPictureHevcLongTermReference;
    p.pic_width_in_luma_samples = static_cast<u16>(a.coded_width);
    p.pic_height_in_luma_samples = static_cast<u16>(a.coded_height);
    auto& f = p.pic_fields.bits;
    f.chroma_format_idc = 1;
    f.pcm_enabled_flag = a.pcm;
    f.scaling_list_enabled_flag = a.scaling_enabled;
    f.transform_skip_enabled_flag = b.transform_skip;
    f.amp_enabled_flag = a.amp;
    f.strong_intra_smoothing_enabled_flag = a.strong_intra_smoothing;
    f.sign_data_hiding_enabled_flag = b.sign_data_hiding;
    f.constrained_intra_pred_flag = b.constrained_intra;
    f.cu_qp_delta_enabled_flag = b.cu_qp_delta;
    f.weighted_pred_flag = b.weighted_pred;
    f.weighted_bipred_flag = b.weighted_bipred;
    f.transquant_bypass_enabled_flag = b.transquant_bypass;
    f.tiles_enabled_flag = b.tiles;
    f.entropy_coding_sync_enabled_flag = b.entropy_sync;
    f.pps_loop_filter_across_slices_enabled_flag = b.loop_filter_slices;
    f.loop_filter_across_tiles_enabled_flag = b.loop_filter_tiles;
    f.pcm_loop_filter_disabled_flag = a.pcm_loop_filter_disabled;
    p.sps_max_dec_pic_buffering_minus1 = static_cast<u8>(a.max_buffering - 1);
    p.pcm_sample_bit_depth_luma_minus1 = a.pcm_luma_depth_minus1;
    p.pcm_sample_bit_depth_chroma_minus1 = a.pcm_chroma_depth_minus1;
    p.log2_min_luma_coding_block_size_minus3 = a.log2_min_cb_minus3;
    p.log2_diff_max_min_luma_coding_block_size = a.log2_diff_cb;
    p.log2_min_transform_block_size_minus2 = a.log2_min_tb_minus2;
    p.log2_diff_max_min_transform_block_size = a.log2_diff_tb;
    p.log2_min_pcm_luma_coding_block_size_minus3 = a.log2_min_pcm_minus3;
    p.log2_diff_max_min_pcm_luma_coding_block_size = a.log2_diff_pcm;
    p.max_transform_hierarchy_depth_intra = a.transform_depth_intra;
    p.max_transform_hierarchy_depth_inter = a.transform_depth_inter;
    p.init_qp_minus26 = b.initial_qp_minus26;
    p.diff_cu_qp_delta_depth = b.diff_cu_qp_depth;
    p.pps_cb_qp_offset = b.cb_qp_offset;
    p.pps_cr_qp_offset = b.cr_qp_offset;
    p.log2_parallel_merge_level_minus2 = b.log2_parallel_merge_minus2;
    p.num_tile_columns_minus1 = b.tile_columns_minus1;
    p.num_tile_rows_minus1 = b.tile_rows_minus1;
    copy_bytes(p.column_width_minus1, b.column_width_minus1, sizeof(p.column_width_minus1));
    copy_bytes(p.row_height_minus1, b.row_height_minus1, sizeof(p.row_height_minus1));
    auto& s = p.slice_parsing_fields.bits;
    s.lists_modification_present_flag = b.lists_modification;
    s.long_term_ref_pics_present_flag = a.long_term_present;
    s.sps_temporal_mvp_enabled_flag = a.temporal_mvp;
    s.cabac_init_present_flag = b.cabac_init;
    s.output_flag_present_flag = b.output_flag_present;
    s.dependent_slice_segments_enabled_flag = b.dependent_slices;
    s.pps_slice_chroma_qp_offsets_present_flag = b.slice_chroma_offsets;
    s.sample_adaptive_offset_enabled_flag = a.sao;
    s.deblocking_filter_override_enabled_flag = b.deblocking_override;
    s.pps_disable_deblocking_filter_flag = b.deblocking_disabled;
    s.slice_segment_header_extension_present_flag = b.slice_header_extension;
    s.rap_pic_flag = picture.irap;
    s.idr_pic_flag = picture.idr;
    s.intra_pic_flag = intra;
    p.log2_max_pic_order_cnt_lsb_minus4 = static_cast<u8>(a.poc_bits - 4);
    p.num_short_term_ref_pic_sets = a.short_term_count;
    p.num_long_term_ref_pic_sps = a.long_term_count;
    p.num_ref_idx_l0_default_active_minus1 = static_cast<u8>(b.references_l0 - 1);
    p.num_ref_idx_l1_default_active_minus1 = static_cast<u8>(b.references_l1 - 1);
    p.pps_beta_offset_div2 = b.beta_offset_div2;
    p.pps_tc_offset_div2 = b.tc_offset_div2;
    p.num_extra_slice_header_bits = b.extra_slice_bits;
    p.st_rps_bits = picture.short_term_bits;
}

void hevc_iq_matrix(const HevcPps& pps, IqMatrixHevc* output) noexcept {
    clear_bytes(output, sizeof(*output));
    const auto& scaling = pps.scaling;
    for (u32 i = 0; i < 6; ++i) {
        hevc_gpu_scaling_diagonal<4>(scaling.list4x4[i], output->scaling_list_4x4[i]);
        hevc_gpu_scaling_diagonal<8>(scaling.list8x8[i], output->scaling_list_8x8[i]);
        hevc_gpu_scaling_diagonal<8>(scaling.list16x16[i], output->scaling_list_16x16[i]);
    }
    for (u32 i = 0; i < 2; ++i)
        hevc_gpu_scaling_diagonal<8>(scaling.list32x32[i], output->scaling_list_32x32[i]);
    copy_bytes(output->scaling_list_dc_16x16, scaling.dc16, sizeof(output->scaling_list_dc_16x16));
    copy_bytes(output->scaling_list_dc_32x32, scaling.dc32, sizeof(output->scaling_list_dc_32x32));
}

void hevc_slice_parameters(const HevcSliceDetail& d, const HevcGpuReferences& references, bool last, u32 data_bytes,
                           SliceParameterHevc* output) noexcept {
    SliceParameterHevc& s = *output;
    clear_bytes(&s, sizeof(s));
    s.slice_data_size = data_bytes;
    s.slice_data_offset = 0;
    s.slice_data_flag = kSliceDataFlagAll;
    s.slice_data_byte_offset = d.data_byte_offset;
    s.slice_segment_address = d.address;
    for (auto& list : s.ref_pic_list)
        for (u8& entry : list)
            entry = 0xff;
    // 8.3.4: the temporary lists cycle through the current sets until they are as long as the larger of the active
    // count and the number of current pictures; the entries are then picked in order or by list_entry.
    const u32 total = u32{references.before_count} + references.following_count + references.long_count;
    for (u32 list = 0; list < 2; ++list) {
        if (!d.active[list] || !total || d.slice_type == 2)
            continue;
        u8 temporary[32];
        const u32 length = d.active[list] > total ? d.active[list] : total;
        const u8* const first_set = list == 0 ? references.before : references.following;
        const u32 first_count = list == 0 ? references.before_count : references.following_count;
        const u8* const second_set = list == 0 ? references.following : references.before;
        const u32 second_count = list == 0 ? references.following_count : references.before_count;
        u32 at = 0;
        while (at < length && at < 32) {
            for (u32 i = 0; i < first_count && at < length; ++i)
                temporary[at++] = first_set[i];
            for (u32 i = 0; i < second_count && at < length; ++i)
                temporary[at++] = second_set[i];
            for (u32 i = 0; i < references.long_count && at < length; ++i)
                temporary[at++] = references.long_term[i];
        }
        for (u32 i = 0; i < d.active[list]; ++i)
            s.ref_pic_list[list][i] = temporary[d.modified[list] ? d.list_entry[list][i] : i];
    }
    auto& f = s.long_slice_flags.fields;
    f.last_slice_of_pic = last;
    f.dependent_slice_segment_flag = d.dependent;
    f.slice_type = d.slice_type;
    f.slice_sao_luma_flag = d.sao_luma;
    f.slice_sao_chroma_flag = d.sao_chroma;
    f.mvd_l1_zero_flag = d.mvd_l1_zero;
    f.cabac_init_flag = d.cabac_init;
    f.slice_temporal_mvp_enabled_flag = d.temporal_mvp;
    f.slice_deblocking_filter_disabled_flag = d.deblocking_disabled;
    f.collocated_from_l0_flag = d.collocated_from_l0;
    f.slice_loop_filter_across_slices_enabled_flag = d.loop_filter_across_slices;
    s.collocated_ref_idx = d.collocated_ref_idx;
    s.num_ref_idx_l0_active_minus1 = d.active[0] ? d.active[0] - 1u : 0;
    s.num_ref_idx_l1_active_minus1 = d.active[1] ? d.active[1] - 1u : 0;
    s.slice_qp_delta = d.qp_delta;
    s.slice_cb_qp_offset = d.cb_qp_offset;
    s.slice_cr_qp_offset = d.cr_qp_offset;
    s.slice_beta_offset_div2 = d.beta_offset_div2;
    s.slice_tc_offset_div2 = d.tc_offset_div2;
    if (d.weighted) {
        s.luma_log2_weight_denom = d.luma_log2_weight_denom;
        s.delta_chroma_log2_weight_denom = d.delta_chroma_log2_weight_denom;
        for (u32 i = 0; i < 15; ++i) {
            s.delta_luma_weight_l0[i] = d.delta_luma_weight[0][i];
            s.luma_offset_l0[i] = d.luma_offset[0][i];
            s.delta_luma_weight_l1[i] = d.delta_luma_weight[1][i];
            s.luma_offset_l1[i] = d.luma_offset[1][i];
            for (u32 c = 0; c < 2; ++c) {
                s.delta_chroma_weight_l0[i][c] = d.delta_chroma_weight[0][i][c];
                s.chroma_offset_l0[i][c] = d.chroma_offset[0][i][c];
                s.delta_chroma_weight_l1[i][c] = d.delta_chroma_weight[1][i][c];
                s.chroma_offset_l1[i][c] = d.chroma_offset[1][i][c];
            }
        }
    }
    s.five_minus_max_num_merge_cand = d.five_minus_max_num_merge_cand;
    s.num_entry_point_offsets = d.entry_points;
    s.entry_offset_to_subset_array = 0;
    s.slice_data_num_emu_prevn_bytes = d.escape_bytes;
}

} // namespace maicord::vaapi
#pragma clang attribute pop
