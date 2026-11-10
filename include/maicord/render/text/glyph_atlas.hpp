#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

struct GlyphAtlasHandle {
    u16 slot;
    u16 generation;
};

// An allocation inside a shelf. `span` is the slot it took (its padded width): a
// later entry no wider reuses the slot when this one is evicted.
struct GlyphAtlasEntry {
    u64 key;
    u64 last_used; // allocator clock of the last lookup, pin or upload (entry LRU)
    u16 x;
    u16 y;
    u16 width;
    u16 height;
    u16 generation;
    u16 span;
    u16 visibility_epoch;
    u8 row; // an allocator holds at most 256 shelves
    bool occupied;
};

// A shelf: one height, filled left to right. Shelves never cross a page.
struct GlyphAtlasRow {
    u64 last_used;
    u16 y;
    u16 height;
    u16 cursor_x;
    u16 visibility_epoch;
    u16 occupied_entries;
    u16 pinned_entries;
    u8 pool; // 0: the general region; n: slots of pool n - 1
    u8 reserved[3];
};

// The surface is a virtual one of `page_count` pages of `page_rows` rows. A page
// belongs to the general region or to one pool while it holds shelves, which are
// stacked from its top (`used` rows). Memory backs a prefix of each page
// (`committed` rows): a whole page on backends that make one texture per page,
// otherwise whole commit units (a backend's tile band) covering the shelves.
inline constexpr u32 kGlyphAtlasMaximumPages = 8;
struct GlyphAtlasPage {
    u16 used;
    u16 committed;
    u8 owner; // pool + 1, or 0 for the general region, while `used` is nonzero
    u8 reserved[3];
};

// A class of requests of one known size, kept apart from the general region:
// slots of slot_width x slot_height in pages of its own. Up to `capacity`
// slots are made; beyond that the least recently used slot that is not
// visible is replaced, and only a fully visible pool makes more.
struct GlyphAtlasPool {
    u16 slot_width;
    u16 slot_height;
    u16 capacity;
    u16 reserved;
};

struct GlyphAtlasPaging {
    u16 page_rows;
    u8 page_count;
    bool whole_pages;       // memory comes a whole page at a time
    u16 commit_rows;        // otherwise in units of this many rows
    u16 budget_rows;        // committed general rows kept beyond the visible ones (0: no limit)
    u32 protect_ms;         // entries used this recently survive idle trim (0: only visible ones)
    bool protect_allocations; // ...and count as visible when making room
    u8 reserved[3];
};

struct GlyphAtlasAllocator {
    GlyphAtlasEntry* entries;
    GlyphAtlasRow* rows;
    const GlyphAtlasPool* pools;
    u32 entry_capacity;
    u32 row_capacity;
    u32 entry_count;
    u32 row_count;
    u32 evicted_entries;
    u32 evicted_rows;
    u32 full_resets;
    u32 free_cursor;
    u32 commits;   // commit units (or pages) backed
    u32 decommits; // commit units (or pages) returned
    u16 visibility_epoch;
    u16 visibility_rebases;
    u16 width;
    u16 height; // page_rows * page_count
    u16 padding;
    u16 page_rows;
    u16 commit_rows;
    u16 budget_rows;
    u8 page_count;
    u8 pool_count;
    bool whole_pages;
    bool protect_allocations;
    u32 protect_ms;
    // Recency: every lookup, pin or upload takes the next tick; observing the time moves the clock to at least
    // now_ms << kGlyphAtlasClockTimeShift, so a tick tells when (to the last observation) it was taken.
    u64 clock;
    // Entries used at or after this clock count as visible (see GlyphAtlasPaging).
    u64 protect_after;
    u64 now_ms;
    u64 last_activity_ms; // last new allocation
    bool trim_pending;
    u8 reserved[7];
    GlyphAtlasPage pages[kGlyphAtlasMaximumPages];
};
inline constexpr u32 kGlyphAtlasClockTimeShift = 20;

static_assert(sizeof(GlyphAtlasEntry) == 32);
static_assert(sizeof(GlyphAtlasRow) == 24);
static_assert(sizeof(GlyphAtlasPage) == 8);
static_assert(sizeof(GlyphAtlasAllocator) == 192);

enum class GlyphAtlasError : u8 {
    none,
    invalid_input,
    not_found,
    too_large,
    no_capacity,
};

struct GlyphAtlasAllocation {
    GlyphAtlasHandle handle;
    u16 x;
    u16 y;
    u16 width;
    u16 height;
    u32 evicted_entries;
    GlyphAtlasError error;
    bool cached;
    bool atlas_reset;

    constexpr bool ok() const noexcept {
        return error == GlyphAtlasError::none;
    }
};

