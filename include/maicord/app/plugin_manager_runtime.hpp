#pragma once

#include "maicord/app/plugin_manager_install.hpp"

namespace maicord {

// What the client does with the plugin catalog between checks: startup
// recovery and gating, applying admitted indexes to installed plugins
// (revocations, the blocklist, host compatibility, adoption, update offers)
// and the read-only browser snapshot. Same IO and logging rules as
// plugin_manager_install.hpp.

// ---- sources ----------------------------------------------------------------------------------
// Imported keys of one operation: the compiled official keys and each signed
// custom provider's pin. Release with plugin_catalog_keys_release.
struct PluginCatalogKeys {
    P256Key official[2];
    P256Key custom[kPluginProviderMaximum];
    u32 official_count;
};
void plugin_catalog_keys_release(PluginCatalogKeys*) noexcept;
// Slot 0 is the official provider (present when repository and keys are
// given), then the stored custom providers in providers.bin order. A provider
// whose key does not import is skipped.
struct PluginCatalogSources {
    u32 count;
    bool official;
    PluginCatalogSource sources[kPluginIndexAcceptanceSlots];
    const PluginProviderRecord* records[kPluginIndexAcceptanceSlots]; // custom: into the caller's providers
};
void plugin_catalog_sources_build(Utf8View repository, ConstSlice<const u8*> official_keys, const PluginProviders& providers,
    PluginCatalogKeys* keys, PluginCatalogSources* output) noexcept;

// ---- startup (UI thread, before anything is loaded) -------------------------------------------
inline constexpr u8 kPluginGateRevoked = 1u << 0;      // revoked as malicious or blocked: never autoloaded
inline constexpr u8 kPluginGateIncompatible = 1u << 1; // the catalog says this build does not run on this host
struct PluginStartupGate {
    u32 count;
    u64 owners[kPluginManagerStateMaximumEntries];
    u8 reasons[kPluginManagerStateMaximumEntries];
};
u8 plugin_startup_gate_reason(const PluginStartupGate&, u64 owner) noexcept;
// Gate reasons of one state.bin entry on host.
u8 plugin_manager_entry_gate(const PluginManagerEntry&, const PluginHost&) noexcept;
struct PluginStartupResult {
    PluginManagerResult manager;  // recovery or roll-forward failure; installed plugins still load
    PluginManagerSettled settled;
    bool rolled_forward;          // a staged official automatic update was committed
    bool catalog_installed;       // state.bin binds a plugin to a provider: scheduled checks may run
};
// A journal naming a staged update from the official provider whose
// manifest is still the replaced one is committed through the installer when
// automatic updates are still on for it (the roll-forward of "download now,
// apply at next start"). Any other journal goes to plugin_manager_recover.
// gate then lists the owners restore must skip.
PluginStartupResult plugin_manager_startup(const PluginManagerPaths& paths, const PluginHost& host, PluginManagerWork* work,
    PluginStartupGate* gate) noexcept;

// ---- applying an admitted index to installed plugins ------------------------------------------
// One provider per call, official first so its blocklist and owner
// reservations apply to the custom passes. The caller zeroes evaluation once
// and passes the same one to every call.
struct PluginCatalogPass {
    u64 provider;
    PluginProviderKind kind;
    const PluginIndex* index;  // null: no usable cache; only the official blocklist applies
    Utf8View document;
    bool installable;          // admitted and unexpired: updates may be offered
    bool auto_update;          // official: "Update official plugins automatically"
};
struct PluginRevokedPlugin {
    u64 owner;
    u32 name_bytes;
    char name[kNativePluginNameCapacity];
};
struct PluginAutomaticUpdate {
    u64 owner, provider, version_code, approved_permissions;
};
struct PluginEvaluation {
    u32 adopted, pruned, offers, reviews, incompatible;
    u32 revoked_count, update_count;
    bool state_written, trust_written;
    PluginRevokedPlugin revoked[kPluginManagerStateMaximumEntries];       // unload and distrust now
    PluginAutomaticUpdate updates[kPluginManagerStateMaximumEntries];     // within the approved mask
};
// Adopts manually installed plugins whose SHA-256 is a listed build of this
// provider (an unofficial provider never adopts an owner the official index
// reserves); prunes entries whose manifest is gone (journal clear only);
// sets or clears the revoked, withdrawn, review and incompatible flags and
// max_host_abi of the provider's plugins; flags blocked builds of any
// provider as malicious and removes their trust from plugins.bin. Writes
// state.bin and plugins.bin only when they change. Official automatic
// updates are listed, never started.
PluginManagerResult plugin_manager_evaluate(const PluginManagerPaths& paths, const PluginCatalogPass& pass,
    const PluginIndex* official, const PluginHost& host, PluginManagerWork* work, PluginEvaluation* evaluation) noexcept;

// ---- settings of the plugin manager -----------------------------------------------------------
// The official header switches (kPluginOfficial*); providers.bin is created on first write.
AcceptanceRecordResult plugin_manager_set_official_flags(const PluginManagerPaths& paths, u32 set, u32 clear) noexcept;
// Entry flags of a custom provider (kPluginProviderEnabled, kPluginProviderCheckDaily).
AcceptanceRecordResult plugin_manager_set_provider_flags(const PluginManagerPaths& paths, u64 provider, u32 set, u32 clear) noexcept;
// state.bin flags of an installed plugin: kPluginStateAutoUpdateOff, or
// kPluginStateHeld with held_code ("Skip this version").
AcceptanceRecordResult plugin_manager_set_plugin_flags(const PluginManagerPaths& paths, u64 owner, u32 set, u32 clear,
    u64 held_code = 0) noexcept;
// Removes a custom provider: its providers.bin entry, its floor and its
// cache. Plugins installed from it stay installed and bound (no updates).
AcceptanceRecordResult plugin_manager_remove_provider(const PluginManagerPaths& paths, u64 provider) noexcept;
// "Trust new key": replaces the pin of a provider after the user confirmed a
// probe whose document is still in work; the old floor is forgotten.
PluginProviderAddResult plugin_manager_replace_provider_key(const PluginManagerPaths& paths, const PluginProviderAddress& address,
    const PluginProviderProbe& probe, KdfSha256Workspace& workspace, PluginIndexWork* work, u64 now) noexcept;

// ---- the browser snapshot ---------------------------------------------------------------------
// Immutable once built; text stays escaped in the copied documents and is
// decoded with plugin_index_text_decode(document, text) for visible rows.
enum class PluginCatalogStatus : u8 {
    none,        // never checked: no cache
    ok,          // admitted and unexpired
    expired,     // browse only
    unadmitted,  // the cache is not the admitted index (a newer floor): browse only
    failed,      // the cache is unreadable or no longer verifies
    key_changed, // the provider serves another key (last check)
    disabled,    // checks are off
};
struct PluginCatalogProviderView {
    u64 id;
    PluginProviderKind kind;
    PluginCatalogStatus status;
    bool installable, enabled, check_daily;
    u64 sequence, issued, expires, added;
    u32 plugin_count, document_offset, document_bytes;
    u32 host_bytes, path_bytes, name_bytes;
    char host[kPluginProviderHostCapacity];
    char path[kPluginProviderPathCapacity];
    char name[kPluginProviderNameCapacity];
    u8 fingerprint[32];
};
struct PluginCatalogVersionView {
    u64 code, released, permissions;
    u32 abi, min_host_abi, max_host_abi, bytes;
    u8 sha256[32];
    PluginIndexText changelog;
    PluginCompatibility compatibility;
    bool revoked, blocked;
    PluginRevocationSeverity severity;
};
struct PluginCatalogEntryView {
    u64 owner;
    u32 provider; // slot in providers
    PluginIndexText slug, name, summary, publisher, license, homepage, author;
    u32 author_count, categories, disclosures;
    u32 first_version, version_count; // into versions, newest first
    u32 newest;                       // offset of the newest installable version, or kPluginIndexNoVersion
    u32 installed_index;              // into installed, or ~0u
    bool bound;                       // the installed plugin came from this provider
    bool reserved;                    // an unofficial listing of an owner the official index lists
    bool official_name;               // an unofficial listing named like an official plugin
    PluginUpdateSelection update;     // bound plugins
    PluginRevocationMatch revocation; // of the installed build
    PluginIndexText revocation_reason; // its reason, in the document of provider slot revocation_provider
    u32 revocation_provider;
};
// Every local manifest, with its state.bin entry and trust.
struct PluginCatalogInstalledView {
    u64 owner, provider, installed_code, previous_code, held_code, approved_permissions;
    u32 state_flags, entry; // entry: the bound catalog entry, or ~0u
    bool trusted, has_state;
    u8 sha256[32];
    u32 name_bytes, version_bytes;
    char name[kNativePluginNameCapacity];
    char version[kNativePluginVersionCapacity];
};
inline constexpr u32 kPluginCatalogSnapshotEntries = kPluginIndexAcceptanceSlots * kPluginIndexMaximumPlugins;
struct PluginCatalogSnapshot {
    u32 official_flags, provider_count, entry_count, version_count, installed_count;
    bool configured;      // the build has an official provider
    PluginCatalogProviderView providers[kPluginIndexAcceptanceSlots];
    PluginCatalogInstalledView installed[kNativePluginCatalogMaximumEntries];
    PluginCatalogEntryView* entries;
    PluginCatalogVersionView* versions;
    char* documents;
    u32 documents_bytes;
    u64 reserved_bytes; // the whole VirtualAlloc reservation, header included
};
inline Utf8View plugin_catalog_document(const PluginCatalogSnapshot& s, u32 provider) noexcept {
    return provider < s.provider_count ? Utf8View{s.documents + s.providers[provider].document_offset, s.providers[provider].document_bytes}
                                       : Utf8View{};
}
// key_changed: providers whose last check served another key (ids), so their rows say so.
struct PluginCatalogSnapshotInput {
    const PluginCatalogSources* sources;
    const PluginProviders* providers;
    bool configured;
    u32 key_changed_count;
    u64 key_changed[kPluginProviderMaximum];
};
// Loads and re-verifies each cache (expired ones read-only) into work, merges
// state.bin, plugins.bin and the local inventory. Null when memory or
// records fail; release with plugin_catalog_snapshot_release.
PluginCatalogSnapshot* plugin_catalog_snapshot_build(const PluginManagerPaths& paths, const PluginCatalogSnapshotInput& input,
    const PluginHost& host, KdfSha256Workspace& workspace, PluginIndexWork* official_work, PluginIndexWork* work,
    PluginManagerWork* manager_work, u64 now) noexcept;
void plugin_catalog_snapshot_release(PluginCatalogSnapshot*) noexcept;

} // namespace maicord
