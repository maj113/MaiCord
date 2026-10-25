#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {

inline constexpr u32 kRegionalFlagAssetCount = 262;
inline constexpr char kRegionalFlagSvgTargetPrefix[] =
    "/jdecked/twemoji/b6b55fef1e8636b540a6d016a4729ca8cdf2e60b/assets/svg/";
inline constexpr u32 kRegionalFlagSvgTargetPrefixBytes = sizeof(kRegionalFlagSvgTargetPrefix) - 1;
inline constexpr u32 kRegionalFlagSvgTargetBytes = kRegionalFlagSvgTargetPrefixBytes + 45;

// Exact Unicode flag identity: RI pairs use 1 + first-letter * 26 + second;
// 677..679 identify England, Scotland and Wales. Zero means unsupported.
// Only complete, pinned-upstream-supported sequences are admitted; a lone
// indicator or an unrecognized tag sequence yields zero.
u16 regional_flag_sequence(Utf8View glyph) noexcept;

struct RegionalFlagRange { u32 first_byte, end_byte; u16 sequence; };
// Sequential scanner over unchanged UTF-8. Unsupported RI pairs are consumed
// as pairs; a third indicator cannot shift the identity of the first pair.
// Complete tag sequences and emoji extensions are consumed before admission.
// False means end or malformed input, with no published range. No allocation.
bool regional_flag_next(Utf8View text, u32* cursor, RegionalFlagRange* range) noexcept;

// Writes the pinned upstream prefix and canonical lowercase Unicode filename.
// TargetBytes is maximum capacity; RI targets are shorter than subdivisions.
// No caller-controlled URL, revision, asset hash or path is accepted.
// Returns zero without writing when identity/capacity is invalid.
u32 regional_flag_svg_target(u16 sequence, Slice<char> output) noexcept;


} // namespace maicord
