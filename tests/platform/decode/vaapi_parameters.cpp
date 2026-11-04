// The VA-API parameter and slice builders (src/platform/decode/vaapi/va_*.cpp), pure functions of what the in-house
// parsers and reference trackers decide, checked without a GPU: libva is never loaded here.
//
// Every picture of the encoder fixtures (tools/generate_video_fixtures.py: x264 Baseline, Main with B pictures and
// weighted prediction, High with slices and cropping; x265 Main with B pictures; libaom with tiles) goes through the
// same stream state machines the decoder uses. For each one the VA picture parameters are compared field by field
// with what the DXVA builders of the Windows build (src/platform/decode/*_dxva_parameters.*) produce for the same
// picture: the two APIs describe the same bitstream, so every shared field must agree (the encodings differ where
// the APIs do: scaling lists in raster order, packed CDEF strengths). The slice parameters have no DXVA twin (DXVA
// short slices leave them to the driver); their reference lists, weights, offsets and flags are checked against
// values derived here, apart from the builders, from the parsers' own results.
//
// Real hardware validation of the same buffers is pending: VA-API needs a GPU, and WSL has no /dev/dri.
#include "../../fixtures/video/video_fixtures.hpp"
#include "../../support/hevc_stream_fixture.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/video/av1.hpp"
#include "maicord/media/video/h264_slice_detail.hpp"
#include "maicord/media/video/h264_stream.hpp"
#include "maicord/media/video/hevc_slice_detail.hpp"
#include "maicord/media/video/hevc_stream.hpp"
#include "../../../src/platform/decode/av1_dxva_parameters.hpp"
#include "../../../src/platform/decode/h264_dxva_parameters.hpp"
#include "../../../src/platform/decode/hevc_dxva_parameters.hpp"
#include "../../../src/platform/decode/vaapi/va_parameters.hpp"

