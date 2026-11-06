#include "maicord/media/video/h264_slice_detail.hpp"
#include "h264_bits.hpp"
// Header parsing, once per slice and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

void list_changes(H264Bits* b, u32 max_frame_num, H264SliceDetail* detail, u32 list) noexcept {
    if (!b->bit())
        return;
    for (u32 count = 0; b->error == H264Error::none; ++count) {
        const u32 operation = b->ue(3);
        if (operation == 3) {
            if (!count)
                b->error = H264Error::malformed;
            return;
        }
        if (count == kH264MaximumListChanges - 1) {
            b->error = H264Error::unsupported;
            return;
        }
        const u32 value = b->ue(operation == 2 ? 15u : max_frame_num - 1u);
        detail->changes[list][count] = {static_cast<u8>(operation), value};
        detail->change_count[list] = static_cast<u8>(count + 1);
    }
}

void weights(H264Bits* b, H264SliceDetail* d, u32 list, u32 count) noexcept {
    const i16 luma_default = static_cast<i16>(1 << d->luma_log2_weight_denom);
    const i16 chroma_default = static_cast<i16>(1 << d->chroma_log2_weight_denom);
    for (u32 i = 0; i < count && b->error == H264Error::none; ++i) {
        d->luma_weight[list][i] = luma_default;
        d->luma_offset[list][i] = 0;
        d->chroma_weight[list][i][0] = d->chroma_weight[list][i][1] = chroma_default;
        d->chroma_offset[list][i][0] = d->chroma_offset[list][i][1] = 0;
        if (b->bit()) {
            d->luma_flag[list] = true;
            d->luma_weight[list][i] = static_cast<i16>(b->se(-128, 127));
            d->luma_offset[list][i] = static_cast<i16>(b->se(-128, 127));
        }
        if (b->bit()) {
            d->chroma_flag[list] = true;
            for (u32 component = 0; component < 2; ++component) {
                d->chroma_weight[list][i][component] = static_cast<i16>(b->se(-128, 127));
                d->chroma_offset[list][i][component] = static_cast<i16>(b->se(-128, 127));
            }
        }
    }
}

// dec_ref_pic_marking(): only its length matters here.
void skip_marking(H264Bits* b, u32 max_frame_num, bool idr) noexcept {
    if (idr) {
        b->bit();
        b->bit();
        return;
    }
    if (!b->bit())
        return;
    u32 operations = 0;
    while (b->error == H264Error::none) {
        const u32 operation = b->ue(6);
        if (!operation)
            return;
        if (++operations > 32) {
            b->error = H264Error::unsupported;
            return;
        }
        if (operation == 1 || operation == 3)
            b->ue(max_frame_num - 1u);
        if (operation == 2 || operation == 3 || operation == 6)
            b->ue(15);
        if (operation == 4)
            b->ue(16);
    }
}

// Emulation prevention bytes among the first `bytes` of nal (the bytes the slice header takes in the stream).
u32 h264_escape_bytes(ConstSlice<u8> nal, u32 bytes) noexcept {
    u32 escapes = 0, zeroes = 0;
    for (u32 i = 0; i < bytes && i < nal.count; ++i) {
        const u8 value = nal.data[i];
        if (zeroes >= 2 && value == 3) {
            ++escapes;
            zeroes = 0;
            continue;
        }
        zeroes = value ? 0u : zeroes + 1u;
    }
    return escapes;
}

} // namespace

