#include "maicord/media/av1.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u8 kAllFrames = 0xff;
constexpr i32 kWarpedModelOne = 1 << 16; // WARPEDMODEL_PREC_BITS
constexpr signed char kDefaultRefDeltas[8]{1, 0, 0, 0, -1, 0, -1, -1};
constexpr u8 kFeatureBits[8]{8, 6, 6, 6, 6, 3, 0, 0};
constexpr u8 kFeatureMax[8]{255, 63, 63, 63, 63, 7, 0, 0};
constexpr u8 kRemapRestoration[4]{0, 3, 1, 2};

// MSB-first bit reader; an overrun returns zeros and is reported once at the end.
struct Bits {
    const u8* data;
    u32 bits, position;
    bool overrun;
    explicit Bits(ConstSlice<u8> payload) noexcept
        : data(payload.data), bits(payload.count > 0x1fffffffu ? 0xfffffff8u : payload.count * 8u),
          position(0), overrun(false) {
    }
    __declspec(noinline) u32 f(u32 count) noexcept {
        if (count > bits - position) {
            overrun = true;
            position = bits;
            return 0;
        }
        u32 value = 0;
        for (u32 i = 0; i < count; ++i, ++position)
            value = value << 1 | ((data[position >> 3] >> (7 - (position & 7))) & 1u);
        return value;
    }
    __declspec(noinline) bool flag() noexcept {
        return f(1) != 0;
    }
    __declspec(noinline) i32 su(u32 count) noexcept {
        const u32 value = f(count);
        const u32 sign = 1u << (count - 1);
        return (value & sign) ? static_cast<i32>(value) - static_cast<i32>(2 * sign)
                              : static_cast<i32>(value);
    }
    __declspec(noinline) u32 ns(u32 n) noexcept {
        u32 w = 0;
        for (u32 x = n; x; x >>= 1)
            ++w;
        const u32 m = (1u << w) - n;
        const u32 v = f(w - 1);
        return v < m ? v : (v << 1) - m + f(1);
    }
    void uvlc() noexcept {
        u32 zeros = 0;
        while (!overrun && !flag())
            ++zeros;
        if (zeros < 32)
            f(zeros);
    }
    u32 byte_aligned() noexcept {
        position = (position + 7) & ~7u;
        if (position > bits) {
            overrun = true;
            position = bits;
        }
        return position >> 3;
    }
};

__declspec(noinline) i32 relative_distance(const Av1SequenceHeader& sequence, u32 a, u32 b) noexcept {
    if (!sequence.enable_order_hint)
        return 0;
    const i32 m = 1 << (sequence.order_hint_bits - 1);
    const i32 diff = static_cast<i32>(a) - static_cast<i32>(b);
    return (diff & (m - 1)) - (diff & m);
}

u32 tile_log2(u32 block, u32 target) noexcept {
    u32 k = 0;
    while ((block << k) < target)
        ++k;
    return k;
}

void reset_film_grain(Av1FilmGrain* grain) noexcept {
    *grain = {};
}

struct Parser {
    const Av1SequenceHeader& sequence;
    const Av1References& references;
    Av1FrameHeader& h;
    Bits& b;
    u32 mi_columns, mi_rows;
    i32 previous_gm[kAv1RefsPerFrame][6];
    bool intra;

    void superres() noexcept {
        h.use_superres = sequence.enable_superres && b.flag();
        h.superres_denom = h.use_superres ? static_cast<u8>(b.f(3) + 9) : 8;
        h.upscaled_width = h.frame_width;
        h.frame_width = (h.upscaled_width * 8 + h.superres_denom / 2) / h.superres_denom;
        mi_columns = 2 * ((h.frame_width + 7) >> 3);
        mi_rows = 2 * ((h.frame_height + 7) >> 3);
    }
    void frame_size(bool override) noexcept {
        if (override) {
            h.frame_width = b.f(sequence.frame_width_bits) + 1;
            h.frame_height = b.f(sequence.frame_height_bits) + 1;
        } else {
            h.frame_width = sequence.max_width;
            h.frame_height = sequence.max_height;
        }
        superres();
    }
    void render_size() noexcept {
        if (b.flag()) {
            h.render_width = b.f(16) + 1;
            h.render_height = b.f(16) + 1;
        } else {
            h.render_width = h.upscaled_width;
            h.render_height = h.frame_height;
        }
    }
    void frame_size_with_refs(bool override) noexcept {
        for (u32 i = 0; i < kAv1RefsPerFrame; ++i)
            if (b.flag()) {
                const Av1Reference& reference = references[h.ref_frame_idx[i]];
                h.frame_width = reference.upscaled_width;
                h.frame_height = reference.frame_height;
                h.render_width = reference.render_width;
                h.render_height = reference.render_height;
                superres();
                return;
            }
        frame_size(override);
        render_size();
    }

