#pragma once
#include "maicord/text/gpos_execute.hpp"
#include "maicord/text/gpos_attachment.hpp"
namespace maicord {
enum class SfntNullAnchorPolicy : u8 {skip,parent_coordinates};
struct SfntMarkExecution {u64 operations,matches,search_steps,compatibility_anchors;};
// Resolved target slots in a caller-supplied target directory. UINT32_MAX means
// no target. Type 6 compares target identity without resolving physical targets.
// These are shaping inputs; glyph edits invalidate the resolved directory.
struct SfntMarkAssociation {u32 target,component;};
// No computed ligature component history: use component zero, with the selected
// null-anchor policy. Known component indices never use this value.
inline constexpr u32 sfnt_component_unknown=0xffffffffu;
inline constexpr u8 sfnt_mark_source_target=2;
static_assert(sizeof(SfntMarkAssociation)==8);
static_assert(sizeof(SfntMarkExecution)==32);
// Natural horizontal lookup types 4/6. Nominal advances are the same metric
// inputs used by position initialization; RTL anchor conversion needs them after
// advances have changed.
// Type 4 target selection ignores intervening marks; type 6 follows the lookup
// filter. A non-mark source-cluster boundary stops the backward search; a mark may
// attach across its own source-cluster boundary. A preceding uncovered eligible
// glyph blocks attachment. When both optional associations have targets, type 6
// requires equal target/component pairs; an unresolved target does not conflict.
// Target searches can precede start. No allocation.
// component_records requires SfntComponentGlyph records. Type 4 then skips
// component-less GSUB expansion outputs as bases but keeps their advances in the
// attachment distance; original source bases are not skipped.
// With instance.source_mark_targets, the reserved sfnt_mark_source_target value
// selects the supplied association's earlier base from target_directory.
// Unresolved targets use ordinary lookup selection. That distance scan is
// charged to work_limit.
// Both anchors resolve before a mark position commits. Earlier attachments may
// remain on failure, so discard failed attempts. Anchor format 2 requires a
// caller point resolver; its cost and side effects are the caller's.
// Null policy: skip absent targets, or use parent-header coordinates as Windows
// does for natural nonvariable fonts (unsupported for variable fonts; header
// words are never followed as pointers).
SfntStatus sfnt_position_execute_mark_lookup(const SfntGposAttachmentLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,
                                              ConstSlice<double> nominal_advances,ConstSlice<u8> position_flags,Slice<SfntPosition> positions,u32 start,u32 input_end,bool rtl,
                                              const SfntPositionInstance& instance,SfntNullAnchorPolicy null_policy,SfntMarkExecution* result,
                                              u64 work_limit=1000000,SfntAnchorPointResolver point_resolver=nullptr,void* point_context=nullptr,
                                              ConstSlice<SfntMarkAssociation> associations={},
                                              SfntPositionApplication application=SfntPositionApplication::scan,bool component_records=false,
                                              ConstSlice<u32> target_directory={}) noexcept;
}
