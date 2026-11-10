#include "maicord/media/video/hevc_slice_detail.hpp"
#include "hevc_bits.hpp"
// Header parsing, once per slice and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

// pred_weight_table(): the coded weights and offsets, ChromaOffset derived by 7-56.
void weights(HevcBits& b, HevcSliceDetail& d) noexcept {
    d.weighted = true;
    d.luma_log2_weight_denom = static_cast<u8>(b.ue(7));
    const i32 chroma_denominator = static_cast<i32>(d.luma_log2_weight_denom) + b.se(-7, 7);
    if (chroma_denominator < 0 || chroma_denominator > 7) {
        b.error = HevcError::malformed;
        return;
    }
    d.delta_chroma_log2_weight_denom = static_cast<i8>(chroma_denominator - d.luma_log2_weight_denom);
    for (u32 list = 0; list < (d.slice_type == 0 ? 2u : 1u); ++list) {
        const u32 count = d.active[list];
        bool y[15]{}, uv[15]{};
        for (u32 i = 0; i < count; ++i)
            y[i] = b.bit();
        for (u32 i = 0; i < count; ++i)
            uv[i] = b.bit();
        for (u32 i = 0; i < count; ++i) {
            if (y[i]) {
                d.delta_luma_weight[list][i] = static_cast<i8>(b.se(-128, 127));
                d.luma_offset[list][i] = static_cast<i8>(b.se(-128, 127));
            }
            for (u32 c = 0; c < 2; ++c) {
                i32 delta_weight = 0, delta_offset = 0;
                if (uv[i]) {
                    delta_weight = b.se(-128, 127);
                    delta_offset = b.se(-512, 511);
                }
                const i32 weight = (1 << chroma_denominator) + delta_weight;
                i32 offset = 128 + delta_offset - ((128 * weight) >> chroma_denominator);
                offset = offset < -128 ? -128 : offset > 127 ? 127 : offset;
                d.delta_chroma_weight[list][i][c] = static_cast<i8>(delta_weight);
                d.chroma_offset[list][i][c] = static_cast<i8>(offset);
            }
        }
    }
}

u32 escapes_in(const u8* data, u32 bytes) noexcept {
    u32 count = 0, zeroes = 0;
    for (u32 i = 0; i < bytes; ++i) {
        if (zeroes >= 2 && data[i] == 3) {
            ++count;
            zeroes = 0;
            ++i; // the byte after an emulation prevention byte is data
            if (i < bytes)
                zeroes = data[i] == 0 ? 1u : 0u;
            continue;
        }
        zeroes = data[i] == 0 ? zeroes + 1u : 0u;
    }
    return count;
}

} // namespace

