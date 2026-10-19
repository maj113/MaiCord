#pragma once
#include "maicord/text/unicode.hpp"

namespace maicord {
enum class UnicodeBidiStatus : u8 { ok,invalid_input,output_too_small,malformed_paragraph };
enum class UnicodeParagraphDirection : u8 { ltr,rtl,auto_ltr,auto_rtl };
inline constexpr u8 unicode_bidi_removed=255;
// Caller-owned working cells. scalar/original are inputs; classify fills them
// from Unicode. Supplying explicit classes is intended for higher-level
// protocols and conformance tests. link/next are private working fields;
// resolved, explicit_level and level are read-only outputs after resolution.
// level==255 marks X9 removal from visual ordering, not deletion from source.
struct UnicodeBidiCell {
    u32 scalar,link,next;
    UnicodeBidiClass original,resolved;
    u8 explicit_level,level;
};
struct UnicodeBidiParagraph { u32 count;u8 level; };
static_assert(sizeof(UnicodeBidiCell)==16);

UnicodeBidiStatus unicode_bidi_classify(ConstSlice<u32> scalars,Slice<UnicodeBidiCell> cells) noexcept;
// P1 boundary on classified cells: include the first B in this paragraph.
// An empty remainder returns end==first. Higher-level newline conventions
// remain the caller's policy; this function does not rewrite source text.
UnicodeBidiStatus unicode_bidi_paragraph_end(ConstSlice<UnicodeBidiCell> cells,u32 first,u32* end) noexcept;
// UAX #9 rev 51, P2 through I2, on one paragraph. A B is allowed only at the
// end; the caller splits paragraphs (P1). FSI always uses P2/P3's LTR
// fallback; auto_rtl is an explicit higher-level paragraph fallback.
// Needs one u32 scratch slot per cell; no allocation. Buffers must not overlap
// and must stay exclusively owned during resolution. On failure, working
// outputs are unusable and paragraph is cleared.
UnicodeBidiStatus unicode_bidi_resolve(Slice<UnicodeBidiCell> cells,UnicodeParagraphDirection direction,
                                     Slice<u32> scratch,UnicodeBidiParagraph* paragraph) noexcept;
// L1/L2 for [first,end). Does not change the paragraph's resolved levels.
// Output levels are line-relative, visual indices are paragraph-relative.
// Needs end-first slots in each output; the earlier u32 scratch can be reused
// as visual storage. An empty visual slice resolves levels only and still
// counts the visible positions. Returns X9-removed positions as level 255 and
// excludes them from visual order. L3/L4, shaping and caret policy are
// separate stages.
UnicodeBidiStatus unicode_bidi_line(ConstSlice<UnicodeBidiCell> cells,u8 paragraph_level,u32 first,u32 end,
                                  Slice<u8> levels,Slice<u32> visual,u32* visual_count) noexcept;
} // namespace maicord
