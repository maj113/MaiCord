#include "maicord/media/hevc.hpp"
#include "maicord/media/gpu_video_frame.hpp"
#include "maicord/core/memory.hpp"
#include "hevc_bits.hpp"
// Header parsing, once per picture and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {
void profile_tier(HevcBits& b, u8 layers, u8* profile = nullptr, u8* level = nullptr) noexcept {
    const u32 space = b.read(2);
    b.bit();
    const u32 id = b.read(5);
    const u32 compatible = b.read(32);
    b.read(32);
    b.read(16);
    const u32 lev = b.read(8);
    if (space || (id != 1 && !(compatible & 0x40000000u)))
        b.error = HevcError::unsupported;
    if (profile)
        *profile = static_cast<u8>(id);
    if (level)
        *level = static_cast<u8>(lev);
    bool profiles[7]{}, levels[7]{};
    for (u32 i = 0; i < layers; ++i) {
        profiles[i] = b.bit();
        levels[i] = b.bit();
    }
    if (layers)
        for (u32 i = layers; i < 8; ++i)
            if (b.read(2))
                b.error = HevcError::malformed;
    for (u32 i = 0; i < layers; ++i) {
        if (profiles[i]) {
            b.read(32);
            b.read(32);
            b.read(24);
        }
        if (levels[i])
            b.read(8);
    }
}
constexpr u8 default_intra[64] = {16, 16, 16, 16, 17, 18, 21, 24, 16, 16, 16, 16, 17, 19, 22, 25,
                                  16, 16, 17, 18, 20, 22, 25, 29, 16, 16, 18, 21, 24, 27, 31, 36,
                                  17, 17, 20, 24, 30, 35, 41, 47, 18, 19, 22, 27, 35, 44, 54, 65,
                                  21, 22, 25, 31, 41, 54, 70, 88, 24, 25, 29, 36, 47, 65, 88, 115};
constexpr u8 default_inter[64] = {16, 16, 16, 16, 17, 18, 20, 24, 16, 16, 16, 17, 18, 20, 24, 25,
                                  16, 16, 17, 18, 20, 24, 25, 28, 16, 17, 18, 20, 24, 25, 28, 33,
                                  17, 18, 20, 24, 25, 28, 33, 41, 18, 20, 24, 25, 28, 33, 41, 54,
                                  20, 24, 25, 28, 33, 41, 54, 71, 24, 25, 28, 33, 41, 54, 71, 91};
