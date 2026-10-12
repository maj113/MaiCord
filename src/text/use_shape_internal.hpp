#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord::use_shape {
// Generic nominal grouping for non-admitted fonts. Tibetan keeps its observed
// mark grouping and control-source policy separate from Sinhala.
SfntStatus nominal_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,float,
    SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits,bool rtl,bool tibetan=false) noexcept;
enum Role:u8 {base_role=1,halant_role=2,pre_role=4,control_role=8,join_role=16,cgj_role=32,reph_role=64};
// Internal: caller has validated and mapped one original cluster. Borrow
// positions for change snapshots, then apply shared USE reorder/GSUB/GPOS.
SfntStatus cluster(const SfntShapePlan&,const SfntCmap&,ConstSlice<SfntClusterGlyph>,u32 source_units,
    u32 halant_scalar,float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits,bool rtl,bool defer_output,
    const u16* control_glyphs=nullptr) noexcept;
// Internal: mapped data borrows positions; original group IDs and boundaries
// occupy character_glyphs/targets. Steps and their early prefix are validated.
SfntStatus run(const SfntShapePlan&,const SfntCmap&,ConstSlice<SfntClusterGlyph>,u32 source_units,u32 boundaries,
    u32 early_steps,u32 halant_scalar,float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits,bool rtl,
    const u16* control_glyphs=nullptr) noexcept;
}