    // Spec 7.8: refs from LAST and GOLDEN plus the order hints of all slots.
    bool set_frame_refs(u32 last, u32 gold) noexcept {
        i32 index[kAv1RefsPerFrame];
        for (auto& value : index)
            value = -1;
        index[0] = static_cast<i32>(last);
        index[3] = static_cast<i32>(gold);
        bool used[kAv1RefFrames]{};
        used[last] = used[gold] = true;
        const i32 current = 1 << (sequence.order_hint_bits - 1);
        i32 shifted[kAv1RefFrames];
        for (u32 i = 0; i < kAv1RefFrames; ++i)
            shifted[i] = current + relative_distance(sequence, references[i].order_hint, h.order_hint);
        if (shifted[last] >= current || shifted[gold] >= current)
            return false;
        // ALTREF: furthest backward; BWDREF, ALTREF2: nearest backward; then
        // LAST2, LAST3, BWDREF, ALTREF2, ALTREF still unset: latest forward.
        constexpr u8 order[]{6, 4, 5, 1, 2, 4, 5, 6};
        for (u32 step = 0; step < sizeof(order); ++step) {
            const u32 reference = order[step];
            const bool backward = step < 3;
            if (index[reference] >= 0)
                continue;
            i32 found = -1, best = 0;
            for (u32 i = 0; i < kAv1RefFrames; ++i) {
                const i32 hint = shifted[i];
                if (used[i] || (backward ? hint < current : hint >= current))
                    continue;
                if (found < 0 || (step && step < 3 ? hint < best : hint >= best)) {
                    found = static_cast<i32>(i);
                    best = hint;
                }
            }
            if (found >= 0) {
                index[reference] = found;
                used[found] = true;
            }
        }
        i32 earliest = -1, earliest_hint = 0;
        for (u32 i = 0; i < kAv1RefFrames; ++i)
            if (earliest < 0 || shifted[i] < earliest_hint) {
                earliest = static_cast<i32>(i);
                earliest_hint = shifted[i];
            }
        for (u32 i = 0; i < kAv1RefsPerFrame; ++i)
            h.ref_frame_idx[i] = static_cast<u8>(index[i] < 0 ? earliest : index[i]);
        return true;
    }

    bool tile_info() noexcept {
        const u32 shift = sequence.use_128x128_superblock ? 5 : 4;
        const u32 sb_columns = (mi_columns + (1u << shift) - 1) >> shift;
        const u32 sb_rows = (mi_rows + (1u << shift) - 1) >> shift;
        const u32 sb_size = shift + 2;
        const u32 max_width_sb = 4096u >> sb_size;
        u32 max_area_sb = (4096u * 2304u) >> (2 * sb_size);
        const u32 min_columns_log2 = tile_log2(max_width_sb, sb_columns);
        const u32 max_columns_log2 = tile_log2(1, minimum<u32>(sb_columns, kAv1MaxTileColumns));
        const u32 max_rows_log2 = tile_log2(1, minimum<u32>(sb_rows, kAv1MaxTileRows));
        const u32 min_tiles_log2 = maximum(min_columns_log2, tile_log2(max_area_sb, sb_rows * sb_columns));
        u32 columns = 0, rows = 0, columns_log2 = 0, rows_log2 = 0;
        if (b.flag()) {
            columns_log2 = min_columns_log2;
            while (columns_log2 < max_columns_log2 && b.flag())
                ++columns_log2;
            const u32 width = (sb_columns + (1u << columns_log2) - 1) >> columns_log2;
            for (u32 start = 0; start < sb_columns; start += width)
                h.tile_widths[columns++] = static_cast<u16>(minimum(width, sb_columns - start));
            rows_log2 = min_tiles_log2 > columns_log2 ? min_tiles_log2 - columns_log2 : 0;
            while (rows_log2 < max_rows_log2 && b.flag())
                ++rows_log2;
            const u32 height = (sb_rows + (1u << rows_log2) - 1) >> rows_log2;
            for (u32 start = 0; start < sb_rows; start += height)
                h.tile_heights[rows++] = static_cast<u16>(minimum(height, sb_rows - start));
        } else {
            u32 widest = 0;
            for (u32 start = 0; start < sb_columns && !b.overrun; ++columns) {
                if (columns == kAv1MaxTileColumns)
                    return false;
                const u32 size = b.ns(minimum(sb_columns - start, max_width_sb)) + 1;
                h.tile_widths[columns] = static_cast<u16>(size);
                widest = maximum(widest, size);
                start += size;
            }
            columns_log2 = tile_log2(1, columns);
            max_area_sb =
                min_tiles_log2 ? (sb_rows * sb_columns) >> (min_tiles_log2 + 1) : sb_rows * sb_columns;
            const u32 max_height_sb = maximum<u32>(widest ? max_area_sb / widest : 1, 1);
            for (u32 start = 0; start < sb_rows && !b.overrun; ++rows) {
                if (rows == kAv1MaxTileRows)
                    return false;
                const u32 size = b.ns(minimum(sb_rows - start, max_height_sb)) + 1;
                h.tile_heights[rows] = static_cast<u16>(size);
                start += size;
            }
            rows_log2 = tile_log2(1, rows);
        }
        if (columns > kAv1MaxTileColumns || rows > kAv1MaxTileRows || !columns || !rows)
            return false;
        h.tile_columns = static_cast<u8>(columns);
        h.tile_rows = static_cast<u8>(rows);
        h.tile_columns_log2 = static_cast<u8>(columns_log2);
        h.tile_rows_log2 = static_cast<u8>(rows_log2);
        if (columns_log2 || rows_log2) {
            h.context_update_tile_id = static_cast<u16>(b.f(rows_log2 + columns_log2));
            h.tile_size_bytes = static_cast<u8>(b.f(2) + 1);
            if (h.context_update_tile_id >= columns * rows)
                return false;
        }
        return true;
    }

