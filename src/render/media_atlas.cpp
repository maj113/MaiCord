#include "maicord/render/media_atlas.hpp"

namespace maicord {
namespace {

inline constexpr u32 kEmptyBucket = 0;
inline constexpr u32 kTombstoneBucket = 0xFFFFFFFFu;

constexpr u16 next_generation(u16 generation) noexcept {
    ++generation;
    return generation == 0 ? 1 : generation;
}

// Bucket tables are powers of two in practice: mask instead of dividing on
// every probe of the per-frame visibility lookups.
u32 wrap_bucket(u64 value, u32 capacity) noexcept {
    return (capacity & (capacity - 1)) == 0 ? static_cast<u32>(value) & (capacity - 1)
                                            : static_cast<u32>(value % capacity);
}

u32 key_bucket(MediaAssetKey key, u32 capacity) noexcept {
    return wrap_bucket(media_asset_hash(key), capacity);
}

bool key_valid(MediaAssetKey key) noexcept {
    return media_asset_key_valid(key);
}

MediaAtlasError translate_error(GlyphAtlasError error) noexcept {
    switch (error) {
    case GlyphAtlasError::none:
        return MediaAtlasError::none;
    case GlyphAtlasError::invalid_input:
        return MediaAtlasError::invalid_input;
    case GlyphAtlasError::not_found:
        return MediaAtlasError::not_found;
    case GlyphAtlasError::too_large:
        return MediaAtlasError::too_large;
    case GlyphAtlasError::no_capacity:
        return MediaAtlasError::no_capacity;
    default:
        return MediaAtlasError::corrupt_state;
    }
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

u32 find_bucket(const MediaAtlas* atlas, MediaAssetKey key) noexcept {
    if (!atlas || !atlas->entries || !atlas->buckets ||
        atlas->bucket_capacity == 0 || !key_valid(key)) {
        return 0xFFFFFFFFu;
    }
    const u32 first = key_bucket(key, atlas->bucket_capacity);
    for (u32 probe = 0; probe < atlas->bucket_capacity; ++probe) {
        const u32 bucket = wrap_bucket(first + probe, atlas->bucket_capacity);
        const u32 value = atlas->buckets[bucket];
        if (value == kEmptyBucket) {
            break;
        }
        if (value == kTombstoneBucket) {
            continue;
        }
        const u32 slot = value - 1u;
        if (slot < atlas->entry_capacity && atlas->entries[slot].occupied &&
            media_asset_key_equal(atlas->entries[slot].key, key)) {
            return bucket;
        }
    }
    return 0xFFFFFFFFu;
}

bool insert_bucket(MediaAtlas* atlas, MediaAssetKey key, u32 slot) noexcept {
    const u32 first = key_bucket(key, atlas->bucket_capacity);
    u32 tombstone = 0xFFFFFFFFu;
    for (u32 probe = 0; probe < atlas->bucket_capacity; ++probe) {
        const u32 bucket = wrap_bucket(first + probe, atlas->bucket_capacity);
        const u32 value = atlas->buckets[bucket];
        if (value == kTombstoneBucket) {
            if (tombstone == 0xFFFFFFFFu) {
                tombstone = bucket;
            }
            continue;
        }
        if (value == kEmptyBucket) {
            atlas->buckets[tombstone == 0xFFFFFFFFu ? bucket : tombstone] =
                slot + 1u;
            return true;
        }
    }
    if (tombstone != 0xFFFFFFFFu) {
        atlas->buckets[tombstone] = slot + 1u;
        return true;
    }
    return false;
}

u32 find_free_slot(MediaAtlas* atlas) noexcept {
    if (!atlas || atlas->entry_count >= atlas->entry_capacity) {
        return 0xFFFFFFFFu;
    }
    const u32 first = atlas->free_cursor % atlas->entry_capacity;
    for (u32 offset = 0; offset < atlas->entry_capacity; ++offset) {
        const u32 slot = (first + offset) % atlas->entry_capacity;
        if (!atlas->entries[slot].occupied) {
            atlas->free_cursor = (slot + 1u) % atlas->entry_capacity;
            return slot;
        }
    }
    return 0xFFFFFFFFu;
}

void erase_entry(MediaAtlas* atlas, u32 slot, u32 bucket) noexcept {
    MediaAtlasEntry& entry = atlas->entries[slot];
    const u16 generation = entry.generation;
    entry = MediaAtlasEntry{};
    entry.generation = generation;
    atlas->buckets[bucket] = kTombstoneBucket;
    atlas->free_cursor = slot;
    --atlas->entry_count;
    ++atlas->stale_entries;
}

bool rebuild_buckets_and_clear_stale(MediaAtlas* atlas) noexcept {
    if (!atlas || !atlas->entries || !atlas->buckets) {
        return false;
    }
    // Entry scans stop after the last occupied entry: capacity past the
    // working set is never touched.
    for (u32 index = 0, live = atlas->entry_count; index < atlas->entry_capacity && live; ++index) {
        MediaAtlasEntry& entry = atlas->entries[index];
        live -= entry.occupied;
        if (entry.occupied &&
            !glyph_atlas_validate(atlas->allocations, entry.allocation)) {
            const u16 generation = entry.generation;
            entry = MediaAtlasEntry{};
            entry.generation = generation;
            --atlas->entry_count;
            ++atlas->stale_entries;
        }
    }
    for (u32 index = 0; index < atlas->bucket_capacity; ++index) {
        atlas->buckets[index] = kEmptyBucket;
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
    atlas->reuse_requests = 0;
    atlas->stale_entries = 0;
    atlas->rejected = 0;
    atlas->next_allocation_key = 0;
    atlas->growth = GlyphAtlasGrowth{};
    atlas->disposal_backup_frames = 0;
    atlas->disposal_backup_used_ms = 0;
    if (zeroed)
        return true; // kEmptyBucket is zero
    for (u32 index = 0; index < entries.count; ++index) {
        entries[index] = MediaAtlasEntry{};
    }
    for (u32 index = 0; index < buckets.count; ++index) {
        buckets[index] = kEmptyBucket;
    }
    return true;
}

void media_atlas_reset(MediaAtlas* atlas) noexcept {
    if (!atlas || !atlas->entries || !atlas->buckets ||
        !atlas->allocations.entries) {
        return;
    }
    glyph_atlas_reset(&atlas->allocations);
    // Free entries are already clear apart from their generation.
    for (u32 index = 0, live = atlas->entry_count; index < atlas->entry_capacity && live; ++index) {
        live -= atlas->entries[index].occupied;
        const u16 generation = atlas->entries[index].generation;
        atlas->entries[index] = MediaAtlasEntry{};
        atlas->entries[index].generation = generation;
    }
    for (u32 index = 0; index < atlas->bucket_capacity; ++index) {
        atlas->buckets[index] = kEmptyBucket;
    }
    atlas->entry_count = 0;
    atlas->free_cursor = 0;
    atlas->next_allocation_key = 0;
}

void media_atlas_begin_visibility(MediaAtlas* atlas) noexcept {
    if (atlas) {
        glyph_atlas_clear_pins(&atlas->allocations);
    }
}

MediaAtlasResult media_atlas_peek(MediaAtlas* atlas, MediaAssetKey key) noexcept {
    if (!atlas || !atlas->entries || !atlas->buckets ||
        !atlas->allocations.entries ||
        !key_valid(key)) {
        if (atlas) {
            ++atlas->rejected;
        }
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

MediaAtlasResult media_atlas_find(MediaAtlas* atlas, MediaAssetKey key) noexcept {
    const auto result = media_atlas_peek(atlas, key);
    if (result.ok() && !glyph_atlas_pin(&atlas->allocations, atlas->entries[result.handle.slot].allocation))
        return failure(MediaAtlasError::corrupt_state);
    return result;
}

bool media_atlas_unpin(MediaAtlas* atlas, MediaAssetKey key) noexcept {
    const auto result = media_atlas_peek(atlas, key);
    return result.ok() && glyph_atlas_unpin(&atlas->allocations, atlas->entries[result.handle.slot].allocation);
}

bool media_atlas_forget(MediaAtlas* atlas, MediaAssetKey key) noexcept {
    if (!atlas || !atlas->entries || !atlas->buckets || !key_valid(key))
        return false;
    const u32 bucket = find_bucket(atlas, key);
    if (bucket == 0xFFFFFFFFu)
        return false;
    const u32 slot = atlas->buckets[bucket] - 1u;
    glyph_atlas_unpin(&atlas->allocations, atlas->entries[slot].allocation);
    erase_entry(atlas, slot, bucket);
    return true;
}

MediaAtlasResult media_atlas_allocate(MediaAtlas* atlas, MediaAssetKey key,
                                      u16 width, u16 height) noexcept {
    if (!atlas || !atlas->entries || !key_valid(key) || width == 0 ||
        height == 0) {
        if (atlas) {
            ++atlas->rejected;
        }
        return failure(MediaAtlasError::invalid_input, width, height);
    }
    MediaAtlasResult cached = media_atlas_find(atlas, key);
    if (cached.ok()) {
        if (cached.width != width || cached.height != height) {
            ++atlas->rejected;
            cached.error = MediaAtlasError::invalid_input;
            return cached;
        }
        ++atlas->reuse_requests;
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
    // Below capacity, recently used rows are not reclaimed: prepare already
    // grew the atlas when nothing stale could hold the request. If the
    // protected attempt still fails (bookkeeping limits), fall back to plain LRU.
    const bool protect = atlas->growth.maximum_height != 0 &&
                         atlas->allocations.height < atlas->growth.maximum_height;
    atlas->allocations.protect_after = protect ? glyph_atlas_growth_protect_after(atlas->growth) : 0;
    GlyphAtlasAllocation allocation = glyph_atlas_allocate_unique(
        &atlas->allocations, allocation_key, width, height);
    atlas->allocations.protect_after = 0;
    if (!allocation.ok() && protect && allocation.error == GlyphAtlasError::no_capacity) {
        allocation = glyph_atlas_allocate_unique(&atlas->allocations, allocation_key, width, height);
    }
    if (!allocation.ok()) {
        ++atlas->rejected;
        return MediaAtlasResult{{}, 0, 0, width, height,
                                allocation.evicted_entries,
                                translate_error(allocation.error), false,
                                allocation.atlas_reset};
    }

    if ((allocation.evicted_entries != 0 || allocation.atlas_reset ||
         atlas->entry_count >= atlas->entry_capacity) &&
        !rebuild_buckets_and_clear_stale(atlas)) {
        ++atlas->rejected;
        return failure(MediaAtlasError::corrupt_state, width, height);
    }

    const u32 slot = find_free_slot(atlas);
    if (slot == 0xFFFFFFFFu || !insert_bucket(atlas, key, slot)) {
        // A bookkeeping-capacity miss must not reset the shared GPU atlas: that
        // would invalidate unrelated visible media.
        ++atlas->rejected;
        return MediaAtlasResult{{}, 0, 0, width, height,
                                allocation.evicted_entries,
                                MediaAtlasError::corrupt_state, false,
                                allocation.atlas_reset};
    }
    MediaAtlasEntry& entry = atlas->entries[slot];
    const u16 generation = entry.generation;
    entry = MediaAtlasEntry{};
    entry.key = key;
    entry.allocation_key = allocation_key;
    entry.allocation = allocation.handle;
    entry.generation = next_generation(generation);
    entry.occupied = true;
    if (!glyph_atlas_pin(&atlas->allocations, entry.allocation)) {
        const u32 bucket = find_bucket(atlas, key);
        if (bucket != 0xFFFFFFFFu) {
            atlas->buckets[bucket] = kTombstoneBucket;
        }
        entry.occupied = false;
        ++atlas->rejected;
        return MediaAtlasResult{{}, 0, 0, width, height,
                                allocation.evicted_entries,
                                MediaAtlasError::corrupt_state, false,
                                allocation.atlas_reset};
    }
    ++atlas->entry_count;
    ++atlas->insertions;
    glyph_atlas_growth_note_activity(&atlas->growth);
    return result_from(entry, slot, allocation, false);
}

bool media_atlas_configure_growth(MediaAtlas* atlas, u16 initial_height, u16 step, u32 protect_ms) noexcept {
    return atlas && atlas->entries && atlas->entry_count == 0 &&
           glyph_atlas_growth_configure(&atlas->growth, &atlas->allocations, initial_height, step, protect_ms);
}

void media_atlas_observe_time(MediaAtlas* atlas, u64 now_ms) noexcept {
    if (atlas) {
        glyph_atlas_growth_observe(&atlas->growth, atlas->allocations, now_ms);
    }
}

u16 media_atlas_capacity_height(const MediaAtlas& atlas) noexcept {
    return atlas.growth.maximum_height ? atlas.growth.maximum_height : atlas.allocations.height;
}

bool media_atlas_prepare(MediaAtlas* atlas, MediaAssetKey key, u16 width, u16 height) noexcept {
    if (!atlas || !atlas->entries || !atlas->growth.maximum_height || !key_valid(key)) {
        return false;
    }
    // A resident identity needs no space; peek does not pin it.
    const u32 bucket = find_bucket(atlas, key);
    if (bucket != 0xFFFFFFFFu &&
        glyph_atlas_validate(atlas->allocations, atlas->entries[atlas->buckets[bucket] - 1u].allocation)) {
        return false;
    }
    return glyph_atlas_growth_prepare(&atlas->growth, &atlas->allocations, width, height);
}

void media_atlas_revert_growth(MediaAtlas* atlas, u16 height) noexcept {
    if (atlas && atlas->growth.maximum_height && height >= atlas->allocations.next_y &&
        height < atlas->allocations.height) {
        atlas->allocations.height = height;
        if (atlas->growth.growths) {
            --atlas->growth.growths;
        }
    }
}

u64 media_atlas_trim_deadline(const MediaAtlas& atlas) noexcept {
    return glyph_atlas_growth_trim_deadline(atlas.growth, kMediaAtlasIdleTrimMs);
}

u32 media_atlas_trim(MediaAtlas* atlas, Slice<GlyphAtlasRowMove> moves, bool* resized) noexcept {
    if (resized) {
        *resized = false;
    }
    if (!atlas || !atlas->entries || !atlas->growth.maximum_height) {
        return 0;
    }
    const u32 evicted_before = atlas->allocations.evicted_entries;
    const u32 moved = glyph_atlas_growth_trim(&atlas->growth, &atlas->allocations, moves, resized);
    if (atlas->allocations.evicted_entries != evicted_before) {
        rebuild_buckets_and_clear_stale(atlas);
    }
    return moved;
}

void media_atlas_reset_growth(MediaAtlas* atlas) noexcept {
    if (atlas && atlas->growth.maximum_height && atlas->allocations.row_count == 0) {
        atlas->allocations.height = atlas->growth.minimum_height;
        atlas->allocations.next_y = 0;
        atlas->growth.trim_pending = false;
    }
}

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
        *result = MediaAtlasResult{};
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