HevcError hevc_parse_slice_detail(const HevcNal& n, const HevcConfiguration& c, const HevcSliceDetail* previous,
                                  HevcSliceDetail* output) noexcept {
    if (!output)
        return HevcError::invalid_input;
    const auto& s = c.sps;
    const auto& p = c.pps;
    auto b = hevc_payload(n.bytes, n.type);
    HevcSliceDetail d{};
    const bool irap = n.type >= 16 && n.type <= 23;
    const bool idr = n.type == 19 || n.type == 20;
    d.first = b.bit();
    if (irap)
        b.bit();
    if (b.ue(63) != p.id)
        return HevcError::unsupported;
    const u32 ctb = 1u << (s.log2_min_cb_minus3 + 3 + s.log2_diff_cb),
              width = (s.coded_width + ctb - 1) / ctb, height = (s.coded_height + ctb - 1) / ctb,
              total = width * height;
    if (!d.first) {
        d.dependent = p.dependent_slices && b.bit();
        d.address = b.read(hevc_ceil_log2(total));
        if (!d.address || d.address >= total)
            b.error = HevcError::malformed;
    }
    if (d.dependent) {
        if (!previous)
            return HevcError::malformed;
        const u32 address = d.address;
        d = *previous;
        d.address = address;
        d.first = false;
        d.dependent = true;
    } else {
        for (u32 i = 0; i < p.extra_slice_bits; ++i)
            b.bit();
        d.slice_type = static_cast<u8>(b.ue(2));
        if (p.output_flag_present)
            b.bit();
        u32 current = 0;
        if (!idr) {
            b.read(s.poc_bits);
            const bool from_sps = b.bit();
            HevcReferenceSet set{};
            if (!from_sps) {
                hevc_parse_reference_set(b, s.short_term, s.short_term_count, s.short_term_count, &set);
            } else {
                if (!s.short_term_count)
                    return HevcError::malformed;
                const u32 index = s.short_term_count > 1 ? b.read(hevc_ceil_log2(s.short_term_count)) : 0;
                if (index >= s.short_term_count)
                    return HevcError::malformed;
                set = s.short_term[index];
            }
            for (u32 i = 0; i < u32{set.negative} + set.positive; ++i)
                if (set.used & (1u << i))
                    ++current;
            if (s.long_term_present) {
                const u32 from_sps_count = s.long_term_count ? b.ue(s.long_term_count) : 0, explicit_count = b.ue(15);
                if (from_sps_count + explicit_count > 15)
                    return HevcError::unsupported;
                for (u32 i = 0; i < from_sps_count + explicit_count; ++i) {
                    bool used;
                    if (i < from_sps_count) {
                        const u32 index = s.long_term_count > 1 ? b.read(hevc_ceil_log2(s.long_term_count)) : 0;
                        if (index >= s.long_term_count)
                            return HevcError::malformed;
                        used = (s.long_term_used & (1u << index)) != 0;
                    } else {
                        b.read(s.poc_bits);
                        used = b.bit();
                    }
                    if (used)
                        ++current;
                    if (b.bit())
                        b.ue(65535);
                }
            }
            if (s.temporal_mvp)
                d.temporal_mvp = b.bit();
        }
        if (s.sao) {
            d.sao_luma = b.bit();
            d.sao_chroma = b.bit();
        }
        if (d.slice_type != 2) {
            d.active[0] = p.references_l0;
            d.active[1] = d.slice_type == 0 ? p.references_l1 : 0;
            if (b.bit()) {
                d.active[0] = static_cast<u8>(b.ue(14) + 1);
                if (d.slice_type == 0)
                    d.active[1] = static_cast<u8>(b.ue(14) + 1);
            }
            if (!current)
                return HevcError::malformed;
            if (p.lists_modification && current > 1) {
                const u32 bits = hevc_ceil_log2(current);
                for (u32 list = 0; list < (d.slice_type == 0 ? 2u : 1u); ++list) {
                    d.modified[list] = b.bit();
                    if (d.modified[list])
                        for (u32 i = 0; i < d.active[list]; ++i) {
                            const u32 entry = b.read(bits);
                            if (entry >= current)
                                b.error = HevcError::malformed;
                            d.list_entry[list][i] = static_cast<u8>(entry);
                        }
                }
            }
            if (d.slice_type == 0)
                d.mvd_l1_zero = b.bit();
            if (p.cabac_init)
                d.cabac_init = b.bit();
            d.collocated_from_l0 = true;
            if (d.temporal_mvp) {
                if (d.slice_type == 0)
                    d.collocated_from_l0 = b.bit();
                const u32 list_size = d.collocated_from_l0 ? d.active[0] : d.active[1];
                if (list_size > 1)
                    d.collocated_ref_idx = static_cast<u8>(b.ue(list_size - 1));
            }
            if ((p.weighted_pred && d.slice_type == 1) || (p.weighted_bipred && d.slice_type == 0))
                weights(b, d);
            d.five_minus_max_num_merge_cand = static_cast<u8>(b.ue(4));
        }
        const i32 qp_delta = b.se(-51, 51);
        const i32 qp = 26 + p.initial_qp_minus26 + qp_delta;
        if (qp < 0 || qp > 51)
            b.error = HevcError::malformed;
        d.qp_delta = static_cast<i8>(qp_delta);
        if (p.slice_chroma_offsets) {
            const i32 cb = b.se(-12, 12), cr = b.se(-12, 12);
            if (p.cb_qp_offset + cb < -12 || p.cb_qp_offset + cb > 12 || p.cr_qp_offset + cr < -12 ||
                p.cr_qp_offset + cr > 12)
                b.error = HevcError::malformed;
            d.cb_qp_offset = static_cast<i8>(cb);
            d.cr_qp_offset = static_cast<i8>(cr);
        }
        d.deblocking_disabled = p.deblocking_disabled;
        d.beta_offset_div2 = p.beta_offset_div2;
        d.tc_offset_div2 = p.tc_offset_div2;
        if (p.deblocking_override && b.bit()) {
            d.deblocking_disabled = b.bit();
            if (!d.deblocking_disabled) {
                d.beta_offset_div2 = static_cast<i8>(b.se(-6, 6));
                d.tc_offset_div2 = static_cast<i8>(b.se(-6, 6));
            }
        }
        d.loop_filter_across_slices = p.loop_filter_slices;
        if (p.loop_filter_slices && (d.sao_luma || d.sao_chroma || !d.deblocking_disabled))
            d.loop_filter_across_slices = b.bit();
    }
    d.entry_points = 0;
    if (p.tiles || p.entropy_sync) {
        const u32 entries = b.ue(4096);
        d.entry_points = static_cast<u16>(entries);
        if (entries) {
            const u32 bits = b.ue(31) + 1;
            u64 offsets{};
            for (u32 i = 0; i < entries && b.error == HevcError::none; ++i)
                offsets += u64{b.read(bits)} + 1;
            if (offsets >= n.bytes.count)
                b.error = HevcError::malformed;
        }
    }
    if (p.slice_header_extension) {
        const u32 bytes = b.ue(256);
        for (u32 i = 0; i < bytes; ++i)
            b.read(8);
    }
    if (!b.bit())
        b.error = HevcError::malformed;
    while (b.remaining && b.error == HevcError::none)
        if (b.bit())
            b.error = HevcError::malformed;
    if (b.error == HevcError::none && b.cursor >= b.source.count)
        b.error = HevcError::truncated;
    if (b.error != HevcError::none)
        return b.error;
    d.escape_bytes = static_cast<u16>(escapes_in(b.source.data, b.cursor));
    d.data_byte_offset = 2u + b.position / 8u;
    *output = d;
    return HevcError::none;
}

} // namespace maicord
#pragma clang attribute pop
