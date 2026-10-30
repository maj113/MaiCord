#pragma once
#include "maicord/text/sfnt.hpp"

namespace maicord {
// True for a scalar that takes part in the Arabic combining sequence; the
// argument must be a Unicode scalar value.
// This is a script profile property, not the Unicode general Mark category.
bool sfnt_arabic_combining_mark(u32 scalar) noexcept;
// Windows-compatible Arabic mark-order profile, applied before GSUB. Writes a
// permutation of scalar indices; the text itself is not changed. Marks of equal
// class keep their order. Recognized marks are canonical classes 27..35, 220
// and 230 with Arabic Script_Extensions; other characters are barriers. Within
// a span, leading modifier marks of classes 220/230 precede shadda, then the
// remaining canonical-class groups.
// Input and output must be disjoint; all checks run before any write. Work
// reserves 16 units per scalar. reorder=false only validates and writes the
// identity permutation (2 units per scalar), for forced-LTR input mark order.
SfntStatus sfnt_arabic_mark_order(ConstSlice<u32> scalars,Slice<u32> order,u64 work_limit=1000000,bool reorder=true) noexcept;
} // namespace maicord
