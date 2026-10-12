#pragma once
#include "maicord/text/gsub_execute.hpp"
#include "maicord/text/gpos_program.hpp"
#include "maicord/text/arabic_plan.hpp"

namespace maicord {
// The script/font layer supplies mapped glyphs, initial classes, source-unit
// positions, cluster boundaries and ordered lookup roots. This coordinator
// does not infer a Latin profile for joining/reordering scripts.
struct SfntShapePlan {
    const SfntGsubProgram* substitutions;
    const SfntGposProgram* positioning;
    ConstSlice<u16> substitution_roots,positioning_roots;
    SfntGdef gdef;
    SfntPositionInstance position_instance;
    void* metric_context;
    SfntStatus (*advance)(void*,u16,double*) noexcept;
    SfntAnchorPointResolver anchor_point=nullptr;
    void* anchor_context=nullptr;
    u16 units_per_em=0;
    // Opt in only for a pure glyph->advance callback. Target scratch is
    // idle until component export and can hold a small per-run metric cache.
    bool cache_advances=false;
    // The Latin compatibility profile refreshes GDEF classes between top-level
    // substitution roots after edits. Nested actions keep effective classes.
    bool refresh_substitution_classes=false;
    // Greek/Cyrillic placement policy; independent of cluster maps.
    bool compensate_consecutive_zeroes=false;
    // The fallback Hebrew placement profile keeps GPOS advance changes made
    // after initial mark/ignorable advances were cleared.
    bool retain_zero_positioning=false;
};
struct SfntShapeStorage {
    Slice<SfntComponentGlyph> glyphs;
    Slice<SfntComponentNode> components;
    Slice<u64> cluster_bits;
    Slice<u32> character_glyphs,targets;
    Slice<SfntMarkAssociation> associations;
    Slice<double> advances;
    Slice<u8> flags;
    Slice<SfntPosition> positions;
    Slice<SfntPlacement> placements;
    SfntGsubExecutionScratch substitutions;
    SfntGposExecutionScratch positioning;
};
struct SfntShapeLimits {
    // Shared across every GSUB and GPOS root, including their nested actions.
    u64 lookup_operations=1000000;
    u32 match_work=100000;
    // Separate bound for each of the fixed number of setup/export passes.
    u32 stage_work=1000000;
};
struct SfntShapeRunResult {
    u64 lookup_operations,substitution_edits,position_matches;
    u32 glyph_count,source_units,target_count;
};
// Optional script policy after GSUB and before metrics/GPOS. Glyph count is fixed.
// It resolves final glyph IDs/classes and fills every zero-advance byte with 0/1.
// Context and buffers are borrowed. On error the attempt's outputs are not valid.
struct SfntShapeGlyphPolicy {
    void* context=nullptr;
    SfntStatus (*prepare)(void*,Slice<SfntComponentGlyph>,Slice<u8>,u64) noexcept=nullptr;
    // Optional script-specific source boundary repair after GSUB. The source
    // metadata and component forest remain live; no glyph-count change is allowed.
    SfntStatus (*clusters)(void*,SfntClusterMap*,const SfntComponentForest&,Slice<SfntComponentGlyph>,u64) noexcept=nullptr;
};
static_assert(sizeof(SfntShapeGlyphPolicy)==24);
// Optional pre-GSUB composition over initialized component/source history.
// State is borrowed for this call. Edits must preserve buffer/forest/map
// invariants and honor the supplied setup-work bound. On failure no output is valid.
struct SfntShapeInputPolicy {
    void* context;
    SfntStatus (*prepare)(void*,SfntGlyphBuffer*,SfntClusterMap*,SfntComponentForest*,SfntGsubExecutionScratch,u64) noexcept;
};

// Script processing between substitution stages (for example Indic final
// reordering before presentation forms). Called before each stage 0..count-1,
// including stages with no lookups, but never for empty input. Steps must be
// ordered with stage IDs < count; legacy substitution_roots must be empty. Edits
// follow the input policy's buffer-ownership rules. Each call charges one lookup
// operation and the callback reports its own consumed work on every return,
// bounded by min(remaining work, stage_work). Later roots revalidate the changed
// run. Failure leaves result lengths unset.
struct SfntShapeStagePolicy {
    void* context;
    SfntStatus (*prepare)(void*,u16,SfntGlyphBuffer*,SfntClusterMap*,SfntComponentForest*,SfntGsubExecutionScratch,u64,u64*) noexcept;
    u16 count; // 1..256; stage IDs use a one-byte representation.
};
static_assert(sizeof(SfntShapeStagePolicy)==24);

// Successful glyphs are contiguous in storage.glyphs; source map, flags and
// placements occupy their respective prefixes. Result lengths are set only on
// success; a failed attempt can leave scratch/output bytes changed and must be
// restarted from the original input. Counters are diagnostic. All mutable arrays
// and the result are disjoint from each other, the input, boundaries and
// font/program storage, except for the borrows listed below. No allocation.
// Initial glyph classes drive substitution filtering; final classes come from
// GDEF unless the optional glyph policy resolves them. Initial glyph metadata
// carries caller-computed forms. Optional ordered steps replace the legacy roots
// (which must then be empty), carry feature masks and enable component form
// preservation; their stage IDs are nondecreasing 0..2 unless a stage policy
// supplies the stage count.
// Optional input_order is a caller-prepared permutation of the initial mapped
// glyphs (count == input.count); source records and forms move together. It may
// borrow storage.character_glyphs, which is not overwritten until after the
// initial copy. Initial boundaries may borrow storage.targets, consumed while
// opening the cluster map before targets are reused for export. Initial input
// may borrow the substitution replacement scratch (consumed by the initial glyph
// copy before any lookup reuses it) or the position buffer prefix (consumed
// before positioning).
// defer_output returns raw design-unit positions and flags, skipping source map
// export and placement writes; a script frontend must do its final reordering,
// source map export and finalization before using the placed run. Ordinary
// callers use the default complete finalization.
SfntStatus sfnt_shape_run(const SfntShapePlan& plan,ConstSlice<SfntClusterGlyph> input,
    u32 source_units,ConstSlice<u32> cluster_boundaries,bool rtl,float em_size,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits={},ConstSlice<SfntLookupStep> steps={},ConstSlice<u32> input_order={},SfntShapeGlyphPolicy glyph_policy={},const SfntShapeInputPolicy* input_policy=nullptr,const SfntShapeStagePolicy* stage_policy=nullptr,bool defer_output=false) noexcept;
}
