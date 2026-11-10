#include "av1_headers.hpp"

// Once per encoded picture, a hundred bits: compiled for size (the bit writer's loop is not unrolled).
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

constexpr u8 kObuSequenceHeader = 1, kObuFrameHeader = 3;

// MSB-first bit writer over a caller's buffer; `overflow` once a write would pass `capacity`. Runs of fields the
// stream shape fixes go in one put, named in order in the comment before it.
struct BitWriter {
    u8* data;
    u32 capacity;
    u32 position; // bits
    bool overflow;

    void put(u32 value, u32 bits) noexcept {
        for (u32 i = bits; i--;) {
            const u32 byte = position >> 3;
            if (byte >= capacity) {
                overflow = true;
                return;
            }
            const u8 mask = static_cast<u8>(0x80u >> (position & 7));
            data[byte] = static_cast<u8>((value >> i & 1) ? data[byte] | mask : data[byte] & ~mask);
            ++position;
        }
    }
};

constexpr u32 bits_for(u32 value) noexcept {
    u32 bits = 1;
    while (bits < 32 && (value >> bits))
        ++bits;
    return bits;
}

// The OBU header with obu_has_size_field, and an obu_size field of `size_bytes` bytes to fill once the payload is known.
void begin_obu(BitWriter* w, u8 type, u32 size_bytes) noexcept {
    w->put(static_cast<u32>(type) << 3 | 0x02, 8);
    for (u32 i = 0; i < size_bytes; ++i)
        w->put(0, 8);
}

// Ends the payload with trailing_bits() (a one, then zeros to the byte boundary), fills the obu_size field (leb128,
// padded to its width) and returns the OBU's bytes; 0 when it does not fit.
u32 end_obu(BitWriter* w, u32 size_bytes) noexcept {
    w->put(1, 1);
    w->put(0, (8 - (w->position & 7)) & 7);
    if (w->overflow)
        return 0;
    const u32 bytes = w->position >> 3;
    const u64 payload = bytes - 1 - size_bytes; // 64 bits: an 8-byte field shifts by up to 49
    if (payload >> (7 * size_bytes))
        return 0;
    for (u32 i = 0; i < size_bytes; ++i)
        w->data[1 + i] = static_cast<u8>((payload >> (7 * i) & 0x7f) | (i + 1 < size_bytes ? 0x80 : 0));
    return bytes;
}

} // namespace

