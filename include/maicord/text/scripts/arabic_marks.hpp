#pragma once
#include "maicord/text/opentype/sfnt.hpp"

namespace maicord {
// True for a scalar that takes part in the Arabic combining sequence; the
// argument must be a Unicode scalar value.
// This is a script profile property, not the Unicode general Mark category.
bool sfnt_arabic_combining_mark(u32 scalar) noexcept;
// HarfBuzz's Arabic mark order, applied before GSUB: each run of up to 32 marks of
// nonzero class sorted by HarfBuzz's modified combining class (shadda first), then its
// leading modifier combining marks of classes 220 and 230 moved to the front
// (reorder_marks_arabic). Writes a permutation of scalar indices; the text itself is
// not changed. Marks of equal class keep their order; a longer run keeps its order.
// Input and output must be disjoint; all checks run before any write. Work
// reserves 16 units per scalar.
SfntStatus sfnt_arabic_mark_order(ConstSlice<u32> scalars,Slice<u32> order,u64 work_limit=1000000) noexcept;
} // namespace maicord
