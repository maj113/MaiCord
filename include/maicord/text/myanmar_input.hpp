#pragma once
#include "maicord/text/indic_input.hpp"
namespace maicord {
// Horizontal modern Myanmar source boundaries for fonts that support Myanmar: one byte
// per original scalar (1=start, 0=continuation). Does not reorder or shape.
// One selector extends its preceding scalar and does not consume the
// 31-character cluster limit. Caller-owned disjoint buffers, no allocation.
// Sixteen work units per input scalar. All validation precedes output writes.
SfntStatus sfnt_myanmar_cluster_boundaries(ConstSlice<u32>,Slice<u8>,u64 work_limit=1000000) noexcept;
// Prepares an accepted modern Myanmar run: reorders characters within original
// clusters and inserts required dotted circles. Selectors stay adjacent to their base
// scalar to identify consumed records; later mapping must consume a selector record
// only when its source offset immediately follows its base original scalar (not just
// its output neighbor). Windows variation selection uses original ordinal selector
// slots separately from these reordered records; see the Myanmar cluster shaping
// adapter. Source offsets are original UTF-16 units; inserted circles inherit the
// cluster start. A truncated kinzi immediately before punctuation produces a zero
// scalar in the missing-base slot and drops its last halant group, as Windows does.
// All views and result are disjoint. Capacity reports both required counts; other
// errors clear the result. Sixty-four work units per original scalar. Output is
// untouched on every failure.
SfntStatus sfnt_myanmar_prepare_input(ConstSlice<u32>,Slice<SfntIndicInputScalar>,
    SfntIndicInputResult*,u64 work_limit=1000000) noexcept;
}
