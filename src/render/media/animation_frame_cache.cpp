#include "maicord/render/media/animation_frame_cache.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/media/core/slot_index.hpp"

namespace maicord {
namespace {

bool key_valid(const AnimationFrameCacheKey& key) noexcept {
    return media_asset_key_valid(key.asset) && key.source_generation != 0;
}

bool key_equal(const AnimationFrameCacheKey& left,
               const AnimationFrameCacheKey& right) noexcept {
    if (left.content_identity != 0 || right.content_identity != 0)
        return left.content_identity == right.content_identity &&
               left.frame_index == right.frame_index;
    return media_asset_key_equal(left.asset, right.asset) &&
           left.frame_index == right.frame_index &&
           left.source_generation == right.source_generation;
}

u64 key_hash(const AnimationFrameCacheKey& key) noexcept {
    if (key.content_identity != 0)
        return media_asset_hash_mix(key.content_identity ^ media_asset_hash_mix(key.frame_index));
    const u64 asset_tail = static_cast<u64>(key.asset.variant) |
                           (static_cast<u64>(key.asset.flags) << 32) |
                           (static_cast<u64>(key.asset.kind) << 48);
    const u64 frame_tail = static_cast<u64>(key.frame_index) |
                           (static_cast<u64>(key.source_generation) << 32);
    return media_asset_hash_mix(key.asset.id ^ media_asset_hash_mix(asset_tail) ^
                                media_asset_hash_mix(frame_tail));
}

AnimationFrameCacheResult failure(AnimationFrameCacheError error) noexcept {
    AnimationFrameCacheResult result{};
    result.error = error;
    return result;
}

AnimationFrameCacheError translate_error(GlyphAtlasError error) noexcept {
    switch (error) {
    case GlyphAtlasError::none:
        return AnimationFrameCacheError::none;
    case GlyphAtlasError::invalid_input:
        return AnimationFrameCacheError::invalid_input;
    case GlyphAtlasError::too_large:
        return AnimationFrameCacheError::too_large;
    case GlyphAtlasError::no_capacity:
        return AnimationFrameCacheError::no_capacity;
    default:
        return AnimationFrameCacheError::corrupt_state;
    }
}

// Erased buckets are back-shifted (erase_bucket): the index holds no tombstones.
u32 find_bucket(const AnimationFrameCache* cache,
                const AnimationFrameCacheKey& key) noexcept {
    return slot_index_find(cache->buckets, cache->bucket_capacity, key_hash(key), [&](u32 slot) noexcept {
        return slot < cache->entry_capacity && cache->entries[slot].occupied &&
               key_equal(cache->entries[slot].key, key);
    });
}

bool insert_bucket(AnimationFrameCache* cache,
                   const AnimationFrameCacheKey& key, u32 slot, u32* home_bucket) noexcept {
    const u64 hash = key_hash(key);
    *home_bucket = slot_index_wrap(hash, cache->bucket_capacity);
    return slot_index_insert(cache->buckets, cache->bucket_capacity, hash, slot);
}

void erase_bucket(AnimationFrameCache* cache, u32 bucket) noexcept {
    u32 hole = bucket;
    u32 current = bucket;
    for (u32 visited = 1; visited < cache->bucket_capacity; ++visited) {
        if (++current == cache->bucket_capacity)
            current = 0;
        const u32 value = cache->buckets[current];
        if (value == kSlotIndexEmpty)
            break;
        const u32 slot = value - 1;
        if (slot >= cache->entry_capacity || !cache->entries[slot].occupied)
            continue;
        const u32 home = cache->entries[slot].home_bucket;
        const u32 hole_distance = hole >= home ? hole - home : cache->bucket_capacity - home + hole;
        const u32 current_distance = current >= home ? current - home : cache->bucket_capacity - home + current;
        // Continue past keys at their home bucket. A later key can still cross the hole.
        if (hole_distance < current_distance) {
            cache->buckets[hole] = value;
            hole = current;
        }
    }
    cache->buckets[hole] = kSlotIndexEmpty;
}

void erase_entry(AnimationFrameCache* cache, u32 slot,
                 u32 bucket) noexcept {
    reset_value<AnimationFrameCacheEntry>(&cache->entries[slot]);
    erase_bucket(cache, bucket);
    cache->free_cursor = slot;
    --cache->entry_count;
    ++cache->counters.evictions;
}

void clear_stale(AnimationFrameCache* cache) noexcept {
    for (u32 slot = 0; slot < cache->entry_capacity; ++slot) {
        const AnimationFrameCacheEntry& entry = cache->entries[slot];
        if (!entry.occupied ||
            glyph_atlas_validate(cache->allocations, entry.allocation)) {
            continue;
        }
        const u32 bucket = find_bucket(cache, entry.key);
        if (bucket != 0xFFFFFFFFu) {
            erase_entry(cache, slot, bucket);
        }
    }
}

u32 find_free_slot(AnimationFrameCache* cache) noexcept {
    return slot_table_take_free(cache->entry_count, cache->entry_capacity, &cache->free_cursor,
                                [&](u32 slot) noexcept { return cache->entries[slot].occupied != 0; });
}

AnimationFrameCacheResult result_from(
    const AnimationFrameCacheEntry& entry,
    const GlyphAtlasAllocation& allocation, bool cached) noexcept {
    AnimationFrameCacheResult result{};
    result.allocation = allocation;
    result.control = entry.control;
    result.duration_us = entry.duration_us;
    result.background_rgba = entry.background_rgba;
    result.frame_index = entry.key.frame_index;
    result.source_generation = entry.key.source_generation;
    result.kind = entry.kind;
    result.error = AnimationFrameCacheError::none;
    result.cached = cached;
    return result;
}

} // namespace

// Storage setup, growth and reset run per animation size or atlas reset:
// compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool animation_frame_cache_initialize(
    AnimationFrameCache* cache, u16 width, u16 height, u16 padding,
    Slice<AnimationFrameCacheEntry> entries, Slice<u32> buckets,
    Slice<GlyphAtlasEntry> allocations,
    Slice<GlyphAtlasRow> rows) noexcept {
    if (!cache || !entries.data || entries.count == 0 ||
        entries.count > (~0u / 2u) || !buckets.data ||
        buckets.count < entries.count * 2u ||
        allocations.count != entries.count) {
        return false;
    }
    reset_value<AnimationFrameCache>(cache);
    if (!glyph_atlas_initialize(&cache->allocations, width, height, padding,
                                allocations, rows)) {
        return false;
    }
    cache->entries = entries.data;
    cache->buckets = buckets.data;
    cache->entry_capacity = entries.count;
    cache->bucket_capacity = buckets.count;
    for (u32 index = 0; index < entries.count; ++index) {
        reset_value<AnimationFrameCacheEntry>(&entries[index]);
    }
    for (u32 index = 0; index < buckets.count; ++index) {
        buckets[index] = kSlotIndexEmpty;
    }
    return true;
}