    signed char delta_q() noexcept {
        return static_cast<signed char>(b.flag() ? b.su(7) : 0);
    }
    void quantization() noexcept {
        h.base_q_idx = static_cast<u8>(b.f(8));
        h.delta_q_y_dc = delta_q();
        if (!sequence.mono_chrome) {
            const bool different = sequence.separate_uv_delta_q && b.flag();
            h.delta_q_u_dc = delta_q();
            h.delta_q_u_ac = delta_q();
            h.delta_q_v_dc = different ? delta_q() : h.delta_q_u_dc;
            h.delta_q_v_ac = different ? delta_q() : h.delta_q_u_ac;
        }
        h.qm_y = h.qm_u = h.qm_v = 0xff;
        if (b.flag()) {
            h.qm_y = static_cast<u8>(b.f(4));
            h.qm_u = static_cast<u8>(b.f(4));
            h.qm_v = sequence.separate_uv_delta_q ? static_cast<u8>(b.f(4)) : h.qm_u;
        }
    }
    void segmentation() noexcept {
        h.segmentation_enabled = b.flag();
        if (!h.segmentation_enabled) {
            clear_bytes(h.feature_enabled, sizeof(h.feature_enabled));
            clear_bytes(h.feature_data, sizeof(h.feature_data));
            return;
        }
        if (h.primary_ref_frame == kAv1PrimaryRefNone) {
            h.segmentation_update_map = true;
            h.segmentation_update_data = true;
        } else {
            h.segmentation_update_map = b.flag();
            h.segmentation_temporal_update = h.segmentation_update_map && b.flag();
            h.segmentation_update_data = b.flag();
        }
        if (!h.segmentation_update_data)
            return; // Kept from the primary reference (load_previous).
        for (u32 i = 0; i < 8; ++i) {
            h.feature_enabled[i] = 0;
            for (u32 j = 0; j < 8; ++j) {
                i32 value = 0;
                if (b.flag()) {
                    h.feature_enabled[i] |= static_cast<u8>(1u << j);
                    const i32 limit = kFeatureMax[j];
                    if (j < 5) {
                        value = b.su(1 + kFeatureBits[j]);
                        value = value < -limit ? -limit : value > limit ? limit : value;
                    } else {
                        value = static_cast<i32>(b.f(kFeatureBits[j]));
                        value = value > limit ? limit : value;
                    }
                }
                h.feature_data[i][j] = static_cast<i16>(value);
            }
        }
    }
    // Every segment's qindex (clipped at 0) is 0 and no DC/AC delta is set.
    void coded_lossless() noexcept {
        h.coded_lossless =
            !(h.delta_q_y_dc | h.delta_q_u_dc | h.delta_q_u_ac | h.delta_q_v_dc | h.delta_q_v_ac);
        for (u32 segment = 0; segment < 8 && h.coded_lossless; ++segment) {
            const bool alternative = h.segmentation_enabled && (h.feature_enabled[segment] & 1);
            h.coded_lossless = h.base_q_idx + (alternative ? h.feature_data[segment][0] : 0) <= 0;
        }
    }
    void loop_filter() noexcept {
        if (h.coded_lossless || h.allow_intrabc) {
            copy_bytes(h.loop_filter_ref_deltas, kDefaultRefDeltas, 8);
            h.loop_filter_mode_deltas[0] = h.loop_filter_mode_deltas[1] = 0;
            return;
        }
        h.loop_filter_level[0] = static_cast<u8>(b.f(6));
        h.loop_filter_level[1] = static_cast<u8>(b.f(6));
        if (!sequence.mono_chrome && (h.loop_filter_level[0] || h.loop_filter_level[1])) {
            h.loop_filter_level[2] = static_cast<u8>(b.f(6));
            h.loop_filter_level[3] = static_cast<u8>(b.f(6));
        }
        h.loop_filter_sharpness = static_cast<u8>(b.f(3));
        h.loop_filter_delta_enabled = b.flag();
        if (!h.loop_filter_delta_enabled)
            return;
        h.loop_filter_delta_update = b.flag();
        if (!h.loop_filter_delta_update)
            return;
        for (auto& delta : h.loop_filter_ref_deltas)
            if (b.flag())
                delta = static_cast<signed char>(b.su(7));
        for (auto& delta : h.loop_filter_mode_deltas)
            if (b.flag())
                delta = static_cast<signed char>(b.su(7));
    }
    void cdef() noexcept {
        if (h.coded_lossless || h.allow_intrabc || !sequence.enable_cdef)
            return;
        h.cdef_damping_minus_3 = static_cast<u8>(b.f(2));
        h.cdef_bits = static_cast<u8>(b.f(2));
        for (u32 i = 0; i < (1u << h.cdef_bits); ++i) {
            h.cdef_y_primary[i] = static_cast<u8>(b.f(4));
            h.cdef_y_secondary[i] = static_cast<u8>(b.f(2));
            if (!sequence.mono_chrome) {
                h.cdef_uv_primary[i] = static_cast<u8>(b.f(4));
                h.cdef_uv_secondary[i] = static_cast<u8>(b.f(2));
            }
        }
    }
    void restoration() noexcept {
        h.log2_restoration_unit_size[0] = h.log2_restoration_unit_size[1] =
            h.log2_restoration_unit_size[2] = 8;
        const bool all_lossless = h.coded_lossless && h.frame_width == h.upscaled_width;
        if (all_lossless || h.allow_intrabc || !sequence.enable_restoration)
            return;
        bool used = false, chroma = false;
        for (u32 plane = 0; plane < (sequence.mono_chrome ? 1u : 3u); ++plane) {
            h.restoration_type[plane] = kRemapRestoration[b.f(2)];
            used = used || h.restoration_type[plane];
            chroma = chroma || (plane && h.restoration_type[plane]);
        }
        if (!used)
            return;
        u32 shift = b.f(1);
        if (sequence.use_128x128_superblock)
            ++shift;
        else if (shift)
            shift += b.f(1);
        const u32 uv_shift = sequence.subsampling_x && sequence.subsampling_y && chroma ? b.f(1) : 0;
        h.log2_restoration_unit_size[0] = static_cast<u8>(6 + shift);
        h.log2_restoration_unit_size[1] = h.log2_restoration_unit_size[2] =
            static_cast<u8>(6 + shift - uv_shift);
    }
    void skip_mode() noexcept {
        if (intra || !h.reference_select || !sequence.enable_order_hint)
            return;
        i32 forward = -1, backward = -1, second = -1;
        u32 forward_hint = 0, backward_hint = 0, second_hint = 0;
        for (u32 i = 0; i < kAv1RefsPerFrame; ++i) {
            const u32 hint = references[h.ref_frame_idx[i]].order_hint;
            if (relative_distance(sequence, hint, h.order_hint) < 0) {
                if (forward < 0 || relative_distance(sequence, hint, forward_hint) > 0) {
                    forward = static_cast<i32>(i);
                    forward_hint = hint;
                }
            } else if (relative_distance(sequence, hint, h.order_hint) > 0 &&
                       (backward < 0 || relative_distance(sequence, hint, backward_hint) < 0)) {
                backward = static_cast<i32>(i);
                backward_hint = hint;
            }
        }
        if (forward < 0)
            return;
        if (backward < 0)
            for (u32 i = 0; i < kAv1RefsPerFrame; ++i) {
                const u32 hint = references[h.ref_frame_idx[i]].order_hint;
                if (relative_distance(sequence, hint, forward_hint) < 0 &&
                    (second < 0 || relative_distance(sequence, hint, second_hint) > 0)) {
                    second = static_cast<i32>(i);
                    second_hint = hint;
                }
            }
        if (backward >= 0 || second >= 0)
            h.skip_mode_present = b.flag();
    }

