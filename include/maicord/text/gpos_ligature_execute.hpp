#pragma once
#include "maicord/text/gpos_mark_execute.hpp"
namespace maicord {
enum class SfntComponentAnchorPolicy : u8 {strict,skip_missing};
struct SfntLigatureExecution {u64 operations,matches,scan_steps,prefix_queries,ordered_steps,missing_components,compatibility_anchors;};
static_assert(sizeof(SfntLigatureExecution)==56);
// Natural horizontal lookup type 5 with caller-resolved target/component inputs.
// Associations and glyphs must not change during the lookup; glyph edits
// invalidate the target indices. Targets may precede start. A missing target
// association skips attachment; the nearest base or component is never guessed.
// Target glyph indices are sorted, unique and precede their associated marks;
// non-sentinel slots resolve to an earlier glyph; directory entries use current
// logical glyph indices.
// Scan mode: the optional prefix scratch (one double per target, rebuilt on
// every call) is used only when all advances are integral and their absolute
// sum <= 2^52, so a subtraction equals the ordered interval sum. Otherwise
// ordered sums are used and repeated targets reuse the accumulated interval;
// nonintegral alternating targets can need quadratic work, bounded by work_limit.
// Selected mode applies only at start, bypasses its initial lookup filter,
// ignores prefix scratch and sums the touched interval in order; repeated
// contextual attachments can still cost quadratic interval-sum work.
// All arrays, output and scratch are disjoint. Both anchors resolve before each
// mark commits; earlier marks can remain on failure. Null anchors of a known
// component are skipped; unknown history uses null_policy. component_policy:
// strict rejects an out-of-range component; skip_missing tries later subtables.
SfntStatus sfnt_position_execute_ligature_lookup(const SfntGposAttachmentLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,
                                                  ConstSlice<double> nominal_advances,ConstSlice<SfntMarkAssociation> associations,ConstSlice<u32> target_glyphs,Slice<SfntPosition> positions,
                                                  u32 start,u32 input_end,bool rtl,const SfntPositionInstance& instance,SfntNullAnchorPolicy null_policy,
                                                  SfntComponentAnchorPolicy component_policy,Slice<double> prefix_scratch,SfntLigatureExecution* result,
                                                  u64 work_limit=1000000,
                                                  SfntPositionApplication application=SfntPositionApplication::scan) noexcept;
}
