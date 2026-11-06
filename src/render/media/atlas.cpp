#include "maicord/render/media/atlas.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/gpu/gpu.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/media/core/slot_index.hpp"

namespace maicord {
namespace {
// Glyph allocator errors are the leading media atlas errors.
static_assert(static_cast<u8>(GlyphAtlasError::invalid_input) == static_cast<u8>(MediaAtlasError::invalid_input) &&
              static_cast<u8>(GlyphAtlasError::not_found) == static_cast<u8>(MediaAtlasError::not_found) &&
              static_cast<u8>(GlyphAtlasError::too_large) == static_cast<u8>(MediaAtlasError::too_large) &&
              static_cast<u8>(GlyphAtlasError::no_capacity) == static_cast<u8>(MediaAtlasError::no_capacity));

void clear_entry(MediaAtlasEntry* entry) noexcept {
    const u16 generation = entry->generation;
    reset_value<MediaAtlasEntry>(entry);
    entry->generation = generation;
}

MediaAtlasResult rejected_allocation(const GlyphAtlasAllocation& allocation, u16 width, u16 height,
                                     MediaAtlasError error) noexcept {
    return MediaAtlasResult{{}, 0, 0, width, height, allocation.evicted_entries, error, false, allocation.atlas_reset};
}

MediaAtlasResult failure(MediaAtlasError error, u16 width = 0,
                         u16 height = 0) noexcept {
    return MediaAtlasResult{{}, 0, 0, width, height, 0, error, false,
                            false};
}

MediaAtlasResult result_from(const MediaAtlasEntry& entry,
                             u32 slot,
                             const GlyphAtlasAllocation& allocation,
                             bool cached) noexcept {
    return MediaAtlasResult{
        MediaAtlasHandle{static_cast<u16>(slot), entry.generation},
        allocation.x,
        allocation.y,
        allocation.width,
        allocation.height,
        allocation.evicted_entries,
        MediaAtlasError::none,
        cached,
        allocation.atlas_reset,
    };
}

u32 find_bucket(const MediaAtlas* atlas, const MediaAssetKey& key) noexcept {
    return slot_index_find(atlas->buckets, atlas->bucket_capacity, media_asset_hash(key), [&](u32 slot) noexcept {
        return slot < atlas->entry_capacity && atlas->entries[slot].occupied &&
               media_asset_key_equal(atlas->entries[slot].key, key);
    });
}

bool insert_bucket(MediaAtlas* atlas, const MediaAssetKey& key, u32 slot) noexcept {
    return slot_index_insert(atlas->buckets, atlas->bucket_capacity, media_asset_hash(key), slot);
}

u32 find_free_slot(MediaAtlas* atlas) noexcept {
    return slot_table_take_free(atlas->entry_count, atlas->entry_capacity, &atlas->free_cursor,
                                [&](u32 slot) noexcept { return atlas->entries[slot].occupied != 0; });
}

void erase_entry(MediaAtlas* atlas, u32 slot, u32 bucket) noexcept {
    clear_entry(&atlas->entries[slot]);
    atlas->buckets[bucket] = kSlotIndexTombstone;
    atlas->free_cursor = slot;
    --atlas->entry_count;
    ++atlas->stale_entries;
}

// Rebuild, setup and reset run per atlas start or eviction pass: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool rebuild_buckets_and_clear_stale(MediaAtlas* atlas) noexcept {
    // Entry scans stop after the last occupied entry: capacity past the
    // working set is never touched.
    for (u32 index = 0, live = atlas->entry_count; index < atlas->entry_capacity && live; ++index) {
        MediaAtlasEntry& entry = atlas->entries[index];
        live -= entry.occupied;
        if (entry.occupied &&
            !glyph_atlas_validate(atlas->allocations, entry.allocation)) {
            clear_entry(&entry);
            --atlas->entry_count;
            ++atlas->stale_entries;
        }
    }
    for (u32 index = 0; index < atlas->bucket_capacity; ++index) {
        atlas->buckets[index] = kSlotIndexEmpty;
    }
    for (u32 slot = 0, live = atlas->entry_count; slot < atlas->entry_capacity && live; ++slot) {
        if (!atlas->entries[slot].occupied)
            continue;
        --live;
        if (!insert_bucket(atlas, atlas->entries[slot].key, slot))
            return false;
    }
    return true;
}

} // namespace

