#pragma once
#include "maicord/text/font_catalog.hpp"
#include "maicord/text/font_session.hpp"
namespace maicord {
struct NativeFontCoverageSlot {const NativeFontCatalog* catalog;NativeFontHandle handle;u64 stamp;};
struct NativeFontCoverageCache {
    NativeFontSession* session;Slice<NativeFontCoverageSlot> slots;u64 clock,acquisitions,evictions;
};
// Zeroed cache provided by the caller, with at least one zeroed slot at stable
// addresses. Keeps a reference to the session, allocates nothing and opens no
// file. Serialize operations. Catalogs must stay alive and unchanged until reset;
// reset before replacing any catalog. Operations need an initialized cache.
bool native_font_coverage_initialize(NativeFontSession*,Slice<NativeFontCoverageSlot>,NativeFontCoverageCache*) noexcept;
// Release cached face references. Reset keeps the session/storage; release also
// drops the session reference. Instances and runs keep their own mappings.
bool native_font_coverage_reset(NativeFontCoverageCache*) noexcept;
bool native_font_coverage_release(NativeFontCoverageCache*) noexcept;
// Coverage lookup for a catalog's physical face, loaded on first use in the
// session. A hit needs no second mapping store, instance preparation or
// per-scalar file IO; a miss validates the catalog file's identity/size/time
// (reset to force revalidation). A fixed-size LRU bounds the cache; cached faces
// may share live mappings with instances and runs. A missing glyph is a
// successful false; IO, stale catalog, capacity and malformed-font failures are
// errors, with the result false. The scalar must be valid.
// The optional face output borrows the cache's view (also for a missing glyph),
// adds no reference and is cleared on failure. Discard it before any later
// operation on this cache, including reset and release.
NativeFontResult native_font_coverage_probe(NativeFontCoverageCache*,const NativeFontCatalog&,u32 scalar,bool*,const NativeFontFace** borrowed=nullptr) noexcept;
// Acquire a face with its own reference through the same cache, for immediate
// preparation of the selected instance. The caller keeps the session alive and releases
// this handle in native_font_session_fonts(). Failure clears the output.
// Refresh reopens the selected path and validates its current identity/version,
// replacing any cached entry; existing instances keep their mappings.
NativeFontResult native_font_coverage_acquire(NativeFontCoverageCache*,const NativeFontCatalog&,NativeFontHandle*,bool refresh=false) noexcept;
// Cache a face the caller just opened from this catalog's path and built the
// catalog from. The caller keeps its own reference; later coverage lookups use
// the cache's reference. No revalidation.
NativeFontResult native_font_coverage_remember(NativeFontCoverageCache*,const NativeFontCatalog&,NativeFontHandle) noexcept;
static_assert(sizeof(NativeFontCoverageSlot)==24&&sizeof(NativeFontCoverageCache)==48);
}
