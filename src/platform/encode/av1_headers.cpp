#include "av1_headers.hpp"

namespace maicord {
namespace {

constexpr u8 kObuSequenceHeader = 1, kObuFrameHeader = 3;
constexpr u32 kRefsPerFrame = 7;

// MSB-first bit writer over a caller's buffer; `overflow` once a write would pass `capacity`.
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
    void flag(bool value) noexcept {
        put(value ? 1 : 0, 1);
    }
    // trailing_bits(): a one, then zeros to the byte boundary.
    void trailing() noexcept {
        put(1, 1);
        while (position & 7)
            put(0, 1);
    }
};

u32 bits_for(u32 value) noexcept {
    u32 bits = 1;
    while (bits < 32 && (value >> bits))
        ++bits;
    return bits;
}

u32 tile_log2(u32 block, u32 target) noexcept {
    u32 k = 0;
    while ((block << k) < target)
        ++k;
    return k;
}

// The OBU header with obu_has_size_field, and an obu_size field of `size_bytes` bytes to fill once the payload is known.
void begin_obu(BitWriter* w, u8 type, u32 size_bytes) noexcept {
    w->put(static_cast<u32>(type) << 3 | 0x02, 8);
    for (u32 i = 0; i < size_bytes; ++i)
        w->put(0, 8);
}

// Fills the obu_size field (leb128, padded to its width) and returns the OBU's bytes; 0 when it does not fit.
u32 end_obu(BitWriter* w, u32 size_bytes) noexcept {
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
    w.put(0, 3);     // seq_profile
    w.flag(false);   // still_picture
    w.flag(false);   // reduced_still_picture_header
    w.flag(false);   // timing_info_present_flag
    w.flag(false);   // initial_display_delay_present_flag
    w.put(0, 5);     // operating_points_cnt_minus_1
    w.put(0, 12);    // operating_point_idc[0]
    w.put(s.level, 5);
    if (s.level > 7)
        w.flag(false); // seq_tier[0]: main
    const u32 width_bits = bits_for(s.width - 1), height_bits = bits_for(s.height - 1);
    w.put(width_bits - 1, 4);
    w.put(height_bits - 1, 4);
    w.put(s.width - 1, width_bits);
    w.put(s.height - 1, height_bits);
    w.flag(false);   // frame_id_numbers_present_flag
    w.flag(false);   // use_128x128_superblock
    w.flag(false);   // enable_filter_intra
    w.flag(false);   // enable_intra_edge_filter
    w.flag(false);   // enable_interintra_compound
    w.flag(false);   // enable_masked_compound
    w.flag(false);   // enable_warped_motion
    w.flag(false);   // enable_dual_filter
    w.flag(true);    // enable_order_hint
    w.flag(false);   // enable_jnt_comp
    w.flag(false);   // enable_ref_frame_mvs
    w.flag(false);   // seq_choose_screen_content_tools
    w.put(0, 1);     // seq_force_screen_content_tools
    w.put(7, 3);     // order_hint_bits_minus_1
    w.flag(false);   // enable_superres
    w.flag(s.enable_cdef);
    w.flag(false);   // enable_restoration
    // color_config(): 8-bit, colour, BT.709 primaries, transfer and matrix, limited range, 4:2:0.
    w.flag(false);   // high_bitdepth
    w.flag(false);   // mono_chrome
    w.flag(true);    // color_description_present_flag
    w.put(1, 8);     // color_primaries
    w.put(1, 8);     // transfer_characteristics
    w.put(1, 8);     // matrix_coefficients
    w.flag(false);   // color_range
    w.put(0, 2);     // chroma_sample_position
    w.flag(false);   // separate_uv_delta_q
    w.flag(false);   // film_grain_params_present
    w.trailing();
    return end_obu(&w, 1);
}

u32 av1_write_frame_header(const Av1SequenceSettings& s, const Av1FrameSettings& f, u32 size_bytes, u8* output,
                           u32 capacity, Av1FrameHeaderLayout* layout) noexcept {
    if (!output || !layout || !size_bytes || size_bytes > 8 || !f.base_q_idx || f.cdef_bits > 3 ||
        f.interpolation_filter > 4)
        return 0;
    BitWriter w{output, capacity, 0, false};
    begin_obu(&w, kObuFrameHeader, size_bytes);
    w.flag(false);             // show_existing_frame
    w.put(f.key ? 0 : 1, 2);   // frame_type: KEY_FRAME or INTER_FRAME
    w.flag(true);              // show_frame
    if (!f.key)
        w.flag(false);         // error_resilient_mode (a shown key frame has it implied)
    w.flag(false);             // disable_cdf_update
    w.flag(false);             // frame_size_override_flag
    w.put(f.order_hint, 8);
    if (!f.key) {
        w.put(f.primary_ref_frame, 3);
        w.put(0xFF, 8);        // refresh_frame_flags (implied for a shown key frame)
        w.flag(false);         // frame_refs_short_signaling
        for (u32 i = 0; i < kRefsPerFrame; ++i)
            w.put(f.ref_slot, 3);
    }
    w.flag(false);             // render_and_frame_size_different
    if (!f.key) {
        w.flag(f.allow_high_precision_mv);
        w.flag(f.interpolation_filter == 4); // is_filter_switchable
        if (f.interpolation_filter != 4)
            w.put(f.interpolation_filter, 2);
        w.flag(false);         // is_motion_mode_switchable
    }
    w.flag(false);             // disable_frame_end_update_cdf
    // tile_info(): uniform spacing, one tile (no increments where any are possible).
    const u32 columns = av1_superblocks(s.width), rows = av1_superblocks(s.height);
    w.flag(true);              // uniform_tile_spacing_flag
    if (tile_log2(1, columns < 64 ? columns : 64))
        w.flag(false);         // increment_tile_cols_log2
    if (tile_log2(1, rows < 64 ? rows : 64))
        w.flag(false);         // increment_tile_rows_log2
    // quantization_params()
    layout->qindex = w.position;
    w.put(f.base_q_idx, 8);
    w.flag(false);             // DeltaQYDc delta_coded
    w.flag(false);             // DeltaQUDc delta_coded
    w.flag(false);             // DeltaQUAc delta_coded
    w.flag(false);             // using_qmatrix
    layout->segmentation = w.position;
    w.flag(false);             // segmentation_enabled
    w.flag(false);             // delta_q_present (base_q_idx is never 0)
    // loop_filter_params()
    layout->loop_filter = w.position;
    w.put(f.loop_filter_level[0], 6);
    w.put(f.loop_filter_level[1], 6);
    if (f.loop_filter_level[0] || f.loop_filter_level[1]) {
        w.put(f.loop_filter_level[2], 6);
        w.put(f.loop_filter_level[3], 6);
    }
    w.put(f.loop_filter_sharpness, 3);
    w.flag(false);             // loop_filter_delta_enabled
    // cdef_params()
    layout->cdef = w.position;
    if (s.enable_cdef) {
        w.put(f.cdef_damping_minus_3, 2);
        w.put(f.cdef_bits, 2);
        for (u32 i = 0; i < (1u << f.cdef_bits); ++i) {
            w.put(f.cdef_y_strengths[i] >> 2, 4);
            w.put(f.cdef_y_strengths[i] & 3, 2);
            w.put(f.cdef_uv_strengths[i] >> 2, 4);
            w.put(f.cdef_uv_strengths[i] & 3, 2);
        }
    }
    layout->cdef_size = w.position - layout->cdef;
    w.flag(f.tx_mode_select);  // tx_mode_select
    if (!f.key)
        w.flag(false);         // reference_select (skip mode then needs no bits)
    w.flag(false);             // reduced_tx_set
    if (!f.key)
        for (u32 i = 0; i < kRefsPerFrame; ++i)
            w.flag(false);     // is_global
    w.trailing();
    const u32 bytes = end_obu(&w, size_bytes);
    layout->obu_size_byte = 1;
    layout->bits = bytes * 8;
    return bytes;
}

} // namespace maicord
