#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord::use_shape {
// Generic nominal grouping for non-admitted fonts. Tibetan keeps its own mark
// grouping and control-source policy, separate from Sinhala.
SfntStatus nominal_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,float,
    const SfntShapeStorage&,SfntShapeRunResult*,const SfntShapeLimits&,bool rtl,bool tibetan=false) noexcept;
enum Role:u8 {base_role=1,halant_role=2,pre_role=4,control_role=8,join_role=16,cgj_role=32,reph_role=64};
// Internal: mapped data borrows positions; original group IDs and boundaries
// occupy character_glyphs/targets. Steps and their early prefix are validated.
SfntStatus run(const SfntShapePlan&,const SfntCmap&,ConstSlice<SfntClusterGlyph>,u32 source_units,u32 boundaries,
    u32 early_steps,u32 halant_scalar,float,const SfntShapeStorage&,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,const SfntShapeLimits&,bool rtl,
    const u16* control_glyphs=nullptr) noexcept;
// After late substitution, re-derives each glyph's original source group from its
// components and reopens the cluster map on the surviving group boundaries.
SfntStatus restore_groups(Slice<u32> groups,Slice<u32> boundaries,u32 boundary_count,SfntClusterMap*,
    const SfntComponentForest&,Slice<SfntComponentGlyph>,u64 work) noexcept;
// Glyph policy of grouped Khmer/Myanmar runs (a script policy may extend it as
// its first member). The font's nominal glyphs for the script's invisible
// controls become spaces with zero advance, as do marks; reserved records
// invisibility. Source groups are restored after late substitution.
struct GroupRun {const SfntGdef* gdef;u16 space=0,invisible[4]{};Slice<u32> groups,boundaries;u32 boundary_count=0;};
SfntStatus finish_group_run(void*,Slice<SfntComponentGlyph>,Slice<u8> zero,u64 work) noexcept;
SfntStatus restore_group_run(void*,SfntClusterMap*,const SfntComponentForest&,Slice<SfntComponentGlyph>,u64 work) noexcept;
// Setup-work meter shared by the script policies; used counts what was spent.
struct Work {
    u64 left,used=0;
    bool spend(u64 n) noexcept {if(n>left)return false;left-=n;used+=n;return true;}
};
// Moves the glyph gap to at, charging the distance.
SfntStatus seek(Work&,SfntGlyphBuffer&,u32 at) noexcept;
// Seeks buffer to end, then opens local over glyphs [first,end) and the existing
// gap, so lookups cannot see or edit beyond that cluster group.
SfntStatus open_group(Work&,SfntGlyphBuffer& buffer,u32 first,u32 end,SfntGlyphBuffer& local) noexcept;
// Runs the early lookup steps of one phase (any phase for 0xffff) on a group's local view.
SfntStatus run_early(Work&,const SfntShapePlan&,ConstSlice<SfntLookupStep> early,u32& step,u32 phase,SfntGlyphBuffer& local,
    const SfntGsubExecutionScratch&,u32 match_work,SfntClusterMap*,SfntComponentForest*,u64& edits) noexcept;
// Index of the glyph owning a component node's root (nil when absent), charging the search.
SfntStatus locate(const SfntComponentForest&,const Slice<SfntComponentGlyph>&,u32 node,u32* index,Work&) noexcept;
// Moves one glyph record in front of position before, keeping the others in order.
SfntStatus move_before(const Slice<SfntComponentGlyph>&,u32 from,u32 before,Work&) noexcept;
}
