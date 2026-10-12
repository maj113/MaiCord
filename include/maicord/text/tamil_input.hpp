#pragma once
#include "maicord/text/indic_input.hpp"
namespace maicord {
// Original UTF-16 source offset survives expansion. Both pieces of a split
// vowel come from the same input scalar; they must not become separate caret stops.
using SfntTamilInputScalar=SfntIndicInputScalar;
static_assert(sizeof(SfntTamilInputScalar)==8);
using SfntTamilInputResult=SfntIndicInputResult;
// Expand Tamil split dependent vowels for ONE already identified input cluster.
// Segment the original input first: an orphan composite vowel gets one virtual
// circle, while two separately typed orphan signs can require two. Independent
// AU (U+0B94) remains nominal; this is shaping expansion, not Unicode NFD/NFC.
// Other valid scalars pass through. No allocation, mapping, circles or reorder.
// Output holds at most twice input.count records. All views/result are disjoint.
// Invalid scalars, overlap, capacity or work errors (three units per input
// scalar) precede output writes. On output_too_small, result reports required
// sizes; other errors clear it.
SfntStatus sfnt_tamil_expand_cluster(ConstSlice<u32>,Slice<SfntTamilInputScalar>,
    SfntTamilInputResult*,u64 work_limit=1000000) noexcept;
}
