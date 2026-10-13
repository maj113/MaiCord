#pragma once
#include "maicord/text/font_store.hpp"
#include "maicord/text/tt_outline.hpp"
#include "maicord/text/base.hpp"
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
// as for NativeFontStore. Each live instance holds one face reference; shaped
// runs hold an instance reference, keeping all mapped/prepared views alive.
// Equivalent normalized settings share one instance; static raw instances
// allocate nothing. Optional variable/instruction state occupies one read-only
// arena. When the font's prep disables glyph programs, the unused preparation
// arrays are dropped and whole tail pages decommitted, keeping a compact
// profile header for shared-instance keys.
// With instructions requested, a variable font fails as unsupported unless its
// prep disables glyph programs (variable instructions are not approximated).
// Synthetic oblique shares the base instance; the slant is applied in outline
// emission.
// Scratch is mutable, disjoint from mapped/prepared views and reusable after
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