bool media_atlas_initialize(MediaAtlas* atlas, u16 width, u16 height,
                            u16 padding, Slice<MediaAtlasEntry> entries,
                            Slice<u32> buckets,
                            Slice<GlyphAtlasEntry> allocations,
                            Slice<GlyphAtlasRow> rows, bool zeroed) noexcept {
    if (!atlas || !entries.data || entries.count == 0 ||
        entries.count > 65'535 || !buckets.data ||
        buckets.count < entries.count * 2u ||
        allocations.count != entries.count ||
        !glyph_atlas_initialize(&atlas->allocations, width, height, padding,
                                allocations, rows, zeroed)) {
        return false;
    }
    atlas->entries = entries.data;
    atlas->buckets = buckets.data;
    atlas->entry_capacity = entries.count;
    atlas->bucket_capacity = buckets.count;
    atlas->entry_count = 0;
    atlas->free_cursor = 0;
    atlas->insertions = 0;
    atlas->stale_entries = 0;
    atlas->next_allocation_key = 0;
    atlas->disposal_backup_used_ms = 0;
    if (zeroed)
        return true; // kSlotIndexEmpty is zero
    for (u32 index = 0; index < entries.count; ++index) {
        reset_value<MediaAtlasEntry>(&entries[index]);
    }
    for (u32 index = 0; index < buckets.count; ++index) {
        buckets[index] = kSlotIndexEmpty;
    }
    return true;
}

bool media_atlas_configure_surface(MediaAtlas* atlas, MediaAtlasSurface surface) noexcept {
    const bool identity = surface == MediaAtlasSurface::identity;
    const u16 rows = identity ? kMediaAtlasPageRows : kPreviewMediaAtlasPageRows;
    const u16 commit_rows = gpu_paged_commit_rows(4);
    // One page of rows is kept beyond the visible ones as cache.
    const GlyphAtlasPaging paging{rows, static_cast<u8>(identity ? 8 : 4), commit_rows == 0, commit_rows, rows,
                                  kMediaAtlasProtectMs, false, {}};
    return atlas && atlas->entries && atlas->entry_count == 0 &&
           atlas->allocations.width == (identity ? kMediaAtlasExtent : kPreviewMediaAtlasExtent) &&
           glyph_atlas_configure_paging(&atlas->allocations, paging,
                                        identity ? ConstSlice<GlyphAtlasPool>{kMediaAtlasProfilePools, 3}
                                                 : ConstSlice<GlyphAtlasPool>{});
}

void media_atlas_reset(MediaAtlas* atlas) noexcept {
    if (!atlas || !atlas->entries) {
        return;
    }
    glyph_atlas_reset(&atlas->allocations);
    // Free entries are already clear apart from their generation.
    for (u32 index = 0, live = atlas->entry_count; index < atlas->entry_capacity && live; ++index) {
        live -= atlas->entries[index].occupied;
        clear_entry(&atlas->entries[index]);
    }
    for (u32 index = 0; index < atlas->bucket_capacity; ++index) {
        atlas->buckets[index] = kSlotIndexEmpty;
    }
    atlas->entry_count = 0;
    atlas->free_cursor = 0;
    atlas->next_allocation_key = 0;
}
#pragma clang attribute pop

void media_atlas_begin_visibility(MediaAtlas* atlas) noexcept {
    if (atlas) {
        glyph_atlas_clear_pins(&atlas->allocations);
    }
}

