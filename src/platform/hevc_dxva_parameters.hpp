#pragma once

#include "maicord/media/hevc_references.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/dxva.hpp"
#include "hevc_gpu_geometry.hpp"
#include "hevc_gpu_references.hpp"

namespace maicord {

// DXVA HEVC picture parameters shared by the MP4 attachment decoder and the
// live stream decoder. previous is the DPB before the picture's RPS marking,
// plan its result (hevc_reference_plan); feedback is the last accepted status
// number. The matrix is filled only when the SPS enables scaling lists, and
// DXVA forbids sending it otherwise. False when the references do not map.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
inline bool hevc_dxva_picture_parameters(const HevcSps& a, const HevcPps& b,
                                         const HevcPictureHeader& picture, bool intra,
                                         const HevcReferenceState& previous, const HevcReferencePlan& plan,
                                         u8 surface, u32 feedback, DXVA_PicParams_HEVC* output,
                                         DXVA_Qmatrix_HEVC* matrix) noexcept {
    HevcGpuReferences references{};
    if (!hevc_gpu_reference_map(previous, plan, surface, &references))
        return false;
    DXVA_PicParams_HEVC& p = *output;
    reset_value(&p);
    p.PicWidthInMinCbsY = static_cast<USHORT>(a.coded_width >> (a.log2_min_cb_minus3 + 3));
    p.PicHeightInMinCbsY = static_cast<USHORT>(a.coded_height >> (a.log2_min_cb_minus3 + 3));
    p.chroma_format_idc = 1;
    p.log2_max_pic_order_cnt_lsb_minus4 = static_cast<USHORT>(a.poc_bits - 4);
    // Optional accelerator hint stays conservative; the output queue enforces
    // the SPS reorder limit.
    p.NoPicReorderingFlag = 0;
    p.CurrPic.Index7Bits = surface;
    p.sps_max_dec_pic_buffering_minus1 = static_cast<UCHAR>(a.max_buffering - 1);
    p.log2_min_luma_coding_block_size_minus3 = a.log2_min_cb_minus3;
    p.log2_diff_max_min_luma_coding_block_size = a.log2_diff_cb;
    p.log2_min_transform_block_size_minus2 = a.log2_min_tb_minus2;
    p.log2_diff_max_min_transform_block_size = a.log2_diff_tb;
    p.max_transform_hierarchy_depth_inter = a.transform_depth_inter;
    p.max_transform_hierarchy_depth_intra = a.transform_depth_intra;
    p.num_short_term_ref_pic_sets = a.short_term_count;
    p.num_long_term_ref_pics_sps = a.long_term_count;
    p.num_ref_idx_l0_default_active_minus1 = static_cast<UCHAR>(b.references_l0 - 1);
    p.num_ref_idx_l1_default_active_minus1 = static_cast<UCHAR>(b.references_l1 - 1);
    p.init_qp_minus26 = b.initial_qp_minus26;
    p.ucNumDeltaPocsOfRefRpsIdx = picture.reference_rps_delta_count;
    p.wNumBitsForShortTermRPSInSlice = picture.short_term_bits;
    p.scaling_list_enabled_flag = a.scaling_enabled;
    p.amp_enabled_flag = a.amp;
    p.sample_adaptive_offset_enabled_flag = a.sao;
    p.pcm_enabled_flag = a.pcm;
    p.pcm_sample_bit_depth_luma_minus1 = a.pcm_luma_depth_minus1;
    p.pcm_sample_bit_depth_chroma_minus1 = a.pcm_chroma_depth_minus1;
    p.log2_min_pcm_luma_coding_block_size_minus3 = a.log2_min_pcm_minus3;
    p.log2_diff_max_min_pcm_luma_coding_block_size = a.log2_diff_pcm;
    p.pcm_loop_filter_disabled_flag = a.pcm_loop_filter_disabled;
    p.long_term_ref_pics_present_flag = a.long_term_present;
    p.sps_temporal_mvp_enabled_flag = a.temporal_mvp;
    p.strong_intra_smoothing_enabled_flag = a.strong_intra_smoothing;
    p.dependent_slice_segments_enabled_flag = b.dependent_slices;
    p.output_flag_present_flag = b.output_flag_present;
    p.num_extra_slice_header_bits = b.extra_slice_bits;
    p.sign_data_hiding_enabled_flag = b.sign_data_hiding;
    p.cabac_init_present_flag = b.cabac_init;
    p.constrained_intra_pred_flag = b.constrained_intra;
    p.transform_skip_enabled_flag = b.transform_skip;
    p.cu_qp_delta_enabled_flag = b.cu_qp_delta;
    p.pps_slice_chroma_qp_offsets_present_flag = b.slice_chroma_offsets;
    p.weighted_pred_flag = b.weighted_pred;
    p.weighted_bipred_flag = b.weighted_bipred;
    p.transquant_bypass_enabled_flag = b.transquant_bypass;
    p.tiles_enabled_flag = b.tiles;
    p.entropy_coding_sync_enabled_flag = b.entropy_sync;
    p.uniform_spacing_flag = b.uniform_spacing;
    p.loop_filter_across_tiles_enabled_flag = b.loop_filter_tiles;
    p.pps_loop_filter_across_slices_enabled_flag = b.loop_filter_slices;
    p.deblocking_filter_override_enabled_flag = b.deblocking_override;
    p.pps_deblocking_filter_disabled_flag = b.deblocking_disabled;
    p.lists_modification_present_flag = b.lists_modification;
    p.slice_segment_header_extension_present_flag = b.slice_header_extension;
    p.IrapPicFlag = picture.irap;
    p.IdrPicFlag = picture.idr;
    p.IntraPicFlag = intra;
    p.pps_cb_qp_offset = b.cb_qp_offset;
    p.pps_cr_qp_offset = b.cr_qp_offset;
    p.num_tile_columns_minus1 = b.tile_columns_minus1;
    p.num_tile_rows_minus1 = b.tile_rows_minus1;
    copy_bytes(p.column_width_minus1, b.column_width_minus1, sizeof(p.column_width_minus1));
    copy_bytes(p.row_height_minus1, b.row_height_minus1, sizeof(p.row_height_minus1));
    p.diff_cu_qp_delta_depth = b.diff_cu_qp_depth;
    p.pps_beta_offset_div2 = b.beta_offset_div2;
    p.pps_tc_offset_div2 = b.tc_offset_div2;
    p.log2_parallel_merge_level_minus2 = b.log2_parallel_merge_minus2;
    p.CurrPicOrderCntVal = plan.poc;
    p.StatusReportFeedbackNumber = feedback == ~u32{0} ? 1 : feedback + 1;
    for (auto& ref : p.RefPicList)
        ref.bPicEntry = 0xff;
    for (u32 i = 0; i < 8; ++i)
        p.RefPicSetStCurrBefore[i] = p.RefPicSetStCurrAfter[i] = p.RefPicSetLtCurr[i] = 0xff;
    for (u32 i = 0; i < references.count; ++i) {
        const auto& ref = references.pictures[i];
        p.RefPicList[i].Index7Bits = ref.surface;
        p.RefPicList[i].AssociatedFlag = ref.long_term;
        p.PicOrderCntValList[i] = ref.poc;
    }
    copy_bytes(p.RefPicSetStCurrBefore, references.before, references.before_count);
    copy_bytes(p.RefPicSetStCurrAfter, references.following, references.following_count);
    copy_bytes(p.RefPicSetLtCurr, references.long_term, references.long_count);
    if (a.scaling_enabled) {
        const auto& scaling = b.scaling;
        for (u32 i = 0; i < 6; ++i) {
            hevc_gpu_scaling_diagonal<4>(scaling.list4x4[i], matrix->ucScalingLists0[i]);
            hevc_gpu_scaling_diagonal<8>(scaling.list8x8[i], matrix->ucScalingLists1[i]);
            hevc_gpu_scaling_diagonal<8>(scaling.list16x16[i], matrix->ucScalingLists2[i]);
        }
        for (u32 i = 0; i < 2; ++i)
            hevc_gpu_scaling_diagonal<8>(scaling.list32x32[i], matrix->ucScalingLists3[i]);
        copy_bytes(matrix->ucScalingListDCCoefSizeID2, scaling.dc16,
                   sizeof(matrix->ucScalingListDCCoefSizeID2));
        copy_bytes(matrix->ucScalingListDCCoefSizeID3, scaling.dc32,
                   sizeof(matrix->ucScalingListDCCoefSizeID3));
    }
    return true;
}
#pragma clang attribute pop

} // namespace maicord
