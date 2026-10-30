#pragma once
#include "maicord/core/base.hpp"

namespace maicord {
// Working memory for one Opus encode or decode call (the codec uses no stack
// arrays or alloca). It holds nothing between calls: each thread that encodes
// or decodes owns one and passes it to every call, whichever stream it serves.
// Sized for the encoder's worst case; decoding touches at most the first
// 19248 bytes. Equal to maiopus's OPUS_SCRATCH_BYTES (asserted in
// src/media/opus_*.cpp).
inline constexpr u32 kOpusScratchBytes = 44640;
struct alignas(16) OpusScratch {
    u8 bytes[kOpusScratchBytes];
};
} // namespace maicord
