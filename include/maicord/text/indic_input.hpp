#pragma once
#include "maicord/text/indic_plan.hpp"
namespace maicord {
struct SfntIndicInputScalar {u32 scalar,source;};
static_assert(sizeof(SfntIndicInputScalar)==8);
struct SfntIndicInputResult {u32 scalar_count,source_units;};
// Expands dependent vowels within one original, already segmented cluster. Pieces
// share the original UTF-16 source position. Independent vowels stay nominal; other
// valid scalars pass through, without circles or reordering. All views and result
// are disjoint. Errors precede output writes. A sizing failure reports both counts;
// other failures clear them. Three work units per scalar.
// Optional extra_scalar_limit caps added pieces in logical order. A multi-part
// expansion is all-or-none, and the first one that exceeds the limit ends expansion
// for this cluster, leaving later composites nominal. This is a shaping policy
// independent of storage capacity and never truncates the original input.
SfntStatus sfnt_indic_expand_cluster(SfntIndicProfile,ConstSlice<u32>,Slice<SfntIndicInputScalar>,
    SfntIndicInputResult*,u64 work_limit=1000000,u32 extra_scalar_limit=0xffffffffu) noexcept;
}
