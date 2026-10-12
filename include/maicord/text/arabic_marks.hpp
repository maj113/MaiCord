#pragma once
#include "maicord/text/sfnt.hpp"

namespace maicord {
// True for a scalar that takes part in the Arabic combining sequence.
// This is a script profile property, not the Unicode general Mark category.
bool sfnt_arabic_combining_mark(u32 scalar) noexcept;
// MaiCord's Arabic mark-order profile (matching Windows), applied before GSUB. Produces
// a permutation of scalar indices; does not change the source text, normalize,
// delete controls or insert dotted circles. Equal-class order stays stable.
// Recognized classes are 27..35, 220, and 230 with Arabic Script_Extensions.
// Other characters form barriers. Within each span, leading modifier marks of
// classes 220/230 precede shadda, then the remaining canonical-class groups.
// Uses 11 fixed buckets, no allocation and no quadratic insertion sort. The caller
// may reuse the character-map storage that is not yet in use for the permutation.
// Input/output must be disjoint. Capacity, scalar and work checks happen before any
// output is written. Work reserves 16 units per input scalar.
// reorder=false validates and writes the identity permutation in two linear
// passes (two work units per scalar), for forced-LTR input mark order.
SfntStatus sfnt_arabic_mark_order(ConstSlice<u32> scalars,Slice<u32> order,u64 work_limit=1000000,bool reorder=true) noexcept;
} // namespace maicord
