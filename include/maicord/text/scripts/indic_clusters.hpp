#pragma once
#include "maicord/text/scripts/indic_plan.hpp"
namespace maicord {
// Indic syllable boundaries of one run of a modern Indic script, as HarfBuzz segments it: vowel-constraint
// dotted circles, canonical decomposition and combining order (every part treated as mapped), the
// syllable grammar of the Microsoft Indic shaping specs. One output byte per scalar: 1 where a syllable
// starts. A syllable whose characters combining order moved starts at its lowest scalar. At most 256
// scalars; sixteen work units each. No allocation; views are disjoint; errors preserve output.
SfntStatus sfnt_indic_cluster_boundaries(SfntIndicProfile,ConstSlice<u32>,Slice<u8>,u64 work_limit=1000000) noexcept;
}
