#pragma once

#include "maicord/media/core/asset_key.hpp"
#include "maicord/render/text/glyph_atlas.hpp"

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
    u32 stale_entries;
    u64 next_allocation_key;
    // The last use of the previous-disposal backup; the backup's pages are
    // released once unused for kMediaAtlasIdleTrimMs.
    u64 disposal_backup_used_ms;
};

// Idle trim runs once allocation has been idle this long and drops what was
// neither visible nor used within kMediaAtlasProtectMs.
inline constexpr u32 kMediaAtlasProtectMs = 8'000;
inline constexpr u32 kMediaAtlasIdleTrimMs = 10'000;

// The two media surfaces. Identity media (avatars, emoji, stickers, icons,
// profile pictures): 2048 x 8 pages of 1024 rows (8 MiB a page), 8 MiB of
// rows kept beyond the visible ones, and the profile pools below. Large
// previews and call video: 2064 x 4 pages of 2176 rows (17 MiB a page: the
// px2048 tier with its padding, in whole 128-row tile bands), one page kept
// beyond the visible ones.
enum class MediaAtlasSurface : u8 { identity, preview };
inline constexpr u16 kMediaAtlasExtent = 2'048;
inline constexpr u16 kMediaAtlasPageRows = 1'024;
inline constexpr u16 kPreviewMediaAtlasExtent = 2'064;
inline constexpr u16 kPreviewMediaAtlasPageRows = 2'176;
// Profile media the CDN serves at fixed sizes, each in pages of its own (a
// slot's padded size; memory of a full pool on Direct3D 11's 128-row bands):
//  - effect layers 450x880, four a row: two cards of four layers (2 rows, 14 MiB);
//  - banners 600x211 still and 512 wide animated, three a row: 8 (3 rows, 6 MiB);
//  - nameplates 448x84, four a row: a member list screen of 24 (6 rows, 5 MiB).
inline constexpr GlyphAtlasPool kMediaAtlasProfilePools[] = {
    {450, 880, 8, 0},
    {600, 240, 8, 0},
    {448, 84, 24, 0},
};

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
// The atlas is then a fixed one of `height` rows (tests);
// media_atlas_configure_surface makes it one of the client's virtual surfaces.
bool media_atlas_initialize(MediaAtlas* atlas, u16 width, u16 height,
                            u16 padding, Slice<MediaAtlasEntry> entries,
                            Slice<u32> buckets,
                            Slice<GlyphAtlasEntry> allocations,
                            Slice<GlyphAtlasRow> rows, bool zeroed = false) noexcept;
// Right after initialization, on an atlas initialized at the surface's width:
// its pages, the backend's commit unit, its budget and (identity) the pools.
bool media_atlas_configure_surface(MediaAtlas* atlas, MediaAtlasSurface surface) noexcept;
void media_atlas_reset(MediaAtlas* atlas) noexcept;
void media_atlas_begin_visibility(MediaAtlas* atlas) noexcept;
MediaAtlasResult media_atlas_find(MediaAtlas* atlas,
                                  const MediaAssetKey& key) noexcept;
// Cache admission is not visibility; speculative lookups must not pin shelves.
MediaAtlasResult media_atlas_peek(MediaAtlas* atlas, const MediaAssetKey& key) noexcept;
bool media_atlas_unpin(MediaAtlas* atlas, const MediaAssetKey& key) noexcept;
// Drops a resident identity so the key can be allocated at another size (a
// video whose reduced picture changed extent); its slot is reused. False when
// the key was not resident.
bool media_atlas_forget(MediaAtlas* atlas, MediaAssetKey key) noexcept;
// A key the profile pools hold: their index + 1, else 0 (the general region).
u8 media_atlas_pool(const MediaAtlas& atlas, const MediaAssetKey& key, u16 width, u16 height) noexcept;
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

void media_atlas_observe_time(MediaAtlas* atlas, u64 now_ms) noexcept;
// Zero when no trim is pending; otherwise the earliest time it may run.
u64 media_atlas_trim_deadline(const MediaAtlas& atlas) noexcept;
// Idle trim (glyph_atlas_trim); the owner then returns the decommitted rows.
u32 media_atlas_trim(MediaAtlas* atlas) noexcept;

static_assert(sizeof(MediaAtlasEntry) == 24);

} // namespace maicord