u32 av1_write_sequence_header(const Av1SequenceSettings& s, u8* output, u32 capacity) noexcept {
    if (!output || !s.width || !s.height)
        return 0;
    BitWriter w{output, capacity, 0, false};
    begin_obu(&w, kObuSequenceHeader, 1);
    // seq_profile 0 (3 bits); still_picture, reduced_still_picture_header, timing_info_present_flag and
    // initial_display_delay_present_flag clear; operating_points_cnt_minus_1 0 (5); operating_point_idc[0] 0 (12).
    w.put(0, 24);
    w.put(s.level, 5);
    if (s.level > 7)
        w.put(0, 1); // seq_tier[0]: main
    const u32 width_bits = bits_for(s.width - 1), height_bits = bits_for(s.height - 1);
    w.put(width_bits - 1, 4);
    w.put(height_bits - 1, 4);
    w.put(s.width - 1, width_bits);
    w.put(s.height - 1, height_bits);
    // frame_id_numbers_present_flag, use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter,
    // enable_interintra_compound, enable_masked_compound, enable_warped_motion and enable_dual_filter clear;
    // enable_order_hint set; enable_jnt_comp, enable_ref_frame_mvs, seq_choose_screen_content_tools and
    // seq_force_screen_content_tools clear; order_hint_bits_minus_1 7 (3 bits); enable_superres clear.
    w.put(0b0000'0000'1'0000'111'0, 17);
    w.put(s.enable_cdef, 1);
    // enable_restoration clear; color_config(): high_bitdepth and mono_chrome clear (8-bit colour),
    // color_description_present_flag set.
    w.put(0b0001, 4);
    // color_primaries, transfer_characteristics and matrix_coefficients: BT.709 (8 bits each).
    w.put(0x010101, 24);
    // color_range clear (limited); chroma_sample_position 0 (2 bits); separate_uv_delta_q clear;
    // film_grain_params_present clear.
    w.put(0, 5);
    return end_obu(&w, 1);
}

u32 av1_write_frame_header(const Av1SequenceSettings& s, const Av1FrameSettings& f, u32 size_bytes, u8* output,
                           u32 capacity, Av1FrameHeaderLayout* layout) noexcept {
    if (!output || !layout || !size_bytes || size_bytes > 8 || !f.base_q_idx || f.cdef_bits > 3 ||
        f.interpolation_filter > 4)
        return 0;
    BitWriter w{output, capacity, 0, false};
    begin_obu(&w, kObuFrameHeader, size_bytes);
    // show_existing_frame clear; frame_type KEY_FRAME or INTER_FRAME (2 bits); show_frame set; for an inter frame
    // error_resilient_mode clear (a shown key frame has it implied); disable_cdf_update and frame_size_override_flag
    // clear.
    if (f.key)
        w.put(0b0'00'1'00, 6);
    else
        w.put(0b0'01'1'0'00, 7);
    w.put(f.order_hint, 8);
    if (!f.key) {
        w.put(f.primary_ref_frame, 3);
        // refresh_frame_flags: every slot (implied for a shown key frame); frame_refs_short_signaling clear.
        w.put(0xFF << 1, 9);
        // ref_frame_idx: the one slot for all seven references (3 bits each).
        w.put((f.ref_slot & 7u) * 0b001'001'001'001'001'001'001u, 21);
    }
    w.put(0, 1); // render_and_frame_size_different
    if (!f.key) {
        w.put(f.allow_high_precision_mv, 1);
        w.put(f.interpolation_filter == 4, 1); // is_filter_switchable
        if (f.interpolation_filter != 4)
            w.put(f.interpolation_filter, 2);
        w.put(0, 1); // is_motion_mode_switchable
    }
    // disable_frame_end_update_cdf clear; tile_info(): uniform_tile_spacing_flag set, then one tile, which takes an
    // increment_tile_cols_log2 and an increment_tile_rows_log2 of 0 where the frame is more than one superblock wide
    // or high.
    const u32 increments = (av1_superblocks(s.width) > 1) + (av1_superblocks(s.height) > 1);
    w.put(1u << increments, 2 + increments);
    // quantization_params()
    layout->qindex = w.position;
    w.put(f.base_q_idx, 8);
    w.put(0, 4); // DeltaQYDc, DeltaQUDc and DeltaQUAc delta_coded, using_qmatrix
    layout->segmentation = w.position;
    w.put(0, 2); // segmentation_enabled, delta_q_present (base_q_idx is never 0)
    // loop_filter_params()
    layout->loop_filter = w.position;
    w.put(f.loop_filter_level[0], 6);
    w.put(f.loop_filter_level[1], 6);
    if (f.loop_filter_level[0] || f.loop_filter_level[1]) {
        w.put(f.loop_filter_level[2], 6);
        w.put(f.loop_filter_level[3], 6);
    }
    w.put(f.loop_filter_sharpness, 3);
    w.put(0, 1); // loop_filter_delta_enabled
    // cdef_params()
    layout->cdef = w.position;
    if (s.enable_cdef) {
        w.put(f.cdef_damping_minus_3, 2);
        w.put(f.cdef_bits, 2);
        // Each strength is primary (4 bits) and secondary (2), the 6 bits VA-API takes them as.
        for (u32 i = 0; i < (1u << f.cdef_bits); ++i) {
            w.put(f.cdef_y_strengths[i], 6);
            w.put(f.cdef_uv_strengths[i], 6);
        }
    }
    layout->cdef_size = w.position - layout->cdef;
    w.put(f.tx_mode_select, 1);
    // For an inter frame reference_select clear (skip mode then needs no bits); reduced_tx_set clear; for an inter
    // frame is_global clear for the seven references.
    w.put(0, f.key ? 1 : 9);
    const u32 bytes = end_obu(&w, size_bytes);
    layout->obu_size_byte = 1;
    layout->bits = bytes * 8;
    return bytes;
}

} // namespace maicord
#pragma clang attribute pop