H264Error h264_parse_slice_detail(ConstSlice<u8> nal, const H264Sps& sps, const H264Pps& pps,
                                  H264SliceDetail* output) noexcept {
    if (!output || !nal.data || nal.count < 2 || sps.id != pps.sps_id)
        return H264Error::invalid_input;
    const u8 nal_type = nal.data[0] & 31u;
    if (nal.data[0] & 0x80u)
        return H264Error::malformed;
    if (nal_type != 1 && nal_type != 5)
        return H264Error::unsupported;
    const bool idr = nal_type == 5;
    const bool reference = ((nal.data[0] >> 5u) & 3u) != 0;
    if (idr && !reference)
        return H264Error::malformed;
    H264Bits b{{nal.data + 1, nal.count - 1}};
    H264SliceDetail d{};
    const u32 max_frame_num = 1u << sps.frame_num_bits;
    const u32 macroblocks = (sps.coded_width / 16u) * (sps.coded_height / 16u);
    d.first_mb = b.ue(macroblocks ? macroblocks - 1u : 0);
    const u32 type = b.ue(9);
    if (b.error != H264Error::none)
        return b.error;
    if (type % 5u > 2)
        return H264Error::unsupported;
    d.type = static_cast<H264SliceType>(type % 5u);
    if (b.ue(255) != pps.id)
        return H264Error::malformed;
    b.read(sps.frame_num_bits);
    if (!sps.frame_only && b.bit())
        return H264Error::unsupported;
    if (idr)
        b.ue(65'535);
    if (sps.poc_type == 0) {
        b.read(sps.poc_bits);
        if (pps.bottom_poc_present)
            b.se();
    } else if (sps.poc_type == 1 && !sps.delta_always_zero) {
        b.se();
        if (pps.bottom_poc_present)
            b.se();
    }
    if (pps.redundant_count && b.ue(127))
        return H264Error::unsupported;
    const bool inter = d.type != H264SliceType::i;
    const bool bipred = d.type == H264SliceType::b;
    if (bipred)
        d.direct_spatial_mv_pred = b.bit();
    if (inter) {
        d.active[0] = pps.references_l0;
        d.active[1] = bipred ? pps.references_l1 : 0;
        if (b.bit()) {
            d.active[0] = static_cast<u8>(b.ue(15) + 1u);
            if (bipred)
                d.active[1] = static_cast<u8>(b.ue(15) + 1u);
        }
        if (!d.active[0] || d.active[0] > 16 || (bipred && (!d.active[1] || d.active[1] > 16)))
            return H264Error::malformed;
        list_changes(&b, max_frame_num, &d, 0);
        if (bipred)
            list_changes(&b, max_frame_num, &d, 1);
    }
    if ((d.type == H264SliceType::p && pps.weighted_pred) || (bipred && pps.weighted_bipred == 1)) {
        d.weighted = true;
        d.luma_log2_weight_denom = static_cast<u8>(b.ue(7));
        d.chroma_log2_weight_denom = static_cast<u8>(b.ue(7));
        weights(&b, &d, 0, d.active[0]);
        if (bipred)
            weights(&b, &d, 1, d.active[1]);
    }
    if (reference)
        skip_marking(&b, max_frame_num, idr);
    if (pps.cabac && inter)
        d.cabac_init_idc = static_cast<u8>(b.ue(2));
    const i32 qp = 26 + pps.initial_qp;
    d.qp_delta = static_cast<i8>(b.se(-qp, 51 - qp));
    if (pps.deblocking_control) {
        d.disable_deblocking_filter_idc = static_cast<u8>(b.ue(2));
        if (d.disable_deblocking_filter_idc != 1) {
            d.alpha_c0_offset_div2 = static_cast<i8>(b.se(-6, 6));
            d.beta_offset_div2 = static_cast<i8>(b.se(-6, 6));
        }
    }
    if (pps.cabac)
        while (b.remaining && b.error == H264Error::none)
            if (!b.bit())
                b.error = H264Error::malformed;
    if (b.error != H264Error::none)
        return b.error;
    if (!b.remaining && b.offset == b.source.count)
        return H264Error::truncated;
    const u32 escapes = h264_escape_bytes(b.source, b.offset);
    d.data_bit_offset = 8u + (b.offset - escapes) * 8u - b.remaining;
    *output = d;
    return H264Error::none;
}

} // namespace maicord
#pragma clang attribute pop
