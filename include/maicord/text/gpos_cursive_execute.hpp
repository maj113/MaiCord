#pragma once
#include "maicord/text/gpos_execute.hpp"
#include "maicord/text/gpos_attachment.hpp"
namespace maicord {
struct SfntCursiveExecution {u64 operations,matches,scan_steps;u32 chain_words;};
// Reverse contextual links form disjoint ordered chains (child < parent).
// A new child replaces the parent's old connection, keeping the old child's
// current position. Only cursive reattachment propagates through prior links.
struct SfntCursiveChainNode {u32 parent,child;};
static_assert(sizeof(SfntCursiveChainNode)==8);
// A growing reverse chain can defer exact integer translations. Head names the
// last changed node; next names its still-absolute target. UINT32_MAX is idle.
// While active, positions on the pending chain are private intermediate values.
struct SfntCursiveDeferred {double translation=0,magnitude=0;u32 head=0xffffffffu,next=0xffffffffu;};
static_assert(sizeof(SfntCursiveDeferred)==24);
SfntStatus sfnt_position_cursive_flush(Slice<SfntPosition> positions,Slice<SfntCursiveChainNode> chains,
    SfntCursiveDeferred* deferred,SfntCursiveExecution* result,u64 work_limit=1000000) noexcept;
static_assert(sizeof(SfntCursiveExecution)==32);
// Natural horizontal root cursive lookup. Both ends of each connection must lie
// in [start,input_end). Glyphs/nominal metrics are unchanged prepared inputs.
// Lookup RTL chooses the cross-stream chain root independently of run RTL.
// A clear flag streams vertical placement. A set flag stores relative edges in
// position state and one connection bit per input glyph, then resolves backward.
// chain_bits needs ceil((input_end-start)/64) words only when lookup RTL is set.
// It must not overlap any input/position storage. No parent array or allocation.
// On error, positions/scratch can contain partial or unresolved work and must be
// discarded. Point callback and variation semantics match mark execution.
SfntStatus sfnt_position_execute_cursive_lookup(const SfntGposAttachmentLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,
                                                 ConstSlice<double> nominal_advances,Slice<SfntPosition> positions,u32 start,u32 input_end,bool rtl,
                                                 const SfntPositionInstance& instance,Slice<u64> chain_bits,SfntCursiveExecution* result,
                                                 u64 work_limit=1000000,SfntAnchorPointResolver point_resolver=nullptr,void* point_context=nullptr) noexcept;
// One contextual action. The selected glyph bypasses initial filtering. Forward
// attachment finds its predecessor anywhere in the run; reverse attachment
// finds its successor before input_end. Reverse links are caller-initialized to
// UINT32_MAX and kept across children of one scheduled program lookup.
// Unchanged glyph order and disjoint chain storage are required. Forward-only
// actions need no chain storage. Failed state must be discarded.
// Optional deferred state coalesces a growing reverse chain only while all
// affected values and translations are bounded integers. Flush it before any
// other position read/write or before using the result. Forward/discontinuous calls
// flush automatically. Point callbacks disable coalescing. No extra per-glyph
// storage is used. Arithmetic outside the proven range follows eager execution.
SfntStatus sfnt_position_execute_cursive_selected(const SfntGposAttachmentLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,
    ConstSlice<double> nominal_advances,Slice<SfntPosition> positions,u32 selected,u32 input_end,bool rtl,
    const SfntPositionInstance& instance,Slice<SfntCursiveChainNode> chains,SfntCursiveExecution* result,
    u64 work_limit=1000000,SfntAnchorPointResolver point_resolver=nullptr,void* point_context=nullptr,
    SfntCursiveDeferred* deferred=nullptr) noexcept;
}