    // decode_signed_subexp_with_ref() with its helpers (spec 5.9.26-28).
    u32 subexp(u32 symbols) noexcept {
        u32 i = 0, mk = 0;
        for (;;) {
            const u32 b2 = i ? 3 + i - 1 : 3;
            const u32 a = 1u << b2;
            if (symbols <= mk + 3 * a)
                return b.ns(symbols - mk) + mk;
            if (!b.flag())
                return b.f(b2) + mk;
            ++i;
            mk += a;
            if (b.overrun || b2 > 24)
                return 0;
        }
    }
    static i32 recenter(i32 r, i32 v) noexcept {
        return v > 2 * r ? v : (v & 1) ? r - ((v + 1) >> 1) : r + (v >> 1);
    }
    i32 signed_subexp(i32 low, i32 high, i32 r) noexcept {
        const i32 mx = high - low;
        r -= low;
        const i32 v = static_cast<i32>(subexp(static_cast<u32>(mx)));
        const i32 x = (r << 1) <= mx ? recenter(r, v) : mx - 1 - recenter(mx - 1 - r, v);
        return x + low;
    }
    __declspec(noinline) void global_param(u32 type, u32 reference, u32 index) noexcept {
        u32 abs_bits = 12, precision_bits = 15;
        if (index < 2) {
            if (type == 1) { // TRANSLATION
                abs_bits = 9 - !h.allow_high_precision_mv;
                precision_bits = 3 - !h.allow_high_precision_mv;
            } else {
                abs_bits = 12;
                precision_bits = 6;
            }
        }
        const u32 difference = 16 - precision_bits;
        const i32 round = index % 3 == 2 ? kWarpedModelOne : 0;
        const i32 sub = index % 3 == 2 ? 1 << precision_bits : 0;
        const i32 mx = 1 << abs_bits;
        const i32 r = (previous_gm[reference][index] >> difference) - sub;
        h.gm_params[reference][index] = (signed_subexp(-mx, mx + 1, r) << difference) + round;
    }
    void global_motion() noexcept {
        for (u32 reference = 0; reference < kAv1RefsPerFrame; ++reference) {
            h.gm_type[reference] = 0;
            for (u32 i = 0; i < 6; ++i)
                h.gm_params[reference][i] = i % 3 == 2 ? kWarpedModelOne : 0;
        }
        if (intra)
            return;
        for (u32 reference = 0; reference < kAv1RefsPerFrame; ++reference) {
            u32 type = 0;
            if (b.flag())
                type = b.flag() ? 2 : b.flag() ? 1 : 3; // ROTZOOM, TRANSLATION, AFFINE
            h.gm_type[reference] = static_cast<u8>(type);
            if (type >= 2) {
                global_param(type, reference, 2);
                global_param(type, reference, 3);
                if (type == 3) {
                    global_param(type, reference, 4);
                    global_param(type, reference, 5);
                } else {
                    h.gm_params[reference][4] = -h.gm_params[reference][3];
                    h.gm_params[reference][5] = h.gm_params[reference][2];
                }
            }
            if (type >= 1) {
                global_param(type, reference, 0);
                global_param(type, reference, 1);
            }
        }
    }
    bool film_grain() noexcept {
        Av1FilmGrain& g = h.film_grain;
        reset_film_grain(&g);
        if (!sequence.film_grain_params_present || (!h.show_frame && !h.showable_frame))
            return true;
        g.apply_grain = b.flag();
        if (!g.apply_grain)
            return true;
        const u16 seed = static_cast<u16>(b.f(16));
        if (h.frame_type == av1_inter_frame && !b.flag()) {
            const Av1Reference& reference = references[b.f(3)];
            if (!reference.valid)
                return false;
            g = reference.film_grain;
            g.grain_seed = seed;
            return true;
        }
        g.grain_seed = seed;
        g.num_y_points = static_cast<u8>(b.f(4));
        if (g.num_y_points > 14)
            return false;
        for (u32 i = 0; i < g.num_y_points; ++i) {
            g.points_y[i][0] = static_cast<u8>(b.f(8));
            g.points_y[i][1] = static_cast<u8>(b.f(8));
        }
        g.chroma_scaling_from_luma = !sequence.mono_chrome && b.flag();
        if (!sequence.mono_chrome && !g.chroma_scaling_from_luma &&
            !(sequence.subsampling_x && sequence.subsampling_y && !g.num_y_points)) {
            g.num_cb_points = static_cast<u8>(b.f(4));
            if (g.num_cb_points > 10)
                return false;
            for (u32 i = 0; i < g.num_cb_points; ++i) {
                g.points_cb[i][0] = static_cast<u8>(b.f(8));
                g.points_cb[i][1] = static_cast<u8>(b.f(8));
            }
            g.num_cr_points = static_cast<u8>(b.f(4));
            if (g.num_cr_points > 10)
                return false;
            for (u32 i = 0; i < g.num_cr_points; ++i) {
                g.points_cr[i][0] = static_cast<u8>(b.f(8));
                g.points_cr[i][1] = static_cast<u8>(b.f(8));
            }
        }
        g.grain_scaling_minus_8 = static_cast<u8>(b.f(2));
        g.ar_coeff_lag = static_cast<u8>(b.f(2));
        const u32 luma = 2 * g.ar_coeff_lag * (g.ar_coeff_lag + 1);
        const u32 chroma = g.num_y_points ? luma + 1 : luma;
        if (g.num_y_points)
            for (u32 i = 0; i < luma; ++i)
                g.ar_coeffs_y[i] = static_cast<u8>(b.f(8));
        if (g.chroma_scaling_from_luma || g.num_cb_points)
            for (u32 i = 0; i < chroma; ++i)
                g.ar_coeffs_cb[i] = static_cast<u8>(b.f(8));
        if (g.chroma_scaling_from_luma || g.num_cr_points)
            for (u32 i = 0; i < chroma; ++i)
                g.ar_coeffs_cr[i] = static_cast<u8>(b.f(8));
        g.ar_coeff_shift_minus_6 = static_cast<u8>(b.f(2));
        g.grain_scale_shift = static_cast<u8>(b.f(2));
        if (g.num_cb_points) {
            g.cb_mult = static_cast<u8>(b.f(8));
            g.cb_luma_mult = static_cast<u8>(b.f(8));
            g.cb_offset = static_cast<u16>(b.f(9));
        }
        if (g.num_cr_points) {
            g.cr_mult = static_cast<u8>(b.f(8));
            g.cr_luma_mult = static_cast<u8>(b.f(8));
            g.cr_offset = static_cast<u16>(b.f(9));
        }
        g.overlap_flag = b.flag();
        g.clip_to_restricted_range = b.flag();
        return true;
    }

