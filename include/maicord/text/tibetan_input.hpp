#pragma once
#include "maicord/text/indic_input.hpp"
namespace maicord {
// Original-scalar horizontal Tibetan boundaries. One byte per scalar; 1=start.
// No normalization, allocation, font access or font support check. Four work units
// per input scalar; at most 31 original non-selector items in a cluster.
// An attached selector does not consume an item. Invalid scalars,
// overlap, work or capacity errors leave output unchanged. Views are disjoint.
SfntStatus sfnt_tibetan_cluster_boundaries(ConstSlice<u32>,Slice<u8>,u64 work_limit=1000000) noexcept;
}
