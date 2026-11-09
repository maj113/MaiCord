#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// The AV1 OBU headers a VA-API encoder takes from the application as packed headers: the sequence header and each
// picture's frame header, with the bit positions the driver patches after its rate control (base_q_idx, the loop
// filter and CDEF parameters) and the position of the frame header's obu_size field.
//
// One stream shape, the one the Go Live encoder uses: profile 0, 8-bit 4:2:0, 64x64 superblocks, BT.709 limited range,
// order hints of 8 bits, no screen content tools, superres, loop restoration, segmentation or film grain, one tile.
// Key frames refresh every slot; inter frames reference the slot of the picture before them through every reference
// and also refresh every slot. Pure: the tests parse the output with MaiCord's own AV1 parser.

struct Av1SequenceSettings {
    u32 width, height; // the coded frame, at most 4096 x 2304
    u8 level;          // seq_level_idx (4.1 is 9, 5.1 is 13)
    bool enable_cdef;
};

struct Av1FrameSettings {
    bool key;
    u8 order_hint;
    u8 primary_ref_frame; // inter frames: 0 (LAST) or 7 (none)
    u8 ref_slot;          // inter frames: the slot every reference names
    u8 interpolation_filter; // 0-3 a fixed filter, 4 switchable
    bool allow_high_precision_mv;
    u8 base_q_idx;        // 1-255
    u8 loop_filter_level[4];
    u8 loop_filter_sharpness;
    u8 cdef_damping_minus_3, cdef_bits;
    u8 cdef_y_strengths[8], cdef_uv_strengths[8]; // primary << 2 | secondary, as VA-API takes them
    bool tx_mode_select;  // else the largest transform
};

// Bit positions from the first bit of the frame header OBU (its obu_header byte), as VA-API takes them.
struct Av1FrameHeaderLayout {
    u32 qindex, segmentation, loop_filter, cdef, cdef_size;
    u32 obu_size_byte; // where the obu_size field starts
    u32 bits;          // the whole OBU, trailing bits included
};

// The superblock columns and rows of a frame (64x64 superblocks).
constexpr u32 av1_superblocks(u32 pixels) noexcept {
    return (2 * ((pixels + 7) >> 3) + 15) >> 4;
}

// The sequence header OBU (with obu_size) into `output`. The bytes written; 0 when `capacity` is too small.
u32 av1_write_sequence_header(const Av1SequenceSettings& sequence, u8* output, u32 capacity) noexcept;

// The frame header OBU (OBU_FRAME_HEADER) with an obu_size field of `size_bytes` bytes (1-8: drivers rewrite the size
// in place, so a field wider than the value needs is padded). The bytes written; 0 when `capacity` is too small.
u32 av1_write_frame_header(const Av1SequenceSettings& sequence, const Av1FrameSettings& frame, u32 size_bytes,
                           u8* output, u32 capacity, Av1FrameHeaderLayout* layout) noexcept;

} // namespace maicord
