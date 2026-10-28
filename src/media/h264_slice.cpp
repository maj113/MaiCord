#include "maicord/media/h264_slice.hpp"
#include "maicord/core/memory.hpp"
#include "h264_bits.hpp"

namespace maicord {
namespace {

void list_changes(H264Bits* b, u32 max_frame_num) noexcept {
    if (!b->bit()) return;
    for (u32 count = 0; b->error == H264Error::none; ++count) {
        const u32 operation = b->ue(3);
        if (operation == 3) {
            if (!count) b->error = H264Error::malformed;
            return;
        }
        if (count == 32) { b->error = H264Error::unsupported; return; }
        b->ue(operation == 2 ? 15u : max_frame_num - 1u);
    }
}

void weights(H264Bits* b, u32 count) noexcept {
    for (u32 i = 0; i < count && b->error == H264Error::none; ++i) {
        if (b->bit()) { b->se(-128, 127); b->se(-128, 127); }
        if (b->bit()) for (u32 component = 0; component < 2; ++component) {
            b->se(-128, 127); b->se(-128, 127);
        }
    }
}

void marking(H264Bits* b, u32 max_frame_num, H264Slice* slice) noexcept {
    if (slice->idr) {
        slice->discard_prior = b->bit(); slice->long_term = b->bit();
        return;
    }
    slice->adaptive_marking = b->bit();
    if (!slice->adaptive_marking) return;
    while (b->error == H264Error::none) {
        H264Marking item{};
        item.operation = static_cast<u8>(b->ue(6));
        if (!item.operation) return;
        if (slice->marking_count == 32) { b->error = H264Error::unsupported; return; }
        if (item.operation == 1 || item.operation == 3)
            item.difference_minus1 = static_cast<u16>(b->ue(max_frame_num - 1u));
        if (item.operation == 2 || item.operation == 3 || item.operation == 6)
            item.long_term = static_cast<u8>(b->ue(15));
        if (item.operation == 4) item.long_term = static_cast<u8>(b->ue(16));
        slice->marking[slice->marking_count++] = item;
    }
}

} // namespace

H264Error h264_slice_parameter_id(ConstSlice<u8> nal, u8* output) noexcept {
    if (!output || !nal.data || nal.count < 2) return H264Error::invalid_input;
    if (nal.data[0] & 0x80u) return H264Error::malformed;
    const u8 type = nal.data[0] & 31u;
    if (type != 1 && type != 5) return H264Error::unsupported;
    H264Bits b{{nal.data + 1, nal.count - 1}};
    b.ue(); b.ue(9); const u8 id = static_cast<u8>(b.ue(255));
    if (b.error == H264Error::none) *output = id;
    return b.error;
}

H264Error h264_parse_slice(ConstSlice<u8> nal, const H264Sps& sps,
    const H264Pps& pps, H264Slice* output) noexcept {
    if (!output || !nal.data || nal.count < 2 || sps.id != pps.sps_id)
        return H264Error::invalid_input;
    const u8 nal_type = nal.data[0] & 31u;
    if (nal.data[0] & 0x80u) return H264Error::malformed;
    if (nal_type != 1 && nal_type != 5) return H264Error::unsupported;
    H264Bits b{{nal.data + 1, nal.count - 1}};
    H264Slice slice{};
    slice.reference = (nal.data[0] >> 5u) & 3u;
    slice.idr = nal_type == 5;
    if (slice.idr && !slice.reference) return H264Error::malformed;
    const u32 max_frame_num = 1u << sps.frame_num_bits;
    const u32 macroblocks = (sps.coded_width / 16u) * (sps.coded_height / 16u);
    slice.first_mb = b.ue(macroblocks ? macroblocks - 1u : 0);
    const u32 type = b.ue(9);
    if (b.error != H264Error::none) return b.error;
    if (type % 5u > 2) return H264Error::unsupported;
    slice.type = static_cast<H264SliceType>(type % 5u);
    slice.all_same_type = type >= 5;
    slice.pps_id = static_cast<u8>(b.ue(255));
    if (slice.pps_id != pps.id) return H264Error::malformed;
    slice.frame_num = static_cast<u16>(b.read(sps.frame_num_bits));
    if (!sps.frame_only && b.bit()) return H264Error::unsupported;
    if (sps.mbaff && slice.first_mb >= macroblocks / 2u) return H264Error::malformed;
    if (slice.idr) {
        if (slice.type != H264SliceType::i || slice.frame_num) return H264Error::malformed;
        slice.idr_id = static_cast<u16>(b.ue(65'535));
    }
    if (sps.poc_type == 0) {
        slice.poc_lsb = static_cast<u16>(b.read(sps.poc_bits));
        if (pps.bottom_poc_present) slice.delta_bottom = b.se();
    } else if (sps.poc_type == 1 && !sps.delta_always_zero) {
        slice.delta_poc0 = b.se();
        if (pps.bottom_poc_present) slice.delta_poc1 = b.se();
    }
    if (pps.redundant_count && b.ue(127)) return H264Error::unsupported;
    const bool inter = slice.type != H264SliceType::i;
    const bool bipred = slice.type == H264SliceType::b;
    if (bipred) b.bit(); // direct_spatial_mv_pred_flag stays in compressed data.
    if (inter) {
        slice.references_l0 = pps.references_l0;
        slice.references_l1 = bipred ? pps.references_l1 : 0;
        if (b.bit()) {
            slice.references_l0 = static_cast<u8>(b.ue(15) + 1u);
            if (bipred) slice.references_l1 = static_cast<u8>(b.ue(15) + 1u);
        }
        if (!slice.references_l0 || slice.references_l0 > 16 ||
            (bipred && (!slice.references_l1 || slice.references_l1 > 16)))
            return H264Error::malformed;
        list_changes(&b, max_frame_num);
        if (bipred) list_changes(&b, max_frame_num);
    }
    if ((slice.type == H264SliceType::p && pps.weighted_pred) ||
        (bipred && pps.weighted_bipred == 1)) {
        b.ue(7); b.ue(7); // luma/chroma denominators, 4:2:0 only.
        weights(&b, slice.references_l0);
        if (bipred) weights(&b, slice.references_l1);
    }
    if (slice.reference) marking(&b, max_frame_num, &slice);
    if (pps.cabac && inter) b.ue(2);
    const i32 qp = 26 + pps.initial_qp;
    b.se(-qp, 51 - qp);
    if (pps.deblocking_control && b.ue(2) != 1) { b.se(-6, 6); b.se(-6, 6); }
    // CABAC byte alignment belongs to slice_data, but checking it here avoids
    // submitting a truncated header to a driver. CAVLC remains bit-aligned.
    if (pps.cabac) while (b.remaining && b.error == H264Error::none)
        if (!b.bit()) b.error = H264Error::malformed;
    if (b.error != H264Error::none) return b.error;
    if (!b.remaining && b.offset == b.source.count) return H264Error::truncated;
    *output = slice;
    return H264Error::none;
}

bool h264_same_picture(const H264Slice& a, const H264Slice& b) noexcept {
    return a.marking_count <= 32 && b.marking_count <= 32 &&
        a.frame_num == b.frame_num && a.pps_id == b.pps_id &&
        (a.reference != 0) == (b.reference != 0) && a.idr == b.idr &&
        (!a.idr || a.idr_id == b.idr_id) && a.poc_lsb == b.poc_lsb &&
        a.delta_bottom == b.delta_bottom && a.delta_poc0 == b.delta_poc0 &&
        a.delta_poc1 == b.delta_poc1 &&
        (!(a.all_same_type || b.all_same_type) || a.type == b.type) &&
        a.discard_prior == b.discard_prior && a.long_term == b.long_term &&
        a.adaptive_marking == b.adaptive_marking && a.marking_count == b.marking_count &&
        bytes_equal(a.marking, b.marking, a.marking_count * sizeof(H264Marking));
}

} // namespace maicord
