#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Single original Telugu cluster. Original-input grouping precedes
// split-vowel expansion. Retained font_forms covers its 128-scalar block.
// Borrowed source/font views are disjoint from mutable caller storage.
// No system shaping or heap allocation; failed calls publish no output lengths.
SfntStatus sfnt_telugu_shape_cluster(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,ConstSlice<u8> font_forms,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},
    bool followed_by_text=false,float trailing_spacing=0,bool rtl=false,bool defer_output=false) noexcept;
}
