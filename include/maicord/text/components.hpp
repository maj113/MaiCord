#pragma once
#include "maicord/text/gpos_mark_execute.hpp"
namespace maicord {
// Every initial glyph owns one stable component node. A mark's dormant node
// refers to its preceding glyph until it is consumed into a ligature sequence.
// Metadata moves with the glyph record. Source units and initial classes remain
// caller policy; this initializer implements logical-order predecessor affinity.
struct SfntComponentGlyph {SfntGsubGlyph glyph;u32 source,component,attachment;};
struct SfntComponentNode {u32 left,right,parent,count,source;u8 height,flags;u16 reserved;};
struct SfntComponentForest {Slice<SfntComponentNode> nodes;bool sealed,preserve_forms;};
struct SfntComponentUndo {u32 index;SfntComponentNode before;};
struct SfntComponentEdit {u32 undo_count;u64 operations;};
static_assert(sizeof(SfntComponentGlyph)==16 && sizeof(SfntComponentNode)==24 && sizeof(SfntComponentForest)==24 && sizeof(SfntComponentUndo)==28);
// preserve_forms keeps initial eligibility in the node's padding. Ligatures
// take the last component's form; decomposition restores each output sequence's
// last form, repeating the last value for outputs with no remaining components.
// This opt-in policy preserves Arabic forms through composition round trips.
SfntStatus sfnt_components_open(Slice<SfntComponentGlyph> glyphs,Slice<SfntComponentNode> storage,SfntComponentForest* output,u64 work_limit=1000000,bool preserve_forms=false) noexcept;
// After open, the caller may set immutable script metadata in reserved's upper
// byte on each initial node. Tree edits preserve it; form selection uses only
// the lower byte. Query the first owned component (an alias stays a singleton).
SfntStatus sfnt_component_first_metadata(const SfntComponentForest&,u32 node,u8* output,u64 work_limit=1000000,u64* work_used=nullptr) noexcept;
// Query an owned sequence or a dormant mark singleton. No mutation. UINT32_MAX
// is an empty sequence. Inputs require a successfully initialized unchanged forest.
SfntStatus sfnt_component_location(const SfntComponentForest& forest,u32 node,u32* root,u32* ordinal,u64 work_limit=1000000,u64* work_used=nullptr) noexcept;
// Logical source extent of an owned sequence or one dormant mark. Traversal
// does not follow a dormant mark's attachment into its preceding base sequence.
// Success sets the min/max sources. Failure leaves first=UINT32_MAX,last=0
// when both output pointers are valid; work_used includes failed traversal.
SfntStatus sfnt_component_source_bounds(const SfntComponentForest&,u32 node,u32* first,u32* last,u64 work_limit=1000000,u64* work_used=nullptr) noexcept;
// Prepare component metadata for one GSUB2 expansion or GSUB4 ligature. Output
// records are already initialized from the first consumed glyph. Failure rolls
// back the forest; outputs/journal are scratch. On success keep the journal
// until the glyph edit commits, or roll it back if that edit fails.
// All mutable arrays are disjoint. No allocation. Edits are forbidden after export.
SfntStatus sfnt_components_edit(SfntComponentForest* forest,SfntGlyphView glyphs,ConstSlice<u32> consumed,bool ligature,
                                Slice<u8> replacement_records,Slice<SfntComponentUndo> journal,SfntComponentEdit* result,u64 work_limit=1000000) noexcept;
void sfnt_components_rollback(SfntComponentForest* forest,ConstSlice<SfntComponentUndo> journal) noexcept;
// Export after final glyph classification, sealing the forest against further
// edits. Sorted target directory uses logical glyph indices; associations use
// owner slots. Partial export/cache state can remain on failure. Retry with the
// same immutable glyph order/classes or discard the attempt. No extra owner map.
SfntStatus sfnt_components_export(SfntComponentForest* forest,SfntGlyphView glyphs,Slice<u32> targets,
                                  Slice<SfntMarkAssociation> associations,u32* target_count,u64 work_limit=1000000,u64* work_used=nullptr) noexcept;
}
