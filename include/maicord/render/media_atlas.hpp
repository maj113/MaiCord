#pragma once

#include "maicord/media/asset_key.hpp"
#include "maicord/render/glyph_atlas.hpp"

namespace maicord {

struct MediaAtlasHandle {
    u16 slot;
    u16 generation;
};

enum MediaAtlasEntryFlags : u8 {
    media_atlas_entry_decoded_animation = 1u << 0,
};

struct MediaAtlasEntry {
    MediaAssetKey key;
    GlyphAtlasHandle allocation;
    u16 generation;
    bool occupied;
    u8 flags;
};

struct MediaAtlas {
    GlyphAtlasAllocator allocations;
    MediaAtlasEntry* entries;
    u32* buckets;
    u32 entry_capacity;
    u32 bucket_capacity;
    u32 entry_count;
    u32 free_cursor;
    u32 insertions;
    u32 reuse_requests;
    u32 stale_entries;
    u32 rejected;
    u64 next_allocation_key;
    // Height growth/idle trim (zero maximum: fixed size). The renderer resizes
    // the GPU surface whenever allocations.height changes.
    GlyphAtlasGrowth growth;
    // Frames that needed the previous-disposal backup and the last such use;
    // the backup texture is released once unused for kMediaAtlasIdleTrimMs.
    u32 disposal_backup_frames;
    u64 disposal_backup_used_ms;
};

// Rows used within this window are kept (and the atlas grows instead) while
// below capacity; trim runs once after this much allocation idleness.
inline constexpr u32 kMediaAtlasProtectMs = 8'000;
inline constexpr u32 kMediaAtlasIdleTrimMs = 10'000;

enum class MediaAtlasError : u8 {
    none,
    invalid_input,
    not_found,
    too_large,
    no_capacity,
    corrupt_state,
};

struct MediaAtlasResult {
    MediaAtlasHandle handle;
    u16 x;
    u16 y;
    u16 width;
    u16 height;
    u32 evicted_entries;
    MediaAtlasError error;
    bool cached;
    bool atlas_reset;

    constexpr bool ok() const noexcept {
        return error == MediaAtlasError::none;
    }
};

// zeroed: every array is freshly committed zero pages and is left untouched.
bool media_atlas_initialize(MediaAtlas* atlas, u16 width, u16 height,
                            u16 padding, Slice<MediaAtlasEntry> entries,
                            Slice<u32> buckets,
                            Slice<GlyphAtlasEntry> allocations,
                            Slice<GlyphAtlasRow> rows, bool zeroed = false) noexcept;
void media_atlas_reset(MediaAtlas* atlas) noexcept;
void media_atlas_begin_visibility(MediaAtlas* atlas) noexcept;
MediaAtlasResult media_atlas_find(MediaAtlas* atlas,
                                  const MediaAssetKey& key) noexcept;
// Cache admission is not visibility; speculative lookups must not pin shelves.
MediaAtlasResult media_atlas_peek(MediaAtlas* atlas, const MediaAssetKey& key) noexcept;
bool media_atlas_unpin(MediaAtlas* atlas, const MediaAssetKey& key) noexcept;
// Visible decorative media (member nameplates and avatar decorations) gives
// way to a foreground image that found no room: their pins for this
// visibility epoch drop, so allocation may evict shelves holding nothing else
// visible. Returns how many resident keys were unpinned.
u32 media_atlas_yield(MediaAtlas* atlas, ConstSlice<MediaAssetKey> decorative) noexcept;
// Drops a resident identity so the key can be allocated at another size (a
// video whose reduced picture changed extent). Its physical rows become stale
// and are reclaimed by normal LRU eviction. False when the key was not resident.
bool media_atlas_forget(MediaAtlas* atlas, MediaAssetKey key) noexcept;
MediaAtlasResult media_atlas_allocate(MediaAtlas* atlas, MediaAssetKey key,
                                      u16 width, u16 height) noexcept;
bool media_atlas_validate(const MediaAtlas& atlas, MediaAtlasHandle handle,
                          MediaAtlasResult* result = nullptr) noexcept;
// Decoder classification belongs to the current physical residency, not the
// message owner or an asset metadata hint. Stale handles cannot read or change it.
bool media_atlas_set_decoded_animation(MediaAtlas* atlas, MediaAtlasHandle handle,
                                       bool animated) noexcept;
bool media_atlas_decoded_animation(const MediaAtlas& atlas, MediaAtlasHandle handle) noexcept;
// Resolves a generation-safe handle and pins its allocation for the current
// visibility epoch in O(1), unlike key lookup.
bool media_atlas_resolve_visible(MediaAtlas* atlas, MediaAtlasHandle handle,
                                 MediaAtlasResult* result) noexcept;

// Growable atlases start at initial_height and grow by `step` up to the
// initialized height. Call right after initialization.
bool media_atlas_configure_growth(MediaAtlas* atlas, u16 initial_height, u16 step,
                                  u32 protect_ms = kMediaAtlasProtectMs) noexcept;
void media_atlas_observe_time(MediaAtlas* atlas, u64 now_ms) noexcept;
// Largest height the atlas can reach (requests are validated against it).
u16 media_atlas_capacity_height(const MediaAtlas& atlas) noexcept;
// Before allocating: grows allocations.height when the request needs it.
// True means the owner must resize its texture before writing pixels.
bool media_atlas_prepare(MediaAtlas* atlas, MediaAssetKey key, u16 width, u16 height) noexcept;
// Undo a prepare whose texture resize failed (no allocation happened since).
void media_atlas_revert_growth(MediaAtlas* atlas, u16 height) noexcept;
// Zero when no trim is pending; otherwise the earliest time it may run.
u64 media_atlas_trim_deadline(const MediaAtlas& atlas) noexcept;
// Idle trim; returns the row moves the owner must copy (old -> new texture).
u32 media_atlas_trim(MediaAtlas* atlas, Slice<GlyphAtlasRowMove> moves, bool* resized) noexcept;
// After media_atlas_reset with the GPU surface released: back to initial height.
void media_atlas_reset_growth(MediaAtlas* atlas) noexcept;

static_assert(sizeof(MediaAtlasEntry) == 24);

} // namespace maicord
