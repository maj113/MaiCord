#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Modern Devanagari single-cluster execution, en-US. Input is nominally mapped
// with ascending cluster-local source indices; form bits align with original
// scalars and all views belong to one font. Scalars/forms must not change
// during initial stage preparation or alias mutable shape storage. Permutation
// and masks reuse character-map/flags; mapped input with masks reuses
// substitution replacement scratch until copied. No allocation. Failed attempts
// set no output lengths. Orphan marks get a virtual circle with the same
// original source position; caller storage must fit that extra glyph/component.
// followed_by_text gives a singleton ZWJ its next-cluster context: Windows adds
// the circle only at the end of the analyzed text, not before another scalar.
// rtl selects directional positioning; input stays in logical source order.
// Nonnegative trailing_spacing follows that reading order and is applied after
// positioning, while component provenance is live. Indic marks can accept
// spacing; controls cannot.
SfntStatus sfnt_devanagari_shape_cluster(const SfntShapePlan&,const SfntCmap&,
    ConstSlice<SfntClusterGlyph> input,ConstSlice<u32> scalars,ConstSlice<u8> forms,
    u32 source_units,float em_size,SfntShapeStorage,SfntShapeRunResult*,
    ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits={},bool followed_by_text=false,float trailing_spacing=0,bool rtl=false) noexcept;
}