MediaAtlasResult media_atlas_peek(MediaAtlas* atlas, const MediaAssetKey& key) noexcept {
    if (!atlas || !atlas->entries || !media_asset_key_valid(key)) {
        return failure(MediaAtlasError::invalid_input);
    }
    const u32 bucket = find_bucket(atlas, key);
    if (bucket == 0xFFFFFFFFu) {
        return failure(MediaAtlasError::not_found);
    }
    const u32 slot = atlas->buckets[bucket] - 1u;
    MediaAtlasEntry& entry = atlas->entries[slot];
    GlyphAtlasAllocation allocation{};
    if (!glyph_atlas_validate(atlas->allocations, entry.allocation, &allocation)) {
        erase_entry(atlas, slot, bucket);
        return failure(MediaAtlasError::not_found);
    }
    return result_from(entry, slot, allocation, true);
}

MediaAtlasResult media_atlas_find(MediaAtlas* atlas, const MediaAssetKey& key) noexcept {
    const auto result = media_atlas_peek(atlas, key);
    if (result.ok() && !glyph_atlas_pin(&atlas->allocations, atlas->entries[result.handle.slot].allocation))
        return failure(MediaAtlasError::corrupt_state);
    return result;
}

bool media_atlas_unpin(MediaAtlas* atlas, const MediaAssetKey& key) noexcept {
    const auto result = media_atlas_peek(atlas, key);
    return result.ok() && glyph_atlas_unpin(&atlas->allocations, atlas->entries[result.handle.slot].allocation);
}

bool media_atlas_forget(MediaAtlas* atlas, MediaAssetKey key) noexcept {
    if (!atlas || !atlas->entries || !media_asset_key_valid(key))
        return false;
    const u32 bucket = find_bucket(atlas, key);
    if (bucket == 0xFFFFFFFFu)
        return false;
    const u32 slot = atlas->buckets[bucket] - 1u;
    glyph_atlas_release(&atlas->allocations, atlas->entries[slot].allocation);
    erase_entry(atlas, slot, bucket);
    return true;
}

// A key the profile pools hold: their index + 1, else 0 (the general region).
static u8 media_atlas_pool(const MediaAtlas& atlas, const MediaAssetKey& key, u16 width,
                           u16 height) noexcept {
    if (key.kind != MediaAssetKind::profile_image)
        return 0;
    // A pool takes what fills at least half of its slot.
    for (u32 index = 0; index < atlas.allocations.pool_count; ++index) {
        const GlyphAtlasPool& pool = atlas.allocations.pools[index];
        if (width <= pool.slot_width && height <= pool.slot_height &&
            u32{width} * height * 2u > u32{pool.slot_width} * pool.slot_height)
            return static_cast<u8>(index + 1);
    }
    return 0;
}

MediaAtlasResult media_atlas_allocate(MediaAtlas* atlas, MediaAssetKey key,
                                      u16 width, u16 height) noexcept {
    if (!atlas || !atlas->entries || !media_asset_key_valid(key) || width == 0 ||
        height == 0) {
        return failure(MediaAtlasError::invalid_input, width, height);
    }
    MediaAtlasResult cached = media_atlas_find(atlas, key);
    if (cached.ok()) {
        if (cached.width != width || cached.height != height) {
            cached.error = MediaAtlasError::invalid_input;
        }
        return cached;
    }
    if (cached.error != MediaAtlasError::not_found) {
        return cached;
    }

    // Stale logical identities are harmless until the open-addressed table is
    // full; sweeping every slot before each upload would make a burst of N
    // unique visible emoji O(N^2). When full, the physical allocator evicts by
    // LRU first and only the identities that made stale are reclaimed.
    if (atlas->next_allocation_key == ~0ull) {
        media_atlas_reset(atlas);
    }
    const u64 allocation_key = ++atlas->next_allocation_key;
    const GlyphAtlasAllocation allocation = glyph_atlas_allocate_unique(
        &atlas->allocations, allocation_key, width, height, media_atlas_pool(*atlas, key, width, height));
    if (!allocation.ok())
        return rejected_allocation(allocation, width, height, static_cast<MediaAtlasError>(allocation.error));

    if ((allocation.evicted_entries != 0 || allocation.atlas_reset ||
         atlas->entry_count >= atlas->entry_capacity) &&
        !rebuild_buckets_and_clear_stale(atlas)) {
        return failure(MediaAtlasError::corrupt_state, width, height);
    }

    const u32 slot = find_free_slot(atlas);
    // A bookkeeping-capacity miss must not reset the shared GPU atlas: that
    // would invalidate unrelated visible media.
    if (slot == 0xFFFFFFFFu || !insert_bucket(atlas, key, slot))
        return rejected_allocation(allocation, width, height, MediaAtlasError::corrupt_state);
    MediaAtlasEntry& entry = atlas->entries[slot];
    clear_entry(&entry);
    entry.key = key;
    entry.allocation = allocation.handle;
    entry.generation = next_generation(entry.generation);
    entry.occupied = true;
    if (!glyph_atlas_pin(&atlas->allocations, entry.allocation)) {
        const u32 bucket = find_bucket(atlas, key);
        if (bucket != 0xFFFFFFFFu) {
            atlas->buckets[bucket] = kSlotIndexTombstone;
        }
        entry.occupied = false;
        return rejected_allocation(allocation, width, height, MediaAtlasError::corrupt_state);
    }
    ++atlas->entry_count;
    ++atlas->insertions;
    return result_from(entry, slot, allocation, false);
}

