#pragma once

#include "catalog_service.hpp"

// The catalog's one background thread (catalog_worker.cpp): network through the update fetch, the
// signature and digest checks, and the catalog's file writes. Jobs go in and results come out through two
// small queues; the UI thread takes results in application_catalog_poll.
namespace maicord::app_detail {

enum class CatalogJobKind : u8 {
    none,
    load_cache,  // read and re-verify catalog.bin (an expired index reads as browse-only)
    check,       // fetch, verify and cache the index
    download,    // fetch, verify and write one listed file
    asset,       // a details or preview blob: disk cache, else fetch and verify
    delete_data, // a removed plugin's plugin-data folder
    copy,        // Developer Mode: copy a DLL into plugins\<sha256>.dll
};
struct CatalogJob {
    CatalogJobKind kind;
    CatalogKind item_kind;     // download
    CatalogAssetKind asset;    // asset
    u8 slug_bytes;             // download: the catalog slug of a theme or effect
    char slug[kCatalogSlugMaximumBytes];
    u64 serial;
    u64 floor_sequence;        // check and load_cache: the replay floor
    u8 floor_digest[32];
    CatalogBlob blob;          // download and asset
    u64 owner;                 // delete_data
    u32 path_characters;       // copy
    wchar_t path[520];
};
struct CatalogJobResult {
    CatalogJobKind kind;
    u64 serial;
    CatalogError error;
    CatalogIndex* index; // load_cache and check: the verified index, owned by the taker
    u8* bytes;           // asset: owned by the taker (catalog_worker_free)
    u32 count;
    u8 sha256[32];       // download, asset and copy
    u64 retry_at_unix;   // check: a rate limit's retry time
};

struct CatalogWorkerOptions {
    ConstSlice<wchar_t> base;
    Utf8View repository;
    ConstSlice<const u8*> keys;
    UpdateTransport transport;
    u64 (*clock_unix)() noexcept;
    bool (*validate_package)(CatalogKind, ConstSlice<u8>, Utf8View) noexcept;
    void (*library_changed)() noexcept;
};
bool catalog_worker_start(const CatalogWorkerOptions&) noexcept;
// Cancels the request in flight and joins the thread; results not taken are freed.
void catalog_worker_stop() noexcept;
HANDLE catalog_worker_notification() noexcept;
// False when the queue is full or the worker is not running.
bool catalog_worker_submit(const CatalogJob&) noexcept;
bool catalog_worker_take(CatalogJobResult*) noexcept;
// The download in flight: bytes received of total.
void catalog_worker_progress(u64* bytes, u64* total) noexcept;
void catalog_worker_free(void* memory) noexcept;

// <base>\<relative>, NUL-terminated; characters written or 0.
u32 catalog_path(const ConstSlice<wchar_t>& base, const wchar_t* relative, wchar_t (&output)[600]) noexcept;
// "<64 hex>.dll": a catalog plugin's file in plugins\.
void catalog_plugin_leaf(const u8 (&sha256)[32], wchar_t (&leaf)[69]) noexcept;

} // namespace maicord::app_detail
