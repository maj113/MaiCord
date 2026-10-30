#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord::use_shape {
// Generic nominal grouping for non-admitted fonts. Tibetan keeps its own mark
// grouping and control-source policy, separate from Sinhala.
SfntStatus nominal_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,float,
    const SfntShapeStorage&,SfntShapeRunResult*,const SfntShapeLimits&,bool rtl,bool tibetan=false) noexcept;
enum Role:u8 {base_role=1,halant_role=2,pre_role=4,control_role=8,join_role=16,cgj_role=32,reph_role=64};
// Fills one mapped record for a scalar (mirrored for rtl) and its variation
// selector, or the scalar alone where the font has no such sequence.
SfntStatus map_glyph(const SfntShapePlan&,const SfntCmap&,u32 scalar,u32 selector,bool rtl,u8 role,u32 source,SfntClusterGlyph* out) noexcept;
// The scripts that share one grouped execution path. Sinhala and Tibetan reorder
// reph, pref and pre-vowels inside each source group between their early lookup
// stages; Myanmar only runs the early stages group by group; Khmer runs every
// stage over the whole run. Sinhala hides role-marked controls, the others the
// font's nominal ZWNJ/ZWJ/LRM/RLM (Khmer: ZWSP/ZWNJ/ZWJ) glyphs, as spaces with zero advance.
enum class Script:u8 {sinhala,tibetan,myanmar,khmer};
// Internal: mapped data borrows positions; original group IDs and boundaries
// occupy character_glyphs/targets. Steps and their early prefix are validated.
// Source groups are restored after late substitution.
SfntStatus run(Script,const SfntShapePlan&,const SfntCmap&,ConstSlice<SfntClusterGlyph>,u32 source_units,u32 boundaries,
    u32 early_steps,float,const SfntShapeStorage&,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,const SfntShapeLimits&,bool rtl) noexcept;
// Admission shared by the Sinhala, Tibetan and Myanmar frontends: views, step order, scalar validity and
// the UTF-16 total. *early receives the number of steps before stage 5.
SfntStatus admit_run(const SfntShapePlan&,ConstSlice<u32> input,ConstSlice<SfntLookupStep> steps,const SfntShapeStorage&,
    const SfntShapeLimits&,u32* early) noexcept;
// After late substitution, re-derives each glyph's original source group from its
// components and reopens the cluster map on the surviving group boundaries.
SfntStatus restore_groups(Slice<u32> groups,Slice<u32> boundaries,u32 boundary_count,SfntClusterMap*,
    const SfntComponentForest&,Slice<SfntComponentGlyph>,u64 work) noexcept;
// Setup-work meter shared by the script policies; used counts what was spent.
struct Work {
    u64 left,used=0;
    bool spend(u64 n) noexcept {if(n>left)return false;left-=n;used+=n;return true;}
};
// Work meter that reports what it spent to *out when the policy callback returns.
struct Metered : Work {
    u64* out;
    Metered(u64 available,u64* report) noexcept : Work{available},out(report) {}
    ~Metered() noexcept {*out=used;}
};
// Index of the glyph owning a component node's root (nil when absent), charging the search.
SfntStatus locate(const SfntComponentForest&,const Slice<SfntComponentGlyph>&,u32 node,u32* index,Work&) noexcept;
// Moves one glyph record in front of position before, keeping the others in order.
SfntStatus move_before(const Slice<SfntComponentGlyph>&,u32 from,u32 before,Work&) noexcept;
// Moves [first,mid) behind [mid,last) with three reversals; the caller charges.
void rotate(Slice<SfntComponentGlyph>,u32 first,u32 mid,u32 last) noexcept;
// First-component metadata byte of a glyph (zero for the nil component), charging the walk.
inline SfntStatus first_role(const SfntComponentForest& forest,const SfntComponentGlyph& glyph,u8* value,Work& w) noexcept {
    u64 used=0;const auto status=sfnt_component_first_metadata(forest,glyph.component,value,w.left,&used);w.spend(used);return status;
}
// Whether the glyph is a surviving control: its first component carries control_bit and it still stands for one component.
inline SfntStatus control_glyph(const SfntComponentForest& forest,const SfntComponentGlyph& glyph,u8 control_bit,bool* control,Work& w) noexcept {
    u8 value=0;const auto status=first_role(forest,glyph,&value,w);
    *control=status==SfntStatus::ok&&(value&control_bit)&&forest.nodes[glyph.component].count==1;return status;
}
}
