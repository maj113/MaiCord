#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Experimental modern Devanagari single-cluster execution, en-US. Input is
// nominally mapped and source indices are ascending/local to this cluster.
// Font form bits align with original scalars; all views belong to one font.
// Scalars/forms must not change during initial stage preparation and do not
// alias mutable shape storage. Permutation and masks reuse character-map/flags;
// mapped input with masks reuses substitution replacement scratch until copied.
// No allocation. Failed attempts set no output lengths. Orphan marks get a
// virtual circle with the same original source position; caller storage must
// fit that extra glyph/component. Full normalization, font support checks and app
// integration are not implemented.
// followed_by_text carries the next-cluster context for a singleton ZWJ: Windows
// adds its circle only at the end of the analyzed text, not before another scalar.
// Nonnegative trailing spacing is applied after positioning while component
// provenance is still live. Indic marks can accept spacing; controls cannot.
// rtl selects directional positioning; input remains in logical source order.
// Trailing spacing follows that reading order.
SfntStatus sfnt_devanagari_shape_cluster(const SfntShapePlan&,const SfntCmap&,
    ConstSlice<SfntClusterGlyph> input,ConstSlice<u32> scalars,ConstSlice<u8> forms,
    u32 source_units,float em_size,SfntShapeStorage,SfntShapeRunResult*,
    ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits={},bool followed_by_text=false,float trailing_spacing=0,bool rtl=false) noexcept;
}