bool animation_frame_cache_grow_storage(
    AnimationFrameCache* cache, Slice<AnimationFrameCacheEntry> entries,
    Slice<u32> buckets, Slice<GlyphAtlasEntry> allocations) noexcept {
    if (!cache || !cache->entries || !cache->buckets ||
        !cache->allocations.entries || entries.data != cache->entries ||
        buckets.data != cache->buckets ||
        allocations.data != cache->allocations.entries ||
        entries.count <= cache->entry_capacity || entries.count > 65'535u ||
        buckets.count < entries.count * 2u ||
        allocations.count != entries.count) {
        return false;
    }

    u32 occupied = 0;
    for (u32 index = 0; index < cache->entry_capacity; ++index) {
        if (cache->entries[index].occupied) {
            if (!key_valid(cache->entries[index].key)) {
                return false;
            }
            ++occupied;
        }
    }
    if (occupied != cache->entry_count) {
        return false;
    }

    const u32 previous_capacity = cache->entry_capacity;
    for (u32 index = previous_capacity; index < entries.count; ++index) {
        reset_value<AnimationFrameCacheEntry>(&entries[index]);
        reset_value<GlyphAtlasEntry>(&allocations[index]);
    }
    for (u32 index = 0; index < buckets.count; ++index) {
        buckets[index] = kSlotIndexEmpty;
    }

    cache->entry_capacity = entries.count;
    cache->bucket_capacity = buckets.count;
    cache->allocations.entry_capacity = allocations.count;
    for (u32 index = 0; index < previous_capacity; ++index) {
        if (entries[index].occupied &&
            !insert_bucket(cache, entries[index].key, index, &entries[index].home_bucket)) {
            // A half-rebuilt hash table must never escape. Resetting only
            // discards cached frames; callers can decode them again.
            animation_frame_cache_reset(cache);
            return false;
        }
    }
    return true;
}

void animation_frame_cache_reset(AnimationFrameCache* cache) noexcept {
    if (!cache || !cache->entries || !cache->buckets) {
        return;
    }
    glyph_atlas_reset(&cache->allocations);
    for (u32 index = 0; index < cache->entry_capacity; ++index) {
        reset_value<AnimationFrameCacheEntry>(&cache->entries[index]);
    }
    for (u32 index = 0; index < cache->bucket_capacity; ++index) {
        cache->buckets[index] = kSlotIndexEmpty;
    }
    cache->entry_count = 0;
    cache->free_cursor = 0;
    cache->next_allocation_key = 0;
}

