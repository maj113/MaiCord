#pragma once

#include "maicord/media/h264_references.hpp"
#include "maicord/core/memory.hpp"
#include <d3d11.h>
#include <dxva.h>

namespace maicord {

// DXVA short-slice picture parameters shared by the MP4 attachment decoder
// and the live stream decoder. References are the CURRENT DPB (before the
// picture's own marking); feedback is the last accepted status number.
inline void h264_dxva_picture_parameters(const H264Sps& a, const H264Pps& b, const H264Slice& first,
                                         bool all_intra, const H264ReferenceState& references,
                                         const H264PictureOrder& decode_order, u8 surface, u32 feedback,
                                         DXVA_PicParams_H264* output, DXVA_Qmatrix_H264* matrix) noexcept {
    DXVA_PicParams_H264 p{};
    p.wFrameWidthInMbsMinus1 = static_cast<USHORT>(a.coded_width / 16u - 1u);
    p.wFrameHeightInMbsMinus1 = static_cast<USHORT>(a.coded_height / 16u - 1u);
    p.num_ref_frames = a.max_references;
    p.CurrPic.Index7Bits = surface;
    p.frame_num = first.frame_num;
    p.RefPicFlag = first.reference != 0;
    p.MbsConsecutiveFlag = 1; p.chroma_format_idc = 1;
    p.frame_mbs_only_flag = a.frame_only; p.MbaffFrameFlag = a.mbaff;
    p.MinLumaBipredSize8x8Flag = a.level >= 31;
    p.IntraPicFlag = all_intra; p.ContinuationFlag = 1; p.Reserved16Bits = 3;
    p.StatusReportFeedbackNumber = feedback == ~0u ? 1 : feedback + 1u;
    p.log2_max_frame_num_minus4 = a.frame_num_bits - 4u;
    p.pic_order_cnt_type = a.poc_type;
    p.log2_max_pic_order_cnt_lsb_minus4 = a.poc_type == 0 ? a.poc_bits - 4u : 0;
    p.delta_pic_order_always_zero_flag = a.delta_always_zero;
    p.direct_8x8_inference_flag = a.direct_8x8;
    p.transform_8x8_mode_flag = b.transform_8x8;
    p.entropy_coding_mode_flag = b.cabac;
    p.pic_order_present_flag = b.bottom_poc_present;
    p.constrained_intra_pred_flag = b.constrained_intra;
    p.weighted_pred_flag = b.weighted_pred; p.weighted_bipred_idc = b.weighted_bipred;
    p.pic_init_qp_minus26 = b.initial_qp; p.pic_init_qs_minus26 = b.initial_qs;
    p.chroma_qp_index_offset = b.chroma_qp; p.second_chroma_qp_index_offset = b.second_chroma_qp;
    p.num_ref_idx_l0_active_minus1 = b.references_l0 - 1u;
    p.num_ref_idx_l1_active_minus1 = b.references_l1 - 1u;
    p.deblocking_filter_control_present_flag = b.deblocking_control;
    p.redundant_pic_cnt_present_flag = b.redundant_count;
    p.CurrFieldOrderCnt[0] = decode_order.top; p.CurrFieldOrderCnt[1] = decode_order.bottom;
    for (u32 i = 0; i < 16; ++i) p.RefFrameList[i].bPicEntry = 0xFF;
    if (!first.idr) for (u32 i = 0; i < references.count && i < 16; ++i) {
        const auto& ref = references.pictures[i];
        p.RefFrameList[i].Index7Bits = ref.surface;
        p.RefFrameList[i].AssociatedFlag = ref.long_term();
        p.FrameNumList[i] = ref.long_term() ? ref.long_term_index : ref.frame_num;
        p.FieldOrderCntList[i][0] = ref.top; p.FieldOrderCntList[i][1] = ref.bottom;
        p.UsedForReferenceFlags |= 3u << (2u * i);
    }
    *output = p;
    DXVA_Qmatrix_H264 q{};
    copy_bytes(q.bScalingLists4x4, b.scaling.list4x4, sizeof(q.bScalingLists4x4));
    copy_bytes(q.bScalingLists8x8, b.scaling.list8x8, sizeof(q.bScalingLists8x8));
    *matrix = q;
}

// Copies bytes into the driver's mapped decoder buffer of the given type and
// describes it for SubmitDecoderBuffers.
inline HRESULT dxva_upload_buffer(ID3D11VideoContext* context, ID3D11VideoDecoder* decoder,
                                  D3D11_VIDEO_DECODER_BUFFER_TYPE type, const void* data, u32 bytes,
                                  D3D11_VIDEO_DECODER_BUFFER_DESC* description) noexcept {
    UINT capacity = 0;
    void* destination = nullptr;
    HRESULT status = context->GetDecoderBuffer(decoder, type, &capacity, &destination);
    if (FAILED(status)) return status;
    const bool fits = destination && bytes <= capacity;
    if (fits) copy_bytes(destination, data, bytes);
    status = context->ReleaseDecoderBuffer(decoder, type);
    if (!fits) return E_OUTOFMEMORY;
    *description = {};
    description->BufferType = type;
    description->DataSize = bytes;
    return status;
}

} // namespace maicord
