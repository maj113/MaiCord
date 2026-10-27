#pragma once

// DXVA picture parameter buffers, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/com.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <dxva.h>
#endif

MAICORD_SYS_TYPES_BEGIN
// <dxva.h>
#pragma pack(push, 1)
struct _DXVA_PicEntry_H264 {
    union {
        struct {
            UCHAR Index7Bits : 7;
            UCHAR AssociatedFlag : 1;
        };
        UCHAR bPicEntry;
    };
};
#pragma pack(pop)
typedef _DXVA_PicEntry_H264 DXVA_PicEntry_H264;
#pragma pack(push, 1)
struct _DXVA_PicParams_H264 {
    USHORT wFrameWidthInMbsMinus1;
    USHORT wFrameHeightInMbsMinus1;
    DXVA_PicEntry_H264 CurrPic;
    UCHAR num_ref_frames;
    union {
        struct {
            USHORT field_pic_flag : 1;
            USHORT MbaffFrameFlag : 1;
            USHORT residual_colour_transform_flag : 1;
            USHORT sp_for_switch_flag : 1;
            USHORT chroma_format_idc : 2;
            USHORT RefPicFlag : 1;
            USHORT constrained_intra_pred_flag : 1;
            USHORT weighted_pred_flag : 1;
            USHORT weighted_bipred_idc : 2;
            USHORT MbsConsecutiveFlag : 1;
            USHORT frame_mbs_only_flag : 1;
            USHORT transform_8x8_mode_flag : 1;
            USHORT MinLumaBipredSize8x8Flag : 1;
            USHORT IntraPicFlag : 1;
        };
        USHORT wBitFields;
    };
    UCHAR bit_depth_luma_minus8;
    UCHAR bit_depth_chroma_minus8;
    USHORT Reserved16Bits;
    UINT StatusReportFeedbackNumber;
    DXVA_PicEntry_H264 RefFrameList[16];
    INT CurrFieldOrderCnt[2];
    INT FieldOrderCntList[16] [2];
    CHAR pic_init_qs_minus26;
    CHAR chroma_qp_index_offset;
    CHAR second_chroma_qp_index_offset;
    UCHAR ContinuationFlag;
    CHAR pic_init_qp_minus26;
    UCHAR num_ref_idx_l0_active_minus1;
    UCHAR num_ref_idx_l1_active_minus1;
    UCHAR Reserved8BitsA;
    USHORT FrameNumList[16];
    UINT UsedForReferenceFlags;
    USHORT NonExistingFrameFlags;
    USHORT frame_num;
    UCHAR log2_max_frame_num_minus4;
    UCHAR pic_order_cnt_type;
    UCHAR log2_max_pic_order_cnt_lsb_minus4;
    UCHAR delta_pic_order_always_zero_flag;
    UCHAR direct_8x8_inference_flag;
    UCHAR entropy_coding_mode_flag;
    UCHAR pic_order_present_flag;
    UCHAR num_slice_groups_minus1;
    UCHAR slice_group_map_type;
    UCHAR deblocking_filter_control_present_flag;
    UCHAR redundant_pic_cnt_present_flag;
    UCHAR Reserved8BitsB;
    USHORT slice_group_change_rate_minus1;
    UCHAR SliceGroupMap[810];
};
#pragma pack(pop)
typedef _DXVA_PicParams_H264 DXVA_PicParams_H264;
#pragma pack(push, 1)
struct _DXVA_Qmatrix_H264 {
    UCHAR bScalingLists4x4[6] [16];
    UCHAR bScalingLists8x8[2] [64];
};
#pragma pack(pop)
typedef _DXVA_Qmatrix_H264 DXVA_Qmatrix_H264;
#pragma pack(push, 1)
struct _DXVA_Slice_H264_Short {
    UINT BSNALunitDataLocation;
    UINT SliceBytesInBuffer;
    USHORT wBadSliceChopping;
};
#pragma pack(pop)
typedef _DXVA_Slice_H264_Short DXVA_Slice_H264_Short;
#pragma pack(push, 1)
struct _DXVA_PicEntry_HEVC {
    union {
        struct {
            UCHAR Index7Bits : 7;
            UCHAR AssociatedFlag : 1;
        };
        UCHAR bPicEntry;
    };
};
#pragma pack(pop)
typedef _DXVA_PicEntry_HEVC DXVA_PicEntry_HEVC;
#pragma pack(push, 1)
struct _DXVA_PicParams_HEVC {
    USHORT PicWidthInMinCbsY;
    USHORT PicHeightInMinCbsY;
    union {
        struct {
            USHORT chroma_format_idc : 2;
            USHORT separate_colour_plane_flag : 1;
            USHORT bit_depth_luma_minus8 : 3;
            USHORT bit_depth_chroma_minus8 : 3;
            USHORT log2_max_pic_order_cnt_lsb_minus4 : 4;
            USHORT NoPicReorderingFlag : 1;
            USHORT NoBiPredFlag : 1;
            USHORT ReservedBits1 : 1;
        };
        USHORT wFormatAndSequenceInfoFlags;
    };
    DXVA_PicEntry_HEVC CurrPic;
    UCHAR sps_max_dec_pic_buffering_minus1;
    UCHAR log2_min_luma_coding_block_size_minus3;
    UCHAR log2_diff_max_min_luma_coding_block_size;
    UCHAR log2_min_transform_block_size_minus2;
    UCHAR log2_diff_max_min_transform_block_size;
    UCHAR max_transform_hierarchy_depth_inter;
    UCHAR max_transform_hierarchy_depth_intra;
    UCHAR num_short_term_ref_pic_sets;
    UCHAR num_long_term_ref_pics_sps;
    UCHAR num_ref_idx_l0_default_active_minus1;
    UCHAR num_ref_idx_l1_default_active_minus1;
    CHAR init_qp_minus26;
    UCHAR ucNumDeltaPocsOfRefRpsIdx;
    USHORT wNumBitsForShortTermRPSInSlice;
    USHORT ReservedBits2;
    union {
        struct {
            UINT32 scaling_list_enabled_flag : 1;
            UINT32 amp_enabled_flag : 1;
            UINT32 sample_adaptive_offset_enabled_flag : 1;
            UINT32 pcm_enabled_flag : 1;
            UINT32 pcm_sample_bit_depth_luma_minus1 : 4;
            UINT32 pcm_sample_bit_depth_chroma_minus1 : 4;
            UINT32 log2_min_pcm_luma_coding_block_size_minus3 : 2;
            UINT32 log2_diff_max_min_pcm_luma_coding_block_size : 2;
            UINT32 pcm_loop_filter_disabled_flag : 1;
            UINT32 long_term_ref_pics_present_flag : 1;
            UINT32 sps_temporal_mvp_enabled_flag : 1;
            UINT32 strong_intra_smoothing_enabled_flag : 1;
            UINT32 dependent_slice_segments_enabled_flag : 1;
            UINT32 output_flag_present_flag : 1;
            UINT32 num_extra_slice_header_bits : 3;
            UINT32 sign_data_hiding_enabled_flag : 1;
            UINT32 cabac_init_present_flag : 1;
            UINT32 ReservedBits3 : 5;
        };
        UINT32 dwCodingParamToolFlags;
    };
    union {
        struct {
            UINT32 constrained_intra_pred_flag : 1;
            UINT32 transform_skip_enabled_flag : 1;
            UINT32 cu_qp_delta_enabled_flag : 1;
            UINT32 pps_slice_chroma_qp_offsets_present_flag : 1;
            UINT32 weighted_pred_flag : 1;
            UINT32 weighted_bipred_flag : 1;
            UINT32 transquant_bypass_enabled_flag : 1;
            UINT32 tiles_enabled_flag : 1;
            UINT32 entropy_coding_sync_enabled_flag : 1;
            UINT32 uniform_spacing_flag : 1;
            UINT32 loop_filter_across_tiles_enabled_flag : 1;
            UINT32 pps_loop_filter_across_slices_enabled_flag : 1;
            UINT32 deblocking_filter_override_enabled_flag : 1;
            UINT32 pps_deblocking_filter_disabled_flag : 1;
            UINT32 lists_modification_present_flag : 1;
            UINT32 slice_segment_header_extension_present_flag : 1;
            UINT32 IrapPicFlag : 1;
            UINT32 IdrPicFlag : 1;
            UINT32 IntraPicFlag : 1;
            UINT32 ReservedBits4 : 13;
        };
        UINT32 dwCodingSettingPicturePropertyFlags;
    };
    CHAR pps_cb_qp_offset;
    CHAR pps_cr_qp_offset;
    UCHAR num_tile_columns_minus1;
    UCHAR num_tile_rows_minus1;
    USHORT column_width_minus1[19];
    USHORT row_height_minus1[21];
    UCHAR diff_cu_qp_delta_depth;
    CHAR pps_beta_offset_div2;
    CHAR pps_tc_offset_div2;
    UCHAR log2_parallel_merge_level_minus2;
    INT CurrPicOrderCntVal;
    DXVA_PicEntry_HEVC RefPicList[15];
    UCHAR ReservedBits5;
    INT PicOrderCntValList[15];
    UCHAR RefPicSetStCurrBefore[8];
    UCHAR RefPicSetStCurrAfter[8];
    UCHAR RefPicSetLtCurr[8];
    USHORT ReservedBits6;
    USHORT ReservedBits7;
    UINT StatusReportFeedbackNumber;
};
#pragma pack(pop)
typedef _DXVA_PicParams_HEVC DXVA_PicParams_HEVC;
#pragma pack(push, 1)
struct _DXVA_Qmatrix_HEVC {
    UCHAR ucScalingLists0[6] [16];
    UCHAR ucScalingLists1[6] [64];
    UCHAR ucScalingLists2[6] [64];
    UCHAR ucScalingLists3[2] [64];
    UCHAR ucScalingListDCCoefSizeID2[6];
    UCHAR ucScalingListDCCoefSizeID3[2];
};
#pragma pack(pop)
typedef _DXVA_Qmatrix_HEVC DXVA_Qmatrix_HEVC;
#pragma pack(push, 1)
struct _DXVA_Slice_HEVC_Short {
    UINT BSNALunitDataLocation;
    UINT SliceBytesInBuffer;
    USHORT wBadSliceChopping;
};
#pragma pack(pop)
typedef _DXVA_Slice_HEVC_Short DXVA_Slice_HEVC_Short;
#pragma pack(push, 1)
struct _DXVA_PicEntry_AV1 {
    UINT width;
    UINT height;
    INT wmmat[6];
    union {
        struct {
            UCHAR wminvalid : 1;
            UCHAR wmtype : 2;
            UCHAR Reserved : 5;
        };
        UCHAR GlobalMotionFlags;
    };
    UCHAR Index;
    UINT16 Reserved16Bits;
};
#pragma pack(pop)
typedef _DXVA_PicEntry_AV1 DXVA_PicEntry_AV1;
#pragma pack(push, 1)
struct _DXVA_PicParams_AV1 {
    UINT width;
    UINT height;
    UINT max_width;
    UINT max_height;
    UCHAR CurrPicTextureIndex;
    UCHAR superres_denom;
    UCHAR bitdepth;
    UCHAR seq_profile;
    struct {
        UCHAR cols;
        UCHAR rows;
        USHORT context_update_id;
        USHORT widths[64];
        USHORT heights[64];
    } tiles;
    union {
        struct {
            UINT use_128x128_superblock : 1;
            UINT intra_edge_filter : 1;
            UINT interintra_compound : 1;
            UINT masked_compound : 1;
            UINT warped_motion : 1;
            UINT dual_filter : 1;
            UINT jnt_comp : 1;
            UINT screen_content_tools : 1;
            UINT integer_mv : 1;
            UINT cdef : 1;
            UINT restoration : 1;
            UINT film_grain : 1;
            UINT intrabc : 1;
            UINT high_precision_mv : 1;
            UINT switchable_motion_mode : 1;
            UINT filter_intra : 1;
            UINT disable_frame_end_update_cdf : 1;
            UINT disable_cdf_update : 1;
            UINT reference_mode : 1;
            UINT skip_mode : 1;
            UINT reduced_tx_set : 1;
            UINT superres : 1;
            UINT tx_mode : 2;
            UINT use_ref_frame_mvs : 1;
            UINT enable_ref_frame_mvs : 1;
            UINT reference_frame_update : 1;
            UINT Reserved : 5;
        };
        UINT32 CodingParamToolFlags;
    } coding;
    union {
        struct {
            UCHAR frame_type : 2;
            UCHAR show_frame : 1;
            UCHAR showable_frame : 1;
            UCHAR subsampling_x : 1;
            UCHAR subsampling_y : 1;
            UCHAR mono_chrome : 1;
            UCHAR Reserved : 1;
        };
        UCHAR FormatAndPictureInfoFlags;
    } format;
    UCHAR primary_ref_frame;
    UCHAR order_hint;
    UCHAR order_hint_bits;
    DXVA_PicEntry_AV1 frame_refs[7];
    UCHAR RefFrameMapTextureIndex[8];
    struct {
        UCHAR filter_level[2];
        UCHAR filter_level_u;
        UCHAR filter_level_v;
        UCHAR sharpness_level;
        union {
            struct {
                UCHAR mode_ref_delta_enabled : 1;
                UCHAR mode_ref_delta_update : 1;
                UCHAR delta_lf_multi : 1;
                UCHAR delta_lf_present : 1;
                UCHAR Reserved : 4;
            };
            UCHAR ControlFlags;
        };
        CHAR ref_deltas[8];
        CHAR mode_deltas[2];
        UCHAR delta_lf_res;
        UCHAR frame_restoration_type[3];
        USHORT log2_restoration_unit_size[3];
        UINT16 Reserved16Bits;
    } loop_filter;
    struct {
        union {
            struct {
                UCHAR delta_q_present : 1;
                UCHAR delta_q_res : 2;
                UCHAR Reserved : 5;
            };
            UCHAR ControlFlags;
        };
        UCHAR base_qindex;
        CHAR y_dc_delta_q;
        CHAR u_dc_delta_q;
        CHAR v_dc_delta_q;
        CHAR u_ac_delta_q;
        CHAR v_ac_delta_q;
        UCHAR qm_y;
        UCHAR qm_u;
        UCHAR qm_v;
        UINT16 Reserved16Bits;
    } quantization;
    struct {
        union {
            struct {
                UCHAR damping : 2;
                UCHAR bits : 2;
                UCHAR Reserved : 4;
            };
            UCHAR ControlFlags;
        };
        union {
            struct {
                UCHAR primary : 6;
                UCHAR secondary : 2;
            };
            UCHAR combined;
        } y_strengths[8];
        union {
            struct {
                UCHAR primary : 6;
                UCHAR secondary : 2;
            };
            UCHAR combined;
        } uv_strengths[8];
    } cdef;
    UCHAR interp_filter;
    struct {
        union {
            struct {
                UCHAR enabled : 1;
                UCHAR update_map : 1;
                UCHAR update_data : 1;
                UCHAR temporal_update : 1;
                UCHAR Reserved : 4;
            };
            UCHAR ControlFlags;
        };
        UCHAR Reserved24Bits[3];
        union {
            struct {
                UCHAR alt_q : 1;
                UCHAR alt_lf_y_v : 1;
                UCHAR alt_lf_y_h : 1;
                UCHAR alt_lf_u : 1;
                UCHAR alt_lf_v : 1;
                UCHAR ref_frame : 1;
                UCHAR skip : 1;
                UCHAR globalmv : 1;
            };
            UCHAR mask;
        } feature_mask[8];
        SHORT feature_data[8][8];
    } segmentation;
    struct {
        union {
            struct {
                USHORT apply_grain : 1;
                USHORT scaling_shift_minus8 : 2;
                USHORT chroma_scaling_from_luma : 1;
                USHORT ar_coeff_lag : 2;
                USHORT ar_coeff_shift_minus6 : 2;
                USHORT grain_scale_shift : 2;
                USHORT overlap_flag : 1;
                USHORT clip_to_restricted_range : 1;
                USHORT matrix_coeff_is_identity : 1;
                USHORT Reserved : 3;
            };
            USHORT ControlFlags;
        };
        USHORT grain_seed;
        UCHAR scaling_points_y[14][2];
        UCHAR num_y_points;
        UCHAR scaling_points_cb[10][2];
        UCHAR num_cb_points;
        UCHAR scaling_points_cr[10][2];
        UCHAR num_cr_points;
        UCHAR ar_coeffs_y[24];
        UCHAR ar_coeffs_cb[25];
        UCHAR ar_coeffs_cr[25];
        UCHAR cb_mult;
        UCHAR cb_luma_mult;
        UCHAR cr_mult;
        UCHAR cr_luma_mult;
        UCHAR Reserved8Bits;
        SHORT cb_offset;
        SHORT cr_offset;
    } film_grain;
    UINT Reserved32Bits;
    UINT StatusReportFeedbackNumber;
};
#pragma pack(pop)
typedef _DXVA_PicParams_AV1 DXVA_PicParams_AV1;
#pragma pack(push, 1)
struct _DXVA_Tile_AV1 {
    UINT DataOffset;
    UINT DataSize;
    USHORT row;
    USHORT column;
    UINT16 Reserved16Bits;
    UCHAR anchor_frame;
    UCHAR Reserved8Bits;
};
#pragma pack(pop)
typedef _DXVA_Tile_AV1 DXVA_Tile_AV1;
MAICORD_SYS_TYPES_END

// <dxva.h>
DEFINE_GUID(DXVA_NoEncrypt, 0x1b81beD0, 0xa0c7, 0x11d3, 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5);
