#pragma once
#include "maicord/text/indic_input.hpp"
namespace maicord {
using SfntTibetanInputScalar=SfntIndicInputScalar;
using SfntTibetanInputResult=SfntIndicInputResult;
// Original-scalar horizontal Tibetan boundaries. One byte per scalar; 1=start.
// No normalization, allocation, font access or app admission. Four work units
// per input scalar; at most 31 original non-selector items in a cluster.
// An attached selector does not consume an item. Invalid scalars,
// overlap, work or capacity errors leave output unchanged. Views are disjoint.
SfntStatus sfnt_tibetan_cluster_boundaries(ConstSlice<u32>,Slice<u8>,u64 work_limit=1000000) noexcept;
// Expand F73/F76/F78/F81 inside an already segmented original cluster, retaining
// original UTF-16 source positions. F75/F77/F79 remain nominal. This does not
// reorder glyphs or insert dotted circles. Three work units per input scalar.
// Views/result are disjoint. Sizing failure reports counts without writes;
// other failures clear result. No allocation; supplementary sources count two.
SfntStatus sfnt_tibetan_expand_cluster(ConstSlice<u32>,Slice<SfntTibetanInputScalar>,
    SfntTibetanInputResult*,u64 work_limit=1000000) noexcept;
}
