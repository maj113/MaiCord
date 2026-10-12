#pragma once
#include "maicord/text/gpos_mark_execute.hpp"
namespace maicord {
enum class SfntComponentAnchorPolicy : u8 {strict,skip_missing};
struct SfntLigatureExecution {u64 operations,matches,scan_steps,prefix_queries,ordered_steps,missing_components,compatibility_anchors;};
static_assert(sizeof(SfntLigatureExecution)==56);
// Natural horizontal lookup type 5 with caller-resolved target/component inputs.
// Associations and glyphs stay unchanged during the lookup; glyph edits
// invalidate these target indices. Targets may precede start. A missing target
// association skips attachment; positioning never guesses the nearest base or component.
// Target glyph indices are sorted, unique and precede their associated marks.
// Non-sentinel slots must resolve to an earlier glyph. Directory entries use
// current logical glyph indices.
// Scan mode's optional prefix scratch holds one double per target and is rebuilt on every call.
// It is used only when all advances are integral and their absolute sum <= 2^52,
// so subtraction equals an ordered interval sum. Otherwise ordered sums are used
// and repeated targets reuse the accumulated interval. Nonintegral alternating
// targets can need quadratic work, bounded by work_limit.
// Selected mode applies only at start, bypasses its initial lookup filter and
// ignores prefix scratch. It validates the touched association/target and sums
// that interval in order. Repeated contextual attachments can still have
// quadratic interval-sum work.
// All arrays, output and scratch are disjoint. No hidden allocation. Both anchors
// resolve before each mark commits; earlier marks can remain on failure.
// Known-component null anchors are skipped; unknown history uses null_policy.
// Component policy: strict rejects an out-of-range component; skip_missing
// tries later subtables.
SfntStatus sfnt_position_execute_ligature_lookup(const SfntGposAttachmentLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,
                                                  ConstSlice<double> nominal_advances,ConstSlice<SfntMarkAssociation> associations,ConstSlice<u32> target_glyphs,Slice<SfntPosition> positions,
                                                  u32 start,u32 input_end,bool rtl,const SfntPositionInstance& instance,SfntNullAnchorPolicy null_policy,
                                                  SfntComponentAnchorPolicy component_policy,Slice<double> prefix_scratch,SfntLigatureExecution* result,
                                                  u64 work_limit=1000000,SfntAnchorPointResolver point_resolver=nullptr,void* point_context=nullptr,
                                                  SfntPositionApplication application=SfntPositionApplication::scan) noexcept;
}