void media_atlas_observe_time(MediaAtlas* atlas, u64 now_ms) noexcept {
    if (atlas) {
        glyph_atlas_observe_time(&atlas->allocations, now_ms);
    }
}

u64 media_atlas_trim_deadline(const MediaAtlas& atlas) noexcept {
    return glyph_atlas_trim_deadline(atlas.allocations, kMediaAtlasIdleTrimMs);
}

// Idle trim runs per atlas pass: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
u32 media_atlas_trim(MediaAtlas* atlas) noexcept {
    if (!atlas || !atlas->entries) {
        return 0;
    }
    const u32 evicted = glyph_atlas_trim(&atlas->allocations);
    if (evicted != 0) {
        rebuild_buckets_and_clear_stale(atlas);
    }
    return evicted;
}
#pragma clang attribute pop

bool media_atlas_validate(const MediaAtlas& atlas, MediaAtlasHandle handle,
                          MediaAtlasResult* result) noexcept {
    if (!atlas.entries || handle.generation == 0 ||
        handle.slot >= atlas.entry_capacity) {
        return false;
    }
    const MediaAtlasEntry& entry = atlas.entries[handle.slot];
    GlyphAtlasAllocation allocation{};
    if (!entry.occupied || entry.generation != handle.generation ||
        !glyph_atlas_validate(atlas.allocations, entry.allocation,
                              &allocation)) {
        return false;
    }
    if (result) {
        *result = result_from(entry, handle.slot, allocation, true);
    }
    return true;
}

bool media_atlas_resolve_visible(MediaAtlas* atlas, MediaAtlasHandle handle,
                                 MediaAtlasResult* result) noexcept {
    if (result) {
        reset_value<MediaAtlasResult>(result);
    }
    if (!atlas || !result || !media_atlas_validate(*atlas, handle, result)) {
        return false;
    }
    const MediaAtlasEntry& entry = atlas->entries[handle.slot];
    return glyph_atlas_pin(&atlas->allocations, entry.allocation);
}

bool media_atlas_set_decoded_animation(MediaAtlas* atlas, MediaAtlasHandle handle,
                                       bool animated) noexcept {
    if (!atlas || !media_atlas_validate(*atlas, handle))
        return false;
    auto& flags = atlas->entries[handle.slot].flags;
    if (animated)
        flags |= media_atlas_entry_decoded_animation;
    else
        flags &= static_cast<u8>(~media_atlas_entry_decoded_animation);
    return true;
}

bool media_atlas_decoded_animation(const MediaAtlas& atlas, MediaAtlasHandle handle) noexcept {
    return media_atlas_validate(atlas, handle) &&
           (atlas.entries[handle.slot].flags & media_atlas_entry_decoded_animation) != 0;
}

} // namespace maicord