    // Spec 7.21: a shown existing frame takes over its slot's state.
    Av1Status show_existing() noexcept {
        h.frame_to_show_map_idx = static_cast<u8>(b.f(3));
        if (sequence.decoder_model_info_present && !sequence.equal_picture_interval)
            b.f(sequence.frame_presentation_time_bits);
        if (sequence.frame_id_bits)
            b.f(sequence.frame_id_bits);
        h.header_bytes = b.byte_aligned();
        if (b.overrun)
            return Av1Status::truncated;
        const Av1Reference& reference = references[h.frame_to_show_map_idx];
        if (!reference.valid || !reference.showable_frame)
            return Av1Status::malformed;
        static_cast<Av1Reference&>(h) = reference;
        h.show_frame = true;
        h.frame_width = h.upscaled_width;
        // A key frame is shown this way at most once, and then fills every slot.
        h.refresh_frame_flags = h.frame_type == av1_key_frame ? kAllFrames : 0;
        h.showable_frame = h.frame_type != av1_key_frame;
        return Av1Status::ok;
    }

    Av1Status parse(u8 temporal_id, u8 spatial_id) noexcept {
        if (sequence.reduced_still_picture_header) {
            h.frame_type = av1_key_frame;
            h.show_frame = true;
        } else {
            h.show_existing_frame = b.flag();
            if (h.show_existing_frame)
                return show_existing();
            h.frame_type = static_cast<u8>(b.f(2));
            h.show_frame = b.flag();
            if (h.show_frame && sequence.decoder_model_info_present && !sequence.equal_picture_interval)
                b.f(sequence.frame_presentation_time_bits);
            h.showable_frame = h.show_frame ? h.frame_type != av1_key_frame : b.flag();
            h.error_resilient_mode = h.frame_type == av1_switch_frame ||
                                     (h.frame_type == av1_key_frame && h.show_frame) || b.flag();
        }
        intra = av1_frame_is_intra(h);
        h.disable_cdf_update = b.flag();
        h.allow_screen_content_tools =
            sequence.force_screen_content_tools == 2 ? b.flag() : sequence.force_screen_content_tools != 0;
        if (h.allow_screen_content_tools)
            h.force_integer_mv = sequence.force_integer_mv == 2 ? b.flag() : sequence.force_integer_mv != 0;
        if (intra)
            h.force_integer_mv = true;
        if (sequence.frame_id_bits)
            h.frame_id = b.f(sequence.frame_id_bits);
        const bool size_override =
            h.frame_type == av1_switch_frame || (!sequence.reduced_still_picture_header && b.flag());
        h.order_hint = static_cast<u8>(b.f(sequence.order_hint_bits));
        h.primary_ref_frame =
            intra || h.error_resilient_mode ? kAv1PrimaryRefNone : static_cast<u8>(b.f(3));
        if (sequence.decoder_model_info_present && b.flag())
            for (u32 op = 0; op < sequence.operating_points; ++op)
                if (sequence.decoder_model_ops >> op & 1) {
                    const u32 idc = sequence.op_idc[op];
                    if (!idc || ((idc >> temporal_id & 1) && (idc >> (spatial_id + 8) & 1)))
                        b.f(sequence.buffer_removal_time_bits);
                }
        h.refresh_frame_flags =
            h.frame_type == av1_switch_frame || (h.frame_type == av1_key_frame && h.show_frame)
                ? kAllFrames
                : static_cast<u8>(b.f(8));
        if ((!intra || h.refresh_frame_flags != kAllFrames) && h.error_resilient_mode &&
            sequence.enable_order_hint)
            for (u32 i = 0; i < kAv1RefFrames; ++i)
                b.f(sequence.order_hint_bits); // ref_order_hint: slots are trusted as kept.
        if (intra) {
            if (h.frame_type == av1_intra_only_frame && h.refresh_frame_flags == kAllFrames)
                return Av1Status::malformed;
            frame_size(size_override);
            render_size();
            if (h.allow_screen_content_tools && h.upscaled_width == h.frame_width)
                h.allow_intrabc = b.flag();
        } else {
            const bool short_signaling = sequence.enable_order_hint && b.flag();
            if (short_signaling) {
                const u32 last = b.f(3), gold = b.f(3);
                if (!references[last].valid || !references[gold].valid || !set_frame_refs(last, gold))
                    return Av1Status::malformed;
            }
            for (u32 i = 0; i < kAv1RefsPerFrame; ++i) {
                if (!short_signaling)
                    h.ref_frame_idx[i] = static_cast<u8>(b.f(3));
                if (sequence.frame_id_bits)
                    b.f(sequence.delta_frame_id_bits);
                if (!references[h.ref_frame_idx[i]].valid)
                    return Av1Status::malformed;
            }
            if (size_override && !h.error_resilient_mode)
                frame_size_with_refs(size_override);
            else {
                frame_size(size_override);
                render_size();
            }
            h.allow_high_precision_mv = !h.force_integer_mv && b.flag();
            h.interpolation_filter = b.flag() ? 4 : static_cast<u8>(b.f(2));
            h.is_motion_mode_switchable = b.flag();
            h.use_ref_frame_mvs = !h.error_resilient_mode && sequence.enable_ref_frame_mvs && b.flag();
        }
        if (!h.upscaled_width || !h.frame_height || h.upscaled_width > sequence.max_width ||
            h.frame_height > sequence.max_height)
            return Av1Status::malformed;
        h.disable_frame_end_update_cdf =
            sequence.reduced_still_picture_header || h.disable_cdf_update || b.flag();
        if (h.primary_ref_frame == kAv1PrimaryRefNone) {
            copy_bytes(h.loop_filter_ref_deltas, kDefaultRefDeltas, 8);
            for (u32 i = 0; i < kAv1RefsPerFrame; ++i)
                for (u32 j = 0; j < 6; ++j)
                    previous_gm[i][j] = j % 3 == 2 ? kWarpedModelOne : 0;
        } else {
            const Av1Reference& previous = references[h.ref_frame_idx[h.primary_ref_frame]];
            copy_bytes(previous_gm, previous.gm_params, sizeof(previous_gm));
            copy_bytes(h.loop_filter_ref_deltas, previous.loop_filter_ref_deltas, 8);
            copy_bytes(h.loop_filter_mode_deltas, previous.loop_filter_mode_deltas, 2);
            copy_bytes(h.feature_data, previous.feature_data, sizeof(h.feature_data));
            copy_bytes(h.feature_enabled, previous.feature_enabled, 8);
        }
        if (!tile_info())
            return b.overrun ? Av1Status::truncated : Av1Status::malformed;
        quantization();
        segmentation();
        h.delta_q_present = h.base_q_idx && b.flag();
        if (h.delta_q_present)
            h.delta_q_res = static_cast<u8>(b.f(2));
        if (h.delta_q_present) {
            h.delta_lf_present = !h.allow_intrabc && b.flag();
            if (h.delta_lf_present) {
                h.delta_lf_res = static_cast<u8>(b.f(2));
                h.delta_lf_multi = b.flag();
            }
        }
        coded_lossless();
        loop_filter();
        cdef();
        restoration();
        h.tx_mode = h.coded_lossless ? 0 : b.flag() ? 2 : 1;
        h.reference_select = !intra && b.flag();
        skip_mode();
        h.allow_warped_motion =
            !intra && !h.error_resilient_mode && sequence.enable_warped_motion && b.flag();
        h.reduced_tx_set = b.flag();
        global_motion();
        if (!film_grain())
            return b.overrun ? Av1Status::truncated : Av1Status::malformed;
        h.header_bytes = b.byte_aligned();
        return b.overrun ? Av1Status::truncated : Av1Status::ok;
    }
};
} // namespace