u32 animation_frame_cache_trim(AnimationFrameCache* cache) noexcept {
    if (!cache || !cache->entries || !cache->buckets) {
        return 0;
    }
    const u32 evicted = glyph_atlas_trim(&cache->allocations);
    if (evicted != 0) {
        clear_stale(cache);
    }
    return evicted;
}
#pragma clang attribute pop

void animation_frame_cache_begin_visibility(
    AnimationFrameCache* cache) noexcept {
    if (cache) {
        glyph_atlas_clear_pins(&cache->allocations);
    }
}

AnimationFrameCacheResult animation_frame_cache_find(
    AnimationFrameCache* cache, const AnimationFrameCacheKey& key) noexcept {
    if (!cache || !cache->entries || !cache->buckets || !key_valid(key)) {
        return failure(AnimationFrameCacheError::invalid_input);
    }
    const u32 bucket = find_bucket(cache, key);
    if (bucket == 0xFFFFFFFFu) {
        return failure(AnimationFrameCacheError::not_found);
    }
    const u32 slot = cache->buckets[bucket] - 1;
    AnimationFrameCacheEntry& entry = cache->entries[slot];
    GlyphAtlasAllocation allocation{};
    if (!glyph_atlas_validate(cache->allocations, entry.allocation,
                              &allocation) ||
        !glyph_atlas_pin(&cache->allocations, entry.allocation)) {
        erase_entry(cache, slot, bucket);
        return failure(AnimationFrameCacheError::not_found);
    }
    return result_from(entry, allocation, true);
}

AnimationFrameCacheResult animation_frame_cache_allocate(
    AnimationFrameCache* cache,
    const AnimationFrameCacheDescriptor& descriptor) noexcept {
    if (!cache || !cache->entries || !cache->buckets ||
        !key_valid(descriptor.key) || descriptor.width == 0 ||
        descriptor.height == 0 || descriptor.duration_us == 0 ||
        descriptor.kind == AnimationFrameCacheKind::none) {
        return failure(AnimationFrameCacheError::invalid_input);
    }
    AnimationFrameCacheResult cached =
        animation_frame_cache_find(cache, descriptor.key);
    if (cached.ok()) {
        if (cached.allocation.width != descriptor.width ||
            cached.allocation.height != descriptor.height ||
            cached.duration_us != descriptor.duration_us ||
            cached.kind != descriptor.kind) {
            return failure(AnimationFrameCacheError::invalid_input);
        }
        return cached;
    }
    if (cached.error != AnimationFrameCacheError::not_found) {
        return cached;
    }

    if (cache->next_allocation_key == ~0ull) {
        animation_frame_cache_reset(cache);
    }
    const u64 allocation_key = ++cache->next_allocation_key;
    const GlyphAtlasAllocation allocation = glyph_atlas_allocate_unique(
        &cache->allocations, allocation_key, descriptor.width,
        descriptor.height);
    if (!allocation.ok()) {
        return failure(translate_error(allocation.error));
    }
    if (allocation.evicted_entries != 0 || allocation.atlas_reset ||
        cache->entry_count >= cache->entry_capacity) {
        clear_stale(cache);
    }
    const u32 slot = find_free_slot(cache);
    u32 home_bucket = 0;
    if (slot == 0xFFFFFFFFu ||
        !insert_bucket(cache, descriptor.key, slot, &home_bucket)) {
        return failure(AnimationFrameCacheError::no_capacity);
    }

    AnimationFrameCacheEntry& entry = cache->entries[slot];
    reset_value<AnimationFrameCacheEntry>(&entry);
    entry.key = descriptor.key;
    entry.home_bucket = home_bucket;
    entry.duration_us = descriptor.duration_us;
    entry.control = descriptor.control;
    entry.allocation = allocation.handle;
    entry.background_rgba = descriptor.background_rgba;
    entry.kind = descriptor.kind;
    entry.occupied = true;
    if (!glyph_atlas_pin(&cache->allocations, entry.allocation)) {
        const u32 bucket = find_bucket(cache, descriptor.key);
        if (bucket != 0xFFFFFFFFu) {
            erase_bucket(cache, bucket);
        }
        reset_value<AnimationFrameCacheEntry>(&entry);
        return failure(AnimationFrameCacheError::corrupt_state);
    }
    ++cache->entry_count;
    return result_from(entry, allocation, false);
}

} // namespace maicord
