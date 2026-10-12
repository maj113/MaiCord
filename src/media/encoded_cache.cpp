#include "maicord/media/encoded_cache.hpp"
#include "maicord/core/hash.hpp"

#include <Windows.h>

namespace maicord {
namespace {

constexpr u32 kInvalidIndex = 0xFFFFFFFFu;
constexpr u32 kIndexTombstone = 0xFFFFFFFFu;
volatile LONG64 next_content_identity = 0;

bool etag_valid(ConstSlice<u8> etag) noexcept {
    if ((!etag.data && etag.count != 0) || etag.count > kEncodedMediaCacheMaximumEtagBytes) {
        return false;
    }
    for (u32 index = 0; index < etag.count; ++index) {
        if (etag[index] < 0x21 || etag[index] > 0x7E) {
            return false;
        }
    }
    return true;
}

u16 next_generation(u16 generation) noexcept {
    ++generation;
    return generation == 0 ? 1 : generation;
}

bool power_of_two(u32 value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

void rebuild_index_locked(EncodedMediaCache* cache) noexcept {
    if (!cache->index_buckets || cache->index_bucket_capacity == 0) {
        return;
    }
    for (u32 bucket = 0; bucket < cache->index_bucket_capacity; ++bucket) {
        cache->index_buckets[bucket] = 0;
    }
    cache->index_tombstones = 0;
    const u32 mask = cache->index_bucket_capacity - 1;
    for (u32 index = 0; index < cache->entry_capacity; ++index) {
        if (!cache->entries[index].occupied) {
            continue;
        }
        u32 bucket = static_cast<u32>(media_asset_hash(cache->entries[index].key)) & mask;
        for (u32 probe = 0; probe < cache->index_bucket_capacity; ++probe) {
            if (cache->index_buckets[bucket] == 0) {
                cache->index_buckets[bucket] = index + 1;
                break;
            }
            bucket = (bucket + 1) & mask;
        }
    }
}

void insert_index_locked(EncodedMediaCache* cache, u32 index) noexcept {
    if (!cache->index_buckets || cache->index_bucket_capacity == 0 || index >= cache->entry_capacity ||
        !cache->entries[index].occupied) {
        return;
    }
    const u32 mask = cache->index_bucket_capacity - 1;
    u32 bucket = static_cast<u32>(media_asset_hash(cache->entries[index].key)) & mask;
    u32 tombstone = kInvalidIndex;
    for (u32 probe = 0; probe < cache->index_bucket_capacity; ++probe) {
        const u32 value = cache->index_buckets[bucket];
        if (value == kIndexTombstone && tombstone == kInvalidIndex) {
            tombstone = bucket;
        } else if (value == 0) {
            const u32 destination = tombstone != kInvalidIndex ? tombstone : bucket;
            cache->index_buckets[destination] = index + 1;
            if (tombstone != kInvalidIndex) {
                --cache->index_tombstones;
            }
            return;
        }
        bucket = (bucket + 1) & mask;
    }
    rebuild_index_locked(cache);
}

void remove_index_locked(EncodedMediaCache* cache, MediaAssetKey key, u32 index) noexcept {
    if (!cache->index_buckets || cache->index_bucket_capacity == 0) {
        return;
    }
    const u32 mask = cache->index_bucket_capacity - 1;
    u32 bucket = static_cast<u32>(media_asset_hash(key)) & mask;
    for (u32 probe = 0; probe < cache->index_bucket_capacity; ++probe) {
        const u32 value = cache->index_buckets[bucket];
        if (value == 0) {
            return;
        }
        if (value == index + 1) {
            cache->index_buckets[bucket] = kIndexTombstone;
            ++cache->index_tombstones;
            return;
        }
        bucket = (bucket + 1) & mask;
    }
}

void maintain_index_locked(EncodedMediaCache* cache) noexcept {
    if (cache->index_bucket_capacity != 0 && cache->index_tombstones > cache->index_bucket_capacity / 4) {
        rebuild_index_locked(cache);
    }
}

u32 find_entry_locked(const EncodedMediaCache& cache, MediaAssetKey key) noexcept {
    if (cache.index_buckets && cache.index_bucket_capacity != 0) {
        const u32 mask = cache.index_bucket_capacity - 1;
        u32 bucket = static_cast<u32>(media_asset_hash(key)) & mask;
        for (u32 probe = 0; probe < cache.index_bucket_capacity; ++probe) {
            const u32 encoded_index = cache.index_buckets[bucket];
            if (encoded_index == 0) {
                return kInvalidIndex;
            }
            if (encoded_index == kIndexTombstone) {
                bucket = (bucket + 1) & mask;
                continue;
            }
            const u32 index = encoded_index - 1;
            if (index < cache.entry_capacity && cache.entries[index].occupied &&
                media_asset_key_equal(cache.entries[index].key, key)) {
                return index;
            }
            bucket = (bucket + 1) & mask;
        }
        return kInvalidIndex;
    }
    for (u32 index = 0; index < cache.entry_capacity; ++index) {
        if (cache.entries[index].occupied && media_asset_key_equal(cache.entries[index].key, key)) {
            return index;
        }
    }
    return kInvalidIndex;
}

u32 find_empty_entry_locked(const EncodedMediaCache& cache) noexcept {
    for (u32 index = 0; index < cache.entry_capacity; ++index) {
        if (!cache.entries[index].occupied) {
            return index;
        }
    }
    return kInvalidIndex;
}

u32 find_lru_entry_locked(const EncodedMediaCache& cache) noexcept {
    u32 selected = kInvalidIndex;
    u64 selected_touch = ~0ull;
    for (u32 index = 0; index < cache.entry_capacity; ++index) {
        const EncodedMediaCacheEntry& entry = cache.entries[index];
        if (entry.occupied && entry.leases == 0 && entry.last_touch < selected_touch) {
            selected = index;
            selected_touch = entry.last_touch;
        }
    }
    return selected;
}

void touch_entry_locked(EncodedMediaCache* cache, EncodedMediaCacheEntry* entry) noexcept {
    if (cache->clock == ~0ull) {
        for (u32 index = 0; index < cache->entry_capacity; ++index) {
            if (cache->entries[index].occupied) {
                cache->entries[index].last_touch = (cache->entries[index].last_touch >> 1) + 1;
            }
        }
        cache->clock = (cache->clock >> 1) + 1;
    }
    entry->last_touch = ++cache->clock;
}

bool release_entry_locked(EncodedMediaCache* cache, u32 index) noexcept {
    EncodedMediaCacheEntry& entry = cache->entries[index];
    if (!entry.occupied) {
        return true;
    }
    if (entry.leases != 0) {
        ++cache->counters.pinned_rejections;
        return false;
    }
    remove_index_locked(cache, entry.key, index);
    const u32 page_count = (entry.bytes + kEncodedMediaCachePageBytes - 1) / kEncodedMediaCachePageBytes;
    const u32 end = entry.first_page + page_count;
    if (end <= cache->page_capacity) {
        for (u32 page = entry.first_page; page < end; ++page) {
            if (cache->page_owners[page] == static_cast<u16>(index)) {
                cache->page_owners[page] = kEncodedMediaCacheFreePage;
            }
        }
    }
    cache->used_pages -= page_count;
    cache->stored_bytes -= entry.bytes;
    --cache->entry_count;
    entry.occupied = false;
    entry.retiring = false;
    entry.bytes = 0;
    entry.etag_bytes = 0;
    return true;
}

u32 find_page_run_locked(const EncodedMediaCache& cache, u32 required_pages) noexcept {
    if (required_pages == 0 || required_pages > cache.page_capacity) {
        return kInvalidIndex;
    }
    u32 begin = 0;
    u32 run = 0;
    for (u32 page = 0; page < cache.page_capacity; ++page) {
        if (cache.page_owners[page] == kEncodedMediaCacheFreePage) {
            if (run == 0) {
                begin = page;
            }
            if (++run == required_pages) {
                return begin;
            }
        } else {
            run = 0;
        }
    }
    return kInvalidIndex;
}

EncodedMediaCacheLookup lookup_locked(EncodedMediaCache* cache, MediaAssetKey key, u64 now_seconds,
                                      u32* entry_index) noexcept {
    const u32 index = find_entry_locked(*cache, key);
    if (entry_index) {
        *entry_index = index;
    }
    if (index == kInvalidIndex || cache->entries[index].retiring) {
        ++cache->counters.misses;
        return EncodedMediaCacheLookup{{}, {}, 0, 0, EncodedMediaCacheLookupState::miss};
    }
    EncodedMediaCacheEntry& entry = cache->entries[index];
    touch_entry_locked(cache, &entry);
    const bool stale = entry.must_revalidate || entry.expires_at_seconds <= now_seconds;
    if (stale) {
        ++cache->counters.stale_hits;
    } else {
        ++cache->counters.hits;
    }
    return EncodedMediaCacheLookup{
        ConstSlice<u8>{cache->storage + entry.first_page * kEncodedMediaCachePageBytes, entry.bytes},
        ConstSlice<u8>{entry.etag, entry.etag_bytes},
        entry.expires_at_seconds,
        entry.generation,
        stale ? EncodedMediaCacheLookupState::stale : EncodedMediaCacheLookupState::fresh,
        entry.content_identity};
}

} // namespace

u64 encoded_media_new_content_identity() noexcept {
    u64 identity;
    do {
        identity = static_cast<u64>(InterlockedIncrement64(&next_content_identity));
    } while (identity == 0);
    return identity;
}

bool encoded_media_cache_initialize(EncodedMediaCache* cache, Slice<EncodedMediaCacheEntry> entries,
                                    Slice<u16> page_owners, Slice<u8> storage,
                                    Slice<u32> index_buckets) noexcept {
    if (!cache || !entries.data || entries.count == 0 || entries.count >= kEncodedMediaCacheFreePage ||
        !storage.data || storage.count == 0 || storage.count % kEncodedMediaCachePageBytes != 0 ||
        (index_buckets.count != 0 && (!index_buckets.data || !power_of_two(index_buckets.count) ||
                                      index_buckets.count < entries.count * 2u))) {
        return false;
    }
    const u32 pages = storage.count / kEncodedMediaCachePageBytes;
    if (!page_owners.data || page_owners.count < pages) {
        return false;
    }
    *cache = EncodedMediaCache{};
    InitializeSRWLock(cache->lock.native());
    cache->entries = entries.data;
    cache->entry_capacity = entries.count;
    cache->page_owners = page_owners.data;
    cache->page_capacity = pages;
    cache->storage = storage.data;
    cache->index_buckets = index_buckets.data;
    cache->index_bucket_capacity = index_buckets.count;
    for (u32 index = 0; index < entries.count; ++index) {
        entries[index] = EncodedMediaCacheEntry{};
    }
    for (u32 page = 0; page < pages; ++page) {
        page_owners[page] = kEncodedMediaCacheFreePage;
    }
    rebuild_index_locked(cache);
    return true;
}

void encoded_media_cache_reset(EncodedMediaCache* cache) noexcept {
    if (!cache || !cache->entries || !cache->page_owners) {
        return;
    }
    AcquireSRWLockExclusive(cache->lock.native());
    for (u32 index = 0; index < cache->entry_capacity; ++index) {
        cache->entries[index].occupied = false;
        cache->entries[index].bytes = 0;
        cache->entries[index].etag_bytes = 0;
        cache->entries[index].leases = 0;
        cache->entries[index].retiring = false;
    }
    for (u32 page = 0; page < cache->page_capacity; ++page) {
        cache->page_owners[page] = kEncodedMediaCacheFreePage;
    }
    cache->entry_count = 0;
    cache->used_pages = 0;
    cache->stored_bytes = 0;
    cache->clock = 0;
    rebuild_index_locked(cache);
    ReleaseSRWLockExclusive(cache->lock.native());
}

EncodedMediaCacheLookup encoded_media_cache_find(EncodedMediaCache* cache, MediaAssetKey key,
                                                 u64 now_seconds) noexcept {
    if (!cache || !cache->entries || !cache->storage || !media_asset_key_valid(key)) {
        if (cache && cache->entries) {
            AcquireSRWLockExclusive(cache->lock.native());
            ++cache->counters.rejected;
            ReleaseSRWLockExclusive(cache->lock.native());
        }
        return EncodedMediaCacheLookup{{}, {}, 0, 0, EncodedMediaCacheLookupState::invalid};
    }
    AcquireSRWLockExclusive(cache->lock.native());
    const EncodedMediaCacheLookup result = lookup_locked(cache, key, now_seconds, nullptr);
    ReleaseSRWLockExclusive(cache->lock.native());
    return result;
}

EncodedMediaCacheLease encoded_media_cache_acquire(EncodedMediaCache* cache, MediaAssetKey key,
                                                   u64 now_seconds) noexcept {
    EncodedMediaCacheLease lease{};
    lease.entry_index = kInvalidIndex;
    if (!cache || !cache->entries || !cache->storage || !media_asset_key_valid(key)) {
        lease.lookup.state = EncodedMediaCacheLookupState::invalid;
        if (cache && cache->entries) {
            AcquireSRWLockExclusive(cache->lock.native());
            ++cache->counters.rejected;
            ReleaseSRWLockExclusive(cache->lock.native());
        }
        return lease;
    }
    AcquireSRWLockExclusive(cache->lock.native());
    u32 entry_index = kInvalidIndex;
    lease.lookup = lookup_locked(cache, key, now_seconds, &entry_index);
    if (lease.lookup.found() && entry_index != kInvalidIndex) {
        EncodedMediaCacheEntry& entry = cache->entries[entry_index];
        if (entry.leases != 0xFFFFu) {
            ++entry.leases;
            ++cache->counters.lease_acquires;
            lease.entry_index = entry_index;
            lease.generation = entry.generation;
            lease.acquired = true;
        } else {
            ++cache->counters.pinned_rejections;
            lease.lookup = EncodedMediaCacheLookup{{}, {}, 0, 0, EncodedMediaCacheLookupState::invalid};
        }
    }
    ReleaseSRWLockExclusive(cache->lock.native());
    return lease;
}

bool encoded_media_cache_release(EncodedMediaCache* cache, EncodedMediaCacheLease* lease) noexcept {
    if (!cache || !cache->entries || !lease || !lease->acquired ||
        lease->entry_index >= cache->entry_capacity) {
        return false;
    }
    AcquireSRWLockExclusive(cache->lock.native());
    EncodedMediaCacheEntry& entry = cache->entries[lease->entry_index];
    const bool valid = entry.occupied && entry.leases != 0 && entry.generation == lease->generation;
    if (valid) {
        --entry.leases;
        ++cache->counters.lease_releases;
        if (entry.retiring && entry.leases == 0) {
            release_entry_locked(cache, lease->entry_index);
            maintain_index_locked(cache);
            ++cache->counters.erases;
        }
    } else {
        ++cache->counters.rejected;
    }
    ReleaseSRWLockExclusive(cache->lock.native());
    *lease = EncodedMediaCacheLease{};
    lease->entry_index = kInvalidIndex;
    return valid;
}

EncodedMediaCacheStoreResult encoded_media_cache_store(EncodedMediaCache* cache, MediaAssetKey key,
                                                       ConstSlice<u8> encoded,
                                                       EncodedMediaCachePolicy policy) noexcept {
    if (!cache || !cache->entries || !cache->page_owners || !cache->storage ||
        !media_asset_key_valid(key) || !encoded.data || encoded.count == 0 || !etag_valid(policy.etag)) {
        if (cache && cache->entries) {
            AcquireSRWLockExclusive(cache->lock.native());
            ++cache->counters.rejected;
            ReleaseSRWLockExclusive(cache->lock.native());
        }
        return EncodedMediaCacheStoreResult{EncodedMediaCacheStoreState::invalid, 0, 0};
    }
    if (!policy.store_allowed) {
        return EncodedMediaCacheStoreResult{EncodedMediaCacheStoreState::transient, 0, 0};
    }
    const u32 required_pages =
        (encoded.count + kEncodedMediaCachePageBytes - 1) / kEncodedMediaCachePageBytes;
    // Hash cacheable retained animation/audio sources once per download,
    // outside the lock. Audio uses a typed format, never the animation flag.
    // Hashes filter candidates; exact byte equality decides shared identity.
    const bool audio_source = key.kind == MediaAssetKind::attachment &&
                              media_attachment_format_audio_supported(media_asset_attachment_format(key));
    const bool retained_source = (key.flags & media_asset_flag_animated) != 0 || audio_source;
    const u64 fingerprint = retained_source && required_pages <= cache->page_capacity
                                ? hash64_counted_bytes(kHash64Seed, encoded.data, encoded.count)
                                : 0;
    AcquireSRWLockExclusive(cache->lock.native());
    if (required_pages > cache->page_capacity) {
        ++cache->counters.rejected;
        ReleaseSRWLockExclusive(cache->lock.native());
        return EncodedMediaCacheStoreResult{EncodedMediaCacheStoreState::no_capacity, 0, 0};
    }

    u64 content_identity = 0;
    if (retained_source) {
        for (u32 i = 0; i < cache->entry_capacity; ++i) {
            const auto& candidate = cache->entries[i];
            const bool candidate_audio =
                candidate.key.kind == MediaAssetKind::attachment &&
                media_attachment_format_audio_supported(media_asset_attachment_format(candidate.key));
            if ((audio_source || candidate_audio) &&
                !(audio_source && candidate_audio &&
                  media_asset_attachment_format(candidate.key) == media_asset_attachment_format(key)))
                continue;
            if (candidate.occupied && !candidate.retiring && candidate.content_identity != 0 &&
                candidate.content_fingerprint == fingerprint && candidate.bytes == encoded.count &&
                bytes_equal(cache->storage + candidate.first_page * kEncodedMediaCachePageBytes,
                            encoded.data, encoded.count)) {
                content_identity = candidate.content_identity;
                break;
            }
        }
        if (content_identity == 0)
            content_identity = encoded_media_new_content_identity();
    }
    u32 evicted = 0;
    u32 slot = find_entry_locked(*cache, key);
    const bool replacing = slot != kInvalidIndex;
    u16 generation = 0;
    if (replacing) {
        generation = cache->entries[slot].generation;
        if (!release_entry_locked(cache, slot)) {
            ReleaseSRWLockExclusive(cache->lock.native());
            return EncodedMediaCacheStoreResult{EncodedMediaCacheStoreState::no_capacity, 0, 0};
        }
    } else {
        slot = find_empty_entry_locked(*cache);
        if (slot == kInvalidIndex) {
            slot = find_lru_entry_locked(*cache);
            if (slot == kInvalidIndex) {
                ++cache->counters.rejected;
                ReleaseSRWLockExclusive(cache->lock.native());
                return EncodedMediaCacheStoreResult{EncodedMediaCacheStoreState::no_capacity, 0, 0};
            }
            generation = cache->entries[slot].generation;
            release_entry_locked(cache, slot);
            ++evicted;
        } else {
            generation = cache->entries[slot].generation;
        }
    }

    u32 first_page = find_page_run_locked(*cache, required_pages);
    while (first_page == kInvalidIndex) {
        const u32 victim = find_lru_entry_locked(*cache);
        if (victim == kInvalidIndex) {
            ++cache->counters.rejected;
            maintain_index_locked(cache);
            ReleaseSRWLockExclusive(cache->lock.native());
            return EncodedMediaCacheStoreResult{EncodedMediaCacheStoreState::no_capacity, evicted, 0};
        }
        release_entry_locked(cache, victim);
        ++evicted;
        first_page = find_page_run_locked(*cache, required_pages);
    }

    const u32 destination_offset = first_page * kEncodedMediaCachePageBytes;
    CopyMemory(cache->storage + destination_offset, encoded.data, encoded.count);
    for (u32 page = first_page; page < first_page + required_pages; ++page) {
        cache->page_owners[page] = static_cast<u16>(slot);
    }

    EncodedMediaCacheEntry& entry = cache->entries[slot];
    entry.key = key;
    entry.content_fingerprint = fingerprint;
    entry.content_identity = content_identity;
    entry.expires_at_seconds = policy.expires_at_seconds;
    entry.first_page = first_page;
    entry.bytes = encoded.count;
    entry.generation = next_generation(generation);
    entry.leases = 0;
    entry.etag_bytes = static_cast<u8>(policy.etag.count);
    entry.must_revalidate = policy.must_revalidate;
    entry.occupied = true;
    entry.retiring = false;
    if (policy.etag.count != 0) {
        CopyMemory(entry.etag, policy.etag.data, policy.etag.count);
    }
    touch_entry_locked(cache, &entry);
    ++cache->entry_count;
    cache->used_pages += required_pages;
    cache->stored_bytes += encoded.count;
    ++cache->counters.stores;
    cache->counters.evictions += evicted;
    cache->counters.bytes_copied += encoded.count;
    if (replacing) {
        ++cache->counters.replacements;
    }
    insert_index_locked(cache, slot);
    maintain_index_locked(cache);
    const EncodedMediaCacheStoreResult result{replacing ? EncodedMediaCacheStoreState::replaced
                                                        : EncodedMediaCacheStoreState::stored,
                                              evicted, entry.generation};
    ReleaseSRWLockExclusive(cache->lock.native());
    return result;
}

bool encoded_media_cache_refresh(EncodedMediaCache* cache, MediaAssetKey key, u64 expires_at_seconds,
                                 bool must_revalidate, ConstSlice<u8> etag,
                                 u16 expected_generation) noexcept {
    if (!cache || !cache->entries || !media_asset_key_valid(key) || !etag_valid(etag)) {
        if (cache && cache->entries) {
            AcquireSRWLockExclusive(cache->lock.native());
            ++cache->counters.rejected;
            ReleaseSRWLockExclusive(cache->lock.native());
        }
        return false;
    }
    AcquireSRWLockExclusive(cache->lock.native());
    const u32 index = find_entry_locked(*cache, key);
    if (index == kInvalidIndex || cache->entries[index].retiring ||
        (expected_generation && cache->entries[index].generation != expected_generation)) {
        ++cache->counters.misses;
        ReleaseSRWLockExclusive(cache->lock.native());
        return false;
    }
    EncodedMediaCacheEntry& entry = cache->entries[index];
    if (entry.leases != 0) {
        ++cache->counters.pinned_rejections;
        ReleaseSRWLockExclusive(cache->lock.native());
        return false;
    }
    entry.expires_at_seconds = expires_at_seconds;
    entry.must_revalidate = must_revalidate;
    if (etag.count != 0) {
        entry.etag_bytes = static_cast<u8>(etag.count);
        CopyMemory(entry.etag, etag.data, etag.count);
    }
    touch_entry_locked(cache, &entry);
    ReleaseSRWLockExclusive(cache->lock.native());
    return true;
}

bool encoded_media_cache_erase(EncodedMediaCache* cache, MediaAssetKey key) noexcept {
    if (!cache || !cache->entries || !media_asset_key_valid(key)) {
        if (cache && cache->entries) {
            AcquireSRWLockExclusive(cache->lock.native());
            ++cache->counters.rejected;
            ReleaseSRWLockExclusive(cache->lock.native());
        }
        return false;
    }
    AcquireSRWLockExclusive(cache->lock.native());
    const u32 index = find_entry_locked(*cache, key);
    if (index == kInvalidIndex) {
        ReleaseSRWLockExclusive(cache->lock.native());
        return false;
    }
    if (!release_entry_locked(cache, index)) {
        ReleaseSRWLockExclusive(cache->lock.native());
        return false;
    }
    maintain_index_locked(cache);
    ++cache->counters.erases;
    ReleaseSRWLockExclusive(cache->lock.native());
    return true;
}

bool encoded_media_cache_retire(EncodedMediaCache* cache, MediaAssetKey key,
                                u16 expected_generation) noexcept {
    if (!cache || !cache->entries || !media_asset_key_valid(key))
        return false;
    AcquireSRWLockExclusive(cache->lock.native());
    const u32 index = find_entry_locked(*cache, key);
    if (index == kInvalidIndex ||
        (expected_generation && cache->entries[index].generation != expected_generation)) {
        ReleaseSRWLockExclusive(cache->lock.native());
        return false;
    }
    auto& entry = cache->entries[index];
    entry.retiring = true;
    if (entry.leases == 0) {
        release_entry_locked(cache, index);
        maintain_index_locked(cache);
        ++cache->counters.erases;
    }
    ReleaseSRWLockExclusive(cache->lock.native());
    return true;
}

} // namespace maicord
