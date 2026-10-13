#pragma once
#include "application_plugin_settings_projection.hpp"
#include "maicord/app/plugin_manager_runtime.hpp"

namespace maicord::app_detail {

// Settings > Plugins with the catalog: Browse / Installed / Updates /
// Providers, a list and detail view, and the dialog texts. Pure: the caller
// owns the snapshot, the bank and every borrowed view; "Official" and
// "Unofficial · host" chips come only from the provider kind, never from
// index text.
inline constexpr u32 kPluginBrowserControls = 64;
inline constexpr u32 kPluginBrowserRows = 8;
inline constexpr u32 kPluginBrowserChips = 4;
inline constexpr u32 kPluginBrowserTextBytes = 16 * 1024;
inline constexpr u32 kPluginBrowserQueryBytes = 64;
// Control ids; 1-6, 100-107 and 200-231 stay the Installed tab's local rows.
inline constexpr u64 kPluginBrowserTabsId = 300, kPluginBrowserStatusId = 301, kPluginBrowserRevokedId = 302,
    kPluginBrowserSearchId = 303, kPluginBrowserSourceId = 304, kPluginBrowserCategoryId = 305, kPluginBrowserCountId = 306,
    kPluginBrowserRowFirst = 310, kPluginBrowserPreviousId = 318, kPluginBrowserNextId = 319;
inline constexpr u64 kPluginBrowserBackId = 320, kPluginBrowserHeaderId = 321, kPluginBrowserNoticeFirst = 322,
    kPluginBrowserActionsId = 328, kPluginBrowserDescriptionId = 329, kPluginBrowserPermissionsId = 330,
    kPluginBrowserPermissionFirst = 331, kPluginBrowserDisclosuresId = 353, kPluginBrowserChangesId = 354,
    kPluginBrowserChangelogId = 355, kPluginBrowserDetailsId = 356, kPluginBrowserDetailFirst = 357,
    kPluginBrowserHomepageId = 363, kPluginBrowserVersionsId = 364, kPluginBrowserVersionFirst = 365,
    kPluginBrowserOptionsId = 369, kPluginBrowserAutoUpdateId = 370;
inline constexpr u64 kPluginBrowserUpdatesHeaderId = 380, kPluginBrowserUpdatesToggleId = 381, kPluginBrowserUpdateFirst = 382,
    kPluginBrowserUpdatesPreviousId = 390, kPluginBrowserUpdatesNextId = 391;
inline constexpr u64 kPluginBrowserOfficialId = 400, kPluginBrowserProviderFirst = 401, kPluginBrowserAddProviderId = 409;
inline bool plugin_browser_control(u64 id) noexcept { return id >= kPluginBrowserTabsId && id <= kPluginBrowserAddProviderId; }

enum class PluginBrowserTab : u8 { browse, installed, updates, providers };
enum class PluginBrowserSource : u8 { all, official, community };
enum class PluginBrowserDetailAction : u8 { none, install, enable, disable, uninstall, update, review, rollback, settings };
enum class PluginBrowserProviderAction : u8 { none, refresh, checks, remove, trust_key };

// The page's own state, kept by the UI between projections.
struct PluginBrowserUiState {
    PluginBrowserTab tab;
    PluginBrowserSource source;
    u32 category; // 0: all; otherwise 1 + the category index
    u32 first, updates_first;
    u32 query_bytes;
    char query[kPluginBrowserQueryBytes];
    bool detail;
    u64 detail_owner, detail_provider;
};
// Live facts outside the snapshot.
struct PluginBrowserFacts {
    const PluginCatalogSnapshot* snapshot; // null until one is built
    bool configured;                       // the build has an official provider
    bool busy;                             // a check or operation runs
    bool checking;
    u64 now_unix, checked_unix;
    Utf8View status;                       // the page status line, when any
    bool status_critical;
    ConstSlice<UiSettingsControl> local;   // Installed: the local inventory rows
    const NativePluginCatalog* inventory;  // the inventory those rows show
    ConstSlice<u64> loaded;                // owners whose module is enabled
    ConstSlice<u64> settings_owners;       // loaded owners with a settings page
    const PluginStartupGate* gate;
    const PluginRevokedPlugin* revoked;    // the latest plugin turned off as harmful
    u32 offers;                            // Updates (n) before a snapshot exists
};
// What each activatable row of the last projection stands for.
struct PluginBrowserMap {
    u32 rows[kPluginBrowserRows];         // Browse: entry indices
    u32 row_count;
    u32 updates[kPluginBrowserRows];      // Updates: entry indices
    u32 update_count;
    u32 detail_entry;                     // ~0u outside the detail view
    PluginBrowserDetailAction actions[6];
    u32 action_count;
    u32 versions[4];                      // detail: version offsets of "Other versions"
    u32 version_count;
    u32 providers[kPluginProviderMaximum]; // snapshot provider slots of the custom rows
    PluginBrowserProviderAction provider_actions[kPluginProviderMaximum][3];
    u32 provider_count;
    u32 update_all;                       // rows "Update all" starts (Review rows excluded)
};
struct PluginBrowserBank {
    UiSettingsControl controls[kPluginBrowserControls];
    UiSettingsControlPresentation presentation[kPluginBrowserControls];
    Utf8View chips[kPluginBrowserRows + 2][kPluginBrowserChips];
    Utf8View tabs[4], sources[3], categories[kPluginIndexCategoryCount + 1], search[2], detail[6], updates[2];
    Utf8View providers[kPluginProviderMaximum][3];
    ApplicationPluginCatalogText local_text[kApplicationPluginCatalogWindow];
    PluginBrowserMap map;
    u32 used;
    char text[kPluginBrowserTextBytes];
};

// ---- projection -------------------------------------------------------------------------------
// Entries matching the filters, in snapshot order (official first).
u32 plugin_browser_filtered(const PluginCatalogSnapshot* snapshot, const PluginBrowserUiState& ui) noexcept;
// The entry of a provider and owner, or ~0u.
u32 plugin_browser_find(const PluginCatalogSnapshot* snapshot, u64 provider, u64 owner) noexcept;
// The installed plugin of owner, or null.
const PluginCatalogInstalledView* plugin_browser_installed(const PluginCatalogSnapshot* snapshot, u64 owner) noexcept;
// The whole page into bank; returns the control count. A detail view whose
// entry is gone falls back to the list.
u32 plugin_browser_project(const PluginBrowserUiState& ui, const PluginBrowserFacts& facts, PluginBrowserBank* bank) noexcept;

// ---- dialog texts -----------------------------------------------------------------------------
inline constexpr u32 kPluginBrowserDialogBytes = 2048;
struct PluginBrowserDialog {
    Utf8View title, context, accept, option, cancel;
    bool danger;
    bool option_checked;
    bool option_required; // accept is refused while the option is unchecked
    u32 used;
    char text[kPluginBrowserDialogBytes];
};
inline bool plugin_browser_dialog_accepts(const PluginBrowserDialog& dialog, bool option_checked) noexcept {
    return !dialog.option_required || option_checked;
}
struct PluginBrowserInstallFacts {
    Utf8View name, publisher, provider_name, host;
    PluginProviderKind kind;
    u64 version_code, permissions;
    u32 disclosures;
    u64 downgrade_from; // nonzero: an older version than the installed one
    u64 update_from;    // nonzero: a newer version than the installed one
};
// "Install <name>?" (or an update or older version): provider line,
// publisher, permissions, disclosures and the full-access warning; unofficial
// plugins and downgrades use the danger fill.
void plugin_browser_install_dialog(const PluginBrowserInstallFacts& facts, PluginBrowserDialog* dialog) noexcept;
// The update review: "New:" lines, then what stays and what goes.
void plugin_browser_review_dialog(Utf8View name, u64 version_code, const PluginPermissionDiff& diff, PluginBrowserDialog* dialog) noexcept;
struct PluginBrowserProviderFacts {
    Utf8View host, path, name;
    u32 plugin_count;
    bool signed_provider, pin_matched;
    const u8* fingerprint;     // signed: the key to pin
    const u8* old_fingerprint; // key change: the pinned key
};
// Add provider (and "Trust new key" with both fingerprints): danger fill and
// a required "I trust this provider" option.
void plugin_browser_provider_dialog(const PluginBrowserProviderFacts& facts, PluginBrowserDialog* dialog) noexcept;
void plugin_browser_remove_provider_dialog(Utf8View name, u32 installed, PluginBrowserDialog* dialog) noexcept;
void plugin_browser_uninstall_dialog(Utf8View name, PluginBrowserDialog* dialog) noexcept;
void plugin_browser_rollback_dialog(Utf8View name, u64 to_code, u64 from_code, PluginBrowserDialog* dialog) noexcept;
// "Update all" listing the unofficial plugins among entries.
void plugin_browser_update_all_dialog(const PluginCatalogSnapshot& snapshot, ConstSlice<u32> entries, PluginBrowserDialog* dialog) noexcept;

} // namespace maicord::app_detail
