#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// One modern Bengali original cluster, before split vowels. The scanner validates
// it; font_forms is the prepared 128-byte Bengali classification; word_initial
// controls the presentation-stage init feature.
// Expansion borrows position scratch until shaping, and permutation borrows target
// scratch until metrics/export. The character map needs only original source-unit
// capacity, including for orphan split vowels. Views must be disjoint from mutable
// shape storage; failures set no output lengths. Whole-run GPOS is not applied.
// defer_output skips source-map export, pixel finalization and tracking; the run
// coordinator consumes design-unit positions and resolved associations.
SfntStatus sfnt_bengali_shape_cluster(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,ConstSlice<u8> font_forms,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},
    bool word_initial=true,bool followed_by_text=false,float trailing_spacing=0,bool rtl=false,bool defer_output=false) noexcept;
}
