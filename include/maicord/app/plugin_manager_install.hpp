#pragma once

#include "maicord/app/plugin_bundle_install.hpp"
#include "maicord/app/plugin_index_policy.hpp"
#include "maicord/app/plugin_index_service.hpp"
#include "maicord/app/plugin_preferences.hpp"

namespace maicord {

// Journaled catalog install, update, rollback and uninstall around the bundle
// installer. One operation at a time; journal.bin names it until settled.
//   1. prepare (worker): re-verify the admitted cache, re-select the version,
//      journal intent, download at the exact signed size, re-hash, check the
//      PE header, stage the synthesized manifest and the next rollback
//      snapshot, journal staged. Rollback stages the retained DLL instead.
//   2. installer handshake (UI) with prepared.manifest/options; the manifest
//      rename is the commit point.
//   3. finish (UI): journal committed, trust, snapshot, state.bin, clear.
//   4. recover (startup, nothing loaded): the same settling, per
//      plugin_manager_recovery_plan.
// Trust carries over when the replaced identity was trusted (or the install
// enables): approved & version permissions, keeping off flags so a plugin
// turned off after a crash stays off. Growth beyond the approved mask stops
// prepare with `review` before anything is written. Uninstall removes trust
// before the installer runs. Logs: "plugins", never paths, URLs or index text.

enum class PluginManagerError : u8 {
    none,
    invalid_input,
    storage,       // plugin-manager directories or staging files (native_error)
    busy,          // another operation is journaled: recover first
    records,       // state.bin or journal.bin could not be read or written (record_error)
    trust,         // plugins.bin could not be read or written
    inventory,     // the plugins directory could not be read, or holds a conflicting manifest for the owner
    cache,         // no admitted, unexpired cached index for the provider (index_error, record_error)
    not_listed,    // the owner or version is not in the index
    incompatible,  // the version does not run on this host
    revoked,       // revoked by its provider or on the official blocklist
    reserved,      // an unofficial provider lists an owner the official index reserves
    conflict,      // the index re-lists a known version code with other bytes
    review,        // more permissions than approved (permissions); nothing was written
    capacity,      // state.bin already holds 32 plugins
    installed,     // install: the owner already has a manifest (update or uninstall it instead)
    not_installed, // update, rollback or uninstall: no matching manifest and state entry
    current,       // update: that version is already installed
    provider,      // the plugin is bound to another provider
    fetch,         // the download failed (fetch_error)
    integrity,     // the downloaded or copied bytes do not hash to the signed SHA-256
    image,         // not an AMD64 DLL
    manifest,      // the manifest could not be synthesized, read or written
    rollback,      // no retained previous version, or its snapshot or DLL no longer matches
    installer,     // the bundle installer failed during recovery
};
struct PluginManagerResult {
    PluginManagerError error;
    UpdateFetchError fetch_error;
    AcceptanceRecordError record_error;
    PluginIndexError index_error;
    u32 native_error;
    PluginPermissionDiff permissions; // review: added, removed and kept against the approved mask
    constexpr bool ok() const noexcept { return error == PluginManagerError::none; }
};

struct PluginManagerRequest {
    PluginJournalOperation operation; // install, update or rollback
    u64 owner;
    u64 version_code;         // install, update: the version the dialog named
    u64 approved_permissions; // MC_PERM_* the user approved; an automatic update passes the trusted mask
    bool enable_after;        // install: trust (enable) the plugin once installed
};
// Catalog metadata for state.bin. Recovery has none and keeps the replaced
// entry's values until the next check.
struct PluginManagerFacts {
    char slug[kPluginManagerSlugCapacity];
    u32 max_host_abi;
    u64 admitted_sequence;
    bool known;
};
struct PluginManagerPrepared {
    PluginJournal journal; // as saved: phase staged (uninstall: intent)
    NativePluginBundleInstallOptions options;
    PluginManagerFacts facts;
    u32 manifest_count;
    wchar_t manifest[kPluginManagerPathCapacity]; // staging\<owner16>.maicord-plugin.json, the installer's input
};
// Scratch for one call (about 40 KiB). Caller-owned; one thread.
struct PluginManagerWork {
    NativePluginCatalog catalog;
    NativePluginCatalogEntry entries[kNativePluginCatalogMaximumEntries];
    PluginManagerState state;
    NativePluginPreferences trust;
    char record[kNativePluginManifestMaximumBytes];
};

// Step 1. official is the cached official index for the blocklist and owner
// reservation (null when none; unused for the official source). On failure
// the journal is clear, staging empty and plugins\ untouched.
PluginManagerResult plugin_manager_prepare(const PluginManagerPaths& paths, const PluginCatalogSource& source,
    const PluginManagerRequest& request, const UpdateTransport& transport, KdfSha256Workspace& workspace,
    PluginIndexWork* index_work, const PluginIndex* official, PluginManagerWork* work, u64 now_unix,
    PluginManagerPrepared* prepared) noexcept;
// Journals the uninstall and removes the trust; the caller then runs
// native_plugin_bundle_uninstall_start(owner, paths.plugins).
PluginManagerResult plugin_manager_uninstall_prepare(const PluginManagerPaths& paths, u64 owner, PluginManagerWork* work,
    PluginManagerPrepared* prepared) noexcept;

struct PluginManagerSettled {
    PluginRecoveryAction action;
    PluginJournalOperation operation;
    u64 owner;
    bool trusted;      // plugins.bin trusts the installed identity afterwards
    bool data_removed; // uninstall with delete_data: plugin-data\<owner16> is gone
    u32 removed_dlls, removed_manifests, staging_removed;
};
// Step 3, after the installer is released. prepared must be the journaled
// operation; delete_data applies to an uninstall.
PluginManagerResult plugin_manager_finish(const PluginManagerPaths& paths, const PluginManagerPrepared& prepared, bool delete_data,
    PluginManagerWork* work, PluginManagerSettled* settled) noexcept;
// Step 4. An interrupted uninstall is completed synchronously; its
// plugin-data is kept.
PluginManagerResult plugin_manager_recover(const PluginManagerPaths& paths, PluginManagerWork* work, PluginManagerSettled* settled) noexcept;

// Drives a started installer without a UI (nothing loaded): commits at
// prepared, waits for completion, releases it and returns the final status.
NativePluginBundleInstallResult plugin_manager_run_installer(NativePluginBundleInstall* installer, u32 timeout_ms) noexcept;

} // namespace maicord