// A single page of `height` rows, all of it committed, no budget: a fixed
// atlas. glyph_atlas_configure_paging makes it a virtual one.
// zeroed: the arrays are freshly committed zero pages and are left untouched
// (entries and rows become resident only as they are used).
bool glyph_atlas_initialize(GlyphAtlasAllocator* atlas, u16 width, u16 height, u16 padding,
                            Slice<GlyphAtlasEntry> entries,
                            Slice<GlyphAtlasRow> rows, bool zeroed = false) noexcept;
// Right after initialization: the virtual geometry, the memory policy and the
// pools (kept by pointer; requests name them by index).
bool glyph_atlas_configure_paging(GlyphAtlasAllocator* atlas, const GlyphAtlasPaging& paging,
                                  ConstSlice<GlyphAtlasPool> pools = {}) noexcept;
void glyph_atlas_reset(GlyphAtlasAllocator* atlas) noexcept;
void glyph_atlas_clear_pins(GlyphAtlasAllocator* atlas) noexcept;
bool glyph_atlas_pin(GlyphAtlasAllocator* atlas,
                     GlyphAtlasHandle handle) noexcept;
bool glyph_atlas_unpin(GlyphAtlasAllocator* atlas, GlyphAtlasHandle handle) noexcept;
bool glyph_atlas_is_pinned(const GlyphAtlasAllocator& atlas,
                           GlyphAtlasHandle handle) noexcept;
GlyphAtlasAllocation glyph_atlas_find(GlyphAtlasAllocator* atlas, u64 key) noexcept;
// The caller has already established that key is absent. `pool` (0: the general region; n: pool n - 1) must take
// the size.
//
// Room comes, in order, from free space on a shelf or in committed rows, from
// new committed rows while the policy allows (the general region's budget, a
// pool's capacity), from the least recently used slot that is not visible and
// is as wide (on a shelf of the same size class), from a run of shelves holding
// nothing visible, and then from new rows anyway. Once the virtual surface is
// full, entries that only count as visible through protect_allocations give
// way too. Pinned entries are never evicted or moved; no_capacity means the
// virtual surface is full of them.
GlyphAtlasAllocation glyph_atlas_allocate_unique(
    GlyphAtlasAllocator* atlas, u64 key, u16 width, u16 height, u8 pool = 0) noexcept;
bool glyph_atlas_validate(const GlyphAtlasAllocator& atlas, GlyphAtlasHandle handle,
                          GlyphAtlasAllocation* allocation = nullptr) noexcept;
// Drops one entry; its slot is reused by a later entry no wider.
bool glyph_atlas_release(GlyphAtlasAllocator* atlas, GlyphAtlasHandle handle) noexcept;

// The owner's clock (milliseconds) for protection windows and idle trim.
void glyph_atlas_observe_time(GlyphAtlasAllocator* atlas, u64 now_ms) noexcept;
// Zero when no trim is pending; otherwise the earliest time it may run.
inline u64 glyph_atlas_trim_deadline(const GlyphAtlasAllocator& atlas, u32 idle_ms) noexcept {
    return atlas.trim_pending ? atlas.last_activity_ms + idle_ms : 0;
}
// Idle trim: evicts what is neither visible nor protected, drops the empty
// shelves at the end of each page and returns their memory (pages[].committed
// shrinks). Returns the entries evicted.
u32 glyph_atlas_trim(GlyphAtlasAllocator* atlas) noexcept;
// Rows backed by memory over the whole surface.
u32 glyph_atlas_committed_rows(const GlyphAtlasAllocator& atlas) noexcept;
// Rows of the shelves that hold a visible entry.
u32 glyph_atlas_visible_rows(const GlyphAtlasAllocator& atlas) noexcept;

// Optional open-addressed key -> slot index for per-glyph lookup. Buckets hold
// slot + 1 and are validated against the entry on every probe, so evictions,
// resets and external capacity growth never need to touch the index; stale
// buckets act as tombstones until the next rebuild. Bucket count must be a
// power of two of at least twice the atlas entry capacity.
struct GlyphAtlasIndex {
    u16* buckets;
    u16 mask;
    u16 used;
    u32 rebuilds;
};
static_assert(sizeof(GlyphAtlasIndex) == 16);

bool glyph_atlas_index_initialize(GlyphAtlasIndex* index, Slice<u16> buckets) noexcept;
GlyphAtlasAllocation glyph_atlas_find_indexed(GlyphAtlasAllocator* atlas, GlyphAtlasIndex* index,
                                              u64 key) noexcept;
GlyphAtlasAllocation glyph_atlas_allocate_unique_indexed(GlyphAtlasAllocator* atlas,
                                                         GlyphAtlasIndex* index, u64 key, u16 width,
                                                         u16 height) noexcept;

} // namespace maicord
