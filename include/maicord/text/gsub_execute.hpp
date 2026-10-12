#pragma once
#include "maicord/text/context.hpp"
#include "maicord/text/clusters.hpp"
#include "maicord/text/components.hpp"
#include "maicord/text/lookup_bounds.hpp"

namespace maicord {
union SfntGsubExecutableSubtable {SfntGsubSubtable basic;SfntContext context;SfntReverseSubstitution reverse;};
struct SfntGsubExecutableLookup {ConstSlice<SfntGsubExecutableSubtable> subtables;SfntLookupFilter filter;u16 type;SfntLookupBounds bounds;};
struct SfntGsubLookupSource {SfntStatus(*prepare)(SfntGsubLookupSource*,u16) noexcept;const SfntGsubExecutableLookup* const* entries=nullptr;};
struct SfntGsubProgram {ConstSlice<SfntGsubExecutableLookup> lookups;SfntGdef gdef;u32 glyph_limit;SfntGsubLookupSource* source=nullptr;};
// Deferred directories keep the font's lookup count and store only pointers.
// Index is checked by the caller; a null entry has not been prepared yet.
inline const SfntGsubExecutableLookup* sfnt_gsub_lookup(const SfntGsubProgram& program,u16 index) noexcept {
    if(program.source&&program.source->entries)return program.source->entries[index];
    return program.lookups.data?program.lookups.data+index:nullptr;
}
inline bool sfnt_gsub_directory_valid(const SfntGsubProgram& program) noexcept {
    return program.lookups.data||(program.source&&program.source->entries);
}
inline SfntStatus sfnt_gsub_lookup_prepare(const SfntGsubProgram& program,u16 index,const SfntGsubExecutableLookup** prepared=nullptr) noexcept {
    if(prepared)*prepared=nullptr;
    if(index>=program.lookups.count||!sfnt_gsub_directory_valid(program))return SfntStatus::invalid_input;
    const auto* lookup=sfnt_gsub_lookup(program,index);
    if(!lookup||!lookup->type){
        if(!program.source||!program.source->prepare)return SfntStatus::invalid_input;
        const auto status=program.source->prepare(program.source,index);
        if(status!=SfntStatus::ok)return status;lookup=sfnt_gsub_lookup(program,index);
        if(!lookup||!lookup->type)return SfntStatus::invalid_input;
    }
    if(prepared)*prepared=lookup;return SfntStatus::ok;
}
struct SfntGsubProgramRequirements {u32 lookups,subtables,coverages,classes;};
struct SfntGsubProgramStorage {Slice<SfntGsubExecutableLookup> lookups;Slice<SfntGsubExecutableSubtable> subtables;Slice<SfntCoverage> coverages;Slice<SfntClassDef> classes;};
struct SfntGsubLookupStorage {Slice<SfntGsubExecutableSubtable> subtables;Slice<SfntCoverage> coverages;Slice<SfntClassDef> classes;};
// Prepare one directory entry; contextual children keep their font indices
// and must be prepared before execution. Failure leaves output empty.
SfntStatus sfnt_gsub_lookup_requirements(const SfntLayout&,u16,SfntGsubProgramRequirements*,u32 directory_limit=1000000) noexcept;
SfntStatus sfnt_gsub_prepare_lookup(const SfntLayout&,u16,const SfntGdef&,u32 glyph_limit,
    SfntGsubLookupStorage,SfntGsubExecutableLookup*,u32 payload_work_limit=1000000,u32 directory_limit=1000000,
    bool ignore_empty_sequences=false,bool ignore_unreachable_single_substitutions=false) noexcept;
// Font mappings and prepared payloads are borrowed and must stay unchanged. Deferred
// sources may fill empty directory slots. Prepare validates each new payload.
// payload_work_limit is shared across subtable opens; directory_limit caps
// visited directory entries. Optional child work reports preserve that budget.
// A supplied selection includes contextual children unless extending a deferred
// directory in place; its source must prepare children before execution.
// Unselected slots keep their indices and need preparation before execution.
// Inherited storage is dense and must use this layout, GDEF, glyph limit and
// compatibility policy. Selected inherited payloads are borrowed, so the inherited
// program must outlive the result. Destination payloads/output must be disjoint.
// The lookup directory may alias inherited storage if all existing entries
// remain selected. On failure only new slots may be partial; the caller rolls them back.
SfntStatus sfnt_gsub_program_requirements(const SfntLayout& layout,SfntGsubProgramRequirements* output,u32 directory_limit=1000000,ConstSlice<u64> selected={},const SfntGsubProgram* inherited=nullptr) noexcept;
SfntStatus sfnt_gsub_program_prepare(const SfntLayout& layout,const SfntGdef& gdef,u32 glyph_limit,
                                   SfntGsubProgramStorage storage,SfntGsubProgram* output,
                                   u32 payload_work_limit=1000000,u32 directory_limit=1000000,bool ignore_empty_sequences=false,
                                   bool ignore_unreachable_single_substitutions=false,ConstSlice<u64> selected={},const SfntGsubProgram* inherited=nullptr) noexcept;
// The final compatibility option accepts a type-1 subtable only if it validates
// in the complete 16-bit glyph space and every covered input is outside the
// font's actual glyph range. Its prepared coverage is empty: no valid input can
// reach its replacements. Reachable invalid outputs and malformed/truncated
// payloads remain errors. Both validation attempts consume the shared budget.

// Scratch records are internal state, exposed only to permit caller allocation.
// Packed positions scale with active matches, not run length times stack depth.
struct SfntGsubExecutionFrame {ConstSlice<u8> actions;u32 positions,count,next_action,input_end;};
struct SfntGsubExecutionScratch {
    Slice<SfntGsubExecutionFrame> frames;
    Slice<u32> positions,rebased,consumed;
    Slice<u8> replacements;
    Slice<SfntComponentUndo> component_journal;
};
struct SfntGsubExecutionLimits {u64 operations;u32 match_work;};
// operations is shared across scheduling, matches, copies and rebasing; match_work
// also caps an individual match. These are work units, not CPU timings.
struct SfntGsubExecutionResult {u64 operations,edits,moved_records;u32 input_end,max_depth;};
static_assert(sizeof(SfntGsubExecutableSubtable)==64 && sizeof(SfntGsubExecutableLookup)==40 && sizeof(SfntGsubProgram)==88);
static_assert(sizeof(SfntGsubExecutionFrame)==32 && sizeof(SfntGsubExecutionResult)==32);
// Execute one caller-selected lookup over [start,input_end), in logical order
// (type 8 visits positions in reverse). alternate is zero-based. No heap or
// C++ recursion is used. Independent scratch arrays must be disjoint from each
// other, buffer storage and all program mappings/prepared storage.
// A supplied cluster map is prepared; its mutable storage is disjoint from all
// glyph, scratch and program storage.
// Replacement scratch is measured in bytes and uses the buffer's record stride.
// Without clusters this layer edits 4-byte glyph/class records. With clusters it
// edits 8-byte SfntClusterGlyph records and merges source boundaries on ligatures.
// With a component forest (which also requires clusters), it edits 16-byte
// SfntComponentGlyph records. Ligatures join stable component sequences;
// decomposition partitions them and restores distinct source boundaries. The
// caller supplies an initialized forest and disjoint mutation journal. A failed
// glyph edit rolls back its forest changes. Export seals the forest against edits.
// Cluster initialization, source indices and initial effective classes come from
// the caller. Outputs inherit the first consumed glyph's source class, including
// ligatures, matching Windows. Initial attachment affinity, final glyph
// classes/flags, script scheduling and feature selection are separate policy.
// Component history is optional; it is not inferred from final glyph IDs.
// Capacity/work errors may follow earlier committed edits: discard the shaping
// attempt and restart from the original input. Each edit is preflighted before
// mutation. Result counters stay valid on execution errors. Scratch has no valid
// continuation state after an error.
// Exhausting frames is resource_limit; insufficient position/output storage is
// output_too_small. Active cycles therefore end at a caller-set bound.
// feature_mask selects eligibility bits in SfntGsubGlyph::reserved. Every glyph
// in a matched input span, including ignored marks, must be eligible; lookaround
// may cross eligibility boundaries. Nested actions keep this restriction.
// Replacements inherit the first consumed glyph's bits unless a component
// forest enables preserve_forms. Mask zero disables eligibility filtering.
// Script stages and initial form analysis are external.
SfntStatus sfnt_gsub_execute_lookup(const SfntGsubProgram& program,u16 lookup,SfntGlyphBuffer* buffer,
                                   u32 start,u32 input_end,u32 alternate,SfntGsubExecutionScratch scratch,
                                   SfntGsubExecutionLimits limits,SfntGsubExecutionResult* output,
                                   SfntClusterMap* clusters=nullptr,SfntComponentForest* components=nullptr,u8 feature_mask=0) noexcept;
}
