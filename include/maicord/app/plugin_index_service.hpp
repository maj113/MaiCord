#pragma once

#include "maicord/app/plugin_index_acceptance.hpp"
#include "maicord/app/plugin_manager_state.hpp"
#include "maicord/app/plugin_providers.hpp"

namespace maicord {

// Plugin catalog check, one provider per call, network only through the
// injected UpdateTransport. Order (as update_service.cpp): snapshot the floor;
// fetch the index (at most 256 KiB) and its 64-byte .sig (none when
// unsigned); verify signature, schema, provider rules and freshness; refuse
// replay (below the floor, or its sequence with another digest) keeping the
// cache; admit under the acceptance lock; write cache\<provider16>.index
// atomically. Logs go under "plugins": hosts, numbers and enum ordinals,
// never URL paths, queries, provider base paths or index text.

// One provider. keys are caller-owned and outlive the call.
struct PluginCatalogSource {
    u64 provider; // kPluginProviderOfficial or a providers.bin id
    PluginProviderKind kind;
    u32 key_count;           // official: 1-2 compiled keys (current, next); custom_signed: 1; custom_unsigned: 0
    const P256Key* keys[2];
    u8 pinned_key[65];       // custom_signed: the providers.bin pin the index must name
    u32 repository_bytes;    // official: "owner/repo"
    char repository[kUpdateRepositoryMaximumBytes];
    u32 host_bytes, path_bytes; // custom: the provider base
    char host[kPluginProviderHostCapacity];
    char path[kPluginProviderPathCapacity];
};
bool plugin_catalog_source_official(Utf8View repository, ConstSlice<const P256Key*> keys, PluginCatalogSource* output) noexcept;
// pinned is the imported providers.bin key of a signed provider, null for an unsigned one.
bool plugin_catalog_source_custom(const PluginProviderRecord& record, const P256Key* pinned, PluginCatalogSource* output) noexcept;
// Official: https://<owner>.github.io/<repo>/v1/index.json(.sig); custom: <base>v1/index.json(.sig).
bool plugin_catalog_index_location(const PluginCatalogSource& source, bool signature, UpdateLocation* output) noexcept;
// Official locations are derived, never read from the index:
// https://github.com/<repo>/releases/download/<slug>-v<version>/<slug>-<version>-<arch>.dll.
// Custom: <base><path>, or github.com/<repository>/releases/download/<tag>/<asset>.
bool plugin_catalog_file_location(const PluginCatalogSource& source, const PluginIndex& index, Utf8View document, u32 plugin,
    u32 version, PluginIndexArch arch, UpdateLocation* output) noexcept;

enum class PluginIndexCheckError : u8 {
    none,
    invalid_input,
    storage,     // the plugin-manager directories could not be created (native_error)
    fetch,       // the index or signature request failed (fetch_error, status, retry_at_unix)
    document,    // signature, schema, provider or freshness (index_error); floor and cache unchanged
    key_changed, // the custom provider now serves another provider.key (served_key, served_fingerprint)
    replay,      // below the floor, or the floor's sequence with another digest; the cache is kept
    acceptance,  // acceptance.bin could not be read, locked or written (record_error)
    cache,       // the cache could not be read or written (record_error); the floor may already have advanced
};
enum class PluginIndexCheckOutcome : u8 { failed, updated, up_to_date };
struct PluginIndexCheckResult {
    PluginIndexCheckError error;
    PluginIndexCheckOutcome outcome;
    UpdateFetchError fetch_error;
    PluginIndexError index_error;
    AcceptanceRecordError record_error;
    u16 status;
    u32 native_error;
    u64 retry_at_unix;   // a server-requested retry (429, Retry-After)
    u64 next_check_unix; // update_next_check: 24 h +-1 h after success, backoff after failure
    u64 sequence;
    bool cache_written;
    u8 served_key[65];   // key_changed: the unverified key the provider now names
    u8 served_fingerprint[32];
};
// work receives the record and the verified index (zeroed on a document
// failure). consecutive_failures and random feed update_next_check.
PluginIndexCheckResult plugin_index_check(const PluginManagerPaths& paths, const PluginCatalogSource& source,
    const UpdateTransport& transport, KdfSha256Workspace& workspace, PluginIndexWork* work, u64 now_unix,
    u32 consecutive_failures = 0, u32 random = 0) noexcept;

struct PluginIndexCacheResult {
    PluginIndexCheckError error; // cache (missing: record_error missing), document or acceptance
    PluginIndexError index_error;
    AcceptanceRecordError record_error;
    u32 native_error;
    bool expired;     // browse only: install and update are unavailable
    bool admitted;    // the provider's floor names exactly this index (sequence and digest)
    bool installable; // admitted and not expired
};
// Reads and re-verifies the cache, accepting an expired index read-only.
// Expiry or a missing cache never disables installed plugins; it only stops
// installs and updates.
PluginIndexCacheResult plugin_index_load_cache(const PluginManagerPaths& paths, const PluginCatalogSource& source,
    KdfSha256Workspace& workspace, PluginIndexWork* work, u64 now_unix) noexcept;

enum class PluginProviderAddError : u8 { none, invalid_input, insert, providers, acceptance, cache };
struct PluginProviderAddResult {
    PluginProviderAddError error;
    PluginProviderInsertError insert_error;
    AcceptanceRecordError record_error;
    PluginIndexError index_error;
    u32 native_error;
};
// After the user confirmed a probe whose document is still in work: saves
// providers.bin, admits the index as the first floor and writes the cache.
// providers changes only once providers.bin is saved.
PluginProviderAddResult plugin_index_add_provider(const PluginManagerPaths& paths, PluginProviders* providers,
    const PluginProviderAddress& address, const PluginProviderProbe& probe, KdfSha256Workspace& workspace,
    PluginIndexWork* work, u64 now_unix) noexcept;

} // namespace maicord
