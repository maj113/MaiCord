#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Bengali scalar run: cluster GSUB, retained component-owner
// associations, then one run-wide GPOS pass. No allocation. Input and immutable
// font views are disjoint from all mutable storage. Failed output is unpublished.
SfntStatus sfnt_bengali_shape_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,ConstSlice<u8> font_forms,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
}
