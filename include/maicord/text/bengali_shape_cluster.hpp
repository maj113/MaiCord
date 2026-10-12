#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Experimental modern Bengali single ORIGINAL cluster, before split vowels.
// The scanner validates it; font_forms contains the prepared 128-byte Bengali
// classification. Word-initial controls the presentation-stage init feature.
// Expansion temporarily borrows position scratch, consumed before shaping;
// permutation borrows target scratch until metrics/export. The character map
// needs only original source-unit capacity, including for orphan split vowels.
// No allocation. Views must be disjoint from mutable shape storage; failures
// set no output lengths. This does not provide whole-run GPOS or app parity.
// defer_output skips source-map export, pixel finalization and tracking; the
// run coordinator consumes design-unit positions and resolved associations.
SfntStatus sfnt_bengali_shape_cluster(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,ConstSlice<u8> font_forms,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},
    bool word_initial=true,bool followed_by_text=false,float trailing_spacing=0,bool rtl=false,bool defer_output=false) noexcept;
}
