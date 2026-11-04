#include "va_parameters.hpp"

#include "maicord/core/memory.hpp"
// Picture and slice parameters, once per picture and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord::vaapi {
namespace {

constexpr u8 kZigzag4x4[16]{0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
constexpr u8 kZigzag8x8[64]{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                            41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                            30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

PictureH264 invalid_picture() noexcept {
    PictureH264 picture{};
    picture.picture_id = kInvalidId;
    picture.flags = kPictureH264Invalid;
    return picture;
}

PictureH264 reference_picture(const H264Reference& reference, const SurfaceId* surfaces) noexcept {
    PictureH264 picture{};
    picture.picture_id = surfaces[reference.surface];
    picture.frame_idx = reference.long_term() ? reference.long_term_index : reference.frame_num;
    picture.flags = reference.long_term() ? kPictureH264LongTermReference : kPictureH264ShortTermReference;
    picture.top_field_order_cnt = reference.top;
    picture.bottom_field_order_cnt = reference.bottom;
    return picture;
}

i32 frame_order(const H264Reference& reference) noexcept {
    return reference.top < reference.bottom ? reference.top : reference.bottom;
}

// A reference list as indices into the DPB (255: no picture), sorted by key (descending or ascending).
struct List {
    u8 entry[34];
    u32 count;
};

template <typename Key> void sort_range(u8* entries, u32 count, Key key, bool descending) noexcept {
    for (u32 i = 1; i < count; ++i) {
        const u8 value = entries[i];
        u32 j = i;
        while (j > 0 && (descending ? key(entries[j - 1]) < key(value) : key(entries[j - 1]) > key(value))) {
            entries[j] = entries[j - 1];
            --j;
        }
        entries[j] = value;
    }
}

// 8.2.4.2: the initial list of a P (list 0) or B slice (both lists), before truncation and changes.
void initial_lists(const H264Sps& sps, const H264Slice& first, const H264ReferenceState& dpb,
                   const H264PictureOrder& order, bool bipred, List* lists) noexcept {
    const u32 modulus = 1u << sps.frame_num_bits;
    const i32 current_order = order.top < order.bottom ? order.top : order.bottom;
    u8 shorts[16], longs[16];
    u32 short_count = 0, long_count = 0;
    for (u32 i = 0; i < dpb.count && i < 16; ++i)
        (dpb.pictures[i].long_term() ? longs[long_count++] : shorts[short_count++]) = static_cast<u8>(i);
    const auto wrap = [&](u8 index) noexcept {
        const i32 frame_num = dpb.pictures[index].frame_num;
        return frame_num > first.frame_num ? frame_num - static_cast<i32>(modulus) : frame_num;
    };
    const auto order_of = [&](u8 index) noexcept { return frame_order(dpb.pictures[index]); };
    const auto long_number = [&](u8 index) noexcept { return static_cast<i32>(dpb.pictures[index].long_term_index); };
    sort_range(longs, long_count, long_number, false);
    List& l0 = lists[0];
    if (!bipred) {
        sort_range(shorts, short_count, wrap, true);
        for (u32 i = 0; i < short_count; ++i)
            l0.entry[l0.count++] = shorts[i];
        for (u32 i = 0; i < long_count; ++i)
            l0.entry[l0.count++] = longs[i];
        return;
    }
    u8 before[16], after[16];
    u32 before_count = 0, after_count = 0;
    for (u32 i = 0; i < short_count; ++i)
        (order_of(shorts[i]) <= current_order ? before[before_count++] : after[after_count++]) = shorts[i];
    sort_range(before, before_count, order_of, true);
    sort_range(after, after_count, order_of, false);
    List& l1 = lists[1];
    for (u32 i = 0; i < before_count; ++i)
        l0.entry[l0.count++] = before[i];
    for (u32 i = 0; i < after_count; ++i)
        l0.entry[l0.count++] = after[i];
    for (u32 i = 0; i < after_count; ++i)
        l1.entry[l1.count++] = after[i];
    for (u32 i = 0; i < before_count; ++i)
        l1.entry[l1.count++] = before[i];
    for (u32 i = 0; i < long_count; ++i) {
        l0.entry[l0.count++] = longs[i];
        l1.entry[l1.count++] = longs[i];
    }
    // More than one entry and the same order in both: the first two of list 1 swap.
    if (l1.count > 1 && l0.count == l1.count) {
        bool same = true;
        for (u32 i = 0; i < l0.count; ++i)
            same = same && l0.entry[i] == l1.entry[i];
        if (same) {
            const u8 swap = l1.entry[0];
            l1.entry[0] = l1.entry[1];
            l1.entry[1] = swap;
        }
    }
}

// 8.2.4.3: the header's changes applied to a list of `active` entries (the initial list truncated or padded with
// 255). False when a change names a picture that is not in the DPB.
bool apply_changes(const H264Sps& sps, const H264Slice& first, const H264ReferenceState& dpb,
                   const H264SliceDetail& detail, u32 list, u32 active, List* result) noexcept {
    const i32 modulus = 1 << sps.frame_num_bits;
    u8 working[34];
    for (u32 i = 0; i <= active; ++i)
        working[i] = i < result->count && i < active ? result->entry[i] : 255;
    i32 prediction = first.frame_num;
    u32 index = 0;
    for (u32 c = 0; c < detail.change_count[list]; ++c) {
        const H264ListChange& change = detail.changes[list][c];
        if (index >= active)
            return false;
        u8 picture = 255;
        i32 picture_number = 0;
        const bool is_long = change.operation == 2;
        if (!is_long) {
            const i32 difference = static_cast<i32>(change.value) + 1;
            i32 no_wrap = change.operation == 0 ? prediction - difference : prediction + difference;
            if (change.operation == 0 && no_wrap < 0)
                no_wrap += modulus;
            if (change.operation == 1 && no_wrap >= modulus)
                no_wrap -= modulus;
            prediction = no_wrap;
            picture_number = no_wrap > first.frame_num ? no_wrap - modulus : no_wrap;
            for (u32 i = 0; i < dpb.count && i < 16; ++i) {
                const H264Reference& reference = dpb.pictures[i];
                const i32 wrapped = static_cast<i32>(reference.frame_num) > first.frame_num
                                        ? static_cast<i32>(reference.frame_num) - modulus
                                        : static_cast<i32>(reference.frame_num);
                if (!reference.long_term() && wrapped == picture_number)
                    picture = static_cast<u8>(i);
            }
        } else {
            picture_number = static_cast<i32>(change.value);
            for (u32 i = 0; i < dpb.count && i < 16; ++i)
                if (dpb.pictures[i].long_term() && dpb.pictures[i].long_term_index == change.value)
                    picture = static_cast<u8>(i);
        }
        if (picture == 255)
            return false;
        for (u32 shift = active; shift > index; --shift)
            working[shift] = working[shift - 1];
        working[index++] = picture;
        u32 kept = index;
        for (u32 i = index; i <= active; ++i) {
            const u8 entry = working[i];
            const bool same = entry != 255 && dpb.pictures[entry].long_term() == is_long &&
                              (is_long ? static_cast<i32>(dpb.pictures[entry].long_term_index) == picture_number
                                       : (static_cast<i32>(dpb.pictures[entry].frame_num) > first.frame_num
                                              ? static_cast<i32>(dpb.pictures[entry].frame_num) - modulus
                                              : static_cast<i32>(dpb.pictures[entry].frame_num)) == picture_number);
            if (!same)
                working[kept++] = entry;
        }
    }
    for (u32 i = 0; i < active; ++i)
        result->entry[i] = working[i];
    result->count = active;
    return true;
}

} // namespace

void h264_picture_parameters(const H264Sps& a, const H264Pps& b, const H264Slice& first,
                             const H264ReferenceState& references, const H264PictureOrder& order, u8 surface,
                             const SurfaceId* surfaces, PictureParameterH264* output) noexcept {
    PictureParameterH264& p = *output;
    clear_bytes(&p, sizeof(p));
    p.curr_pic.picture_id = surfaces[surface];
    p.curr_pic.frame_idx = first.frame_num;
    p.curr_pic.flags = first.reference ? (first.long_term ? kPictureH264LongTermReference : kPictureH264ShortTermReference) : 0;
    p.curr_pic.top_field_order_cnt = order.top;
    p.curr_pic.bottom_field_order_cnt = order.bottom;
    for (auto& entry : p.reference_frames)
        entry = invalid_picture();
    if (!first.idr)
        for (u32 i = 0; i < references.count && i < 16; ++i)
            p.reference_frames[i] = reference_picture(references.pictures[i], surfaces);
    p.picture_width_in_mbs_minus1 = static_cast<u16>(a.coded_width / 16u - 1u);
    p.picture_height_in_mbs_minus1 = static_cast<u16>(a.coded_height / 16u - 1u);
    p.num_ref_frames = a.max_references;
    p.seq_fields.bits.chroma_format_idc = 1;
    p.seq_fields.bits.gaps_in_frame_num_value_allowed_flag = a.gaps;
    p.seq_fields.bits.frame_mbs_only_flag = a.frame_only;
    p.seq_fields.bits.mb_adaptive_frame_field_flag = a.mbaff;
    p.seq_fields.bits.direct_8x8_inference_flag = a.direct_8x8;
    p.seq_fields.bits.min_luma_bi_pred_size_8x8 = a.level >= 31;
    p.seq_fields.bits.log2_max_frame_num_minus4 = a.frame_num_bits - 4u;
    p.seq_fields.bits.pic_order_cnt_type = a.poc_type;
    p.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = a.poc_type == 0 ? a.poc_bits - 4u : 0;
    p.seq_fields.bits.delta_pic_order_always_zero_flag = a.delta_always_zero;
    p.pic_init_qp_minus26 = b.initial_qp;
    p.pic_init_qs_minus26 = b.initial_qs;
    p.chroma_qp_index_offset = b.chroma_qp;
    p.second_chroma_qp_index_offset = b.second_chroma_qp;
    p.pic_fields.bits.entropy_coding_mode_flag = b.cabac;
    p.pic_fields.bits.weighted_pred_flag = b.weighted_pred;
    p.pic_fields.bits.weighted_bipred_idc = b.weighted_bipred;
    p.pic_fields.bits.transform_8x8_mode_flag = b.transform_8x8;
    p.pic_fields.bits.constrained_intra_pred_flag = b.constrained_intra;
    p.pic_fields.bits.pic_order_present_flag = b.bottom_poc_present;
    p.pic_fields.bits.deblocking_filter_control_present_flag = b.deblocking_control;
    p.pic_fields.bits.redundant_pic_cnt_present_flag = b.redundant_count;
    p.pic_fields.bits.reference_pic_flag = first.reference != 0;
    p.frame_num = first.frame_num;
}

void h264_iq_matrix(const H264Pps& pps, IqMatrixH264* output) noexcept {
    clear_bytes(output, sizeof(*output));
    for (u32 list = 0; list < 6; ++list)
        for (u32 k = 0; k < 16; ++k)
            output->scaling_list_4x4[list][kZigzag4x4[k]] = pps.scaling.list4x4[list][k];
    for (u32 list = 0; list < 2; ++list)
        for (u32 k = 0; k < 64; ++k)
            output->scaling_list_8x8[list][kZigzag8x8[k]] = pps.scaling.list8x8[list][k];
}

bool h264_slice_parameters(const H264Sps& sps, const H264Pps& pps, const H264Slice& first,
                           const H264SliceDetail& detail, const H264ReferenceState& references,
                           const H264PictureOrder& order, const SurfaceId* surfaces, u32 data_bytes,
                           SliceParameterH264* output) noexcept {
    SliceParameterH264& s = *output;
    clear_bytes(&s, sizeof(s));
    s.slice_data_size = data_bytes;
    s.slice_data_offset = 0;
    s.slice_data_flag = kSliceDataFlagAll;
    s.slice_data_bit_offset = static_cast<u16>(detail.data_bit_offset);
    s.first_mb_in_slice = static_cast<u16>(detail.first_mb);
    s.slice_type = static_cast<u8>(detail.type);
    s.direct_spatial_mv_pred_flag = detail.direct_spatial_mv_pred;
    s.num_ref_idx_l0_active_minus1 = detail.active[0] ? detail.active[0] - 1u : 0;
    s.num_ref_idx_l1_active_minus1 = detail.active[1] ? detail.active[1] - 1u : 0;
    s.cabac_init_idc = detail.cabac_init_idc;
    s.slice_qp_delta = detail.qp_delta;
    s.disable_deblocking_filter_idc = detail.disable_deblocking_filter_idc;
    s.slice_alpha_c0_offset_div2 = detail.alpha_c0_offset_div2;
    s.slice_beta_offset_div2 = detail.beta_offset_div2;
    for (auto& entry : s.ref_pic_list0)
        entry = invalid_picture();
    for (auto& entry : s.ref_pic_list1)
        entry = invalid_picture();
    if (detail.type != H264SliceType::i) {
        const bool bipred = detail.type == H264SliceType::b;
        List lists[2]{};
        initial_lists(sps, first, references, order, bipred, lists);
        for (u32 list = 0; list < (bipred ? 2u : 1u); ++list) {
            if (!apply_changes(sps, first, references, detail, list, detail.active[list], &lists[list]))
                return false;
            PictureH264* destination = list ? s.ref_pic_list1 : s.ref_pic_list0;
            for (u32 i = 0; i < lists[list].count && i < 32; ++i)
                if (lists[list].entry[i] != 255)
                    destination[i] = reference_picture(references.pictures[lists[list].entry[i]], surfaces);
        }
    }
    if (detail.weighted) {
        s.luma_log2_weight_denom = detail.luma_log2_weight_denom;
        s.chroma_log2_weight_denom = detail.chroma_log2_weight_denom;
        s.luma_weight_l0_flag = detail.luma_flag[0];
        s.chroma_weight_l0_flag = detail.chroma_flag[0];
        s.luma_weight_l1_flag = detail.luma_flag[1];
        s.chroma_weight_l1_flag = detail.chroma_flag[1];
        for (u32 i = 0; i < 16; ++i) {
            s.luma_weight_l0[i] = detail.luma_weight[0][i];
            s.luma_offset_l0[i] = detail.luma_offset[0][i];
            s.luma_weight_l1[i] = detail.luma_weight[1][i];
            s.luma_offset_l1[i] = detail.luma_offset[1][i];
            for (u32 c = 0; c < 2; ++c) {
                s.chroma_weight_l0[i][c] = detail.chroma_weight[0][i][c];
                s.chroma_offset_l0[i][c] = detail.chroma_offset[0][i][c];
                s.chroma_weight_l1[i][c] = detail.chroma_weight[1][i][c];
                s.chroma_offset_l1[i][c] = detail.chroma_offset[1][i][c];
            }
        }
    }
    (void)pps;
    return true;
}

} // namespace maicord::vaapi
#pragma clang attribute pop
