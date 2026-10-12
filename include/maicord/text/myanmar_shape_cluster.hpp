#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// One original cluster in an admitted horizontal modern Myanmar font. Native
// character preparation, cmap, staged GSUB, GPOS and original UTF-16 export.
// Borrowed disjoint storage; no allocation or system text calls. Position
// scratch holds prepared scalars, mapped in place into glyphs. Failures
// publish no output lengths. Cross-cluster presentation is a separate API.
SfntStatus sfnt_myanmar_shape_cluster(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},
    bool rtl=false,bool defer_output=false) noexcept;
}
