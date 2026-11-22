#pragma once
#include "maicord/text/unicode/unicode.hpp"

namespace maicord {
enum class UnicodeParagraphDirection : u8 { ltr,rtl,auto_ltr,auto_rtl };
inline constexpr u8 unicode_bidi_removed=255;
// Caller-owned working cells. scalar/original are inputs: a Unicode scalar and
// its bidi class from the Unicode properties or, for higher-level protocols and
// conformance tests, an explicit class. link/next are private working fields;
// resolved, explicit_level and level are read-only outputs after resolution.
// level==255 marks X9 removal from visual ordering, not deletion from source.
struct UnicodeBidiCell {
    u32 scalar,link,next;
    UnicodeBidiClass original,resolved;
    u8 explicit_level,level;
};
static_assert(sizeof(UnicodeBidiCell)==16);
// P1 boundary on classified cells from first < cells.count: the end of the
// paragraph, past its first B. Higher-level newline conventions remain the
// caller's policy; this function does not rewrite source text.
u32 unicode_bidi_paragraph_end(ConstSlice<UnicodeBidiCell> cells,u32 first) noexcept;
// L1 for a line of resolved cells: levels gets one level per cell (X9-removed
// cells keep 255). Does not change the paragraph's resolved levels. Visual
// order (L2), L3/L4, shaping and caret policy are separate stages.
void unicode_bidi_line_levels(ConstSlice<UnicodeBidiCell> cells,u8 paragraph_level,u8* levels) noexcept;
} // namespace maicord
