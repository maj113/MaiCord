#pragma once
#include "maicord/text/indic_plan.hpp"
#include "maicord/text/devanagari_order.hpp"
#include "maicord/text/shape_run.hpp"
#include "maicord/core/utf.hpp"
namespace maicord {
// Existing Bengali expansion needs 32 slots. Odia/Telugu/Kannada's observed preparation
// budget is 31, including any added circle and dependent-vowel pieces.
inline constexpr u32 indic_cluster_capacity=32;
// Private run entry: the caller has validated scalar input and established one
// complete original Bengali/Gujarati/Gurmukhi/Odia/Telugu/Kannada cluster, of at most 31 scalars. Public
// cluster APIs retain their independent scanner validation.
SfntStatus indic_scalar_cluster(SfntIndicProfile,const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,ConstSlice<u8>,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits,
    bool word_initial,bool followed_by_text,float spacing,bool rtl,bool defer_output) noexcept;
// Shared retained-run entry; typed public wrappers and owned profiles use the
// same coordinator without another script dispatch inside the ownership layer.
SfntStatus indic_utf8_run(SfntIndicProfile,const SfntShapePlan&,const SfntCmap&,Utf8View,ConstSlice<u8>,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits,bool,float) noexcept;
// Internal shared execution. Bengali/Gujarati/Gurmukhi/Odia/Telugu/Kannada input is already segmented;
// re-scanning Bengali expansion would lose the original orphan/vowel distinction.
SfntStatus indic_initial_order(SfntIndicProfile,ConstSlice<u32>,ConstSlice<u8>,Slice<u32>,Slice<u8>,SfntDevanagariOrderResult*,u64,bool inserted_circle=false) noexcept;
SfntStatus indic_shape_cluster(SfntIndicProfile,const SfntShapePlan&,const SfntCmap&,ConstSlice<SfntClusterGlyph>,
    ConstSlice<u32>,ConstSlice<u8>,u32,float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,
    SfntShapeLimits,bool,float,bool,bool,bool=false) noexcept;
}