namespace {
__declspec(noinline) void store(Av1Reference* slot, const Av1FrameHeader& h) noexcept {
    *slot = h;
    slot->valid = true;
}
} // namespace

Av1Status av1_next_obu(ConstSlice<u8> data, u32* offset, Av1Obu* obu) noexcept {
    if (!offset || !obu || !data.data || *offset >= data.count)
        return Av1Status::truncated;
    u32 at = *offset;
    const u8 header = data[at++];
    if (header & 0x80)
        return Av1Status::malformed;
    Av1Obu result{};
    result.type = (header >> 3) & 15;
    result.extension = (header & 4) != 0;
    if (result.extension) {
        if (at >= data.count)
            return Av1Status::truncated;
        result.temporal_id = data[at] >> 5;
        result.spatial_id = (data[at] >> 3) & 3;
        ++at;
    }
    u32 size = data.count - at;
    if (header & 2) {
        u64 value = 0;
        for (u32 i = 0;; ++i) {
            if (i == 8 || at >= data.count)
                return Av1Status::truncated;
            const u8 byte = data[at++];
            value |= u64{byte & 0x7fu} << (7 * i);
            if (!(byte & 0x80))
                break;
        }
        if (value > data.count - at)
            return Av1Status::truncated;
        size = static_cast<u32>(value);
    }
    result.payload = {data.data + at, size};
    *offset = at + size;
    *obu = result;
    return Av1Status::ok;
}

