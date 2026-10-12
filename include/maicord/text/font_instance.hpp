#pragma once
#include "maicord/text/font_store.hpp"
#include "maicord/text/tt_outline.hpp"
#include "maicord/core/arena.hpp"
namespace maicord {
struct NativeFontInstanceHandle {u32 slot,generation;};
struct NativeFontInstance {
    NativeFontHandle face;
    FontGeometryInstance geometry;
    const TtOutlineInstance* instructions;
};
struct NativeFontInstanceSlot {
    NativeFontInstance instance;
    Arena state;
    u32 references,generation;
};
struct NativeFontInstanceStore {
    NativeFontStore* fonts;
    Slice<NativeFontInstanceSlot> slots;
};
struct NativeFontPrepareScratch {
    Slice<AxisSegmentMap> maps;
    Slice<double> coordinates;
    Slice<i32> stack;
    Slice<TtCallFrame> frames;
};
// Zero-initialized stable slots, serialized mutation and store-local handles,
// as for NativeFontStore. Each live instance holds one face reference. Shaped
// runs hold a reference to the instance, keeping all mapped/prepared views alive together.
// Equivalent normalized settings share state; static raw instances allocate
// nothing. Optional variable/instruction state occupies one read-only arena.
// Disabled glyph programs discard their unused preparation arrays and decommit
// whole tail pages; a compact profile header remains for shared-instance keys.
// The caller selects whether glyph instructions are used.
// Variable fonts are accepted with instructed preparation only when glyph
// instructions are disabled. Active variable instructions are not approximated.
// Oblique draws share these base instances; simulation belongs to outline
// emission and does not duplicate the variation/instruction state.
// Scratch is mutable, disjoint from mapped/prepared views, and reusable after
// acquire returns. state_limit caps newly committed state (including page
// rounding); a matching live instance needs no new allocation.
NativeFontResult native_font_instance_acquire(NativeFontInstanceStore* store,
    NativeFontHandle face,ConstSlice<FontAxisSetting> settings,bool instructions,
    NativeFontPrepareScratch scratch,NativeFontInstanceHandle* output,
    u32 work_limit=1000000,u32* work_used=nullptr,usize state_limit=16*1024*1024) noexcept;
bool native_font_instance_retain(NativeFontInstanceStore* store,NativeFontInstanceHandle handle) noexcept;
bool native_font_instance_release(NativeFontInstanceStore* store,NativeFontInstanceHandle handle) noexcept;
const NativeFontInstance* native_font_instance_get(const NativeFontInstanceStore& store,NativeFontInstanceHandle handle) noexcept;
static_assert(sizeof(NativeFontInstanceHandle)==8);
} // namespace maicord