namespace maicord {
#include "../../fixtures/video/slice_oracle.inc"

namespace {

using namespace vaapi;

u32 g_failures, g_mismatches, g_reported;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

void same(u64 a, u64 b, const char* field) noexcept {
    if (a == b)
        return;
    ++g_mismatches;
    if (g_reported++ < 24) {
        test_write_text("  mismatch ");
        test_write_text(field);
        test_write_text(": ");
        test_write_u64(a);
        test_write_text(" vs ");
        test_write_u64(b);
        test_write_text("\n");
    }
}
#define SAME(a, b) same(static_cast<u64>(a), static_cast<u64>(b), #a " / " #b)

SurfaceId g_ids[32];

void make_ids() noexcept {
    for (u32 i = 0; i < 32; ++i)
        g_ids[i] = 0x100u + i;
}

// ---- H.264 ------------------------------------------------------------------------------------------------------

constexpr u8 kZigzag4x4[16]{0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
constexpr u8 kZigzag8x8[64]{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                            41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                            30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

H264StreamState g_h264;
H264StreamPicture g_h264_picture;

struct Counts {
    u32 pictures, slices, weighted, inter, bipred, modified;
};

// The expected initial reference list of a P or B slice by repeated selection (8.2.4.2), independent of the builder.
u32 expected_list(const H264ReferenceState& dpb, const H264Sps& sps, const H264Slice& first, const H264PictureOrder& order,
                  u32 list, bool bipred, u8* out) noexcept {
    const i32 modulus = 1 << sps.frame_num_bits;
    const i32 poc = order.top < order.bottom ? order.top : order.bottom;
    bool used[16]{};
    u32 count = 0;
    const auto wrap = [&](u32 i) { return static_cast<i32>(dpb.pictures[i].frame_num) > first.frame_num ? static_cast<i32>(dpb.pictures[i].frame_num) - modulus : static_cast<i32>(dpb.pictures[i].frame_num); };
    const auto order_of = [&](u32 i) { return dpb.pictures[i].top < dpb.pictures[i].bottom ? dpb.pictures[i].top : dpb.pictures[i].bottom; };
    // `pass` selects the group; `key` the order within it.
    const auto take = [&](auto allowed, auto key, bool descending) {
        for (;;) {
            i32 best = -1;
            for (u32 i = 0; i < dpb.count; ++i) {
                if (used[i] || !allowed(i))
                    continue;
                if (best < 0 || (descending ? key(i) > key(static_cast<u32>(best)) : key(i) < key(static_cast<u32>(best))))
                    best = static_cast<i32>(i);
            }
            if (best < 0)
                return;
            used[best] = true;
            out[count++] = static_cast<u8>(best);
        }
    };
    const auto is_short = [&](u32 i) { return !dpb.pictures[i].long_term(); };
    const auto is_long = [&](u32 i) { return dpb.pictures[i].long_term(); };
    const auto long_key = [&](u32 i) { return static_cast<i32>(dpb.pictures[i].long_term_index); };
    if (!bipred) {
        take(is_short, wrap, true);
        take(is_long, long_key, false);
        return count;
    }
    const auto before = [&](u32 i) { return is_short(i) && order_of(i) <= poc; };
    const auto after = [&](u32 i) { return is_short(i) && order_of(i) > poc; };
    if (list == 0) {
        take(before, order_of, true);
        take(after, order_of, false);
    } else {
        take(after, order_of, false);
        take(before, order_of, true);
    }
    take(is_long, long_key, false);
    return count;
}

void h264_compare(const H264StreamPicture& p, const H264ReferenceState& dpb, const H264PictureOrder& order, u8 surface) noexcept {
    DXVA_PicParams_H264 d{};
    DXVA_Qmatrix_H264 dm{};
    h264_dxva_picture_parameters(p.sps, p.pps, p.first, p.all_intra, dpb, order, surface, 0, &d, &dm);
    PictureParameterH264 v;
    h264_picture_parameters(p.sps, p.pps, p.first, dpb, order, surface, g_ids, &v);
    IqMatrixH264 vm;
    h264_iq_matrix(p.pps, &vm);
    SAME(v.picture_width_in_mbs_minus1, d.wFrameWidthInMbsMinus1);
    SAME(v.picture_height_in_mbs_minus1, d.wFrameHeightInMbsMinus1);
    SAME(v.num_ref_frames, d.num_ref_frames);
    SAME(v.curr_pic.picture_id, g_ids[d.CurrPic.Index7Bits]);
    SAME(v.frame_num, d.frame_num);
    SAME(v.curr_pic.frame_idx, d.frame_num);
    SAME(v.pic_fields.bits.reference_pic_flag, d.RefPicFlag);
    SAME(v.seq_fields.bits.chroma_format_idc, d.chroma_format_idc);
    SAME(v.seq_fields.bits.frame_mbs_only_flag, d.frame_mbs_only_flag);
    SAME(v.seq_fields.bits.mb_adaptive_frame_field_flag, d.MbaffFrameFlag);
    SAME(v.seq_fields.bits.min_luma_bi_pred_size_8x8, d.MinLumaBipredSize8x8Flag);
    SAME(v.seq_fields.bits.log2_max_frame_num_minus4, d.log2_max_frame_num_minus4);
    SAME(v.seq_fields.bits.pic_order_cnt_type, d.pic_order_cnt_type);
    SAME(v.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4, d.log2_max_pic_order_cnt_lsb_minus4);
    SAME(v.seq_fields.bits.delta_pic_order_always_zero_flag, d.delta_pic_order_always_zero_flag);
    SAME(v.seq_fields.bits.direct_8x8_inference_flag, d.direct_8x8_inference_flag);
    SAME(v.pic_fields.bits.transform_8x8_mode_flag, d.transform_8x8_mode_flag);
    SAME(v.pic_fields.bits.entropy_coding_mode_flag, d.entropy_coding_mode_flag);
    SAME(v.pic_fields.bits.pic_order_present_flag, d.pic_order_present_flag);
    SAME(v.pic_fields.bits.constrained_intra_pred_flag, d.constrained_intra_pred_flag);
    SAME(v.pic_fields.bits.weighted_pred_flag, d.weighted_pred_flag);
    SAME(v.pic_fields.bits.weighted_bipred_idc, d.weighted_bipred_idc);
    SAME(v.pic_init_qp_minus26, d.pic_init_qp_minus26);
    SAME(v.pic_init_qs_minus26, d.pic_init_qs_minus26);
    SAME(v.chroma_qp_index_offset, d.chroma_qp_index_offset);
    SAME(v.second_chroma_qp_index_offset, d.second_chroma_qp_index_offset);
    SAME(v.pic_fields.bits.deblocking_filter_control_present_flag, d.deblocking_filter_control_present_flag);
    SAME(v.pic_fields.bits.redundant_pic_cnt_present_flag, d.redundant_pic_cnt_present_flag);
    SAME(v.curr_pic.top_field_order_cnt, d.CurrFieldOrderCnt[0]);
    SAME(v.curr_pic.bottom_field_order_cnt, d.CurrFieldOrderCnt[1]);
    SAME(v.pic_fields.bits.field_pic_flag, d.field_pic_flag);
    for (u32 i = 0; i < 16; ++i) {
        const bool used = (d.UsedForReferenceFlags >> (2 * i) & 3u) == 3u;
        SAME(v.reference_frames[i].flags & kPictureH264Invalid, used ? 0 : kPictureH264Invalid);
        if (!used)
            continue;
        SAME(v.reference_frames[i].picture_id, g_ids[d.RefFrameList[i].Index7Bits]);
        SAME((v.reference_frames[i].flags & kPictureH264LongTermReference) != 0, d.RefFrameList[i].AssociatedFlag);
        SAME(v.reference_frames[i].frame_idx, d.FrameNumList[i]);
        SAME(v.reference_frames[i].top_field_order_cnt, d.FieldOrderCntList[i][0]);
        SAME(v.reference_frames[i].bottom_field_order_cnt, d.FieldOrderCntList[i][1]);
    }
    for (u32 list = 0; list < 6; ++list)
        for (u32 k = 0; k < 16; ++k)
            SAME(vm.scaling_list_4x4[list][kZigzag4x4[k]], dm.bScalingLists4x4[list][k]);
    for (u32 list = 0; list < 2; ++list)
        for (u32 k = 0; k < 64; ++k)
            SAME(vm.scaling_list_8x8[list][kZigzag8x8[k]], dm.bScalingLists8x8[list][k]);
}

// libavcodec's reading of the slice headers (slice_oracle.inc) against the in-house detail parsers.
static_assert(sizeof(kH264SliceOracle[0]) / sizeof(u32) == 3 + 112 && sizeof(kHevcSliceOracle[0]) / sizeof(u32) == 3 + 18);
constexpr u32 kH264Rows = sizeof(kH264SliceOracle) / sizeof(kH264SliceOracle[0]);
constexpr u32 kHevcRows = sizeof(kHevcSliceOracle) / sizeof(kHevcSliceOracle[0]);
u32 g_h264_fixture = ~0u, g_h264_row, g_h264_checked;
u32 g_hevc_fixture = ~0u, g_hevc_row, g_hevc_checked;

void h264_oracle(const H264SliceDetail& d, const H264Pps& pps, u32 packet, u32 slice) noexcept {
    if (g_h264_fixture == ~0u)
        return;
    const bool found = g_h264_row < kH264Rows && kH264SliceOracle[g_h264_row][0] == g_h264_fixture &&
                       kH264SliceOracle[g_h264_row][1] == packet && kH264SliceOracle[g_h264_row][2] == slice;
    same(found, 1, "an oracle row for the slice");
    if (!found)
        return;
    const u32* row = kH264SliceOracle[g_h264_row++] + 3;
    ++g_h264_checked;
    same(row[0], d.first_mb, "oracle first_mb");
    same(row[1] % 5u, static_cast<u32>(d.type), "oracle slice_type");
    if (row[2]) {
        same(row[3] + 1u, d.active[0], "oracle num_ref_idx_l0_active");
        if (d.type == H264SliceType::b)
            same(row[4] + 1u, d.active[1], "oracle num_ref_idx_l1_active");
    }
    if (pps.cabac && d.type != H264SliceType::i)
        same(row[5], d.cabac_init_idc, "oracle cabac_init_idc");
    same(static_cast<i32>(row[6]), d.qp_delta, "oracle slice_qp_delta");
    if (pps.deblocking_control) {
        same(row[7], d.disable_deblocking_filter_idc, "oracle disable_deblocking_filter_idc");
        same(static_cast<i32>(row[8]), d.alpha_c0_offset_div2, "oracle alpha");
        same(static_cast<i32>(row[9]), d.beta_offset_div2, "oracle beta");
    }
    same(row[10], d.data_bit_offset, "oracle: where slice data starts");
    for (u32 list = 0; list < 2; ++list) {
        same(row[11 + list], d.change_count[list], "oracle list change count");
        for (u32 k = 0; k < row[11 + list] && k < 8 && k < d.change_count[list]; ++k) {
            same(row[13 + list * 16 + 2 * k], d.changes[list][k].operation, "oracle list change operation");
            same(row[14 + list * 16 + 2 * k], d.changes[list][k].value, "oracle list change value");
        }
    }
    same(row[45], d.weighted, "oracle pred_weight_table present");
    if (!row[45] || !d.weighted)
        return;
    same(row[46], d.luma_log2_weight_denom, "oracle luma denominator");
    same(row[47], d.chroma_log2_weight_denom, "oracle chroma denominator");
    for (u32 side = 0; side < 2; ++side)
        for (u32 e = 0; e < 4 && e < d.active[side]; ++e) {
            const u32* w = row + 48 + side * 32 + e * 8;
            same(d.luma_weight[side][e], w[0] ? static_cast<i16>(w[1]) : static_cast<i16>(1 << row[46]), "oracle luma weight");
            same(d.luma_offset[side][e], w[0] ? static_cast<i16>(w[2]) : 0, "oracle luma offset");
            for (u32 c = 0; c < 2; ++c) {
                same(d.chroma_weight[side][e][c], w[3] ? static_cast<i16>(w[4 + 2 * c]) : static_cast<i16>(1 << row[47]), "oracle chroma weight");
                same(d.chroma_offset[side][e][c], w[3] ? static_cast<i16>(w[5 + 2 * c]) : 0, "oracle chroma offset");
            }
        }
}

void hevc_oracle(const HevcSliceDetail& d, u32 packet, u32 slice) noexcept {
    if (g_hevc_fixture == ~0u)
        return;
    const bool found = g_hevc_row < kHevcRows && kHevcSliceOracle[g_hevc_row][0] == g_hevc_fixture &&
                       kHevcSliceOracle[g_hevc_row][1] == packet && kHevcSliceOracle[g_hevc_row][2] == slice;
    same(found, 1, "an oracle row for the slice segment");
    if (!found)
        return;
    const u32* row = kHevcSliceOracle[g_hevc_row++] + 3;
    ++g_hevc_checked;
    same(row[0], d.address, "oracle slice_segment_address");
    same(row[1], d.slice_type, "oracle slice_type");
    same(row[2], d.sao_luma, "oracle slice_sao_luma_flag");
    same(row[3], d.sao_chroma, "oracle slice_sao_chroma_flag");
    if (row[4]) {
        same(row[5] + 1u, d.active[0], "oracle num_ref_idx_l0_active");
        if (d.slice_type == 0)
            same(row[6] + 1u, d.active[1], "oracle num_ref_idx_l1_active");
    }
    same(row[7], d.mvd_l1_zero, "oracle mvd_l1_zero_flag");
    same(row[8], d.cabac_init, "oracle cabac_init_flag");
    if (d.slice_type == 0 && d.temporal_mvp)
        same(row[9], d.collocated_from_l0, "oracle collocated_from_l0_flag");
    same(row[10], d.collocated_ref_idx, "oracle collocated_ref_idx");
    if (d.slice_type != 2)
        same(row[11], d.five_minus_max_num_merge_cand, "oracle five_minus_max_num_merge_cand");
    same(static_cast<i32>(row[12]), d.qp_delta, "oracle slice_qp_delta");
    same(row[13], d.entry_points, "oracle num_entry_point_offsets");
    same(row[14], d.temporal_mvp, "oracle slice_temporal_mvp_enabled_flag");
    same(row[17] / 8u, d.data_byte_offset, "oracle: where slice data starts");
    same(row[17] % 8u, 0, "oracle: slice data starts on a byte");
}

void h264_slices(const H264StreamPicture& p, const H264ReferenceState& dpb, const H264PictureOrder& order, ConstSlice<u8> bytes,
                 u32 packet, Counts* counts) noexcept {
    for (u32 i = 0; i < p.slice_count; ++i) {
        const ConstSlice<u8> nal{bytes.data + p.slices[i].offset, p.slices[i].size};
        H264Slice header{};
        H264SliceDetail detail{};
        const H264Error parsed = h264_parse_slice(nal, p.sps, p.pps, &header);
        const H264Error detailed = h264_parse_slice_detail(nal, p.sps, p.pps, &detail);
        SAME(parsed, 0);
        SAME(detailed, 0);
        if (parsed != H264Error::none || detailed != H264Error::none)
            return;
        SliceParameterH264 s;
        const bool built = h264_slice_parameters(p.sps, p.pps, p.first, detail, dpb, order, g_ids, nal.count, &s);
        SAME(built, 1);
        if (!built)
            return;
        h264_oracle(detail, p.pps, packet, i);
        ++counts->slices;
        // What the existing slice parser (validated against the decoders) says.
        SAME(detail.first_mb, header.first_mb);
        SAME(s.first_mb_in_slice, header.first_mb);
        SAME(static_cast<u8>(detail.type), static_cast<u8>(header.type));
        SAME(detail.active[0], header.references_l0);
        SAME(detail.active[1], header.references_l1);
        SAME(s.slice_data_size, nal.count);
        SAME(s.slice_data_offset, 0);
        SAME(s.slice_data_flag, 0);
        SAME(s.slice_type, static_cast<u8>(header.type));
        // The header ends inside the NAL unit, and for CABAC where slice_data() starts on a byte boundary.
        same(s.slice_data_bit_offset < nal.count * 8u, true, "slice data inside the NAL unit");
        same(!p.pps.cabac || s.slice_data_bit_offset % 8u == 0, true, "CABAC data byte aligned");
        same(s.slice_data_bit_offset > 8, true, "slice data after the header byte");
        if (detail.type != H264SliceType::i) {
            ++counts->inter;
            counts->bipred += detail.type == H264SliceType::b;
            counts->modified += detail.change_count[0] || detail.change_count[1];
            for (u32 list = 0; list < (detail.type == H264SliceType::b ? 2u : 1u); ++list) {
                const PictureH264* entries = list ? s.ref_pic_list1 : s.ref_pic_list0;
                if (detail.change_count[list])
                    continue; // changed lists are checked by the round trip below
                u8 expected[16];
                const u32 count = expected_list(dpb, p.sps, p.first, order, list, detail.type == H264SliceType::b, expected);
                u32 initial = count;
                // Equal lists swap their first two entries in list 1 of a B slice.
                if (list == 1 && count > 1) {
                    u8 first[16];
                    const u32 n = expected_list(dpb, p.sps, p.first, order, 0, true, first);
                    bool equal = n == count;
                    for (u32 k = 0; equal && k < n; ++k)
                        equal = first[k] == expected[k];
                    if (equal) {
                        const u8 swap = expected[0];
                        expected[0] = expected[1];
                        expected[1] = swap;
                    }
                }
                for (u32 k = 0; k < 32; ++k) {
                    const bool wanted = k < detail.active[list] && k < initial;
                    SAME(entries[k].flags & kPictureH264Invalid, wanted ? 0 : kPictureH264Invalid);
                    if (wanted)
                        SAME(entries[k].picture_id, g_ids[dpb.pictures[expected[k]].surface]);
                }
            }
        }
        if (detail.weighted) {
            ++counts->weighted;
            SAME(s.luma_log2_weight_denom, detail.luma_log2_weight_denom);
            SAME(s.luma_weight_l0_flag, detail.luma_flag[0]);
            same(detail.luma_log2_weight_denom <= 7, true, "luma weight denominator");
        }
        SAME(s.cabac_init_idc, detail.cabac_init_idc);
        SAME(s.slice_qp_delta, detail.qp_delta);
    }
}

Counts h264_stream(const VideoFixture& f) noexcept {
    Counts counts{};
    clear_bytes(&g_h264, sizeof(g_h264));
    H264StreamState* state = &g_h264;
    h264_stream_reset(state);
    h264_stream_set_surfaces(state, 0);
    for (u32 index = 0; index < f.packet_count; ++index) {
        const ConstSlice<u8> bytes{f.packet[index], f.packet_bytes[index]};
        auto scanner = h264_stream_scanner(bytes);
        for (;;) {
            const H264ScanStatus scanned = h264_stream_scan(&state->sets, &scanner, &g_h264_picture);
            if (scanned == H264ScanStatus::end)
                break;
            same(scanned == H264ScanStatus::picture, true, "an intact picture");
            if (scanned != H264ScanStatus::picture)
                continue;
            H264StreamDecode decode{};
            const H264StreamAction action = h264_stream_prepare(state, g_h264_picture, &decode);
            same(action == H264StreamAction::decode, true, "the picture is decodable");
            if (action != H264StreamAction::decode)
                continue;
            H264StreamOutput output{};
            while (h264_stream_pop(state, &output)) {
            }
            if (decode.reinitialize)
                h264_stream_set_surfaces(state, decode.surfaces_needed);
            h264_compare(g_h264_picture, state->references, decode.plan.decode_order, decode.surface);
            h264_slices(g_h264_picture, state->references, decode.plan.decode_order, bytes, index, &counts);
            ++counts.pictures;
            h264_stream_commit(state, g_h264_picture, decode, index * 3000);
            while (h264_stream_pop(state, &output)) {
            }
        }
    }
    return counts;
}

// ---- HEVC -------------------------------------------------------------------------------------------------------

HevcStreamState g_hevc;
HevcStreamPicture g_hevc_picture;

void hevc_compare(const HevcConfiguration& c, const HevcStreamPicture& picture, const HevcReferenceState& previous,
                  const HevcReferencePlan& plan, u8 surface, HevcGpuReferences* mapped) noexcept {
    DXVA_PicParams_HEVC d{};
    DXVA_Qmatrix_HEVC dm{};
    const bool built = hevc_dxva_picture_parameters(c.sps, c.pps, picture.header, picture.intra, previous, plan, surface, 0, &d, &dm);
    const bool mapped_ok = hevc_gpu_reference_map(previous, plan, surface, mapped);
    SAME(built, 1);
    SAME(mapped_ok, 1);
    if (!built || !mapped_ok)
        return;
    PictureParameterHevc v;
    hevc_picture_parameters(c.sps, c.pps, picture.header, picture.intra, plan, *mapped, surface, g_ids, &v);
    SAME(v.pic_width_in_luma_samples, d.PicWidthInMinCbsY << (d.log2_min_luma_coding_block_size_minus3 + 3));
    SAME(v.pic_height_in_luma_samples, d.PicHeightInMinCbsY << (d.log2_min_luma_coding_block_size_minus3 + 3));
    SAME(v.curr_pic.picture_id, g_ids[d.CurrPic.Index7Bits]);
    SAME(v.curr_pic.pic_order_cnt, d.CurrPicOrderCntVal);
    SAME(v.pic_fields.bits.chroma_format_idc, d.chroma_format_idc);
    SAME(v.pic_fields.bits.pcm_enabled_flag, d.pcm_enabled_flag);
    SAME(v.pic_fields.bits.scaling_list_enabled_flag, d.scaling_list_enabled_flag);
    SAME(v.pic_fields.bits.transform_skip_enabled_flag, d.transform_skip_enabled_flag);
    SAME(v.pic_fields.bits.amp_enabled_flag, d.amp_enabled_flag);
    SAME(v.pic_fields.bits.strong_intra_smoothing_enabled_flag, d.strong_intra_smoothing_enabled_flag);
    SAME(v.pic_fields.bits.sign_data_hiding_enabled_flag, d.sign_data_hiding_enabled_flag);
    SAME(v.pic_fields.bits.constrained_intra_pred_flag, d.constrained_intra_pred_flag);
    SAME(v.pic_fields.bits.cu_qp_delta_enabled_flag, d.cu_qp_delta_enabled_flag);
    SAME(v.pic_fields.bits.weighted_pred_flag, d.weighted_pred_flag);
    SAME(v.pic_fields.bits.weighted_bipred_flag, d.weighted_bipred_flag);
    SAME(v.pic_fields.bits.transquant_bypass_enabled_flag, d.transquant_bypass_enabled_flag);
    SAME(v.pic_fields.bits.tiles_enabled_flag, d.tiles_enabled_flag);
    SAME(v.pic_fields.bits.entropy_coding_sync_enabled_flag, d.entropy_coding_sync_enabled_flag);
    SAME(v.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag, d.pps_loop_filter_across_slices_enabled_flag);
    SAME(v.pic_fields.bits.loop_filter_across_tiles_enabled_flag, d.loop_filter_across_tiles_enabled_flag);
    SAME(v.pic_fields.bits.pcm_loop_filter_disabled_flag, d.pcm_loop_filter_disabled_flag);
    SAME(v.sps_max_dec_pic_buffering_minus1, d.sps_max_dec_pic_buffering_minus1);
    SAME(v.pcm_sample_bit_depth_luma_minus1, d.pcm_sample_bit_depth_luma_minus1);
    SAME(v.pcm_sample_bit_depth_chroma_minus1, d.pcm_sample_bit_depth_chroma_minus1);
    SAME(v.log2_min_luma_coding_block_size_minus3, d.log2_min_luma_coding_block_size_minus3);
    SAME(v.log2_diff_max_min_luma_coding_block_size, d.log2_diff_max_min_luma_coding_block_size);
    SAME(v.log2_min_transform_block_size_minus2, d.log2_min_transform_block_size_minus2);
    SAME(v.log2_diff_max_min_transform_block_size, d.log2_diff_max_min_transform_block_size);
    SAME(v.log2_min_pcm_luma_coding_block_size_minus3, d.log2_min_pcm_luma_coding_block_size_minus3);
    SAME(v.log2_diff_max_min_pcm_luma_coding_block_size, d.log2_diff_max_min_pcm_luma_coding_block_size);
    SAME(v.max_transform_hierarchy_depth_intra, d.max_transform_hierarchy_depth_intra);
    SAME(v.max_transform_hierarchy_depth_inter, d.max_transform_hierarchy_depth_inter);
    SAME(v.init_qp_minus26, d.init_qp_minus26);
    SAME(v.diff_cu_qp_delta_depth, d.diff_cu_qp_delta_depth);
    SAME(v.pps_cb_qp_offset, d.pps_cb_qp_offset);
    SAME(v.pps_cr_qp_offset, d.pps_cr_qp_offset);
    SAME(v.log2_parallel_merge_level_minus2, d.log2_parallel_merge_level_minus2);
    SAME(v.num_tile_columns_minus1, d.num_tile_columns_minus1);
    SAME(v.num_tile_rows_minus1, d.num_tile_rows_minus1);
    for (u32 i = 0; i < 19; ++i)
        SAME(v.column_width_minus1[i], d.column_width_minus1[i]);
    for (u32 i = 0; i < 21; ++i)
        SAME(v.row_height_minus1[i], d.row_height_minus1[i]);
    SAME(v.slice_parsing_fields.bits.lists_modification_present_flag, d.lists_modification_present_flag);
    SAME(v.slice_parsing_fields.bits.long_term_ref_pics_present_flag, d.long_term_ref_pics_present_flag);
    SAME(v.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag, d.sps_temporal_mvp_enabled_flag);
    SAME(v.slice_parsing_fields.bits.cabac_init_present_flag, d.cabac_init_present_flag);
    SAME(v.slice_parsing_fields.bits.output_flag_present_flag, d.output_flag_present_flag);
    SAME(v.slice_parsing_fields.bits.dependent_slice_segments_enabled_flag, d.dependent_slice_segments_enabled_flag);
    SAME(v.slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag, d.pps_slice_chroma_qp_offsets_present_flag);
    SAME(v.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag, d.sample_adaptive_offset_enabled_flag);
    SAME(v.slice_parsing_fields.bits.deblocking_filter_override_enabled_flag, d.deblocking_filter_override_enabled_flag);
    SAME(v.slice_parsing_fields.bits.pps_disable_deblocking_filter_flag, d.pps_deblocking_filter_disabled_flag);
    SAME(v.slice_parsing_fields.bits.slice_segment_header_extension_present_flag, d.slice_segment_header_extension_present_flag);
    SAME(v.slice_parsing_fields.bits.rap_pic_flag, d.IrapPicFlag);
    SAME(v.slice_parsing_fields.bits.idr_pic_flag, d.IdrPicFlag);
    SAME(v.slice_parsing_fields.bits.intra_pic_flag, d.IntraPicFlag);
    SAME(v.log2_max_pic_order_cnt_lsb_minus4, d.log2_max_pic_order_cnt_lsb_minus4);
    SAME(v.num_short_term_ref_pic_sets, d.num_short_term_ref_pic_sets);
    SAME(v.num_long_term_ref_pic_sps, d.num_long_term_ref_pics_sps);
    SAME(v.num_ref_idx_l0_default_active_minus1, d.num_ref_idx_l0_default_active_minus1);
    SAME(v.num_ref_idx_l1_default_active_minus1, d.num_ref_idx_l1_default_active_minus1);
    SAME(v.pps_beta_offset_div2, d.pps_beta_offset_div2);
    SAME(v.pps_tc_offset_div2, d.pps_tc_offset_div2);
    SAME(v.num_extra_slice_header_bits, d.num_extra_slice_header_bits);
    SAME(v.st_rps_bits, d.wNumBitsForShortTermRPSInSlice);
    for (u32 i = 0; i < 15; ++i) {
        const bool used = d.RefPicList[i].bPicEntry != 0xff;
        SAME(v.reference_frames[i].flags & kPictureHevcInvalid, used ? 0 : kPictureHevcInvalid);
        if (!used)
            continue;
        SAME(v.reference_frames[i].picture_id, g_ids[d.RefPicList[i].Index7Bits]);
        SAME((v.reference_frames[i].flags & kPictureHevcLongTermReference) != 0, d.RefPicList[i].AssociatedFlag);
        SAME(v.reference_frames[i].pic_order_cnt, d.PicOrderCntValList[i]);
    }
    for (u32 i = 0; i < 8; ++i) {
        if (d.RefPicSetStCurrBefore[i] != 0xff)
            SAME(v.reference_frames[d.RefPicSetStCurrBefore[i]].flags & kPictureHevcRpsStCurrBefore, kPictureHevcRpsStCurrBefore);
        if (d.RefPicSetStCurrAfter[i] != 0xff)
            SAME(v.reference_frames[d.RefPicSetStCurrAfter[i]].flags & kPictureHevcRpsStCurrAfter, kPictureHevcRpsStCurrAfter);
        if (d.RefPicSetLtCurr[i] != 0xff)
            SAME(v.reference_frames[d.RefPicSetLtCurr[i]].flags & kPictureHevcRpsLtCurr, kPictureHevcRpsLtCurr);
    }
    if (c.sps.scaling_enabled) {
        IqMatrixHevc vm;
        hevc_iq_matrix(c.pps, &vm);
        for (u32 i = 0; i < 6; ++i)
            for (u32 k = 0; k < 16; ++k)
                SAME(vm.scaling_list_4x4[i][k], dm.ucScalingLists0[i][k]);
        for (u32 i = 0; i < 6; ++i)
            for (u32 k = 0; k < 64; ++k) {
                SAME(vm.scaling_list_8x8[i][k], dm.ucScalingLists1[i][k]);
                SAME(vm.scaling_list_16x16[i][k], dm.ucScalingLists2[i][k]);
            }
        for (u32 i = 0; i < 2; ++i)
            for (u32 k = 0; k < 64; ++k)
                SAME(vm.scaling_list_32x32[i][k], dm.ucScalingLists3[i][k]);
        for (u32 i = 0; i < 6; ++i)
            SAME(vm.scaling_list_dc_16x16[i], dm.ucScalingListDCCoefSizeID2[i]);
        for (u32 i = 0; i < 2; ++i)
            SAME(vm.scaling_list_dc_32x32[i], dm.ucScalingListDCCoefSizeID3[i]);
    }
}

void hevc_slices(const HevcConfiguration& c, const HevcStreamPicture& picture, const HevcGpuReferences& mapped,
                 ConstSlice<u8> bytes, u32 packet, Counts* counts) noexcept {
    HevcSliceDetail previous{}, detail{};
    HevcPictureHeader header = picture.header;
    for (u32 i = 0; i < picture.slice_count; ++i) {
        const ConstSlice<u8> nal{bytes.data + picture.slices[i].offset, picture.slices[i].size};
        const HevcNal unit{nal, HevcError::none, static_cast<u8>((nal[0] >> 1) & 63u), static_cast<u8>((nal[1] & 7u) - 1u)};
        HevcSlice parsed{};
        const HevcError existing = hevc_parse_slice(unit, c, i ? &header : nullptr, &parsed);
        const HevcError detailed = hevc_parse_slice_detail(unit, c, i ? &previous : nullptr, &detail);
        SAME(existing, 0);
        SAME(detailed, 0);
        if (existing != HevcError::none || detailed != HevcError::none)
            return;
        SliceParameterHevc s;
        hevc_slice_parameters(detail, mapped, i + 1 == picture.slice_count, nal.count, &s);
        hevc_oracle(detail, packet, i);
        ++counts->slices;
        SAME(detail.address, parsed.address);
        SAME(detail.first, parsed.first);
        SAME(detail.dependent, parsed.dependent);
        SAME(detail.slice_type, parsed.header.slice_type);
        SAME(s.slice_segment_address, parsed.address);
        SAME(s.long_slice_flags.fields.slice_type, parsed.header.slice_type);
        SAME(s.slice_data_size, nal.count);
        same(s.slice_data_byte_offset > 2 && s.slice_data_byte_offset < nal.count, true, "slice data inside the NAL unit");
        SAME(s.long_slice_flags.fields.last_slice_of_pic, i + 1 == picture.slice_count);
        if (detail.slice_type != 2) {
            ++counts->inter;
            counts->bipred += detail.slice_type == 0;
            counts->modified += detail.modified[0] || detail.modified[1];
            // Lists name entries of ReferenceFrames that exist.
            for (u32 list = 0; list < 2; ++list)
                for (u32 k = 0; k < detail.active[list]; ++k) {
                    const u8 entry = s.ref_pic_list[list][k];
                    same(entry < mapped.count, true, "a list entry names a reference picture");
                }
        }
        if (detail.weighted)
            ++counts->weighted;
        previous = detail;
        if (!detail.dependent)
            header = parsed.header;
    }
}

Counts hevc_units(const ConstSlice<u8>* units, u32 unit_count) noexcept {
    Counts counts{};
    clear_bytes(&g_hevc, sizeof(g_hevc));
    HevcStreamState* state = &g_hevc;
    hevc_stream_reset(state);
    hevc_stream_set_surfaces(state, 0);
    for (u32 index = 0; index < unit_count; ++index) {
        const ConstSlice<u8> bytes = units[index];
        auto scanner = hevc_stream_scanner(bytes);
        for (;;) {
            const HevcScanStatus scanned = hevc_stream_scan(&state->sets, &scanner, &g_hevc_picture);
            if (scanned == HevcScanStatus::end)
                break;
            same(scanned == HevcScanStatus::picture, true, "an intact picture");
            if (scanned != HevcScanStatus::picture)
                continue;
            HevcStreamDecode decode{};
            const HevcStreamAction action = hevc_stream_prepare(state, g_hevc_picture, &decode);
            if (action == HevcStreamAction::skip)
                continue;
            same(action == HevcStreamAction::decode, true, "the picture is decodable");
            if (action != HevcStreamAction::decode)
                continue;
            H264StreamOutput output{};
            while (hevc_stream_pop(state, &output)) {
            }
            if (decode.reinitialize)
                hevc_stream_set_surfaces(state, decode.surfaces_needed);
            const HevcReferenceState none{};
            HevcGpuReferences mapped{};
            hevc_compare(state->sets.active, g_hevc_picture, decode.restart ? none : state->references, decode.plan,
                         decode.surface, &mapped);
            hevc_slices(state->sets.active, g_hevc_picture, mapped, bytes, index, &counts);
            ++counts.pictures;
            hevc_stream_commit(state, g_hevc_picture, decode, index * 3000);
            while (hevc_stream_pop(state, &output)) {
            }
        }
    }
    return counts;
}

Counts hevc_stream(const VideoFixture& f) noexcept {
    ConstSlice<u8> units[kVideoFixtureMaxPackets];
    for (u32 i = 0; i < f.packet_count; ++i)
        units[i] = {f.packet[i], f.packet_bytes[i]};
    return hevc_units(units, f.packet_count);
}

// Pictures of the synthetic generator with several slices each (PCM intra, skipped inter): what the encoder fixture
// lacks. Slice addresses, the last-slice flag and the data offsets come out of the same builders.
Counts hevc_synthetic_slices() noexcept {
    using namespace hevc_fixture;
    static u8 storage[8][8192];
    static Bits bits_storage;
    static u8 bits_buffer[8192];
    bits_storage = {bits_buffer, sizeof(bits_buffer), 0, true};
    const Sps sps{0, 64, 64, 0, 4, 8, 2};
    const Pps pps{0, 0};
    ConstSlice<u8> units[5];
    u32 count = 0;
    for (u32 picture = 0; picture < 5; ++picture) {
        Output out{storage[picture], sizeof(storage[picture]), 0, true};
        if (picture == 0)
            write_idr(&out, &bits_storage, sps, pps, 1, 4);
        else
            write_picture(&out, &bits_storage, sps, pps, kTrailR, static_cast<u16>(picture), picture + 1u, 6 + picture);
        units[count++] = {out.data, out.count};
    }
    return hevc_units(units, count);
}

// ---- AV1 --------------------------------------------------------------------------------------------------------

Av1Tile g_tiles[128];

void av1_compare(const Av1SequenceHeader& s, const Av1FrameHeader& h, const u8 (&slots)[kAv1RefFrames],
                 const Av1References& references, u8 surface) noexcept {
    DXVA_PicParams_AV1 d{};
    av1_dxva_picture_parameters(s, h, slots, references, surface, &d);
    PictureParameterAv1 v;
    av1_picture_parameters(s, h, slots, g_ids, surface, &v);
    SAME(v.frame_width_minus1 + 1u, d.width);
    SAME(v.frame_height_minus1 + 1u, d.height);
    SAME(v.current_frame, g_ids[d.CurrPicTextureIndex]);
    SAME(v.superres_scale_denominator, d.superres_denom);
    SAME(v.bit_depth_idx, d.bitdepth == 8 ? 0 : d.bitdepth == 10 ? 1 : 2);
    SAME(v.profile, d.seq_profile);
    SAME(v.tile_cols, d.tiles.cols);
    SAME(v.tile_rows, d.tiles.rows);
    SAME(v.context_update_tile_id, d.tiles.context_update_id);
    for (u32 i = 0; i + 1u < d.tiles.cols && i < 63; ++i)
        SAME(v.width_in_sbs_minus_1[i] + 1u, d.tiles.widths[i]);
    for (u32 i = 0; i + 1u < d.tiles.rows && i < 63; ++i)
        SAME(v.height_in_sbs_minus_1[i] + 1u, d.tiles.heights[i]);
    SAME(v.seq_info_fields.fields.use_128x128_superblock, d.coding.use_128x128_superblock);
    SAME(v.seq_info_fields.fields.enable_intra_edge_filter, d.coding.intra_edge_filter);
    SAME(v.seq_info_fields.fields.enable_interintra_compound, d.coding.interintra_compound);
    SAME(v.seq_info_fields.fields.enable_masked_compound, d.coding.masked_compound);
    SAME(v.pic_info_fields.bits.allow_warped_motion, d.coding.warped_motion);
    SAME(v.seq_info_fields.fields.enable_dual_filter, d.coding.dual_filter);
    SAME(v.seq_info_fields.fields.enable_jnt_comp, d.coding.jnt_comp);
    SAME(v.pic_info_fields.bits.allow_screen_content_tools, d.coding.screen_content_tools);
    SAME(v.pic_info_fields.bits.force_integer_mv, d.coding.integer_mv);
    SAME(v.seq_info_fields.fields.enable_cdef, d.coding.cdef);
    SAME(v.pic_info_fields.bits.allow_intrabc, d.coding.intrabc);
    SAME(v.pic_info_fields.bits.allow_high_precision_mv, d.coding.high_precision_mv);
    SAME(v.pic_info_fields.bits.is_motion_mode_switchable, d.coding.switchable_motion_mode);
    SAME(v.seq_info_fields.fields.enable_filter_intra, d.coding.filter_intra);
    SAME(v.pic_info_fields.bits.disable_frame_end_update_cdf, d.coding.disable_frame_end_update_cdf);
    SAME(v.pic_info_fields.bits.disable_cdf_update, d.coding.disable_cdf_update);
    SAME(v.mode_control_fields.bits.reference_select, d.coding.reference_mode);
    SAME(v.mode_control_fields.bits.skip_mode_present, d.coding.skip_mode);
    SAME(v.mode_control_fields.bits.reduced_tx_set_used, d.coding.reduced_tx_set);
    SAME(v.pic_info_fields.bits.use_superres, d.coding.superres);
    SAME(v.mode_control_fields.bits.tx_mode, d.coding.tx_mode);
    SAME(v.pic_info_fields.bits.use_ref_frame_mvs, d.coding.use_ref_frame_mvs);
    SAME(v.pic_info_fields.bits.frame_type, d.format.frame_type);
    SAME(v.pic_info_fields.bits.show_frame, d.format.show_frame);
    SAME(v.pic_info_fields.bits.showable_frame, d.format.showable_frame);
    SAME(v.seq_info_fields.fields.subsampling_x, d.format.subsampling_x);
    SAME(v.seq_info_fields.fields.subsampling_y, d.format.subsampling_y);
    SAME(v.seq_info_fields.fields.mono_chrome, d.format.mono_chrome);
    SAME(v.primary_ref_frame, d.primary_ref_frame);
    SAME(v.order_hint, d.order_hint);
    SAME(v.order_hint_bits_minus_1 + (d.order_hint_bits ? 1 : 0), d.order_hint_bits);
    for (u32 i = 0; i < 7; ++i)
        if (d.frame_refs[i].Index != 0xff) {
            SAME(v.ref_frame_idx[i], d.frame_refs[i].Index);
            SAME(v.wm[i].wmtype, d.frame_refs[i].wmtype);
            SAME(v.wm[i].invalid, d.frame_refs[i].wminvalid);
            for (u32 k = 0; k < 6; ++k)
                SAME(v.wm[i].wmmat[k], d.frame_refs[i].wmmat[k]);
        }
    for (u32 i = 0; i < 8; ++i)
        SAME(v.ref_frame_map[i], d.RefFrameMapTextureIndex[i] == 255 ? kInvalidId : g_ids[d.RefFrameMapTextureIndex[i]]);
    SAME(v.filter_level[0], d.loop_filter.filter_level[0]);
    SAME(v.filter_level[1], d.loop_filter.filter_level[1]);
    SAME(v.filter_level_u, d.loop_filter.filter_level_u);
    SAME(v.filter_level_v, d.loop_filter.filter_level_v);
    SAME(v.loop_filter_info_fields.bits.sharpness_level, d.loop_filter.sharpness_level);
    SAME(v.loop_filter_info_fields.bits.mode_ref_delta_enabled, d.loop_filter.mode_ref_delta_enabled);
    SAME(v.loop_filter_info_fields.bits.mode_ref_delta_update, d.loop_filter.mode_ref_delta_update);
    SAME(v.mode_control_fields.bits.delta_lf_multi, d.loop_filter.delta_lf_multi);
    SAME(v.mode_control_fields.bits.delta_lf_present_flag, d.loop_filter.delta_lf_present);
    SAME(v.mode_control_fields.bits.log2_delta_lf_res, d.loop_filter.delta_lf_res);
    for (u32 i = 0; i < 8; ++i)
        SAME(v.ref_deltas[i], d.loop_filter.ref_deltas[i]);
    for (u32 i = 0; i < 2; ++i)
        SAME(v.mode_deltas[i], d.loop_filter.mode_deltas[i]);
    SAME(v.loop_restoration_fields.bits.yframe_restoration_type, d.loop_filter.frame_restoration_type[0]);
    SAME(v.loop_restoration_fields.bits.cbframe_restoration_type, d.loop_filter.frame_restoration_type[1]);
    SAME(v.loop_restoration_fields.bits.crframe_restoration_type, d.loop_filter.frame_restoration_type[2]);
    if (d.loop_filter.frame_restoration_type[0] || d.loop_filter.frame_restoration_type[1] || d.loop_filter.frame_restoration_type[2]) {
        SAME(v.loop_restoration_fields.bits.lr_unit_shift + 6u, d.loop_filter.log2_restoration_unit_size[0]);
        SAME(v.loop_restoration_fields.bits.lr_unit_shift + 6u - v.loop_restoration_fields.bits.lr_uv_shift,
             d.loop_filter.log2_restoration_unit_size[1]);
    }
    SAME(v.mode_control_fields.bits.delta_q_present_flag, d.quantization.delta_q_present);
    SAME(v.mode_control_fields.bits.log2_delta_q_res, d.quantization.delta_q_res);
    SAME(v.base_qindex, d.quantization.base_qindex);
    SAME(v.y_dc_delta_q, d.quantization.y_dc_delta_q);
    SAME(v.u_dc_delta_q, d.quantization.u_dc_delta_q);
    SAME(v.v_dc_delta_q, d.quantization.v_dc_delta_q);
    SAME(v.u_ac_delta_q, d.quantization.u_ac_delta_q);
    SAME(v.v_ac_delta_q, d.quantization.v_ac_delta_q);
    if (d.quantization.qm_y != 0xff) {
        SAME(v.qmatrix_fields.bits.using_qmatrix, 1);
        SAME(v.qmatrix_fields.bits.qm_y, d.quantization.qm_y);
        SAME(v.qmatrix_fields.bits.qm_u, d.quantization.qm_u);
        SAME(v.qmatrix_fields.bits.qm_v, d.quantization.qm_v);
    } else {
        SAME(v.qmatrix_fields.bits.using_qmatrix, 0);
    }
    SAME(v.cdef_damping_minus_3, d.cdef.damping);
    SAME(v.cdef_bits, d.cdef.bits);
    for (u32 i = 0; i < 8; ++i) {
        SAME(v.cdef_y_strengths[i] >> 2, d.cdef.y_strengths[i].primary);
        SAME(v.cdef_y_strengths[i] & 3, d.cdef.y_strengths[i].secondary);
        SAME(v.cdef_uv_strengths[i] >> 2, d.cdef.uv_strengths[i].primary);
        SAME(v.cdef_uv_strengths[i] & 3, d.cdef.uv_strengths[i].secondary);
    }
    SAME(v.interp_filter, d.interp_filter);
    SAME(v.seg_info.segment_info_fields.bits.enabled, d.segmentation.enabled);
    SAME(v.seg_info.segment_info_fields.bits.update_map, d.segmentation.update_map);
    SAME(v.seg_info.segment_info_fields.bits.update_data, d.segmentation.update_data);
    SAME(v.seg_info.segment_info_fields.bits.temporal_update, d.segmentation.temporal_update);
    for (u32 i = 0; i < 8; ++i) {
        SAME(v.seg_info.feature_mask[i], d.segmentation.feature_mask[i].mask);
        for (u32 k = 0; k < 8; ++k)
            SAME(v.seg_info.feature_data[i][k], d.segmentation.feature_data[i][k]);
    }
    SAME(v.film_grain_info.film_grain_info_fields.bits.apply_grain, 0);
}

u32 av1_stream(const VideoFixture& f, u32* tiles) noexcept {
    Av1SequenceHeader sequence{};
    Av1References references{};
    Av1FrameHeader header{};
    u8 slots[kAv1RefFrames];
    for (u8& slot : slots)
        slot = 255;
    u32 frames = 0, surface = 0;
    bool have_sequence = false;
    for (u32 index = 0; index < f.packet_count; ++index) {
        const ConstSlice<u8> unit{f.packet[index], f.packet_bytes[index]};
        u32 offset = 0, tile_count = 0;
        bool open = false;
        while (offset < unit.count) {
            Av1Obu obu{};
            if (av1_next_obu(unit, &offset, &obu) != Av1Status::ok) {
                same(0, 1, "a valid OBU");
                break;
            }
            if (obu.type == kAv1ObuSequenceHeader) {
                have_sequence = av1_parse_sequence_header(obu.payload, &sequence) == Av1Status::ok;
                continue;
            }
            const bool is_header = obu.type == kAv1ObuFrameHeader || obu.type == kAv1ObuFrame;
            if (!have_sequence || (!is_header && obu.type != kAv1ObuTileGroup))
                continue;
            ConstSlice<u8> group = obu.payload;
            if (is_header) {
                if (av1_parse_frame_header(sequence, references, obu.payload, obu.temporal_id, obu.spatial_id, &header) != Av1Status::ok) {
                    same(0, 1, "a valid frame header");
                    break;
                }
                if (header.show_existing_frame)
                    continue;
                open = true;
                tile_count = 0;
                if (obu.type == kAv1ObuFrameHeader)
                    continue;
                group = {obu.payload.data + header.header_bytes, obu.payload.count - header.header_bytes};
            } else if (!open) {
                continue;
            }
            u32 start = 0, end = 0;
            if (av1_parse_tile_group(header, group, {g_tiles + tile_count, 128 - tile_count}, &start, &end) != Av1Status::ok ||
                start != tile_count) {
                same(0, 1, "a valid tile group");
                break;
            }
            for (u32 i = start; i <= end; ++i)
                g_tiles[i].offset += static_cast<u32>(group.data - unit.data);
            tile_count = end + 1;
            if (tile_count != u32{header.tile_columns} * header.tile_rows)
                continue;
            av1_compare(sequence, header, slots, references, static_cast<u8>(surface % 6));
            // The tiles: sizes and positions are what the parser found.
            u32 lowest = g_tiles[0].offset;
            for (u32 i = 0; i < tile_count; ++i)
                lowest = g_tiles[i].offset < lowest ? g_tiles[i].offset : lowest;
            for (u32 i = 0; i < tile_count; ++i) {
                SliceParameterAv1 s;
                av1_tile_parameters(g_tiles[i], g_tiles[i].offset - lowest, 0, tile_count - 1, &s);
                SAME(s.slice_data_size, g_tiles[i].size);
                SAME(s.slice_data_offset, g_tiles[i].offset - lowest);
                SAME(s.tile_row * u32{header.tile_columns} + s.tile_column, i);
                same(g_tiles[i].offset + g_tiles[i].size <= unit.count, true, "a tile inside the temporal unit");
            }
            *tiles += tile_count;
            av1_update_references(references, header);
            for (u32 i = 0; i < kAv1RefFrames; ++i)
                if (header.refresh_frame_flags >> i & 1)
                    slots[i] = static_cast<u8>(surface % 6);
            ++surface;
            ++frames;
            open = false;
        }
    }
    return frames;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_vaapi_parameters_entry) {
    using namespace maicord;
    make_ids();
    VideoFixture baseline, bframes, weighted, high, hevc, av1_basic, av1_tiles;
    check(video_fixture_parse("h264_baseline", kVideoH264Baseline, sizeof(kVideoH264Baseline), &baseline) &&
              video_fixture_parse("h264_main_bframes", kVideoH264MainBFrames, sizeof(kVideoH264MainBFrames), &bframes) &&
              video_fixture_parse("h264_main_weighted", kVideoH264MainWeighted, sizeof(kVideoH264MainWeighted), &weighted) &&
              video_fixture_parse("h264_high_crop_slices", kVideoH264HighCropSlices, sizeof(kVideoH264HighCropSlices), &high) &&
              video_fixture_parse("hevc_main", kVideoHevcMain, sizeof(kVideoHevcMain), &hevc) &&
              video_fixture_parse("av1_basic", kVideoAv1Basic, sizeof(kVideoAv1Basic), &av1_basic) &&
              video_fixture_parse("av1_tiles", kVideoAv1Tiles, sizeof(kVideoAv1Tiles), &av1_tiles),
          "video fixtures parse");
    const VideoFixture* const h264s[]{&baseline, &bframes, &weighted, &high};
    Counts total{};
    u32 h264_id = 0;
    for (const VideoFixture* f : h264s) {
        const u32 before = g_mismatches;
        g_h264_fixture = h264_id++;
        const Counts counts = h264_stream(*f);
        test_write_text(f->name);
        test_write_metric(": pictures ", counts.pictures);
        test_write_metric("  slices ", counts.slices);
        test_write_metric("  inter slices ", counts.inter);
        test_write_metric("  B slices ", counts.bipred);
        test_write_metric("  weighted slices ", counts.weighted);
        test_write_metric("  mismatches ", g_mismatches - before);
        check(counts.pictures == f->packet_count, "H.264: every picture is described");
        total.slices += counts.slices;
        total.bipred += counts.bipred;
        total.weighted += counts.weighted;
        total.inter += counts.inter;
    }
    g_h264_fixture = ~0u;
    check(total.bipred > 0, "H.264: the fixtures have B slices (two reference lists)");
    check(total.weighted > 0, "H.264: the fixtures have explicitly weighted slices");
    check(total.slices > bframes.packet_count + high.packet_count, "H.264: multi-slice pictures are described slice by slice");
    {
        const u32 before = g_mismatches;
        g_hevc_fixture = 0;
        const Counts counts = hevc_stream(hevc);
        g_hevc_fixture = ~0u;
        test_write_text(hevc.name);
        test_write_metric(": pictures ", counts.pictures);
        test_write_metric("  slice segments ", counts.slices);
        test_write_metric("  inter slices ", counts.inter);
        test_write_metric("  B slices ", counts.bipred);
        test_write_metric("  mismatches ", g_mismatches - before);
        check(counts.pictures == hevc.packet_count && counts.slices == counts.pictures, "HEVC: every picture is described");
        check(counts.inter > 0, "HEVC: inter slices with reference lists");
    }
    {
        const u32 before = g_mismatches;
        const Counts counts = hevc_synthetic_slices();
        test_write_text("synthetic HEVC");
        test_write_metric(": pictures ", counts.pictures);
        test_write_metric("  slice segments ", counts.slices);
        test_write_metric("  mismatches ", g_mismatches - before);
        check(counts.pictures == 5 && counts.slices == 4 + 3 + 2 + 2 + 2, "HEVC: multi-slice pictures, slice by slice");
    }
    const VideoFixture* const av1s[]{&av1_basic, &av1_tiles};
    for (const VideoFixture* f : av1s) {
        const u32 before = g_mismatches;
        u32 tiles = 0;
        const u32 frames = av1_stream(*f, &tiles);
        test_write_text(f->name);
        test_write_metric(": frames ", frames);
        test_write_metric("  tiles ", tiles);
        test_write_metric("  mismatches ", g_mismatches - before);
        check(frames >= f->packet_count, "AV1: every frame is described");
        check(f != &av1_tiles || tiles >= 2 * frames, "AV1: the tiled stream has several tiles per frame");
    }
    check(g_h264_checked == kH264Rows && g_hevc_checked == kHevcRows,
          "every slice header the oracle read was compared with the in-house parser");
    check(!g_mismatches, "every VA parameter agrees with the DXVA builder's or the parsers' value");
    test_exit(g_failures, "maicord_vaapi_parameters");
}
