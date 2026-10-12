#pragma once
#include "maicord/text/font_catalog.hpp"
#include "maicord/text/font_session.hpp"
namespace maicord {
struct NativeFontCoverageSlot {const NativeFontCatalog* catalog;NativeFontHandle handle;u32 face,stamp;};
struct NativeFontCoverageCache {
    NativeFontSession* session;Slice<NativeFontCoverageSlot> slots;u32 clock;
    u64 acquisitions,evictions;
};
// Zeroed cache provided by the caller, with 1..64 zeroed slots at stable addresses.
// Keeps a reference to the session, allocates nothing and opens no file. Serialize
// operations. Catalogs must stay alive and unchanged until reset; reset before replacing any catalog.
bool native_font_coverage_initialize(NativeFontSession*,Slice<NativeFontCoverageSlot>,NativeFontCoverageCache*) noexcept;
// Release cached face references. Reset keeps the session/storage; release also
// drops the session reference. Instances and runs keep their own mappings.
bool native_font_coverage_reset(NativeFontCoverageCache*) noexcept;
bool native_font_coverage_release(NativeFontCoverageCache*) noexcept;
// Coverage lookup for a physical face, loaded on first use in the session: no second
// mapping store, instance preparation or per-scalar file IO on a hit. Catalog file
// identity/size/time is validated when a miss loads the face. Reset to force revalidation.
// A fixed-size LRU limits the cache; cached faces may share live mappings
// with existing instances/runs. Missing glyphs are a successful false; IO, stale
// catalogs, capacity and malformed-font failures remain errors. Output clears.
// Optional face output borrows the cache's view, including an
// ordinary missing-glyph result. It clears on failure and adds no reference.
// Discard it before any later operation on this cache (including reset/release).
NativeFontResult native_font_coverage_probe(NativeFontCoverageCache*,const NativeFontCatalog&,u32 candidate,u32 scalar,bool*,const NativeFontFace** borrowed=nullptr) noexcept;
// Acquire a face with its own reference through the same cache, for immediate
// preparation of the selected instance. The caller keeps the session alive and releases
// this handle in native_font_session_fonts(). Failure clears the output.
// Refresh reopens the selected path and validates its current identity/version,
// replacing any cached entry; existing instances keep their mappings.
NativeFontResult native_font_coverage_acquire(NativeFontCoverageCache*,const NativeFontCatalog&,u32 candidate,NativeFontHandle*,bool refresh=false) noexcept;
// Validate and cache a face already opened in this session. The caller keeps
// its own reference; later coverage lookups use the cache's reference.
NativeFontResult native_font_coverage_remember(NativeFontCoverageCache*,const NativeFontCatalog&,u32 candidate,NativeFontHandle) noexcept;
static_assert(sizeof(NativeFontCoverageSlot)==24&&sizeof(NativeFontCoverageCache)==48);
}
