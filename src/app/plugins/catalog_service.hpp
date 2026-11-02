#pragma once

#include "maicord/app/plugins/catalog_state.hpp"
#include "maicord/app/update/fetch.hpp"
#include "maicord/core/win32_types.hpp"

// The official catalog in the client (plugin-design.md §12.2–§12.4, §9.1): one worker fetches and verifies
// the signed index and the files it lists, the UI thread owns the immutable snapshot, catalog-state.bin and
// the ABI-6 plugin runtime, and installs, updates, rolls back, removes and revokes plugins, themes and
// effects. Everything here is UI thread only unless it says otherwise. local://prog/catalog-api.md is the
// contract with the store pages and the package installer.
namespace maicord {
struct PluginRuntime;
struct PluginConsentRequest; // maicord/app/plugins/consent.hpp
namespace app_detail {

enum class CatalogStatus : u8 {
    not_configured, // this build has no catalog key
    none,           // never fetched and nothing cached
    ok,             // a fresh signed index: installs and updates are allowed
    stale,          // the cached index expired: browse only
    unreachable,    // the last fetch failed (index may still hold the cached one)
    invalid,        // the served index failed verification or replays an older one
};
enum class CatalogAction : u8 { none, check, install, update, rollback, remove, enable, sideload };
enum class CatalogPhase : u8 { idle, queued, downloading, verifying, consent, applying, done, failed };
enum class CatalogError : u8 {
    none, busy, not_configured, unreachable, rate_limited, signature, invalid_index, replay, expired,
    not_listed, incompatible, revoked, download, digest, package, storage, plugin_load, plugin_mismatch,
    owner_conflict, declined, capacity, enable_failed, not_installed, developer_mode,
};
// A short sentence for a toast or the "Copy details" line.
Utf8View application_catalog_error_text(CatalogError) noexcept;

struct CatalogOperation {
    CatalogAction action;
    CatalogPhase phase;
    CatalogKind kind;
    u8 slug_bytes;
    char slug[kCatalogSlugMaximumBytes];
    u64 bytes, total; // downloading
    CatalogError error;
};
struct CatalogView {
    u64 revision;              // changes whenever anything below changes
    const CatalogIndex* index; // null without a catalog; immutable until the revision changes
    const CatalogState* state; // what is installed
    CatalogStatus status;
    CatalogError error;        // why the last check failed
    bool fetching;
    bool installable;          // index is fresh and signed
    u64 fetched_unix;          // when the index shown was accepted
    u64 checked_unix;          // the last successful check
    CatalogOperation operation;
};
const CatalogView& application_catalog_view() noexcept;

inline constexpr u32 kCatalogNone = ~0u;
// One item as the store shows it: installed, listed, or both.
struct CatalogItemState {
    const CatalogStateEntry* installed; // null: not installed
    u32 item;                           // into index->items, or kCatalogNone
    u32 update;                         // the newest installable version into index->versions, or kCatalogNone
    bool needs_review;                  // that update adds permissions or hosts
    bool enabled, running, auto_update, sideloaded, unreviewed, can_roll_back;
    bool incompatible;                  // listed, but every version needs a newer MaiCord
    CatalogRevocationSeverity revoked;  // of the installed file
    Utf8View reason;
};
CatalogItemState application_catalog_item_state(CatalogKind, const Utf8View& slug) noexcept;
// The newest version of a listed item this client can install (api_level and revocations), or kCatalogNone.
u32 application_catalog_installable_version(u32 item) noexcept;

// ---- actions (one at a time; progress through view.operation and the revision) ---------------------
// Browse was opened: fetches may run from now on (no catalog traffic for users who never browse and have
// nothing installed). Fetches once an hour while browsing.
void application_catalog_browse() noexcept;
bool application_catalog_check_now() noexcept;
bool application_catalog_install(CatalogKind, const Utf8View& slug) noexcept;
bool application_catalog_update(CatalogKind, const Utf8View& slug) noexcept;
u32 application_catalog_update_all() noexcept;
bool application_catalog_rollback(CatalogKind, const Utf8View& slug) noexcept;
// Plugins: disable, delete the DLLs and, with delete_data, the plugin's settings and store. Themes and
// effects: the package library deletes the file and falls back.
bool application_catalog_remove(CatalogKind, const Utf8View& slug, bool delete_data) noexcept;
bool application_catalog_set_enabled(CatalogKind, const Utf8View& slug, bool enabled) noexcept;
bool application_catalog_set_auto_update(CatalogKind, const Utf8View& slug, bool on) noexcept;
bool application_catalog_set_auto_update_all(bool on) noexcept;
// "Skip this version" on an update that needs review.
bool application_catalog_skip_update(CatalogKind, const Utf8View& slug) noexcept;
// Developer Mode: copies a DLL into the plugins directory and asks the sideload consent.
bool application_catalog_sideload(ConstSlice<wchar_t> dll_path) noexcept;
// Developer Mode: the dev folder (%LOCALAPPDATA%\MaiCord\plugins\dev, or %MAICORD_PLUGIN_DEV%) is watched. A DLL
// that appears or changes there is copied in and reviewed like a sideload; the first build of an owner asks the
// sideload consent, and every later build whose permissions and hosts the user already allowed replaces the
// running one without a question: disable, unload, load and enable, keeping the plugin's state when its
// state_layout is unchanged (MC_CTX_RELOADED) and its settings and store always. What happens, and the
// plugins' log lines and budget strikes, goes to the Developer console (dev_console.hpp).
// The effects dev folder (effects\\dev, or %MAICORD_EFFECT_DEV%) is watched too: a settled change calls
// CatalogServiceHost::library_changed, so the effect library scans again and keeps the selection and knob values.
// The two change notifications (0: plugins, 1: effects) join the UI wait set; null outside Developer Mode.
HANDLE application_catalog_dev_notification(u32 folder) noexcept;
// Scans the dev folder now and loads every DLL in it again ("Reload" on the Developer page).
bool application_catalog_dev_reload() noexcept;
// A dev folder as UTF-8 (0: plugins, 1: effects; empty until Developer Mode has opened it).
Utf8View application_catalog_dev_folder(u32 folder) noexcept;

// While view.operation.phase is consent: what the dialog asks. Answer once; the answer returns true when the
// plugin was installed (a decline ends the operation with CatalogError::declined).
struct CatalogConsent {
    const PluginConsentRequest* request; // views into the service, valid until answered
    bool sideload, update;
    u32 bytes;
};
const CatalogConsent* application_catalog_consent() noexcept;
bool application_catalog_consent_answer(bool accepted, bool enable_now) noexcept;

// ---- details, previews and loops: verified against the signed digest, cached on disk ----------------
enum class CatalogAssetKind : u8 { details, preview };
struct CatalogAsset {
    u8 sha256[32];
    u8* bytes; // owned by the caller after take
    u32 count;
};
bool application_catalog_asset_request(CatalogAssetKind, const CatalogBlob&) noexcept;
// A requested asset that arrived (the revision changed); false while it is still on its way.
bool application_catalog_asset_take(const u8 (&sha256)[32], CatalogAsset*) noexcept;
void application_catalog_asset_release(CatalogAsset*) noexcept;

// ---- the package installer (link and file installs) -------------------------------------------------
bool application_catalog_record_unreviewed(CatalogKind, const Utf8View& slug, const u8 (&sha256)[32], const Utf8View& version) noexcept;
bool application_catalog_forget(CatalogKind, const Utf8View& slug) noexcept;

// ---- lifecycle ------------------------------------------------------------------------------------------
// What the service needs from the rest of the client. Tests supply their own.
struct CatalogServiceHost {
    bool (*developer_mode)() noexcept;
    void (*toast)(Utf8View text, bool failure) noexcept;
    // Worker thread: checks a downloaded theme or effect before it is written (slug: the catalog's).
    bool (*validate_package)(CatalogKind, ConstSlice<u8> bytes, Utf8View slug) noexcept;
    // Any thread: theme or effect files changed on disk.
    void (*library_changed)() noexcept;
    // A theme or effect was revoked (keep: switch it off) or removed (delete its file and switch it off).
    void (*package_removed)(CatalogKind, Utf8View slug, bool keep_file) noexcept;
    // The runtime's toast sink (PluginRuntime::toast).
    void (*plugin_toast)(void* context, u64 owner_id, Utf8View text, u32 tone) noexcept;
};
struct CatalogServiceOptions {
    ConstSlice<wchar_t> base;            // %LOCALAPPDATA%\MaiCord, no trailing separator
    Utf8View repository;                 // "<owner>/<repo>": the catalog lives at https://<owner>.github.io/<repo>/
    ConstSlice<const u8*> keys;          // the compiled 65-byte public keys; empty: not configured
    UpdateTransport transport;           // null get: the HTTPS stack
    u64 (*clock_unix)() noexcept;        // null: the system clock
    CatalogServiceHost host;
};
// Allocates nothing until something is installed or the store is opened.
bool application_catalog_initialize(const CatalogServiceOptions&) noexcept;
// The production options (catalog_app.cpp): the build's catalog configuration and the app's hosts.
bool application_catalog_initialize_default() noexcept;
// After sign-in: loads catalog-state.bin, opens the runtime and enables the approved plugins.
void application_catalog_restore() noexcept;
void application_catalog_release() noexcept;
// The worker's results and the plugins' HTTP completions; both wake the UI loop.
HANDLE application_catalog_notification() noexcept;
HANDLE application_catalog_http_notification() noexcept;
void application_catalog_poll() noexcept;
// Milliseconds until the runtime has settings or a store to write (INFINITE: nothing pending).
u32 application_catalog_wait_timeout(u64 now_ms) noexcept;
// The plugin runtime, null until a plugin is enabled.
PluginRuntime* application_catalog_runtime() noexcept;

} // namespace app_detail
} // namespace maicord
