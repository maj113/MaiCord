#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Already-itemized Khmer scalars with an admitted font and prepared schedule.
// Reordering is cluster-local; every GSUB stage and GPOS sees the complete run.
// No allocation. Positions borrow expanded/mapped input, flags borrow scalar
// boundaries, character_glyphs borrow original group IDs and targets borrow
// group boundaries until export. Original scalars may borrow the exact component
// prefix until mapping completes. Other mutable views must be disjoint.
// Failure leaves output lengths zero; retry from the original input.
SfntStatus sfnt_khmer_shape_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},bool rtl=false) noexcept;
}
