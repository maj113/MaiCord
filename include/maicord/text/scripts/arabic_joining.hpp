#pragma once
#include "maicord/text/unicode/unicode.hpp"

namespace maicord {
enum class ArabicJoiningStatus : u8 {ok,invalid_input,output_too_small,work_limit};
enum class ArabicJoiningForm : u8 {none=0,isolated=1,final=2,medial=4,initial=8};
// HarfBuzz's Arabic joining-form eligibility in logical scalar order, before substitutions.
// The caller supplies a complete joining context, including transparent marks
// and join controls; a font/style boundary is not necessarily a joining boundary.
// Nonjoining and transparent characters receive none; a join-causing one (ZWJ,
// tatweel) joins as a dual-joining one, as in HarfBuzz. No normalization or control
// removal.
// Writes one byte at i*stride; other bytes are untouched, so existing glyph
// metadata can hold the forms. The output span must not overlap scalars.
// Invalid arguments, short storage and insufficient work (2*scalar_count units)
// leave output unchanged. The scalars must be valid Unicode scalar values.
// The context is the text around the scalars (HarfBuzz pre- and post-context): whether
// its nearest non-transparent character before them joins forward and after them joins
// backward, so a run cut from a word keeps the forms of its letters.
struct ArabicJoiningContext {bool before,after;};
ArabicJoiningStatus arabic_joining_forms(ConstSlice<u32> scalars,Slice<u8> output,
                                       u32 stride=1,u64 work_limit=1000000,ArabicJoiningContext context={}) noexcept;
// The joining context of text[begin,end) within its paragraph, read as HarfBuzz does from at
// most five characters on each side that are not transparent, stopping at a paragraph separator.
ArabicJoiningContext arabic_joining_context(Utf8View text,u32 begin,u32 end) noexcept;
} // namespace maicord
