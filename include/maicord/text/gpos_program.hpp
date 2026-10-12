#pragma once
#include "maicord/text/context.hpp"
#include "maicord/text/lookup_selection.hpp"
#include "maicord/text/gpos_cursive_execute.hpp"
#include "maicord/text/gpos_ligature_execute.hpp"
#include "maicord/text/lookup_bounds.hpp"
namespace maicord {
union SfntGposExecutableTables {ConstSlice<SfntGposSubtable> basic;ConstSlice<SfntGposAttachment> attachment;ConstSlice<SfntContext> context;};
struct SfntGposExecutableLookup {SfntGposExecutableTables tables;SfntLookupFilter filter;u16 type;SfntLookupBounds bounds;};
static_assert(sizeof(SfntGposExecutableLookup)==40);
struct SfntGposProgram {ConstSlice<SfntGposExecutableLookup> lookups;SfntGdef gdef;u32 glyph_limit;const SfntGposExecutableLookup* const* entries=nullptr;};
inline const SfntGposExecutableLookup* sfnt_gpos_lookup(const SfntGposProgram& program,u16 index) noexcept {
    if(program.entries)return program.entries[index];return program.lookups.data?program.lookups.data+index:nullptr;
}
struct SfntGposProgramRequirements {u32 lookups,basic,attachments,contexts,coverages,classes,records;};
struct SfntGposProgramStorage {Slice<SfntGposExecutableLookup> lookups;Slice<SfntGposSubtable> basic;Slice<SfntGposAttachment> attachments;Slice<SfntContext> contexts;Slice<SfntCoverage> coverages;Slice<SfntClassDef> classes;Slice<const SfntGposExecutableLookup*> entries={};};
// Borrowed font mappings and prepared arrays; the font bytes must stay unchanged.
// Empty selection prepares every lookup. A supplied mask must include children;
// unselected entries keep their indices and cannot execute.
// An inherited program must come from this same font mapping; its selected
// records borrow payloads instead of preparing them again, so keep it alive.
// Output and destination arrays must be disjoint from all inherited storage.
// Optional entries stores a compact directory: N pointers and only the newly
// prepared records; inherited entries borrow whole records from the inherited program.
SfntStatus sfnt_gpos_program_requirements(const SfntLayout& layout,SfntGposProgramRequirements* output,u32 directory_limit=1000000,ConstSlice<u64> selected={},const SfntGposProgram* inherited=nullptr) noexcept;
SfntStatus sfnt_gpos_program_prepare(const SfntLayout& layout,const SfntGdef& gdef,u32 glyph_limit,SfntGposProgramStorage storage,SfntGposProgram* output,u32 payload_work_limit=1000000,u32 directory_limit=1000000,ConstSlice<u64> selected={},const SfntGposProgram* inherited=nullptr) noexcept;
struct SfntGposExecutionFrame {ConstSlice<u8> actions;u32 positions,count,next_action,input_end;};
struct SfntGposExecutionScratch {Slice<SfntGposExecutionFrame> frames;Slice<u32> positions;Slice<u64> cursive_bits;Slice<double> ligature_prefix;Slice<SfntCursiveChainNode> cursive_chains;};
struct SfntGposExecutionLimits {u64 operations;u32 match_work;};
struct SfntGposExecutionResult {u64 operations,matches,actions;u32 max_depth;};
static_assert(sizeof(SfntGposExecutableLookup)==40 && sizeof(SfntGposProgram)==88 && sizeof(SfntGposExecutionFrame)==32 && sizeof(SfntGposExecutionResult)==32);
// Natural horizontal execution of a caller-scheduled lookup. Contexts match
// formats 1/2/3; extension lookups are resolved during preparation. Nested input
// ranges stay within their parent match (including trailing ignored glyphs);
// lookaround and backward attachment may inspect outside it. Root inputs are
// filtered; explicit child inputs bypass initial filtering. No allocation or
// C++ recursion.
// Associations supplied with a SfntComponentGlyph-sized view require
// SfntComponentGlyph records, which keep their post-GSUB component metadata.
// Frames and packed matched positions scale with active contexts; reverse
// contextual cursive uses 8 bytes per run glyph, initialized on its first child
// call. Nonmonotonic chain updates and nested attachment searches can cost
// quadratic work, bounded by the shared operation limit.
// All scratch, positions, metrics, flags, associations and prepared font storage
// are disjoint. Limits bound work and active cycles. On failure positions may
// hold earlier committed actions: discard the attempt, never retry in place.
SfntStatus sfnt_gpos_execute_program_lookup(const SfntGposProgram& program,u16 lookup,SfntGlyphView glyphs,
    ConstSlice<double> nominal_advances,ConstSlice<u8> position_flags,ConstSlice<SfntMarkAssociation> associations,ConstSlice<u32> target_glyphs,
    Slice<SfntPosition> positions,u32 start,u32 input_end,bool rtl,const SfntPositionInstance& instance,
    SfntNullAnchorPolicy null_policy,SfntComponentAnchorPolicy component_policy,SfntGposExecutionScratch scratch,
    SfntGposExecutionLimits limits,SfntGposExecutionResult* output,SfntAnchorPointResolver point_resolver=nullptr,void* point_context=nullptr) noexcept;
}