Av1Status av1_parse_sequence_header(ConstSlice<u8> payload, Av1SequenceHeader* output) noexcept {
    if (!output || !payload.data)
        return Av1Status::truncated;
    Bits b(payload);
    Av1SequenceHeader s{};
    s.profile = static_cast<u8>(b.f(3));
    if (s.profile > 2)
        return Av1Status::unsupported;
    b.f(1); // still_picture
    s.reduced_still_picture_header = b.flag();
    u32 buffer_delay_bits = 0;
    if (s.reduced_still_picture_header) {
        s.operating_points = 1;
        b.f(5); // seq_level_idx
    } else {
        if (b.flag()) { // timing_info_present_flag
            b.f(32);
            b.f(32);
            s.equal_picture_interval = b.flag();
            if (s.equal_picture_interval)
                b.uvlc();
            s.decoder_model_info_present = b.flag();
            if (s.decoder_model_info_present) {
                buffer_delay_bits = b.f(5) + 1;
                b.f(32);
                s.buffer_removal_time_bits = static_cast<u8>(b.f(5) + 1);
                s.frame_presentation_time_bits = static_cast<u8>(b.f(5) + 1);
            }
        }
        const bool initial_display_delay = b.flag();
        s.operating_points = static_cast<u8>(b.f(5) + 1);
        for (u32 i = 0; i < s.operating_points; ++i) {
            s.op_idc[i] = static_cast<u16>(b.f(12));
            if (b.f(5) > 7)
                b.f(1); // seq_tier
            if (s.decoder_model_info_present && b.flag()) {
                s.decoder_model_ops |= 1u << i;
                b.f(buffer_delay_bits);
                b.f(buffer_delay_bits);
                b.f(1);
            }
            if (initial_display_delay && b.flag())
                b.f(4);
        }
    }
    s.operating_point_idc = s.op_idc[0];
    s.frame_width_bits = static_cast<u8>(b.f(4) + 1);
    s.frame_height_bits = static_cast<u8>(b.f(4) + 1);
    s.max_width = b.f(s.frame_width_bits) + 1;
    s.max_height = b.f(s.frame_height_bits) + 1;
    if (!s.reduced_still_picture_header && b.flag()) {
        const u32 delta = b.f(4) + 2;
        s.delta_frame_id_bits = static_cast<u8>(delta);
        s.frame_id_bits = static_cast<u8>(b.f(3) + 1 + delta);
    }
    s.use_128x128_superblock = b.flag();
    s.enable_filter_intra = b.flag();
    s.enable_intra_edge_filter = b.flag();
    s.force_screen_content_tools = s.force_integer_mv = 2;
    if (!s.reduced_still_picture_header) {
        s.enable_interintra_compound = b.flag();
        s.enable_masked_compound = b.flag();
        s.enable_warped_motion = b.flag();
        s.enable_dual_filter = b.flag();
        s.enable_order_hint = b.flag();
        if (s.enable_order_hint) {
            s.enable_jnt_comp = b.flag();
            s.enable_ref_frame_mvs = b.flag();
        }
        if (!b.flag())
            s.force_screen_content_tools = static_cast<u8>(b.f(1));
        if (s.force_screen_content_tools && !b.flag())
            s.force_integer_mv = static_cast<u8>(b.f(1));
        if (s.enable_order_hint)
            s.order_hint_bits = static_cast<u8>(b.f(3) + 1);
    }
    s.enable_superres = b.flag();
    s.enable_cdef = b.flag();
    s.enable_restoration = b.flag();
    // color_config()
    const bool high_bitdepth = b.flag();
    s.bit_depth = s.profile == 2 && high_bitdepth ? (b.flag() ? 12 : 10) : high_bitdepth ? 10 : 8;
    s.mono_chrome = s.profile != 1 && b.flag();
    u32 primaries = 2, transfer = 2;
    s.matrix_coefficients = 2;
    if (b.flag()) {
        primaries = b.f(8);
        transfer = b.f(8);
        s.matrix_coefficients = static_cast<u8>(b.f(8));
    }
    if (s.mono_chrome) {
        s.color_range = b.flag();
        s.subsampling_x = s.subsampling_y = true;
    } else if (primaries == 1 && transfer == 13 && s.matrix_coefficients == 0) {
        s.color_range = true; // sRGB 4:4:4
    } else {
        s.color_range = b.flag();
        if (s.profile == 0) {
            s.subsampling_x = s.subsampling_y = true;
        } else if (s.profile == 2) {
            s.subsampling_x = s.bit_depth != 12 || b.flag();
            s.subsampling_y = s.subsampling_x && s.bit_depth == 12 && b.flag();
        }
        if (s.subsampling_x && s.subsampling_y)
            b.f(2); // chroma_sample_position
    }
    s.separate_uv_delta_q = !s.mono_chrome && b.flag();
    s.film_grain_params_present = b.flag();
    if (b.overrun)
        return Av1Status::truncated;
    *output = s;
    return Av1Status::ok;
}

