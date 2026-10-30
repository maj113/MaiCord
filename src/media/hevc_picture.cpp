#include "maicord/media/hevc.hpp"
#include "maicord/core/memory.hpp"
#include "hevc_bits.hpp"
// Header parsing, once per picture and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {
bool supported_vcl(u8 type) noexcept {
    return type <= 9 || (type >= 16 && type <= 21);
}
void weights(HevcBits& b, u32 l0, u32 l1, bool bipred) noexcept {
    const u32 luma = b.ue(7);
    const i32 chroma = static_cast<i32>(luma) + b.se(-7, 7);
    if (chroma < 0 || chroma > 7) {
        b.error = HevcError::malformed;
        return;
    }
    for (u32 list = 0; list < (bipred ? 2u : 1u); ++list) {
        const u32 count = list ? l1 : l0;
        bool y[15]{}, uv[15]{};
        for (u32 i = 0; i < count; ++i)
            y[i] = b.bit();
        for (u32 i = 0; i < count; ++i)
            uv[i] = b.bit();
        for (u32 i = 0; i < count; ++i) {
            if (y[i]) {
                b.se(-128, 127);
                b.se(-128, 127);
            }
            if (uv[i])
                for (u32 c = 0; c < 2; ++c) {
                    b.se(-128, 127);
                    b.se(-512, 511);
                }
        }
    }
}
} // namespace
HevcError hevc_parse_slice(const HevcNal& n, const HevcConfiguration& c, const HevcPictureHeader* previous,
                           HevcSlice* output) noexcept {
    if (!output)
        return HevcError::invalid_input;
    // Both callers read *output only after success: parse in place.
    reset_value(output);
    HevcSlice& out = *output;
    const auto& s = c.sps;
    const auto& p = c.pps;
    auto b = hevc_payload(n.bytes, n.type);
    auto& v = out.header;
    v.nal_type = n.type;
    v.temporal_id = n.temporal_id;
    v.irap = n.type >= 16 && n.type <= 23;
    v.idr = n.type == 19 || n.type == 20;
    v.reference = n.type >= 16 || (n.type & 1u);
    out.first = b.bit();
    if (v.irap)
        v.no_output_prior = b.bit();
    v.pps_id = static_cast<u8>(b.ue(63));
    if (v.pps_id != p.id)
        return HevcError::unsupported;
    const u32 ctb = 1u << (s.log2_min_cb_minus3 + 3 + s.log2_diff_cb),
              width = (s.coded_width + ctb - 1) / ctb, height = (s.coded_height + ctb - 1) / ctb,
              total = width * height;
    if (!out.first) {
        out.dependent = p.dependent_slices && b.bit();
        out.address = b.read(hevc_ceil_log2(total));
        if (!out.address || out.address >= total)
            b.error = HevcError::malformed;
    }
    if (out.dependent) {
        if (!previous)
            return HevcError::malformed;
        v = *previous;
    } else {
        for (u32 i = 0; i < p.extra_slice_bits; ++i)
            b.bit();
        v.slice_type = static_cast<u8>(b.ue(2));
        if (p.output_flag_present)
            v.output = b.bit();
        bool temporal_mvp = false;
        if (!v.idr) {
            v.poc_lsb = static_cast<u16>(b.read(s.poc_bits));
            const bool from_sps = b.bit();
            if (!from_sps) {
                const u32 start = b.position;
                hevc_parse_reference_set(b, s.short_term, s.short_term_count, s.short_term_count,
                                         &v.short_term, &v.reference_rps_delta_count);
                const u32 bits = b.position - start;
                if (bits > 65535)
                    b.error = HevcError::unsupported;
                v.short_term_bits = static_cast<u16>(bits);
            } else {
                if (!s.short_term_count)
                    return HevcError::malformed;
                const u32 index = s.short_term_count > 1 ? b.read(hevc_ceil_log2(s.short_term_count)) : 0;
                if (index >= s.short_term_count)
                    return HevcError::malformed;
                v.short_term = s.short_term[index];
            }
            if (s.long_term_present) {
                const u32 from_sps_count = s.long_term_count ? b.ue(s.long_term_count) : 0,
                          explicit_count = b.ue(15);
                if (from_sps_count + explicit_count > 15)
                    return HevcError::unsupported;
                v.long_term_count = static_cast<u8>(from_sps_count + explicit_count);
                u64 cycles{};
                for (u32 i = 0; i < v.long_term_count; ++i) {
                    auto& ref = v.long_term[i];
                    if (i < from_sps_count) {
                        const u32 index =
                            s.long_term_count > 1 ? b.read(hevc_ceil_log2(s.long_term_count)) : 0;
                        if (index >= s.long_term_count)
                            return HevcError::malformed;
                        ref.poc_lsb = s.long_term_lsb[index];
                        ref.used = (s.long_term_used & (1u << index)) != 0;
                    } else {
                        ref.poc_lsb = static_cast<u16>(b.read(s.poc_bits));
                        ref.used = b.bit();
                    }
                    ref.delta_msb_present = b.bit();
                    if (ref.delta_msb_present) {
                        const u32 delta = b.ue(65535);
                        cycles = (i == 0 || i == from_sps_count) ? delta : cycles + delta;
                        if (cycles > 0xffffffffu)
                            return HevcError::malformed;
                        ref.delta_msb_cycles = static_cast<u32>(cycles);
                    }
                }
            }
            if (s.temporal_mvp)
                temporal_mvp = b.bit();
        }
        bool sao_luma = false, sao_chroma = false;
        if (s.sao) {
            sao_luma = b.bit();
            sao_chroma = b.bit();
        }
        if (v.slice_type != 2) {
            u32 l0 = p.references_l0, l1 = p.references_l1;
            if (b.bit()) {
                l0 = b.ue(14) + 1;
                if (v.slice_type == 0)
                    l1 = b.ue(14) + 1;
            }
            u32 current{};
            for (u32 i = 0; i < u32{v.short_term.negative} + v.short_term.positive; ++i)
                if (v.short_term.used & (1u << i))
                    ++current;
            for (u32 i = 0; i < v.long_term_count; ++i)
                if (v.long_term[i].used)
                    ++current;
            if (!current)
                return HevcError::malformed;
            if (p.lists_modification && current > 1) {
                const u32 bits = hevc_ceil_log2(current);
                if (b.bit())
                    for (u32 i = 0; i < l0; ++i)
                        if (b.read(bits) >= current)
                            b.error = HevcError::malformed;
                if (v.slice_type == 0 && b.bit())
                    for (u32 i = 0; i < l1; ++i)
                        if (b.read(bits) >= current)
                            b.error = HevcError::malformed;
            }
            if (v.slice_type == 0)
                b.bit();
            if (p.cabac_init)
                b.bit();
            if (temporal_mvp) {
                const bool from_l0 = v.slice_type == 0 ? b.bit() : true;
                if ((from_l0 && l0 > 1) || (!from_l0 && l1 > 1))
                    b.ue((from_l0 ? l0 : l1) - 1);
            }
            if ((p.weighted_pred && v.slice_type == 1) || (p.weighted_bipred && v.slice_type == 0))
                weights(b, l0, l1, v.slice_type == 0);
            b.ue(4);
        }
        const i32 qp = 26 + p.initial_qp_minus26 + b.se(-51, 51);
        if (qp < 0 || qp > 51)
            b.error = HevcError::malformed;
        if (p.slice_chroma_offsets) {
            const i32 cb = p.cb_qp_offset + b.se(-12, 12), cr = p.cr_qp_offset + b.se(-12, 12);
            if (cb < -12 || cb > 12 || cr < -12 || cr > 12)
                b.error = HevcError::malformed;
        }
        bool disabled = p.deblocking_disabled;
        if (p.deblocking_override && b.bit()) {
            disabled = b.bit();
            if (!disabled) {
                b.se(-6, 6);
                b.se(-6, 6);
            }
        }
        if (p.loop_filter_slices && (sao_luma || sao_chroma || !disabled))
            b.bit();
    }
    if (p.tiles || p.entropy_sync) {
        const u32 entries = b.ue(4096);
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
    return b.error;
}
bool hevc_picture_header_same(const HevcPictureHeader& a, const HevcPictureHeader& b) noexcept {
    if (!(a.poc_lsb == b.poc_lsb && a.nal_type == b.nal_type && a.temporal_id == b.temporal_id &&
          a.pps_id == b.pps_id && a.output == b.output && a.no_output_prior == b.no_output_prior &&
          a.long_term_count == b.long_term_count &&
          bytes_equal(&a.short_term, &b.short_term, sizeof(a.short_term))))
        return false;
    for (u32 i = 0; i < a.long_term_count; ++i) {
        const auto& x = a.long_term[i];
        const auto& y = b.long_term[i];
        if (x.poc_lsb != y.poc_lsb || x.delta_msb_cycles != y.delta_msb_cycles ||
            x.delta_msb_present != y.delta_msb_present || x.used != y.used)
            return false;
    }
    return true;
}
HevcError hevc_picture_parameter_id(ConstSlice<u8> unit, u8 length, u8* id) noexcept {
    if (!id)
        return HevcError::invalid_input;
    HevcNalCursor cursor{unit, 0, length};
    for (u32 count = 0; count < 256; ++count) {
        const auto n = hevc_next_nal(&cursor);
        if (n.error != HevcError::none)
            return n.error;
        if (n.type > 31)
            continue;
        if (!supported_vcl(n.type))
            return HevcError::unsupported;
        auto b = hevc_payload(n.bytes, n.type);
        if (!b.bit())
            return HevcError::malformed;
        if (n.type >= 16 && n.type <= 23)
            b.bit();
        const u32 value = b.ue(63);
        if (b.error != HevcError::none)
            return b.error;
        *id = static_cast<u8>(value);
        return HevcError::none;
    }
    return HevcError::unsupported;
}
HevcError hevc_parse_picture(ConstSlice<u8> unit, const HevcConfiguration& c,
                             HevcPicture* output) noexcept {
    if (!output || !unit.data || !unit.count || !c.sps.coded_width || !c.sps.max_buffering ||
        c.sps.poc_bits < 4 || c.sps.poc_bits > 16)
        return HevcError::invalid_input;
    HevcPicture v; // Spans below slice_count are written before they are read.
    HevcNalCursor cursor{unit, 0, c.nal_length_bytes};
    u32 nals{}, last_address{};
    bool seen{};
    for (;;) {
        const auto n = hevc_next_nal(&cursor);
        if (n.error == HevcError::end)
            break;
        if (n.error != HevcError::none)
            return n.error;
        if (++nals > 256)
            return HevcError::unsupported;
        if (n.type > 31) {
            if (n.type == 32 || n.type == 33 || n.type == 34) {
                const auto e = hevc_validate_parameter(n.bytes, c);
                if (e != HevcError::none)
                    return e;
                continue;
            }
            if (n.type == 35 || n.type == 36 || n.type == 37 || n.type == 38 || n.type == 39 ||
                n.type == 40)
                continue;
            return HevcError::unsupported;
        }
        if (!supported_vcl(n.type) || n.temporal_id > c.sps.max_sub_layers)
            return HevcError::unsupported;
        if (v.slice_count == kHevcMaximumSlices)
            return HevcError::unsupported;
        HevcSlice parsed{};
        const auto e = hevc_parse_slice(n, c, seen ? &v : nullptr, &parsed);
        if (e != HevcError::none)
            return e;
        if (parsed.first == seen)
            return HevcError::malformed;
        if (!seen) {
            static_cast<HevcPictureHeader&>(v) = parsed.header;
            seen = true;
        } else if (!hevc_picture_header_same(v, parsed.header) || parsed.address <= last_address)
            return HevcError::malformed;
        v.slices[v.slice_count++] = {static_cast<u32>(n.bytes.data - unit.data), n.bytes.count};
        last_address = parsed.address;
        if (n.bytes.count > 0xffffffffu - 3 - v.bitstream_bytes)
            return HevcError::unsupported;
        v.bitstream_bytes += n.bytes.count + 3;
    }
    if (!seen)
        return HevcError::malformed;
    static_cast<HevcPictureHeader&>(*output) = v;
    output->bitstream_bytes = v.bitstream_bytes;
    output->slice_count = v.slice_count;
    copy_bytes(output->slices, v.slices, v.slice_count * u32{sizeof(HevcSliceSpan)});
    return HevcError::none;
}
} // namespace maicord
#pragma clang attribute pop
