#pragma once
#include "maicord/text/indic_input.hpp"
namespace maicord {
using SfntSinhalaInputScalar=SfntIndicInputScalar;
using SfntSinhalaInputResult=SfntIndicInputResult;
// Experimental horizontal Sinhala preparation, not app admission. Original
// scalar boundaries, one byte each (1=start, 0=continuation), before expansion.
// Uses existing Unicode 17 properties and the observed Windows USE policy.
// Maximum 31 ORIGINAL scalars per cluster. No font-dependent form lookahead.
// Invalid input, overlap, capacity and work failures leave output untouched.
// Work preflight reserves four units per scalar; all views must be disjoint.
SfntStatus sfnt_sinhala_cluster_boundaries(ConstSlice<u32>,Slice<u8>,u64 work_limit=1000000) noexcept;
// Expand original dependent vowels in an already segmented cluster, retaining
// UTF-16 source offsets. Observed Windows splits: DDA -> DD9 DCA, DDC -> DD9
// DCF, DDD -> DD9 DDD, DDE -> DD9 DDF. DDD's second component is a post-base
// glyph input, not another request for recursive expansion. No normalization,
// circles or reordering. No heap allocation. Three work units per input scalar.
// A sizing failure reports both required counts; other failures clear them.
SfntStatus sfnt_sinhala_expand_cluster(ConstSlice<u32>,Slice<SfntSinhalaInputScalar>,
    SfntSinhalaInputResult*,u64 work_limit=1000000) noexcept;
}
