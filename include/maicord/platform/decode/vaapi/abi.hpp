#pragma once

#include "maicord/core/base.hpp"

// The VA-API interface the hardware tier of live video uses, declared without libva's headers (Linux). libva.so.2 and
// libva-drm.so.2 are dlopen()ed, so a machine without them (or without a render node, or a driver for its GPU) simply
// has no hardware tier. Types and calls keep libva's binary layout; tools/check_video_abi.py compares this file with
// the installed libva-dev headers: constants marked with the macro or enumerator they copy, the sizes and offsets the
// static_asserts below pin, and every function of the table. Only functions of libva 2.14 (Ubuntu 22.04) are used.

namespace maicord {
using i8 = signed char;
}

namespace maicord::vaapi {

using Display = void*;
using Status = i32; // VAStatus
using ConfigId = u32;
using ContextId = u32;
using BufferId = u32;
using SurfaceId = u32;

inline constexpr u32 kInvalidId = 0xffffffffu; // VA_INVALID_ID
inline constexpr Status kStatusSuccess = 0;    // VA_STATUS_SUCCESS
inline constexpr Status kStatusTimedOut = 0x26;      // VA_STATUS_ERROR_TIMEDOUT

// VAProfile
inline constexpr i32 kProfileH264ConstrainedBaseline = 13; // VAProfileH264ConstrainedBaseline
inline constexpr i32 kProfileH264Main = 6;                 // VAProfileH264Main
inline constexpr i32 kProfileH264High = 7;                 // VAProfileH264High
inline constexpr i32 kProfileHevcMain = 17;                // VAProfileHEVCMain
inline constexpr i32 kProfileAv1Profile0 = 32;             // VAProfileAV1Profile0
// VAEntrypoint
inline constexpr i32 kEntrypointVld = 1; // VAEntrypointVLD
// VAConfigAttribType
inline constexpr i32 kConfigAttribRtFormat = 0;       // VAConfigAttribRTFormat
inline constexpr i32 kConfigAttribDecSliceMode = 6;   // VAConfigAttribDecSliceMode
inline constexpr i32 kConfigAttribMaxPictureWidth = 18;  // VAConfigAttribMaxPictureWidth
inline constexpr i32 kConfigAttribMaxPictureHeight = 19; // VAConfigAttribMaxPictureHeight
inline constexpr u32 kAttribNotSupported = 0x80000000u; // VA_ATTRIB_NOT_SUPPORTED
inline constexpr u32 kRtFormatYuv420 = 0x00000001;      // VA_RT_FORMAT_YUV420
inline constexpr u32 kRtFormatYuv420_10 = 0x00000100;   // VA_RT_FORMAT_YUV420_10
inline constexpr u32 kDecSliceModeNormal = 0x00000001;  // VA_DEC_SLICE_MODE_NORMAL
inline constexpr i32 kProgressive = 0x1;                // VA_PROGRESSIVE
// VABufferType
inline constexpr i32 kPictureParameterBuffer = 0; // VAPictureParameterBufferType
inline constexpr i32 kIqMatrixBuffer = 1;         // VAIQMatrixBufferType
inline constexpr i32 kSliceParameterBuffer = 4;   // VASliceParameterBufferType
inline constexpr i32 kSliceDataBuffer = 5;        // VASliceDataBufferType
inline constexpr u32 kSliceDataFlagAll = 0x00;    // VA_SLICE_DATA_FLAG_ALL
// VASurfaceAttribType and VAGenericValueType
inline constexpr i32 kSurfaceAttribPixelFormat = 1;  // VASurfaceAttribPixelFormat
inline constexpr i32 kSurfaceAttribUsageHint = 8;    // VASurfaceAttribUsageHint
inline constexpr i32 kGenericValueInteger = 1;       // VAGenericValueTypeInteger
inline constexpr u32 kSurfaceAttribSettable = 0x00000002; // VA_SURFACE_ATTRIB_SETTABLE
inline constexpr u32 kSurfaceUsageHintDecoder = 0x00000001; // VA_SURFACE_ATTRIB_USAGE_HINT_DECODER
inline constexpr u32 kSurfaceUsageHintExport = 0x00000020;  // VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT
inline constexpr u32 kFourccNv12 = 0x3231564E; // VA_FOURCC_NV12
inline constexpr u32 kFourccP010 = 0x30313050; // VA_FOURCC_P010
// vaExportSurfaceHandle
inline constexpr u32 kSurfaceMemoryTypeDrmPrime2 = 0x40000000; // VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2
inline constexpr u32 kExportSurfaceReadOnly = 0x0001;          // VA_EXPORT_SURFACE_READ_ONLY
inline constexpr u32 kExportSurfaceSeparateLayers = 0x0004;    // VA_EXPORT_SURFACE_SEPARATE_LAYERS
// VAPictureH264.flags
inline constexpr u32 kPictureH264Invalid = 0x00000001;            // VA_PICTURE_H264_INVALID
inline constexpr u32 kPictureH264ShortTermReference = 0x00000008; // VA_PICTURE_H264_SHORT_TERM_REFERENCE
inline constexpr u32 kPictureH264LongTermReference = 0x00000010;  // VA_PICTURE_H264_LONG_TERM_REFERENCE
// VAPictureHEVC.flags
inline constexpr u32 kPictureHevcInvalid = 0x00000001;          // VA_PICTURE_HEVC_INVALID
inline constexpr u32 kPictureHevcLongTermReference = 0x00000008; // VA_PICTURE_HEVC_LONG_TERM_REFERENCE
inline constexpr u32 kPictureHevcRpsStCurrBefore = 0x00000010;  // VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE
inline constexpr u32 kPictureHevcRpsStCurrAfter = 0x00000020;   // VA_PICTURE_HEVC_RPS_ST_CURR_AFTER
inline constexpr u32 kPictureHevcRpsLtCurr = 0x00000040;        // VA_PICTURE_HEVC_RPS_LT_CURR

inline constexpr usize kPaddingLow = 4;    // VA_PADDING_LOW
inline constexpr usize kPaddingMedium = 8; // VA_PADDING_MEDIUM
inline constexpr usize kPaddingHigh = 16;  // VA_PADDING_HIGH
inline constexpr usize kPaddingLarge = 32; // VA_PADDING_LARGE

// Encoding and video processing (the Go Live encoder, src/platform/encode/linux).
inline constexpr i32 kEntrypointEncSlice = 6;    // VAEntrypointEncSlice
inline constexpr i32 kEntrypointEncSliceLp = 8;  // VAEntrypointEncSliceLP
inline constexpr i32 kEntrypointVideoProc = 10;  // VAEntrypointVideoProc
inline constexpr i32 kConfigAttribRateControl = 5;     // VAConfigAttribRateControl
inline constexpr i32 kConfigAttribEncPackedHeaders = 10; // VAConfigAttribEncPackedHeaders
inline constexpr i32 kConfigAttribEncMaxRefFrames = 13;  // VAConfigAttribEncMaxRefFrames
inline constexpr u32 kRcCbr = 0x00000002;              // VA_RC_CBR
inline constexpr u32 kEncPackedHeaderNone = 0x00000000; // VA_ENC_PACKED_HEADER_NONE
inline constexpr u32 kRtFormatRgb32 = 0x00020000;      // VA_RT_FORMAT_RGB32
inline constexpr i32 kEncCodedBuffer = 21;              // VAEncCodedBufferType
inline constexpr i32 kEncSequenceParameterBuffer = 22;  // VAEncSequenceParameterBufferType
inline constexpr i32 kEncPictureParameterBuffer = 23;   // VAEncPictureParameterBufferType
inline constexpr i32 kEncSliceParameterBuffer = 24;     // VAEncSliceParameterBufferType
inline constexpr i32 kEncMiscParameterBuffer = 27;      // VAEncMiscParameterBufferType
inline constexpr i32 kProcPipelineParameterBuffer = 41; // VAProcPipelineParameterBufferType
inline constexpr u32 kEncMiscFrameRate = 0;   // VAEncMiscParameterTypeFrameRate
inline constexpr u32 kEncMiscRateControl = 1; // VAEncMiscParameterTypeRateControl
inline constexpr i32 kSurfaceAttribMemoryType = 6;               // VASurfaceAttribMemoryType
inline constexpr i32 kSurfaceAttribExternalBufferDescriptor = 7; // VASurfaceAttribExternalBufferDescriptor
inline constexpr i32 kGenericValuePointer = 3;                   // VAGenericValueTypePointer
inline constexpr u32 kSurfaceUsageHintEncoder = 0x00000002;  // VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER
inline constexpr u32 kSurfaceUsageHintVppRead = 0x00000004;  // VA_SURFACE_ATTRIB_USAGE_HINT_VPP_READ
inline constexpr u32 kSurfaceUsageHintVppWrite = 0x00000008; // VA_SURFACE_ATTRIB_USAGE_HINT_VPP_WRITE
inline constexpr u32 kColorStandardBt601 = 1; // VAProcColorStandardBT601
inline constexpr u32 kColorStandardBt709 = 2; // VAProcColorStandardBT709
inline constexpr u32 kColorStandardSrgb = 8;  // VAProcColorStandardSRGB
inline constexpr u8 kSourceRangeReduced = 1;  // VA_SOURCE_RANGE_REDUCED
inline constexpr u8 kSourceRangeFull = 2;     // VA_SOURCE_RANGE_FULL
inline constexpr u32 kFilterScalingHq = 0x00000200; // VA_FILTER_SCALING_HQ
inline constexpr u32 kFourccBgrx = 0x58524742; // VA_FOURCC_BGRX
inline constexpr u32 kFourccBgra = 0x41524742; // VA_FOURCC_BGRA
inline constexpr u32 kFourccRgbx = 0x58424752; // VA_FOURCC_RGBX
inline constexpr u32 kFourccRgba = 0x41424752; // VA_FOURCC_RGBA
inline constexpr i32 kConfigAttribEncAv1Ext1 = 53; // VAConfigAttribEncAV1Ext1
inline constexpr i32 kConfigAttribEncAv1Ext2 = 54; // VAConfigAttribEncAV1Ext2
inline constexpr u32 kEncPackedHeaderSequenceFlag = 0x00000001; // VA_ENC_PACKED_HEADER_SEQUENCE
inline constexpr u32 kEncPackedHeaderPictureFlag = 0x00000002;  // VA_ENC_PACKED_HEADER_PICTURE
inline constexpr u32 kEncPackedHeaderSequence = 1; // VAEncPackedHeaderSequence
inline constexpr u32 kEncPackedHeaderPicture = 2;  // VAEncPackedHeaderPicture
inline constexpr i32 kEncPackedHeaderParameterBuffer = 25; // VAEncPackedHeaderParameterBufferType
inline constexpr i32 kEncPackedHeaderDataBuffer = 26;      // VAEncPackedHeaderDataBufferType

struct ConfigAttrib {
    i32 type;
    u32 value;
};
struct GenericValue {
    i32 type;
    union {
        i32 i;
        float f;
        void* p;
        void (*fn)();
    } value;
};
struct SurfaceAttrib {
    i32 type;
    u32 flags;
    GenericValue value;
};
// VADRMPRIMESurfaceDescriptor
struct DrmPrimeDescriptor {
    u32 fourcc;
    u32 width;
    u32 height;
    u32 num_objects;
    struct {
        i32 fd;
        u32 size;
        u64 drm_format_modifier;
    } objects[4];
    u32 num_layers;
    struct {
        u32 drm_format;
        u32 num_planes;
        u32 object_index[4];
        u32 offset[4];
        u32 pitch[4];
    } layers[4];
};

// ---- H.264 (va.h) -----------------------------------------------------------------------------------------------

struct PictureH264 {
    SurfaceId picture_id;
    u32 frame_idx;
    u32 flags;
    i32 top_field_order_cnt;
    i32 bottom_field_order_cnt;
    u32 va_reserved[kPaddingLow];
};
struct PictureParameterH264 {
    PictureH264 curr_pic;
    PictureH264 reference_frames[16];
    u16 picture_width_in_mbs_minus1;
    u16 picture_height_in_mbs_minus1;
    u8 bit_depth_luma_minus8;
    u8 bit_depth_chroma_minus8;
    u8 num_ref_frames;
    union {
        struct {
            u32 chroma_format_idc : 2;
            u32 residual_colour_transform_flag : 1;
            u32 gaps_in_frame_num_value_allowed_flag : 1;
            u32 frame_mbs_only_flag : 1;
            u32 mb_adaptive_frame_field_flag : 1;
            u32 direct_8x8_inference_flag : 1;
            u32 min_luma_bi_pred_size_8x8 : 1; // MinLumaBiPredSize8x8
            u32 log2_max_frame_num_minus4 : 4;
            u32 pic_order_cnt_type : 2;
            u32 log2_max_pic_order_cnt_lsb_minus4 : 4;
            u32 delta_pic_order_always_zero_flag : 1;
        } bits;
        u32 value;
    } seq_fields;
    u8 num_slice_groups_minus1;
    u8 slice_group_map_type;
    u16 slice_group_change_rate_minus1;
    i8 pic_init_qp_minus26;
    i8 pic_init_qs_minus26;
    i8 chroma_qp_index_offset;
    i8 second_chroma_qp_index_offset;
    union {
        struct {
            u32 entropy_coding_mode_flag : 1;
            u32 weighted_pred_flag : 1;
            u32 weighted_bipred_idc : 2;
            u32 transform_8x8_mode_flag : 1;
            u32 field_pic_flag : 1;
            u32 constrained_intra_pred_flag : 1;
            u32 pic_order_present_flag : 1;
            u32 deblocking_filter_control_present_flag : 1;
            u32 redundant_pic_cnt_present_flag : 1;
            u32 reference_pic_flag : 1;
        } bits;
        u32 value;
    } pic_fields;
    u16 frame_num;
    u32 va_reserved[kPaddingMedium];
};
struct IqMatrixH264 {
    u8 scaling_list_4x4[6][16];
    u8 scaling_list_8x8[2][64];
    u32 va_reserved[kPaddingLow];
};
struct SliceParameterH264 {
    u32 slice_data_size;
    u32 slice_data_offset;
    u32 slice_data_flag;
    u16 slice_data_bit_offset;
    u16 first_mb_in_slice;
    u8 slice_type;
    u8 direct_spatial_mv_pred_flag;
    u8 num_ref_idx_l0_active_minus1;
    u8 num_ref_idx_l1_active_minus1;
    u8 cabac_init_idc;
    i8 slice_qp_delta;
    u8 disable_deblocking_filter_idc;
    i8 slice_alpha_c0_offset_div2;
    i8 slice_beta_offset_div2;
    PictureH264 ref_pic_list0[32];
    PictureH264 ref_pic_list1[32];
    u8 luma_log2_weight_denom;
    u8 chroma_log2_weight_denom;
    u8 luma_weight_l0_flag;
    i16 luma_weight_l0[32];
    i16 luma_offset_l0[32];
    u8 chroma_weight_l0_flag;
    i16 chroma_weight_l0[32][2];
    i16 chroma_offset_l0[32][2];
    u8 luma_weight_l1_flag;
    i16 luma_weight_l1[32];
    i16 luma_offset_l1[32];
    u8 chroma_weight_l1_flag;
    i16 chroma_weight_l1[32][2];
    i16 chroma_offset_l1[32][2];
    u32 va_reserved[kPaddingLow];
};

// ---- HEVC (va.h, va_dec_hevc.h) ---------------------------------------------------------------------------------

struct PictureHevc {
    SurfaceId picture_id;
    i32 pic_order_cnt;
    u32 flags;
    u32 va_reserved[kPaddingLow];
};
struct PictureParameterHevc {
    PictureHevc curr_pic;
    PictureHevc reference_frames[15];
    u16 pic_width_in_luma_samples;
    u16 pic_height_in_luma_samples;
    union {
        struct {
            u32 chroma_format_idc : 2;
            u32 separate_colour_plane_flag : 1;
            u32 pcm_enabled_flag : 1;
            u32 scaling_list_enabled_flag : 1;
            u32 transform_skip_enabled_flag : 1;
            u32 amp_enabled_flag : 1;
            u32 strong_intra_smoothing_enabled_flag : 1;
            u32 sign_data_hiding_enabled_flag : 1;
            u32 constrained_intra_pred_flag : 1;
            u32 cu_qp_delta_enabled_flag : 1;
            u32 weighted_pred_flag : 1;
            u32 weighted_bipred_flag : 1;
            u32 transquant_bypass_enabled_flag : 1;
            u32 tiles_enabled_flag : 1;
            u32 entropy_coding_sync_enabled_flag : 1;
            u32 pps_loop_filter_across_slices_enabled_flag : 1;
            u32 loop_filter_across_tiles_enabled_flag : 1;
            u32 pcm_loop_filter_disabled_flag : 1;
            u32 no_pic_reordering_flag : 1; // NoPicReorderingFlag
            u32 no_bi_pred_flag : 1;        // NoBiPredFlag
            u32 reserved_bits : 11;
        } bits;
        u32 value;
    } pic_fields;
    u8 sps_max_dec_pic_buffering_minus1;
    u8 bit_depth_luma_minus8;
    u8 bit_depth_chroma_minus8;
    u8 pcm_sample_bit_depth_luma_minus1;
    u8 pcm_sample_bit_depth_chroma_minus1;
    u8 log2_min_luma_coding_block_size_minus3;
    u8 log2_diff_max_min_luma_coding_block_size;
    u8 log2_min_transform_block_size_minus2;
    u8 log2_diff_max_min_transform_block_size;
    u8 log2_min_pcm_luma_coding_block_size_minus3;
    u8 log2_diff_max_min_pcm_luma_coding_block_size;
    u8 max_transform_hierarchy_depth_intra;
    u8 max_transform_hierarchy_depth_inter;
    i8 init_qp_minus26;
    u8 diff_cu_qp_delta_depth;
    i8 pps_cb_qp_offset;
    i8 pps_cr_qp_offset;
    u8 log2_parallel_merge_level_minus2;
    u8 num_tile_columns_minus1;
    u8 num_tile_rows_minus1;
    u16 column_width_minus1[19];
    u16 row_height_minus1[21];
    union {
        struct {
            u32 lists_modification_present_flag : 1;
            u32 long_term_ref_pics_present_flag : 1;
            u32 sps_temporal_mvp_enabled_flag : 1;
            u32 cabac_init_present_flag : 1;
            u32 output_flag_present_flag : 1;
            u32 dependent_slice_segments_enabled_flag : 1;
            u32 pps_slice_chroma_qp_offsets_present_flag : 1;
            u32 sample_adaptive_offset_enabled_flag : 1;
            u32 deblocking_filter_override_enabled_flag : 1;
            u32 pps_disable_deblocking_filter_flag : 1;
            u32 slice_segment_header_extension_present_flag : 1;
            u32 rap_pic_flag : 1; // RapPicFlag
            u32 idr_pic_flag : 1; // IdrPicFlag
            u32 intra_pic_flag : 1; // IntraPicFlag
            u32 reserved_bits : 18;
        } bits;
        u32 value;
    } slice_parsing_fields;
    u8 log2_max_pic_order_cnt_lsb_minus4;
    u8 num_short_term_ref_pic_sets;
    u8 num_long_term_ref_pic_sps;
    u8 num_ref_idx_l0_default_active_minus1;
    u8 num_ref_idx_l1_default_active_minus1;
    i8 pps_beta_offset_div2;
    i8 pps_tc_offset_div2;
    u8 num_extra_slice_header_bits;
    u32 st_rps_bits;
    u32 va_reserved[kPaddingMedium];
};
struct SliceParameterHevc {
    u32 slice_data_size;
    u32 slice_data_offset;
    u32 slice_data_flag;
    u32 slice_data_byte_offset;
    u32 slice_segment_address;
    u8 ref_pic_list[2][15];
    union {
        u32 value;
        struct {
            u32 last_slice_of_pic : 1; // LastSliceOfPic
            u32 dependent_slice_segment_flag : 1;
            u32 slice_type : 2;
            u32 color_plane_id : 2;
            u32 slice_sao_luma_flag : 1;
            u32 slice_sao_chroma_flag : 1;
            u32 mvd_l1_zero_flag : 1;
            u32 cabac_init_flag : 1;
            u32 slice_temporal_mvp_enabled_flag : 1;
            u32 slice_deblocking_filter_disabled_flag : 1;
            u32 collocated_from_l0_flag : 1;
            u32 slice_loop_filter_across_slices_enabled_flag : 1;
            u32 reserved : 18;
        } fields;
    } long_slice_flags; // LongSliceFlags
    u8 collocated_ref_idx;
    u8 num_ref_idx_l0_active_minus1;
    u8 num_ref_idx_l1_active_minus1;
    i8 slice_qp_delta;
    i8 slice_cb_qp_offset;
    i8 slice_cr_qp_offset;
    i8 slice_beta_offset_div2;
    i8 slice_tc_offset_div2;
    u8 luma_log2_weight_denom;
    i8 delta_chroma_log2_weight_denom;
    i8 delta_luma_weight_l0[15];
    i8 luma_offset_l0[15];
    i8 delta_chroma_weight_l0[15][2];
    i8 chroma_offset_l0[15][2]; // ChromaOffsetL0
    i8 delta_luma_weight_l1[15];
    i8 luma_offset_l1[15];
    i8 delta_chroma_weight_l1[15][2];
    i8 chroma_offset_l1[15][2]; // ChromaOffsetL1
    u8 five_minus_max_num_merge_cand;
    u16 num_entry_point_offsets;
    u16 entry_offset_to_subset_array;
    u16 slice_data_num_emu_prevn_bytes;
    u32 va_reserved[kPaddingLow - 2];
};
struct IqMatrixHevc {
    u8 scaling_list_4x4[6][16];
    u8 scaling_list_8x8[6][64];
    u8 scaling_list_16x16[6][64];
    u8 scaling_list_32x32[2][64];
    u8 scaling_list_dc_16x16[6];
    u8 scaling_list_dc_32x32[2];
    u32 va_reserved[kPaddingLow];
};

// ---- AV1 (va_dec_av1.h) -----------------------------------------------------------------------------------------

struct SegmentationAv1 {
    union {
        struct {
            u32 enabled : 1;
            u32 update_map : 1;
            u32 temporal_update : 1;
            u32 update_data : 1;
            u32 reserved : 28;
        } bits;
        u32 value;
    } segment_info_fields;
    i16 feature_data[8][8];
    u8 feature_mask[8];
    u32 va_reserved[kPaddingLow];
};
struct FilmGrainAv1 {
    union {
        struct {
            u32 apply_grain : 1;
            u32 chroma_scaling_from_luma : 1;
            u32 grain_scaling_minus_8 : 2;
            u32 ar_coeff_lag : 2;
            u32 ar_coeff_shift_minus_6 : 2;
            u32 grain_scale_shift : 2;
            u32 overlap_flag : 1;
            u32 clip_to_restricted_range : 1;
            u32 reserved : 20;
        } bits;
        u32 value;
    } film_grain_info_fields;
    u16 grain_seed;
    u8 num_y_points;
    u8 point_y_value[14];
    u8 point_y_scaling[14];
    u8 num_cb_points;
    u8 point_cb_value[10];
    u8 point_cb_scaling[10];
    u8 num_cr_points;
    u8 point_cr_value[10];
    u8 point_cr_scaling[10];
    i8 ar_coeffs_y[24];
    i8 ar_coeffs_cb[25];
    i8 ar_coeffs_cr[25];
    u8 cb_mult;
    u8 cb_luma_mult;
    u16 cb_offset;
    u8 cr_mult;
    u8 cr_luma_mult;
    u16 cr_offset;
    u32 va_reserved[kPaddingLow];
};
struct WarpedMotionAv1 {
    i32 wmtype; // VAAV1TransformationType
    i32 wmmat[8];
    u8 invalid;
    u32 va_reserved[kPaddingLow];
};
struct PictureParameterAv1 {
    u8 profile;
    u8 order_hint_bits_minus_1;
    u8 bit_depth_idx;
    u8 matrix_coefficients;
    union {
        struct {
            u32 still_picture : 1;
            u32 use_128x128_superblock : 1;
            u32 enable_filter_intra : 1;
            u32 enable_intra_edge_filter : 1;
            u32 enable_interintra_compound : 1;
            u32 enable_masked_compound : 1;
            u32 enable_dual_filter : 1;
            u32 enable_order_hint : 1;
            u32 enable_jnt_comp : 1;
            u32 enable_cdef : 1;
            u32 mono_chrome : 1;
            u32 color_range : 1;
            u32 subsampling_x : 1;
            u32 subsampling_y : 1;
            u32 chroma_sample_position : 1;
            u32 film_grain_params_present : 1;
            u32 reserved : 16;
        } fields;
        u32 value;
    } seq_info_fields;
    SurfaceId current_frame;
    SurfaceId current_display_picture;
    u8 anchor_frames_num;
    SurfaceId* anchor_frames_list;
    u16 frame_width_minus1;
    u16 frame_height_minus1;
    u16 output_frame_width_in_tiles_minus_1;
    u16 output_frame_height_in_tiles_minus_1;
    SurfaceId ref_frame_map[8];
    u8 ref_frame_idx[7];
    u8 primary_ref_frame;
    u8 order_hint;
    SegmentationAv1 seg_info;
    FilmGrainAv1 film_grain_info;
    u8 tile_cols;
    u8 tile_rows;
    u16 width_in_sbs_minus_1[63];
    u16 height_in_sbs_minus_1[63];
    u16 tile_count_minus_1;
    u16 context_update_tile_id;
    union {
        struct {
            u32 frame_type : 2;
            u32 show_frame : 1;
            u32 showable_frame : 1;
            u32 error_resilient_mode : 1;
            u32 disable_cdf_update : 1;
            u32 allow_screen_content_tools : 1;
            u32 force_integer_mv : 1;
            u32 allow_intrabc : 1;
            u32 use_superres : 1;
            u32 allow_high_precision_mv : 1;
            u32 is_motion_mode_switchable : 1;
            u32 use_ref_frame_mvs : 1;
            u32 disable_frame_end_update_cdf : 1;
            u32 uniform_tile_spacing_flag : 1;
            u32 allow_warped_motion : 1;
            u32 large_scale_tile : 1;
            u32 reserved : 15;
        } bits;
        u32 value;
    } pic_info_fields;
    u8 superres_scale_denominator;
    u8 interp_filter;
    u8 filter_level[2];
    u8 filter_level_u;
    u8 filter_level_v;
    union {
        struct {
            u8 sharpness_level : 3;
            u8 mode_ref_delta_enabled : 1;
            u8 mode_ref_delta_update : 1;
            u8 reserved : 3;
        } bits;
        u8 value;
    } loop_filter_info_fields;
    i8 ref_deltas[8];
    i8 mode_deltas[2];
    u8 base_qindex;
    i8 y_dc_delta_q;
    i8 u_dc_delta_q;
    i8 u_ac_delta_q;
    i8 v_dc_delta_q;
    i8 v_ac_delta_q;
    union {
        struct {
            u16 using_qmatrix : 1;
            u16 qm_y : 4;
            u16 qm_u : 4;
            u16 qm_v : 4;
            u16 reserved : 3;
        } bits;
        u16 value;
    } qmatrix_fields;
    union {
        struct {
            u32 delta_q_present_flag : 1;
            u32 log2_delta_q_res : 2;
            u32 delta_lf_present_flag : 1;
            u32 log2_delta_lf_res : 2;
            u32 delta_lf_multi : 1;
            u32 tx_mode : 2;
            u32 reference_select : 1;
            u32 reduced_tx_set_used : 1;
            u32 skip_mode_present : 1;
            u32 reserved : 20;
        } bits;
        u32 value;
    } mode_control_fields;
    u8 cdef_damping_minus_3;
    u8 cdef_bits;
    u8 cdef_y_strengths[8];
    u8 cdef_uv_strengths[8];
    union {
        struct {
            u16 yframe_restoration_type : 2;
            u16 cbframe_restoration_type : 2;
            u16 crframe_restoration_type : 2;
            u16 lr_unit_shift : 2;
            u16 lr_uv_shift : 1;
            u16 reserved : 7;
        } bits;
        u16 value;
    } loop_restoration_fields;
    WarpedMotionAv1 wm[7];
    u32 va_reserved[kPaddingMedium];
};
struct SliceParameterAv1 {
    u32 slice_data_size;
    u32 slice_data_offset;
    u32 slice_data_flag;
    u16 tile_row;
    u16 tile_column;
    u16 tg_start;
    u16 tg_end;
    u8 anchor_frame_idx;
    u16 tile_idx_in_tile_list;
    u32 va_reserved[kPaddingLow];
};

// ---- Encoding and video processing (va.h, va_enc_h264.h, va_enc_hevc.h, va_vpp.h) --------------------------------

struct EncSequenceParameterH264 {
    u8 seq_parameter_set_id;
    u8 level_idc;
    u32 intra_period;
    u32 intra_idr_period;
    u32 ip_period;
    u32 bits_per_second;
    u32 max_num_ref_frames;
    u16 picture_width_in_mbs;
    u16 picture_height_in_mbs;
    union {
        struct {
            u32 chroma_format_idc : 2;
            u32 frame_mbs_only_flag : 1;
            u32 mb_adaptive_frame_field_flag : 1;
            u32 seq_scaling_matrix_present_flag : 1;
            u32 direct_8x8_inference_flag : 1;
            u32 log2_max_frame_num_minus4 : 4;
            u32 pic_order_cnt_type : 2;
            u32 log2_max_pic_order_cnt_lsb_minus4 : 4;
            u32 delta_pic_order_always_zero_flag : 1;
        } bits;
        u32 value;
    } seq_fields;
    u8 bit_depth_luma_minus8;
    u8 bit_depth_chroma_minus8;
    u8 num_ref_frames_in_pic_order_cnt_cycle;
    i32 offset_for_non_ref_pic;
    i32 offset_for_top_to_bottom_field;
    i32 offset_for_ref_frame[256];
    u8 frame_cropping_flag;
    u32 frame_crop_left_offset;
    u32 frame_crop_right_offset;
    u32 frame_crop_top_offset;
    u32 frame_crop_bottom_offset;
    u8 vui_parameters_present_flag;
    union {
        struct {
            u32 aspect_ratio_info_present_flag : 1;
            u32 timing_info_present_flag : 1;
            u32 bitstream_restriction_flag : 1;
            u32 log2_max_mv_length_horizontal : 5;
            u32 log2_max_mv_length_vertical : 5;
            u32 fixed_frame_rate_flag : 1;
            u32 low_delay_hrd_flag : 1;
            u32 motion_vectors_over_pic_boundaries_flag : 1;
            u32 reserved : 16;
        } bits;
        u32 value;
    } vui_fields;
    u8 aspect_ratio_idc;
    u32 sar_width;
    u32 sar_height;
    u32 num_units_in_tick;
    u32 time_scale;
    u32 va_reserved[kPaddingLow];
};
struct EncPictureParameterH264 {
    PictureH264 curr_pic;
    PictureH264 reference_frames[16];
    BufferId coded_buf;
    u8 pic_parameter_set_id;
    u8 seq_parameter_set_id;
    u8 last_picture;
    u16 frame_num;
    u8 pic_init_qp;
    u8 num_ref_idx_l0_active_minus1;
    u8 num_ref_idx_l1_active_minus1;
    i8 chroma_qp_index_offset;
    i8 second_chroma_qp_index_offset;
    union {
        struct {
            u32 idr_pic_flag : 1;
            u32 reference_pic_flag : 2;
            u32 entropy_coding_mode_flag : 1;
            u32 weighted_pred_flag : 1;
            u32 weighted_bipred_idc : 2;
            u32 constrained_intra_pred_flag : 1;
            u32 transform_8x8_mode_flag : 1;
            u32 deblocking_filter_control_present_flag : 1;
            u32 redundant_pic_cnt_present_flag : 1;
            u32 pic_order_present_flag : 1;
            u32 pic_scaling_matrix_present_flag : 1;
        } bits;
        u32 value;
    } pic_fields;
    u32 va_reserved[kPaddingLow];
};
struct EncSliceParameterH264 {
    u32 macroblock_address;
    u32 num_macroblocks;
    BufferId macroblock_info;
    u8 slice_type;
    u8 pic_parameter_set_id;
    u16 idr_pic_id;
    u16 pic_order_cnt_lsb;
    i32 delta_pic_order_cnt_bottom;
    i32 delta_pic_order_cnt[2];
    u8 direct_spatial_mv_pred_flag;
    u8 num_ref_idx_active_override_flag;
    u8 num_ref_idx_l0_active_minus1;
    u8 num_ref_idx_l1_active_minus1;
    PictureH264 ref_pic_list0[32];
    PictureH264 ref_pic_list1[32];
    u8 luma_log2_weight_denom;
    u8 chroma_log2_weight_denom;
    u8 luma_weight_l0_flag;
    i16 luma_weight_l0[32];
    i16 luma_offset_l0[32];
    u8 chroma_weight_l0_flag;
    i16 chroma_weight_l0[32][2];
    i16 chroma_offset_l0[32][2];
    u8 luma_weight_l1_flag;
    i16 luma_weight_l1[32];
    i16 luma_offset_l1[32];
    u8 chroma_weight_l1_flag;
    i16 chroma_weight_l1[32][2];
    i16 chroma_offset_l1[32][2];
    u8 cabac_init_idc;
    i8 slice_qp_delta;
    u8 disable_deblocking_filter_idc;
    i8 slice_alpha_c0_offset_div2;
    i8 slice_beta_offset_div2;
    u32 va_reserved[kPaddingLow];
};
struct EncSequenceParameterHevc {
    u8 general_profile_idc;
    u8 general_level_idc;
    u8 general_tier_flag;
    u32 intra_period;
    u32 intra_idr_period;
    u32 ip_period;
    u32 bits_per_second;
    u16 pic_width_in_luma_samples;
    u16 pic_height_in_luma_samples;
    union {
        struct {
            u32 chroma_format_idc : 2;
            u32 separate_colour_plane_flag : 1;
            u32 bit_depth_luma_minus8 : 3;
            u32 bit_depth_chroma_minus8 : 3;
            u32 scaling_list_enabled_flag : 1;
            u32 strong_intra_smoothing_enabled_flag : 1;
            u32 amp_enabled_flag : 1;
            u32 sample_adaptive_offset_enabled_flag : 1;
            u32 pcm_enabled_flag : 1;
            u32 pcm_loop_filter_disabled_flag : 1;
            u32 sps_temporal_mvp_enabled_flag : 1;
            u32 low_delay_seq : 1;
            u32 hierachical_flag : 1;
            u32 reserved_bits : 14;
        } bits;
        u32 value;
    } seq_fields;
    u8 log2_min_luma_coding_block_size_minus3;
    u8 log2_diff_max_min_luma_coding_block_size;
    u8 log2_min_transform_block_size_minus2;
    u8 log2_diff_max_min_transform_block_size;
    u8 max_transform_hierarchy_depth_inter;
    u8 max_transform_hierarchy_depth_intra;
    u32 pcm_sample_bit_depth_luma_minus1;
    u32 pcm_sample_bit_depth_chroma_minus1;
    u32 log2_min_pcm_luma_coding_block_size_minus3;
    u32 log2_max_pcm_luma_coding_block_size_minus3;
    u8 vui_parameters_present_flag;
    union {
        struct {
            u32 aspect_ratio_info_present_flag : 1;
            u32 neutral_chroma_indication_flag : 1;
            u32 field_seq_flag : 1;
            u32 vui_timing_info_present_flag : 1;
            u32 bitstream_restriction_flag : 1;
            u32 tiles_fixed_structure_flag : 1;
            u32 motion_vectors_over_pic_boundaries_flag : 1;
            u32 restricted_ref_pic_lists_flag : 1;
            u32 log2_max_mv_length_horizontal : 5;
            u32 log2_max_mv_length_vertical : 5;
        } bits;
        u32 value;
    } vui_fields;
    u8 aspect_ratio_idc;
    u32 sar_width;
    u32 sar_height;
    u32 vui_num_units_in_tick;
    u32 vui_time_scale;
    u16 min_spatial_segmentation_idc;
    u8 max_bytes_per_pic_denom;
    u8 max_bits_per_min_cu_denom;
    union {
        struct {
            u32 palette_mode_enabled_flag : 1;
            u32 reserved : 31;
        } bits;
        u32 value;
    } scc_fields;
    u32 va_reserved[kPaddingMedium - 1];
};
struct EncPictureParameterHevc {
    PictureHevc decoded_curr_pic;
    PictureHevc reference_frames[15];
    BufferId coded_buf;
    u8 collocated_ref_pic_index;
    u8 last_picture;
    u8 pic_init_qp;
    u8 diff_cu_qp_delta_depth;
    i8 pps_cb_qp_offset;
    i8 pps_cr_qp_offset;
    u8 num_tile_columns_minus1;
    u8 num_tile_rows_minus1;
    u8 column_width_minus1[19];
    u8 row_height_minus1[21];
    u8 log2_parallel_merge_level_minus2;
    u8 ctu_max_bitsize_allowed;
    u8 num_ref_idx_l0_default_active_minus1;
    u8 num_ref_idx_l1_default_active_minus1;
    u8 slice_pic_parameter_set_id;
    u8 nal_unit_type;
    union {
        struct {
            u32 idr_pic_flag : 1;
            u32 coding_type : 3;
            u32 reference_pic_flag : 1;
            u32 dependent_slice_segments_enabled_flag : 1;
            u32 sign_data_hiding_enabled_flag : 1;
            u32 constrained_intra_pred_flag : 1;
            u32 transform_skip_enabled_flag : 1;
            u32 cu_qp_delta_enabled_flag : 1;
            u32 weighted_pred_flag : 1;
            u32 weighted_bipred_flag : 1;
            u32 transquant_bypass_enabled_flag : 1;
            u32 tiles_enabled_flag : 1;
            u32 entropy_coding_sync_enabled_flag : 1;
            u32 loop_filter_across_tiles_enabled_flag : 1;
            u32 pps_loop_filter_across_slices_enabled_flag : 1;
            u32 scaling_list_data_present_flag : 1;
            u32 screen_content_flag : 1;
            u32 enable_gpu_weighted_prediction : 1;
            u32 no_output_of_prior_pics_flag : 1;
            u32 reserved : 11;
        } bits;
        u32 value;
    } pic_fields;
    u8 hierarchical_level_plus1;
    u8 va_byte_reserved;
    union {
        struct {
            u16 pps_curr_pic_ref_enabled_flag : 1;
            u16 reserved : 15;
        } bits;
        u16 value;
    } scc_fields;
    u32 va_reserved[kPaddingHigh - 1];
};
struct EncSliceParameterHevc {
    u32 slice_segment_address;
    u32 num_ctu_in_slice;
    u8 slice_type;
    u8 slice_pic_parameter_set_id;
    u8 num_ref_idx_l0_active_minus1;
    u8 num_ref_idx_l1_active_minus1;
    PictureHevc ref_pic_list0[15];
    PictureHevc ref_pic_list1[15];
    u8 luma_log2_weight_denom;
    i8 delta_chroma_log2_weight_denom;
    i8 delta_luma_weight_l0[15];
    i8 luma_offset_l0[15];
    i8 delta_chroma_weight_l0[15][2];
    i8 chroma_offset_l0[15][2];
    i8 delta_luma_weight_l1[15];
    i8 luma_offset_l1[15];
    i8 delta_chroma_weight_l1[15][2];
    i8 chroma_offset_l1[15][2];
    u8 max_num_merge_cand;
    i8 slice_qp_delta;
    i8 slice_cb_qp_offset;
    i8 slice_cr_qp_offset;
    i8 slice_beta_offset_div2;
    i8 slice_tc_offset_div2;
    union {
        struct {
            u32 last_slice_of_pic_flag : 1;
            u32 dependent_slice_segment_flag : 1;
            u32 colour_plane_id : 2;
            u32 slice_temporal_mvp_enabled_flag : 1;
            u32 slice_sao_luma_flag : 1;
            u32 slice_sao_chroma_flag : 1;
            u32 num_ref_idx_active_override_flag : 1;
            u32 mvd_l1_zero_flag : 1;
            u32 cabac_init_flag : 1;
            u32 slice_deblocking_filter_disabled_flag : 2;
            u32 slice_loop_filter_across_slices_enabled_flag : 1;
            u32 collocated_from_l0_flag : 1;
        } bits;
        u32 value;
    } slice_fields;
    u32 pred_weight_table_bit_offset;
    u32 pred_weight_table_bit_length;
    u32 va_reserved[kPaddingMedium - 2];
};
// VAEncMiscParameterBuffer: its type, then the parameter structure.
struct EncMiscRateControl {
    u32 bits_per_second;
    u32 target_percentage;
    u32 window_size;
    u32 initial_qp;
    u32 min_qp;
    u32 basic_unit_size;
    union {
        struct {
            u32 reset : 1;
            u32 disable_frame_skip : 1;
            u32 disable_bit_stuffing : 1;
            u32 mb_rate_control : 4;
            u32 temporal_id : 8;
            u32 cfs_I_frames : 1;
            u32 enable_parallel_brc : 1;
            u32 enable_dynamic_scaling : 1;
            u32 frame_tolerance_mode : 2;
            u32 reserved : 12;
        } bits;
        u32 value;
    } rc_flags;
    u32 ICQ_quality_factor;
    u32 max_qp;
    u32 quality_factor;
    u32 target_frame_size;
    u32 va_reserved[kPaddingLow];
};
struct EncMiscFrameRate {
    u32 framerate;
    union {
        struct {
            u32 temporal_id : 8;
            u32 reserved : 24;
        } bits;
        u32 value;
    } framerate_flags;
    u32 va_reserved[kPaddingLow];
};
struct CodedBufferSegment {
    u32 size;
    u32 bit_offset;
    u32 status;
    u32 reserved;
    void* buf;
    void* next;
    u32 va_reserved[kPaddingLow];
};
struct Rectangle {
    i16 x;
    i16 y;
    u16 width;
    u16 height;
};
struct ProcColorProperties {
    u8 chroma_sample_location;
    u8 color_range;
    u8 colour_primaries;
    u8 transfer_characteristics;
    u8 matrix_coefficients;
    u8 reserved[3];
};
struct ProcPipelineParameter {
    SurfaceId surface;
    const Rectangle* surface_region;
    u32 surface_color_standard;
    const Rectangle* output_region;
    u32 output_background_color;
    u32 output_color_standard;
    u32 pipeline_flags;
    u32 filter_flags;
    BufferId* filters;
    u32 num_filters;
    SurfaceId* forward_references;
    u32 num_forward_references;
    SurfaceId* backward_references;
    u32 num_backward_references;
    u32 rotation_state;
    const void* blend_state;
    u32 mirror_state;
    SurfaceId* additional_outputs;
    u32 num_additional_outputs;
    u32 input_surface_flag;
    u32 output_surface_flag;
    ProcColorProperties input_color_properties;
    ProcColorProperties output_color_properties;
    u32 processing_mode;
    void* output_hdr_metadata;
    u32 va_reserved[kPaddingLarge - 16];
};

// ---- AV1 encoding (va_enc_av1.h) --------------------------------------------------------------------------------

struct EncPackedHeaderParameter {
    u32 type;
    u32 bit_length;
    u8 has_emulation_bytes;
    u32 va_reserved[kPaddingLow];
};
// The values of VAConfigAttribEncAV1Ext1 and VAConfigAttribEncAV1Ext2.
union EncAv1Ext1 {
    struct {
        u32 interpolation_filter : 5;
        u32 min_segid_block_size_accepted : 8;
        u32 segment_feature_support : 8;
        u32 reserved : 11;
    } bits;
    u32 value;
};
union EncAv1Ext2 {
    struct {
        u32 tile_size_bytes_minus1 : 2;
        u32 obu_size_bytes_minus1 : 2;
        u32 tx_mode_support : 3;
        u32 max_tile_num_minus1 : 13;
        u32 reserved : 12;
    } bits;
    u32 value;
};
struct EncSequenceParameterAv1 {
    u8 seq_profile;
    u8 seq_level_idx;
    u8 seq_tier;
    u8 hierarchical_flag;
    u32 intra_period;
    u32 ip_period;
    u32 bits_per_second;
    union {
        struct {
            u32 still_picture : 1;
            u32 use_128x128_superblock : 1;
            u32 enable_filter_intra : 1;
            u32 enable_intra_edge_filter : 1;
            u32 enable_interintra_compound : 1;
            u32 enable_masked_compound : 1;
            u32 enable_warped_motion : 1;
            u32 enable_dual_filter : 1;
            u32 enable_order_hint : 1;
            u32 enable_jnt_comp : 1;
            u32 enable_ref_frame_mvs : 1;
            u32 enable_superres : 1;
            u32 enable_cdef : 1;
            u32 enable_restoration : 1;
            u32 bit_depth_minus8 : 3;
            u32 subsampling_x : 1;
            u32 subsampling_y : 1;
            u32 mono_chrome : 1;
            u32 reserved_bits : 12;
        } bits;
        u32 value;
    } seq_fields;
    u8 order_hint_bits_minus_1;
    u32 va_reserved[kPaddingHigh];
};
struct EncSegmentationAv1 {
    union {
        struct {
            u8 segmentation_enabled : 1;
            u8 segmentation_update_map : 1;
            u8 segmentation_temporal_update : 1;
            u8 reserved : 5;
        } bits;
        u8 value;
    } seg_flags;
    u8 segment_number;
    i16 feature_data[8][8];
    u8 feature_mask[8];
    u32 va_reserved[kPaddingLow];
};
struct EncWarpedMotionAv1 {
    u32 wmtype;
    i32 wmmat[8];
    u8 invalid;
    u32 va_reserved[kPaddingLow];
};
union RefFrameControlAv1 {
    struct {
        u32 search_idx0 : 3;
        u32 search_idx1 : 3;
        u32 search_idx2 : 3;
        u32 search_idx3 : 3;
        u32 search_idx4 : 3;
        u32 search_idx5 : 3;
        u32 search_idx6 : 3;
        u32 Reserved : 11;
    } fields;
    u32 value;
};
struct EncPictureParameterAv1 {
    u16 frame_width_minus_1;
    u16 frame_height_minus_1;
    SurfaceId reconstructed_frame;
    BufferId coded_buf;
    SurfaceId reference_frames[8];
    u8 ref_frame_idx[7];
    u8 hierarchical_level_plus1;
    u8 primary_ref_frame;
    u8 order_hint;
    u8 refresh_frame_flags;
    u8 reserved8bits1;
    RefFrameControlAv1 ref_frame_ctrl_l0;
    RefFrameControlAv1 ref_frame_ctrl_l1;
    union {
        struct {
            u32 frame_type : 2;
            u32 error_resilient_mode : 1;
            u32 disable_cdf_update : 1;
            u32 use_superres : 1;
            u32 allow_high_precision_mv : 1;
            u32 use_ref_frame_mvs : 1;
            u32 disable_frame_end_update_cdf : 1;
            u32 reduced_tx_set : 1;
            u32 enable_frame_obu : 1;
            u32 long_term_reference : 1;
            u32 disable_frame_recon : 1;
            u32 allow_intrabc : 1;
            u32 palette_mode_enable : 1;
            u32 reserved : 18;
        } bits;
        u32 value;
    } picture_flags;
    u8 seg_id_block_size;
    u8 num_tile_groups_minus1;
    u8 temporal_id;
    u8 filter_level[2];
    u8 filter_level_u;
    u8 filter_level_v;
    union {
        struct {
            u8 sharpness_level : 3;
            u8 mode_ref_delta_enabled : 1;
            u8 mode_ref_delta_update : 1;
            u8 reserved : 3;
        } bits;
        u8 value;
    } loop_filter_flags;
    u8 superres_scale_denominator;
    u8 interpolation_filter;
    i8 ref_deltas[8];
    i8 mode_deltas[2];
    u8 base_qindex;
    i8 y_dc_delta_q;
    i8 u_dc_delta_q;
    i8 u_ac_delta_q;
    i8 v_dc_delta_q;
    i8 v_ac_delta_q;
    u8 min_base_qindex;
    u8 max_base_qindex;
    union {
        struct {
            u16 using_qmatrix : 1;
            u16 qm_y : 4;
            u16 qm_u : 4;
            u16 qm_v : 4;
            u16 reserved : 3;
        } bits;
        u16 value;
    } qmatrix_flags;
    u16 reserved16bits1;
    union {
        struct {
            u32 delta_q_present : 1;
            u32 delta_q_res : 2;
            u32 delta_lf_present : 1;
            u32 delta_lf_res : 2;
            u32 delta_lf_multi : 1;
            u32 tx_mode : 2;
            u32 reference_mode : 2;
            u32 skip_mode_present : 1;
            u32 reserved : 20;
        } bits;
        u32 value;
    } mode_control_flags;
    EncSegmentationAv1 segments;
    u8 tile_cols;
    u8 tile_rows;
    u16 reserved16bits2;
    u16 width_in_sbs_minus_1[63];
    u16 height_in_sbs_minus_1[63];
    u16 context_update_tile_id;
    u8 cdef_damping_minus_3;
    u8 cdef_bits;
    u8 cdef_y_strengths[8];
    u8 cdef_uv_strengths[8];
    union {
        struct {
            u16 yframe_restoration_type : 2;
            u16 cbframe_restoration_type : 2;
            u16 crframe_restoration_type : 2;
            u16 lr_unit_shift : 2;
            u16 lr_uv_shift : 1;
            u16 reserved : 7;
        } bits;
        u16 value;
    } loop_restoration_flags;
    EncWarpedMotionAv1 wm[7];
    u32 bit_offset_qindex;
    u32 bit_offset_segmentation;
    u32 bit_offset_loopfilter_params;
    u32 bit_offset_cdef_params;
    u32 size_in_bits_cdef_params;
    u32 byte_offset_frame_hdr_obu_size;
    u32 size_in_bits_frame_hdr_obu;
    union {
        struct {
            u8 obu_extension_flag : 1;
            u8 obu_has_size_field : 1;
            u8 temporal_id : 3;
            u8 spatial_id : 2;
            u8 reserved : 1;
        } bits;
        u8 value;
    } tile_group_obu_hdr_info;
    u8 number_skip_frames;
    u16 reserved16bits3;
    i32 skip_frames_reduced_size;
    u32 va_reserved[kPaddingHigh];
};
struct EncTileGroupAv1 {
    u8 tg_start;
    u8 tg_end;
    u32 va_reserved[kPaddingLow];
};

// ---- The functions ----------------------------------------------------------------------------------------------

// libva.so.2 (VAProfile, VAEntrypoint and VABufferType are int-sized enumerations).
#define MAICORD_VA_FUNCTIONS(F)                                                                                        \
    F(Status, vaInitialize, (Display, i32*, i32*))                                                                     \
    F(Status, vaTerminate, (Display))                                                                                  \
    F(i32, vaMaxNumProfiles, (Display))                                                                                \
    F(i32, vaMaxNumEntrypoints, (Display))                                                                             \
    F(Status, vaQueryConfigProfiles, (Display, i32*, i32*))                                                            \
    F(Status, vaQueryConfigEntrypoints, (Display, i32, i32*, i32*))                                                    \
    F(Status, vaGetConfigAttributes, (Display, i32, i32, ConfigAttrib*, i32))                                          \
    F(Status, vaCreateConfig, (Display, i32, i32, ConfigAttrib*, i32, ConfigId*))                                      \
    F(Status, vaDestroyConfig, (Display, ConfigId))                                                                    \
    F(Status, vaCreateSurfaces, (Display, u32, u32, u32, SurfaceId*, u32, SurfaceAttrib*, u32))                        \
    F(Status, vaDestroySurfaces, (Display, SurfaceId*, i32))                                                           \
    F(Status, vaCreateContext, (Display, ConfigId, i32, i32, i32, SurfaceId*, i32, ContextId*))                        \
    F(Status, vaDestroyContext, (Display, ContextId))                                                                  \
    F(Status, vaCreateBuffer, (Display, ContextId, i32, u32, u32, void*, BufferId*))                                   \
    F(Status, vaDestroyBuffer, (Display, BufferId))                                                                    \
    F(Status, vaBeginPicture, (Display, ContextId, SurfaceId))                                                         \
    F(Status, vaRenderPicture, (Display, ContextId, BufferId*, i32))                                                   \
    F(Status, vaEndPicture, (Display, ContextId))                                                                      \
    F(Status, vaSyncSurface, (Display, SurfaceId))                                                                     \
    F(Status, vaSyncSurface2, (Display, SurfaceId, u64))                                                               \
    F(const char*, vaQueryVendorString, (Display))                                                                     \
    F(Status, vaExportSurfaceHandle, (Display, SurfaceId, u32, u32, void*))                                            \
    F(Status, vaMapBuffer, (Display, BufferId, void**))                                                                \
    F(Status, vaUnmapBuffer, (Display, BufferId))
// libva-drm.so.2
#define MAICORD_VA_DRM_FUNCTIONS(F) F(Display, vaGetDisplayDRM, (i32))

struct Api {
#define MAICORD_VA_MEMBER(ret, name, args) ret(*name) args;
    MAICORD_VA_FUNCTIONS(MAICORD_VA_MEMBER)
    MAICORD_VA_DRM_FUNCTIONS(MAICORD_VA_MEMBER)
#undef MAICORD_VA_MEMBER
};

static_assert(sizeof(ConfigAttrib) == 8 && sizeof(GenericValue) == 16 && sizeof(SurfaceAttrib) == 24);
static_assert(sizeof(PictureH264) == 36 && sizeof(PictureHevc) == 28);
static_assert(sizeof(DrmPrimeDescriptor) == 312);
static_assert(__builtin_offsetof(DrmPrimeDescriptor, num_layers) == 80);
static_assert(__builtin_offsetof(DrmPrimeDescriptor, layers) == 84);
static_assert(sizeof(PictureParameterH264) == 672);
static_assert(__builtin_offsetof(PictureParameterH264, frame_num) == 636);
static_assert(sizeof(IqMatrixH264) == 240);
static_assert(sizeof(SliceParameterH264) == 3128);
static_assert(sizeof(PictureParameterHevc) == 604);
static_assert(__builtin_offsetof(PictureParameterHevc, st_rps_bits) == 568);
static_assert(sizeof(SliceParameterHevc) == 264);
static_assert(__builtin_offsetof(SliceParameterHevc, long_slice_flags) == 52);
static_assert(__builtin_offsetof(SliceParameterHevc, num_entry_point_offsets) == 248);
static_assert(sizeof(IqMatrixHevc) == 1016);
static_assert(sizeof(PictureParameterAv1) == 1160);
static_assert(__builtin_offsetof(PictureParameterAv1, tile_cols) == 416);
static_assert(__builtin_offsetof(PictureParameterAv1, wm) == 732);
static_assert(sizeof(SliceParameterAv1) == 40);
static_assert(sizeof(EncSequenceParameterH264) == 1132 && sizeof(EncPictureParameterH264) == 648);
static_assert(sizeof(EncSliceParameterH264) == 3140);
static_assert(sizeof(EncSequenceParameterHevc) == 116 && sizeof(EncPictureParameterHevc) == 576);
static_assert(sizeof(EncSliceParameterHevc) == 1076);
static_assert(sizeof(EncMiscRateControl) == 60 && sizeof(EncMiscFrameRate) == 24 && sizeof(CodedBufferSegment) == 48);
static_assert(sizeof(ProcPipelineParameter) == 224);
static_assert(__builtin_offsetof(ProcPipelineParameter, input_color_properties) == 132);
static_assert(__builtin_offsetof(ProcPipelineParameter, processing_mode) == 148);
static_assert(sizeof(EncPackedHeaderParameter) == 28 && sizeof(EncSequenceParameterAv1) == 88);
static_assert(sizeof(EncPictureParameterAv1) == 1032 && sizeof(EncTileGroupAv1) == 20);
static_assert(sizeof(EncSegmentationAv1) == 156 && sizeof(EncWarpedMotionAv1) == 56);
static_assert(__builtin_offsetof(EncPictureParameterAv1, segments) == 104 &&
              __builtin_offsetof(EncPictureParameterAv1, wm) == 540);
static_assert(__builtin_offsetof(EncPictureParameterAv1, bit_offset_qindex) == 932 &&
              __builtin_offsetof(EncPictureParameterAv1, tile_group_obu_hdr_info) == 960);

} // namespace maicord::vaapi
