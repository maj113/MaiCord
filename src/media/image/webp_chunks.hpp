#pragma once

#include "maicord/core/byte_order.hpp"

namespace maicord {

// The RIFF and chunk tags of a WebP file, as little-endian words.
inline constexpr u32 kRiff = byte_fourcc_le('R', 'I', 'F', 'F');
inline constexpr u32 kWebp = byte_fourcc_le('W', 'E', 'B', 'P');
inline constexpr u32 kVp8 = byte_fourcc_le('V', 'P', '8', ' ');
inline constexpr u32 kVp8l = byte_fourcc_le('V', 'P', '8', 'L');
inline constexpr u32 kVp8x = byte_fourcc_le('V', 'P', '8', 'X');
inline constexpr u32 kAlph = byte_fourcc_le('A', 'L', 'P', 'H');
inline constexpr u32 kAnim = byte_fourcc_le('A', 'N', 'I', 'M');
inline constexpr u32 kAnmf = byte_fourcc_le('A', 'N', 'M', 'F');
inline constexpr u32 kIccp = byte_fourcc_le('I', 'C', 'C', 'P');
inline constexpr u32 kExif = byte_fourcc_le('E', 'X', 'I', 'F');
inline constexpr u32 kXmp = byte_fourcc_le('X', 'M', 'P', ' ');

} // namespace maicord
