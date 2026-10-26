#pragma once
#include "maicord/text/indic_plan.hpp"
#include "maicord/text/devanagari_order.hpp"
#include "maicord/text/shape_run.hpp"
#include "maicord/text/base.hpp"
namespace maicord {
// Bengali expansion needs 32 slots. Odia/Telugu/Kannada preparation is limited to
// 31, including any added circle and dependent-vowel pieces.
inline constexpr u32 indic_cluster_capacity=32;
// Private run entry: the caller has validated scalar input and established one
// complete original Bengali/Gujarati/Gurmukhi/Odia/Telugu/Kannada/Malayalam cluster,
// of at most 31 scalars, before split vowels. font_forms is the prepared 128-byte
// script classification; word_initial controls the Bengali init feature.
// Expansion borrows position scratch until shaping, and permutation borrows target
// scratch until metrics/export. Views must be disjoint from mutable shape storage;
// failures set no output lengths. Whole-run GPOS is not applied. defer_output skips
// source-map export, pixel finalization and tracking for the run coordinator.
SfntStatus indic_scalar_cluster(SfntIndicProfile,const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,ConstSlice<u8>,
    float,const SfntShapeStorage&,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,const SfntShapeLimits&,
    bool word_initial,bool followed_by_text,float spacing,bool rtl,bool defer_output) noexcept;
// Shared retained-run entry for owned profiles: validated UTF-8, a bounded decode
// window of complete clusters and one run-wide GPOS pass. No allocation; failed
// output is unpublished.
SfntStatus indic_utf8_run(SfntIndicProfile,const SfntShapePlan&,const SfntCmap&,Utf8View,ConstSlice<u8>,
    float,const SfntShapeStorage&,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,const SfntShapeLimits&,bool,float) noexcept;
// Internal shared execution. Bengali/Gujarati/Gurmukhi/Odia/Telugu/Kannada input is already segmented;
// re-scanning Bengali expansion would lose the original orphan/vowel distinction.
// Prepare one scanner-recognized cluster (at most 31 scalars). forms[i] holds
// the prepared positional-form bits of scalar i. order is a permutation of
// original indices; masks stays in original order. Selects the base, marks
// initial reph/half/post-base eligibility, places pre-base matras and stably
// orders combining marks by canonical class. A missing base can request a
// circle. Views are borrowed and disjoint. Errors precede any array write and
// zero the result. Work reserves 64 units per scalar.
SfntStatus indic_initial_order(SfntIndicProfile,ConstSlice<u32>,ConstSlice<u8>,Slice<u32>,Slice<u8>,SfntDevanagariOrderResult*,u64,bool inserted_circle=false) noexcept;
SfntStatus indic_shape_cluster(SfntIndicProfile,const SfntShapePlan&,const SfntCmap&,ConstSlice<SfntClusterGlyph>,
    ConstSlice<u32>,ConstSlice<u8>,u32,float,const SfntShapeStorage&,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,
    const SfntShapeLimits&,bool,float,bool,bool,bool=false) noexcept;
}
