#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
enum class HevcError : u8 {
    none,
    end,
    invalid_input,
    truncated,
    malformed,
    unsupported
};
inline constexpr u32 kHevcMaximumSlices = 128;
struct HevcNalCursor {
    ConstSlice<u8> bytes{};
    u32 offset{};
    u8 length_bytes{};
};
struct HevcNal {
    ConstSlice<u8> bytes{};
    HevcError error{};
    u8 type{}, temporal_id{};
};
// nuh_layer_id of a NAL unit with its two header bytes.
constexpr u8 hevc_nal_layer(ConstSlice<u8> nal) noexcept {
    return static_cast<u8>(((nal[0] & 1u) << 5) | (nal[1] >> 3));
}
// The next base-layer NAL unit: units of other layers (an alpha or
// enhancement layer) are skipped, as a single-layer decoder must.
HevcNal hevc_next_nal(HevcNalCursor*) noexcept;
struct HevcScaling {
    u8 list4x4[6][16]{}, list8x8[6][64]{}, list16x16[6][64]{}, list32x32[2][64]{};
    u8 dc16[6]{}, dc32[2]{};
};
struct HevcReferenceSet {
    i32 delta[15]{};
    u16 used{};
    u8 negative{}, positive{};
};
struct HevcSps {
    HevcScaling scaling{};
    HevcReferenceSet short_term[64]{};
    u32 coded_width{}, coded_height{}, crop_x{}, crop_y{}, width{}, height{};
    u32 sar_width{1}, sar_height{1};
    u16 long_term_lsb[15]{};
    u16 long_term_used{};
    u8 id{}, vps_id{}, profile{}, level{}, max_sub_layers{}, poc_bits{}, max_buffering{}, max_reorder{};
    u8 log2_min_cb_minus3{}, log2_diff_cb{}, log2_min_tb_minus2{}, log2_diff_tb{};
    u8 transform_depth_inter{}, transform_depth_intra{}, short_term_count{}, long_term_count{};
    u8 pcm_luma_depth_minus1{}, pcm_chroma_depth_minus1{}, log2_min_pcm_minus3{}, log2_diff_pcm{};
    u8 matrix{};
    bool scaling_enabled{}, scaling_present{}, amp{}, sao{}, pcm{}, pcm_loop_filter_disabled{};
    bool long_term_present{}, temporal_mvp{}, strong_intra_smoothing{}, full_range{};
};
struct HevcPps {
    HevcScaling scaling{};
    u16 column_width_minus1[19]{}, row_height_minus1[21]{};
    u8 id{}, sps_id{}, extra_slice_bits{}, references_l0{}, references_l1{}, diff_cu_qp_depth{};
    u8 tile_columns_minus1{}, tile_rows_minus1{}, log2_parallel_merge_minus2{};
    signed char initial_qp_minus26{}, cb_qp_offset{}, cr_qp_offset{}, beta_offset_div2{}, tc_offset_div2{};
    bool dependent_slices{}, output_flag_present{}, sign_data_hiding{}, cabac_init{}, constrained_intra{};
    bool transform_skip{}, cu_qp_delta{}, slice_chroma_offsets{}, weighted_pred{}, weighted_bipred{};
    bool transquant_bypass{}, tiles{}, entropy_sync{}, uniform_spacing{}, loop_filter_tiles{};
    bool loop_filter_slices{}, deblocking_override{}, deblocking_disabled{}, lists_modification{};
    bool slice_header_extension{}, scaling_present{};
};
struct HevcConfiguration {
    HevcSps sps{};
    HevcPps pps{};
    u8 nal_length_bytes{};
};
struct HevcLongTermReference {
    u32 delta_msb_cycles{};
    u16 poc_lsb{};
    bool delta_msb_present{}, used{};
};
struct HevcSliceSpan {
    u32 offset{}, bytes{}, segment_address{};
    bool dependent{};
};
struct HevcPicture {
    HevcReferenceSet short_term{};
    HevcLongTermReference long_term[15]{};
    HevcSliceSpan slices[kHevcMaximumSlices]{};
    u32 bitstream_bytes{};
    u16 poc_lsb{}, short_term_bits{};
    u8 nal_type{}, temporal_id{}, slice_type{}, pps_id{}, long_term_count{}, slice_count{},
        reference_rps_delta_count{};
    bool output{true}, no_output_prior{}, irap{}, idr{}, reference{};
};
HevcError hevc_parse_sps(ConstSlice<u8>, HevcSps*) noexcept;
HevcError hevc_parse_pps(ConstSlice<u8>, const HevcSps&, HevcPps*) noexcept;
// Validates all hvcC array extents; resolves one exact active parameter chain.
HevcError hevc_select_parameters(ConstSlice<u8> hvcc, u8 pps_id, HevcConfiguration*) noexcept;
HevcError hevc_validate_parameter(ConstSlice<u8> nal, const HevcConfiguration&) noexcept;
HevcError hevc_picture_parameter_id(ConstSlice<u8> unit, u8 length_bytes, u8*) noexcept;
// Output is transactional. No allocation, retained input, or invented POC.
HevcError hevc_parse_picture(ConstSlice<u8> unit, const HevcConfiguration&, HevcPicture*) noexcept;
static_assert(sizeof(HevcConfiguration) <= 8192);
static_assert(sizeof(HevcPicture) <= 2560);
} // namespace maicord
