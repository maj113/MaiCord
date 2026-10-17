#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

struct GlyphAtlasHandle {
    u16 slot;
    u16 generation;
};

struct GlyphAtlasEntry {
    u64 key;
    u16 x;
    u16 y;
    u16 width;
    u16 height;
    u16 row;
    u16 generation;
    bool occupied;
    u8 reserved;
    u16 visibility_epoch;
};

struct GlyphAtlasRow {
    u64 last_used;
    u16 y;
    u16 height;
    u16 cursor_x;
    u16 visibility_epoch;
    u32 occupied_entries;
    u32 pinned_entries;
};

struct GlyphAtlasAllocator {
    GlyphAtlasEntry* entries;
    GlyphAtlasRow* rows;
    u32 entry_capacity;
    u32 row_capacity;
    u32 entry_count;
    u32 row_count;
    u32 evicted_entries;
    u32 evicted_rows;
    u32 full_resets;
    u32 rejected;
    u32 free_cursor;
    u16 visibility_epoch;
    u16 visibility_rebases;
    u64 clock;
    u16 width;
    u16 height;
    u16 next_y;
    u16 padding;
    // Nonzero while a growable owner allocates below its capacity: populated
    // rows used at or after this clock count as pinned, so a taller texture is
    // preferred over evicting recently used (scrolled-away) media.
    u64 protect_after;
};

static_assert(sizeof(GlyphAtlasEntry) == 24);
static_assert(sizeof(GlyphAtlasRow) == 24);
static_assert(sizeof(GlyphAtlasAllocator) == 80);

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

// zeroed: the arrays are freshly committed zero pages and are left untouched
// (entries and rows become resident only as they are used).
bool glyph_atlas_initialize(GlyphAtlasAllocator* atlas, u16 width, u16 height, u16 padding,
                            Slice<GlyphAtlasEntry> entries,
                            Slice<GlyphAtlasRow> rows, bool zeroed = false) noexcept;
void glyph_atlas_reset(GlyphAtlasAllocator* atlas) noexcept;
void glyph_atlas_clear_pins(GlyphAtlasAllocator* atlas) noexcept;
bool glyph_atlas_pin(GlyphAtlasAllocator* atlas,
                     GlyphAtlasHandle handle) noexcept;
bool glyph_atlas_unpin(GlyphAtlasAllocator* atlas, GlyphAtlasHandle handle) noexcept;
bool glyph_atlas_is_pinned(const GlyphAtlasAllocator& atlas,
                           GlyphAtlasHandle handle) noexcept;
GlyphAtlasAllocation glyph_atlas_find(GlyphAtlasAllocator* atlas, u64 key) noexcept;
GlyphAtlasAllocation glyph_atlas_allocate(GlyphAtlasAllocator* atlas, u64 key, u16 width,
                                           u16 height) noexcept;
// The caller has already established that key is absent, so the identity
// lookup is skipped.
GlyphAtlasAllocation glyph_atlas_allocate_unique(
    GlyphAtlasAllocator* atlas, u64 key, u16 width, u16 height) noexcept;
bool glyph_atlas_validate(const GlyphAtlasAllocator& atlas, GlyphAtlasHandle handle,
                          GlyphAtlasAllocation* allocation = nullptr) noexcept;

// Height-growable atlases. The allocator is initialized at its capacity and
// then configured to a smaller initial height; the owner resizes its texture
// whenever `height` changes (prepare grows, trim shrinks). Pixel coordinates
// of surviving entries change only through trim's reported row moves.
struct GlyphAtlasRowMove {
    u16 source_y;
    u16 destination_y;
    u16 height;
    u16 reserved;
};

struct GlyphAtlasGrowth {
    u64 sample_tick[16]; // (allocator clock, time) pairs mapping a recency window to clock values
    u64 sample_ms[16];
    u64 now_ms;
    u64 last_activity_ms; // last new allocation
    u32 sample_count;
    u32 sample_head;
    u32 protect_ms;
    u32 growths;
    u32 shrinks;
    u32 relocated_rows;
    u32 trimmed_entries;
    u16 minimum_height;
    u16 maximum_height; // zero: fixed-size atlas
    u16 step;
    bool trim_pending;
    u8 reserved;
};

static_assert(sizeof(GlyphAtlasRowMove) == 8);

bool glyph_atlas_growth_configure(GlyphAtlasGrowth* growth, GlyphAtlasAllocator* atlas,
                                  u16 initial_height, u16 step, u32 protect_ms) noexcept;
void glyph_atlas_growth_observe(GlyphAtlasGrowth* growth, const GlyphAtlasAllocator& atlas,
                                u64 now_ms) noexcept;
// Clock value from which populated rows count as recently used (0: none).
u64 glyph_atlas_growth_protect_after(const GlyphAtlasGrowth& growth) noexcept;
// Grows `height` by whole steps when the request cannot be placed on free
// space or a stale, unpinned row. True when the height changed.
bool glyph_atlas_growth_prepare(GlyphAtlasGrowth* growth, GlyphAtlasAllocator* atlas, u16 width,
                                u16 height) noexcept;
inline void glyph_atlas_growth_note_activity(GlyphAtlasGrowth* growth) noexcept {
    if (growth && growth->maximum_height) {
        growth->last_activity_ms = growth->now_ms;
        growth->trim_pending = true;
    }
}
inline u64 glyph_atlas_growth_trim_deadline(const GlyphAtlasGrowth& growth, u32 idle_ms) noexcept {
    return growth.maximum_height && growth.trim_pending ? growth.last_activity_ms + idle_ms : 0;
}
// Idle compaction: evicts unpinned rows not used within the protection
// window, moves live tail rows into the highest empty rows that fit (moves are
// reported for the owner's GPU copy) and shrinks to the step covering the rest.
u32 glyph_atlas_growth_trim(GlyphAtlasGrowth* growth, GlyphAtlasAllocator* atlas,
                            Slice<GlyphAtlasRowMove> moves, bool* resized) noexcept;

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
GlyphAtlasAllocation glyph_atlas_allocate_indexed(GlyphAtlasAllocator* atlas, GlyphAtlasIndex* index,
                                                  u64 key, u16 width, u16 height) noexcept;
GlyphAtlasAllocation glyph_atlas_allocate_unique_indexed(GlyphAtlasAllocator* atlas,
                                                         GlyphAtlasIndex* index, u64 key, u16 width,
                                                         u16 height) noexcept;

} // namespace maicord
