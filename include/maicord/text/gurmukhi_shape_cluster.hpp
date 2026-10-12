#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Single original Gurmukhi cluster. The input scanner validates
// grouping; font_forms is the retained 128-byte font classification. No heap
// allocation or system shaping. Source and font views are disjoint from mutable
// caller storage. Failed calls publish no output lengths.
SfntStatus sfnt_gurmukhi_shape_cluster(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,ConstSlice<u8> font_forms,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},
    bool followed_by_text=false,float trailing_spacing=0,bool rtl=false,bool defer_output=false) noexcept;
}