Av1Status av1_parse_frame_header(const Av1SequenceHeader& sequence, const Av1References& references,
                                 ConstSlice<u8> payload, u8 temporal_id, u8 spatial_id,
                                 Av1FrameHeader* output) noexcept {
    if (!output || !payload.data || !sequence.max_width)
        return Av1Status::truncated;
    Bits b(payload);
    Parser parser{sequence, references, *output, b, 0, 0, {}, false};
    clear_bytes(output, sizeof(*output));
    const Av1Status status = parser.parse(temporal_id, spatial_id);
    if (status != Av1Status::ok)
        clear_bytes(output, sizeof(*output));
    return status;
}

void av1_update_references(Av1References& references, const Av1FrameHeader& h) noexcept {
    for (u32 i = 0; i < kAv1RefFrames; ++i)
        if (h.refresh_frame_flags >> i & 1)
            store(&references[i], h);
}

Av1Status av1_parse_tile_group(const Av1FrameHeader& header, ConstSlice<u8> payload, Slice<Av1Tile> tiles,
                               u32* tile_start, u32* tile_end) noexcept {
    if (!payload.data || !tile_start || !tile_end || !header.tile_columns || !header.tile_rows)
        return Av1Status::truncated;
    const u32 count = u32{header.tile_columns} * header.tile_rows;
    Bits b(payload);
    u32 start = 0, end = count - 1;
    if (count > 1 && b.flag()) {
        const u32 bits = header.tile_columns_log2 + header.tile_rows_log2;
        start = b.f(bits);
        end = b.f(bits);
    }
    u32 offset = b.byte_aligned();
    if (b.overrun)
        return Av1Status::truncated;
    if (start > end || end >= count)
        return Av1Status::malformed;
    if (end - start + 1 > tiles.count)
        return Av1Status::unsupported;
    for (u32 tile = start, k = 0; tile <= end; ++tile, ++k) {
        u32 size = payload.count - offset;
        if (tile != end) {
            if (header.tile_size_bytes > payload.count - offset)
                return Av1Status::truncated;
            size = 0;
            for (u32 i = 0; i < header.tile_size_bytes; ++i)
                size |= u32{payload[offset++]} << (8 * i);
            if (size >= payload.count - offset)
                return Av1Status::truncated;
            ++size;
        }
        if (!size)
            return Av1Status::truncated;
        tiles[k] = {offset, size, static_cast<u16>(tile / header.tile_columns),
                    static_cast<u16>(tile % header.tile_columns)};
        offset += size;
    }
    *tile_start = start;
    *tile_end = end;
    return Av1Status::ok;
}
} // namespace maicord