u8* scaling_list(HevcScaling& s, u32 size, u32 matrix) noexcept {
    return size == 0   ? s.list4x4[matrix]
           : size == 1 ? s.list8x8[matrix]
           : size == 2 ? s.list16x16[matrix]
                       : s.list32x32[matrix / 3];
}
void scaling_defaults(HevcScaling& s) noexcept {
    for (u32 m = 0; m < 6; ++m) {
        for (auto& v : s.list4x4[m])
            v = 16;
        copy_bytes(s.list8x8[m], m < 3 ? default_intra : default_inter, 64);
        copy_bytes(s.list16x16[m], m < 3 ? default_intra : default_inter, 64);
        s.dc16[m] = 16;
    }
    copy_bytes(s.list32x32[0], default_intra, 64);
    copy_bytes(s.list32x32[1], default_inter, 64);
    s.dc32[0] = s.dc32[1] = 16;
}
void scaling_parse(HevcBits& b, HevcScaling& s) noexcept {
    for (u32 size = 0; size < 4 && b.error == HevcError::none; ++size)
        for (u32 matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1) {
            auto* list = scaling_list(s, size, matrix);
            const u32 length = size ? 64 : 16;
            if (!b.bit()) {
                const u32 delta = b.ue(matrix);
                if (delta) {
                    const u32 previous = matrix - delta;
                    if (size == 3 && previous % 3) {
                        b.error = HevcError::malformed;
                        return;
                    }
                    copy_bytes(list, scaling_list(s, size, previous), length);
                    if (size == 2)
                        s.dc16[matrix] = s.dc16[previous];
                    if (size == 3)
                        s.dc32[matrix / 3] = s.dc32[previous / 3];
                }
            } else {
                i32 next = 8;
                if (size > 1) {
                    next = b.se(-7, 247) + 8;
                    if (size == 2)
                        s.dc16[matrix] = static_cast<u8>(next);
                    else
                        s.dc32[matrix / 3] = static_cast<u8>(next);
                }
                const u32 n = size ? 8 : 4;
                for (u32 sum = 0; sum < 2 * n - 1; ++sum)
                    for (u32 x = 0; x < n; ++x) {
                        if (sum < x || sum - x >= n)
                            continue;
                        next = (next + b.se(-128, 127) + 256) & 255;
                        list[(sum - x) * n + x] = static_cast<u8>(next);
                    }
            }
        }
}
void sub_hrd(HevcBits& b, u32 count, bool subpic) noexcept {
    for (u32 j = 0; j <= count; ++j) {
        b.ue();
        b.ue();
        if (subpic) {
            b.ue();
            b.ue();
        }
        b.bit();
    }
}
void hrd(HevcBits& b, u32 layers) noexcept {
    const bool nal = b.bit(), vcl = b.bit();
    bool subpic = false;
    if (nal || vcl) {
        subpic = b.bit();
        if (subpic) {
            b.read(8);
            b.read(5);
            b.bit();
            b.read(5);
        }
        b.read(4);
        b.read(4);
        if (subpic)
            b.read(4);
        b.read(5);
        b.read(5);
        b.read(5);
    }
    for (u32 i = 0; i <= layers && b.error == HevcError::none; ++i) {
        const bool general = b.bit();
        const bool fixed = general || b.bit();
        bool low = false;
        if (fixed)
            b.ue();
        else
            low = b.bit();
        const u32 count = low ? 0 : b.ue(31);
        if (nal)
            sub_hrd(b, count, subpic);
        if (vcl)
            sub_hrd(b, count, subpic);
    }
}
void vui(HevcBits& b, HevcSps& s) noexcept {
    static constexpr u16 sar[17][2] = {{0, 0},   {1, 1},    {12, 11}, {10, 11}, {16, 11}, {40, 33},
                                {24, 11}, {20, 11},  {32, 11}, {80, 33}, {18, 11}, {15, 11},
                                {64, 33}, {160, 99}, {4, 3},   {3, 2},   {2, 1}};
    if (b.bit()) {
        const u32 id = b.read(8);
        if (id == 255) {
            s.sar_width = b.read(16);
            s.sar_height = b.read(16);
        } else if (id < 17) {
            s.sar_width = sar[id][0];
            s.sar_height = sar[id][1];
        } else {
            b.error = HevcError::malformed;
            return;
        }
        if (!s.sar_width || !s.sar_height)
            s.sar_width = s.sar_height = 1;
    }
    if (b.bit())
        b.bit();
    if (b.bit()) {
        b.read(3);
        s.full_range = b.bit();
        if (b.bit()) {
            b.read(16); // colour_primaries, transfer_characteristics
            s.matrix = static_cast<u8>(b.read(8));
        }
    }
    if (b.bit()) {
        b.ue(5);
        b.ue(5);
    }
    b.bit();
    const bool field = b.bit();
    b.bit();
    if (field) {
        b.error = HevcError::unsupported;
        return;
    }
    if (b.bit()) {
        b.ue();
        b.ue();
        b.ue();
        b.ue();
    }
    if (b.bit()) {
        const u32 tick = b.read(32), scale = b.read(32);
        if (!tick || !scale)
            b.error = HevcError::malformed;
        if (b.bit())
            b.ue();
        if (b.bit())
            hrd(b, s.max_sub_layers);
    }
    if (b.bit()) {
        b.bit();
        b.bit();
        b.bit();
        b.ue();
        b.ue();
        b.ue();
        b.ue();
        b.ue();
    }
}
HevcError parse_vps(ConstSlice<u8> nal, u8* id, u8* layers) noexcept {
    auto b = hevc_payload(nal, 32);
    const u8 parsed = static_cast<u8>(b.read(4));
    // Only the base layer is decoded, so the stream must carry it. Further
    // layers (vps_max_layers_minus1, e.g. Apple's alpha layer) are ignored.
    const bool base_internal = b.bit(), base_available = b.bit();
    if (!base_internal || !base_available)
        b.error = HevcError::unsupported;
    b.read(6);
    const u8 sub = static_cast<u8>(b.read(3));
    if (sub > 6)
        b.error = HevcError::malformed;
    b.bit();
    if (b.read(16) != 0xffffu)
        b.error = HevcError::malformed;
    profile_tier(b, sub);
    const bool all = b.bit();
    for (u32 i = all ? 0 : sub; i <= sub; ++i) {
        const u32 buffering = b.ue(14);
        if (b.ue(14) > buffering)
            b.error = HevcError::malformed;
        b.ue();
    }
    // Layer sets name which layers each operating point includes.
    const u32 max_layer_id = b.read(6), layer_sets = b.ue(1023);
    for (u32 i = 0; i < layer_sets * (max_layer_id + 1) && b.error == HevcError::none; ++i)
        b.bit();
    if (b.bit()) {
        if (!b.read(32) || !b.read(32))
            b.error = HevcError::malformed;
        if (b.bit())
            b.ue();
        if (b.ue(1024))
            b.error = HevcError::unsupported;
    }
    // vps_extension describes the other layers only; the base layer's syntax
    // ends here.
    const bool extension = b.bit();
    if (b.error == HevcError::none && !extension)
        b.trailing();
    if (b.error == HevcError::none) {
        *id = parsed;
        *layers = sub;
    }
    return b.error;
}
} // namespace
HevcNal hevc_next_nal(HevcNalCursor* c) noexcept {
    HevcNal n{};
    if (!c || !c->bytes.data || c->length_bytes < 1 || c->length_bytes > 4 || c->offset > c->bytes.count) {
        n.error = HevcError::invalid_input;
        return n;
    }
    for (;;) {
        reset_value(&n);
        if (c->offset == c->bytes.count) {
            n.error = HevcError::end;
            return n;
        }
        if (c->bytes.count - c->offset < c->length_bytes) {
            n.error = HevcError::truncated;
            return n;
        }
        u32 bytes{};
        for (u32 i = 0; i < c->length_bytes; ++i)
            bytes = (bytes << 8) | c->bytes[c->offset + i];
        if (bytes < 2 || bytes > c->bytes.count - c->offset - c->length_bytes) {
            n.error = HevcError::truncated;
            return n;
        }
        n.bytes = {c->bytes.data + c->offset + c->length_bytes, bytes};
        n.type = (n.bytes[0] >> 1) & 63u;
        if ((n.bytes[0] & 0x80u) || !(n.bytes[1] & 7u)) {
            n.error = HevcError::malformed;
            return n;
        }
        c->offset += c->length_bytes + bytes;
        // A base-layer (Annex A) decoder ignores every NAL unit with
        // nuh_layer_id > 0, e.g. the alpha layer of Apple's HEVC with alpha.
        if (hevc_nal_layer(n.bytes))
            continue;
        n.temporal_id = static_cast<u8>((n.bytes[1] & 7u) - 1);
        return n;
    }
}
HevcError hevc_parse_reference_set(HevcBits& b, const HevcReferenceSet* sets, u32 index, u32 count,
                                   HevcReferenceSet* out, u8* reference_count) noexcept {
    HevcReferenceSet r{};
    if (index && b.bit()) {
        const u32 back = index == count ? b.ue(index - 1) + 1 : 1;
        const auto& previous = sets[index - back];
        const u32 n = previous.negative + previous.positive;
        if (reference_count)
            *reference_count = static_cast<u8>(n);
        const bool sign = b.bit();
        const i32 delta = static_cast<i32>(b.ue(32767) + 1) * (sign ? -1 : 1);
        i32 values[16]{};
        bool used[16]{};
        u32 valid{};
        for (u32 j = 0; j <= n; ++j) {
            const bool current = b.bit(), include = current || b.bit();
            if (include) {
                const i32 value = (j < n ? previous.delta[j] : 0) + delta;
                // HEVC predicted S0/S1 append only strictly negative /
                // positive deltas. A zero names the current picture, not a ref.
                if (!value)
                    continue;
                if (valid == 15) {
                    b.error = HevcError::unsupported;
                    return b.error;
                }
                values[valid] = value;
                used[valid++] = current;
            }
        }
        for (u32 i = 0; i < valid; ++i)
            for (u32 j = i + 1; j < valid; ++j) {
                const bool before = (values[j] < 0 && values[i] > 0) ||
                                    (values[j] < 0 && values[i] < 0 && values[j] > values[i]) ||
                                    (values[j] > 0 && values[i] > 0 && values[j] < values[i]);
                if (before) {
                    const auto v = values[i];
                    values[i] = values[j];
                    values[j] = v;
                    const bool u = used[i];
                    used[i] = used[j];
                    used[j] = u;
                }
                if (values[i] == values[j]) {
                    b.error = HevcError::malformed;
                    return b.error;
                }
            }
        for (u32 i = 0; i < valid; ++i) {
            r.delta[i] = values[i];
            if (used[i])
                r.used |= static_cast<u16>(1u << i);
            if (values[i] < 0)
                ++r.negative;
            else
                ++r.positive;
        }
    } else {
        const u32 negative = b.ue(15), positive = b.ue(15);
        if (negative + positive > 15) {
            b.error = HevcError::unsupported;
            return b.error;
        }
        r.negative = static_cast<u8>(negative);
        r.positive = static_cast<u8>(positive);
        i32 value{};
        for (u32 i = 0; i < negative; ++i) {
            value -= static_cast<i32>(b.ue(32767) + 1);
            r.delta[i] = value;
            if (b.bit())
                r.used |= static_cast<u16>(1u << i);
        }
        value = 0;
        for (u32 i = 0; i < positive; ++i) {
            value += static_cast<i32>(b.ue(32767) + 1);
            r.delta[negative + i] = value;
            if (b.bit())
                r.used |= static_cast<u16>(1u << (negative + i));
        }
    }
    if (b.error == HevcError::none)
        *out = r;
    return b.error;
}
HevcError hevc_parse_sps(ConstSlice<u8> nal, HevcSps* output) noexcept {
    if (!output)
        return HevcError::invalid_input;
    // Parsed in place: *output holds a valid SPS only when this returns none.
    HevcSps& s = *output;
    clear_bytes(&s, sizeof(s));
    s.sar_width = s.sar_height = 1;
    s.matrix = 2; // Unspecified when VUI colour description is absent.
    scaling_defaults(s.scaling);
    auto b = hevc_payload(nal, 33);
    s.vps_id = static_cast<u8>(b.read(4));
    s.max_sub_layers = static_cast<u8>(b.read(3));
    if (s.max_sub_layers > 6)
        b.error = HevcError::malformed;
    b.bit();
    profile_tier(b, s.max_sub_layers, &s.profile, &s.level);
    if (b.error != HevcError::none)
        return b.error; // Main10 and other profiles stay unsupported, not malformed.
    s.id = static_cast<u8>(b.ue(15));
    const u32 chroma = b.ue(3);
    if (chroma == 3)
        b.bit();
    if (chroma != 1)
        b.error = HevcError::unsupported;
    s.coded_width = b.ue(4096);
    s.coded_height = b.ue(4096);
    // Live streams reach 4K; attachments check their smaller surface budget.
    if (!s.coded_width || !s.coded_height || u64{s.coded_width} * s.coded_height > kGpuVideoCodedPixels)
        b.error = HevcError::unsupported;
    u32 left{}, right{}, top{}, bottom{};
    if (b.bit()) {
        left = b.ue(2048);
        right = b.ue(2048);
        top = b.ue(2048);
        bottom = b.ue(2048);
    }
    if ((left + right) * 2 >= s.coded_width || (top + bottom) * 2 >= s.coded_height)
        b.error = HevcError::malformed;
    s.crop_x = left * 2;
    s.crop_y = top * 2;
    s.width = s.coded_width - (left + right) * 2;
    s.height = s.coded_height - (top + bottom) * 2;
    if (b.ue(8) || b.ue(8))
        b.error = HevcError::unsupported;
    s.poc_bits = static_cast<u8>(b.ue(12) + 4);
    const bool ordering = b.bit();
    for (u32 i = ordering ? 0 : s.max_sub_layers; i <= s.max_sub_layers; ++i) {
        const u32 buffering = b.ue(14), reorder = b.ue(14);
        if (reorder > buffering)
            b.error = HevcError::malformed;
        b.ue();
        if (i == s.max_sub_layers) {
            s.max_buffering = static_cast<u8>(buffering + 1);
            s.max_reorder = static_cast<u8>(reorder);
        }
    }
    s.log2_min_cb_minus3 = static_cast<u8>(b.ue(3));
    s.log2_diff_cb = static_cast<u8>(b.ue(3));
    s.log2_min_tb_minus2 = static_cast<u8>(b.ue(3));
    s.log2_diff_tb = static_cast<u8>(b.ue(3));
    s.transform_depth_inter = static_cast<u8>(b.ue(4));
    s.transform_depth_intra = static_cast<u8>(b.ue(4));
    if (s.log2_min_cb_minus3 + s.log2_diff_cb > 3 || s.log2_min_tb_minus2 + s.log2_diff_tb > 3 ||
        s.log2_min_tb_minus2 + 2 > s.log2_min_cb_minus3 + 3)
        b.error = HevcError::malformed;
    s.scaling_enabled = b.bit();
    if (s.scaling_enabled) {
        s.scaling_present = b.bit();
        if (s.scaling_present)
            scaling_parse(b, s.scaling);
    }
    s.amp = b.bit();
    s.sao = b.bit();
    s.pcm = b.bit();
    if (s.pcm) {
        s.pcm_luma_depth_minus1 = static_cast<u8>(b.read(4));
        s.pcm_chroma_depth_minus1 = static_cast<u8>(b.read(4));
        s.log2_min_pcm_minus3 = static_cast<u8>(b.ue(2));
        s.log2_diff_pcm = static_cast<u8>(b.ue(2));
        s.pcm_loop_filter_disabled = b.bit();
        if (s.pcm_luma_depth_minus1 > 7 || s.pcm_chroma_depth_minus1 > 7 ||
            s.log2_min_pcm_minus3 + s.log2_diff_pcm > 2)
            b.error = HevcError::unsupported;
    }
    s.short_term_count = static_cast<u8>(b.ue(64));
    for (u32 i = 0; i < s.short_term_count && b.error == HevcError::none; ++i)
        hevc_parse_reference_set(b, s.short_term, i, s.short_term_count, &s.short_term[i]);
    s.long_term_present = b.bit();
    if (s.long_term_present) {
        s.long_term_count = static_cast<u8>(b.ue(15));
        for (u32 i = 0; i < s.long_term_count; ++i) {
            s.long_term_lsb[i] = static_cast<u16>(b.read(s.poc_bits));
            if (b.bit())
                s.long_term_used |= static_cast<u16>(1u << i);
        }
    }
    s.temporal_mvp = b.bit();
    s.strong_intra_smoothing = b.bit();
    if (b.bit())
        vui(b, s);
    if (b.bit())
        b.error = HevcError::unsupported;
    if (b.error == HevcError::none)
        b.trailing();
    return b.error;
}
HevcError hevc_sps_id(ConstSlice<u8> nal, u8* id) noexcept {
    auto b = hevc_payload(nal, 33);
    b.read(4);
    const u32 layers = b.read(3);
    b.bit();
    // profile_tier_level of any profile: the id follows it.
    b.read(32);
    b.read(32);
    b.read(32);
    bool profiles[7]{}, levels[7]{};
    for (u32 i = 0; i < layers && i < 7; ++i) {
        profiles[i] = b.bit();
        levels[i] = b.bit();
    }
    if (layers)
        b.read(2 * (8 - layers));
    for (u32 i = 0; i < layers && i < 7; ++i) {
        if (profiles[i]) {
            b.read(32);
            b.read(32);
            b.read(24);
        }
        if (levels[i])
            b.read(8);
    }
    const u32 value = b.ue(15);
    if (layers > 6)
        b.error = HevcError::malformed;
    if (b.error == HevcError::none && id)
        *id = static_cast<u8>(value);
    return id ? b.error : HevcError::invalid_input;
}
HevcError hevc_parse_pps(ConstSlice<u8> nal, const HevcSps& s, HevcPps* output) noexcept {
    if (!output || !s.coded_width || !s.coded_height)
        return HevcError::invalid_input;
    HevcPps p{};
    clear_bytes(&p, sizeof(p));
    copy_value(&p.scaling, s.scaling);
    auto b = hevc_payload(nal, 34);
    p.id = static_cast<u8>(b.ue(63));
    p.sps_id = static_cast<u8>(b.ue(15));
    if (p.sps_id != s.id)
        b.error = HevcError::malformed;
    p.dependent_slices = b.bit();
    p.output_flag_present = b.bit();
    p.extra_slice_bits = static_cast<u8>(b.read(3));
    p.sign_data_hiding = b.bit();
    p.cabac_init = b.bit();
    p.references_l0 = static_cast<u8>(b.ue(14) + 1);
    p.references_l1 = static_cast<u8>(b.ue(14) + 1);
    p.initial_qp_minus26 = static_cast<signed char>(b.se(-26, 25));
    p.constrained_intra = b.bit();
    p.transform_skip = b.bit();
    p.cu_qp_delta = b.bit();
    if (p.cu_qp_delta)
        p.diff_cu_qp_depth = static_cast<u8>(b.ue(s.log2_diff_cb));
    p.cb_qp_offset = static_cast<signed char>(b.se(-12, 12));
    p.cr_qp_offset = static_cast<signed char>(b.se(-12, 12));
    p.slice_chroma_offsets = b.bit();
    p.weighted_pred = b.bit();
    p.weighted_bipred = b.bit();
    p.transquant_bypass = b.bit();
    p.tiles = b.bit();
    p.entropy_sync = b.bit();
    p.uniform_spacing = true;
    const u32 ctb = 1u << (s.log2_min_cb_minus3 + 3 + s.log2_diff_cb),
              columns = (s.coded_width + ctb - 1) / ctb, rows = (s.coded_height + ctb - 1) / ctb;
    if (p.tiles) {
        p.tile_columns_minus1 = static_cast<u8>(b.ue(19));
        p.tile_rows_minus1 = static_cast<u8>(b.ue(21));
        if (p.tile_columns_minus1 >= columns || p.tile_rows_minus1 >= rows)
            b.error = HevcError::malformed;
        p.uniform_spacing = b.bit();
        u32 width{}, height{};
        if (!p.uniform_spacing) {
            for (u32 i = 0; i < p.tile_columns_minus1; ++i) {
                p.column_width_minus1[i] = static_cast<u16>(b.ue(columns - 1));
                width += p.column_width_minus1[i] + 1;
            }
            for (u32 i = 0; i < p.tile_rows_minus1; ++i) {
                p.row_height_minus1[i] = static_cast<u16>(b.ue(rows - 1));
                height += p.row_height_minus1[i] + 1;
            }
            if (width >= columns || height >= rows)
                b.error = HevcError::malformed;
        }
        p.loop_filter_tiles = b.bit();
    }
    p.loop_filter_slices = b.bit();
    if (b.bit()) {
        p.deblocking_override = b.bit();
        p.deblocking_disabled = b.bit();
        if (!p.deblocking_disabled) {
            p.beta_offset_div2 = static_cast<signed char>(b.se(-6, 6));
            p.tc_offset_div2 = static_cast<signed char>(b.se(-6, 6));
        }
    }
    p.scaling_present = b.bit();
    if (p.scaling_present)
        scaling_parse(b, p.scaling);
    p.lists_modification = b.bit();
    p.log2_parallel_merge_minus2 = static_cast<u8>(b.ue(4));
    p.slice_header_extension = b.bit();
    if (b.bit())
        b.error = HevcError::unsupported;
    if (b.error == HevcError::none)
        b.trailing();
    if (b.error == HevcError::none)
        copy_value(output, p);
    return b.error;
}
HevcError hevc_select_parameters(ConstSlice<u8> config, u8 pps_id, HevcConfiguration* output) noexcept {
    if (!output || !config.data || config.count < 23 || config.count > 65536 || pps_id > 63)
        return HevcError::invalid_input;
    if (config[0] != 1 || (config[1] & 31u) != 1)
        return HevcError::unsupported;
    if ((config[13] & 0xf0u) != 0xf0u || (config[15] & 0xfcu) != 0xfcu || (config[16] & 0xfcu) != 0xfcu ||
        (config[17] & 0xf8u) != 0xf8u || (config[18] & 0xf8u) != 0xf8u)
        return HevcError::malformed;
    if ((config[16] & 3u) != 1 || (config[17] & 7u) || (config[18] & 7u))
        return HevcError::unsupported;
    ConstSlice<u8> vps{}, sps{}, pps{};
    u8 wanted_sps = 255, wanted_vps = 255;
    u32 array_total{};
    for (u32 pass = 0; pass < 3; ++pass) {
        u32 offset = 23;
        for (u32 a = 0; a < config[22]; ++a) {
            if (config.count - offset < 3)
                return HevcError::truncated;
            const u8 type = config[offset] & 63u;
            if (config[offset] & 0x40u)
                return HevcError::malformed;
            const u32 count = (u32{config[offset + 1]} << 8) | config[offset + 2];
            offset += 3;
            if (count > 64)
                return HevcError::unsupported;
            if (pass == 0 && (array_total += count) > 128)
                return HevcError::unsupported;
            for (u32 i = 0; i < count; ++i) {
                if (config.count - offset < 2)
                    return HevcError::truncated;
                const u32 bytes = (u32{config[offset]} << 8) | config[offset + 1];
                offset += 2;
                if (bytes < 3 || bytes > config.count - offset)
                    return HevcError::truncated;
                ConstSlice<u8> nal{config.data + offset, bytes};
                if (((nal[0] >> 1) & 63u) != type)
                    return HevcError::malformed;
                // Another layer's parameter sets (HEVC with alpha) are not
                // part of the base-layer chain.
                if (hevc_nal_layer(nal)) {
                    offset += bytes;
                    continue;
                }
                if (pass == 0 && type == 34) {
                    auto b = hevc_payload(nal, 34);
                    const u32 id = b.ue(63), sid = b.ue(15);
                    if (b.error != HevcError::none)
                        return b.error;
                    if (id == pps_id) {
                        if (pps.data &&
                            (pps.count != nal.count || !bytes_equal(pps.data, nal.data, nal.count)))
                            return HevcError::malformed;
                        pps = nal;
                        wanted_sps = static_cast<u8>(sid);
                    }
                }
                if (pass == 1 && type == 33) {
                    auto b = hevc_payload(nal, 33);
                    const u8 vid = static_cast<u8>(b.read(4));
                    const u8 layers = static_cast<u8>(b.read(3));
                    b.bit();
                    profile_tier(b, layers);
                    const u32 id = b.ue(15);
                    if (b.error != HevcError::none)
                        return b.error;
                    if (id == wanted_sps) {
                        if (sps.data)
                            return HevcError::malformed;
                        sps = nal;
                        wanted_vps = vid;
                    }
                }
                if (pass == 2 && type == 32) {
                    auto b = hevc_payload(nal, 32);
                    const u32 id = b.read(4);
                    if (b.error != HevcError::none)
                        return b.error;
                    if (id == wanted_vps) {
                        if (vps.data)
                            return HevcError::malformed;
                        vps = nal;
                    }
                }
                offset += bytes;
            }
        }
        if (offset != config.count)
            return HevcError::malformed;
    }
    if (!vps.data || !sps.data || !pps.data)
        return HevcError::malformed;
    HevcConfiguration c{};
    clear_bytes(&c, sizeof(c));
    u8 vid{}, layers{};
    auto e = parse_vps(vps, &vid, &layers);
    if (e != HevcError::none)
        return e;
    e = hevc_parse_sps(sps, &c.sps);
    if (e != HevcError::none)
        return e;
    if (c.sps.vps_id != vid || c.sps.max_sub_layers > layers)
        return HevcError::malformed;
    e = hevc_parse_pps(pps, c.sps, &c.pps);
    if (e != HevcError::none)
        return e;
    c.nal_length_bytes = static_cast<u8>((config[21] & 3u) + 1);
    if (c.nal_length_bytes == 3)
        return HevcError::unsupported;
    *output = c;
    return HevcError::none;
}
HevcError hevc_validate_parameter(ConstSlice<u8> nal, const HevcConfiguration& c) noexcept {
    if (!nal.data || nal.count < 3)
        return HevcError::invalid_input;
    const u8 type = static_cast<u8>((nal[0] >> 1) & 63u);
    if (type == 32) {
        u8 id{}, layers{};
        const auto e = parse_vps(nal, &id, &layers);
        return e != HevcError::none                                   ? e
               : id == c.sps.vps_id && layers == c.sps.max_sub_layers ? HevcError::none
                                                                      : HevcError::unsupported;
    }
    if (type == 33) {
        HevcSps s{};
        const auto e = hevc_parse_sps(nal, &s);
        return e != HevcError::none                 ? e
               : bytes_equal(&s, &c.sps, sizeof(s)) ? HevcError::none
                                                    : HevcError::unsupported;
    }
    if (type == 34) {
        HevcPps p{};
        const auto e = hevc_parse_pps(nal, c.sps, &p);
        return e != HevcError::none                 ? e
               : bytes_equal(&p, &c.pps, sizeof(p)) ? HevcError::none
                                                    : HevcError::unsupported;
    }
    return HevcError::invalid_input;
}
} // namespace maicord
#pragma clang attribute pop
