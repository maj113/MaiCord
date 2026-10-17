#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/media/asset.hpp"

#include "maicord/core/win32_records.hpp"

namespace maicord {

inline constexpr u32 kEncodedMediaCachePageBytes = 4u * 1024u;
inline constexpr u32 kEncodedMediaCacheMaximumEtagBytes = 96;
inline constexpr u16 kEncodedMediaCacheFreePage = 0xFFFFu;

struct EncodedMediaCacheEntry {
    MediaAssetKey key;
    u64 expires_at_seconds;
    u64 last_touch;
    u32 first_page;
    u32 bytes;
    u16 generation;
    u16 leases;
    u8 etag_bytes;
    bool must_revalidate;
    bool occupied;
    bool retiring; // Hidden from new readers; existing leases retain bytes until final release.
    u8 etag[kEncodedMediaCacheMaximumEtagBytes];
    u64 content_fingerprint;
    u64 content_identity;
};

struct EncodedMediaCacheCounters {
    u64 hits;
    u64 misses;
    u64 stale_hits;
    u64 stores;
    u64 replacements;
    u64 evictions;
    u64 erases;
    u64 rejected;
    u64 bytes_copied;
    u64 lease_acquires;
    u64 lease_releases;
    u64 pinned_rejections;
};

struct EncodedMediaCache {
    EncodedMediaCacheEntry* entries;
    u16* page_owners;
    u8* storage;
    u32* index_buckets;
    u32 entry_capacity;
    u32 page_capacity;
    u32 index_bucket_capacity;
    u32 index_tombstones;
    u32 entry_count;
    u32 used_pages;
    u64 stored_bytes;
    u64 clock;
    EncodedMediaCacheCounters counters;
    SrwLock lock;
};

enum class EncodedMediaCacheLookupState : u8 {
    invalid,
    miss,
    fresh,
    stale,
};

struct EncodedMediaCacheLookup {
    ConstSlice<u8> bytes;
    ConstSlice<u8> etag;
    u64 expires_at_seconds;
    u16 generation;
    EncodedMediaCacheLookupState state;
    u64 content_identity;

    constexpr bool found() const noexcept {
        return state == EncodedMediaCacheLookupState::fresh || state == EncodedMediaCacheLookupState::stale;
    }
};

// A cache lease pins one immutable encoded byte range while a decoder consumes
// it. Stores and eviction remain concurrent across unrelated entries, while a
// leased entry can never be moved or replaced underneath a codec lane.
struct EncodedMediaCacheLease {
    EncodedMediaCacheLookup lookup;
    u32 entry_index;
    u16 generation;
    bool acquired;
    u8 reserved;
};

struct EncodedMediaCachePolicy {
    ConstSlice<u8> etag;
    u64 expires_at_seconds;
    bool store_allowed;
    bool must_revalidate;
};

enum class EncodedMediaCacheStoreState : u8 {
    invalid,
    transient,
    stored,
    replaced,
    no_capacity,
};

struct EncodedMediaCacheStoreResult {
    EncodedMediaCacheStoreState state;
    u32 evicted_entries;
    u16 generation;

    constexpr bool stored() const noexcept {
        return state == EncodedMediaCacheStoreState::stored ||
               state == EncodedMediaCacheStoreState::replaced;
    }
};

// Allocate a nonzero identity for one immutable source lifetime. Cache reset
// does not reset this namespace. Replay and seek retain the assigned identity.
u64 encoded_media_new_content_identity() noexcept;

bool encoded_media_cache_initialize(EncodedMediaCache* cache, Slice<EncodedMediaCacheEntry> entries,
                                    Slice<u16> page_owners, Slice<u8> storage,
                                    Slice<u32> index_buckets = {}) noexcept;
EncodedMediaCacheLookup encoded_media_cache_find(EncodedMediaCache* cache, MediaAssetKey key,
                                                 u64 now_seconds) noexcept;
EncodedMediaCacheLease encoded_media_cache_acquire(EncodedMediaCache* cache, MediaAssetKey key,
                                                   u64 now_seconds) noexcept;
bool encoded_media_cache_release(EncodedMediaCache* cache, EncodedMediaCacheLease* lease) noexcept;
EncodedMediaCacheStoreResult encoded_media_cache_store(EncodedMediaCache* cache, MediaAssetKey key,
                                                       ConstSlice<u8> encoded,
                                                       EncodedMediaCachePolicy policy) noexcept;
// A nonzero generation fences freshness/ETag updates to that exact lifetime.
bool encoded_media_cache_refresh(EncodedMediaCache* cache, MediaAssetKey key, u64 expires_at_seconds,
                                 bool must_revalidate, ConstSlice<u8> etag = {},
                                 u16 expected_generation = 0) noexcept;
bool encoded_media_cache_erase(EncodedMediaCache* cache, MediaAssetKey key) noexcept;
// Immediately hides a source from find/acquire/content reuse. Existing leases
// remain immutable and release its pages after their final joined reader exits.
// A nonzero generation restricts retirement to that exact entry lifetime.
bool encoded_media_cache_retire(EncodedMediaCache*, MediaAssetKey, u16 expected_generation = 0) noexcept;

static_assert(sizeof(EncodedMediaCacheEntry) == 160);
static_assert(sizeof(EncodedMediaCache) <= 192);

} // namespace maicord
