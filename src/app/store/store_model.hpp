#pragma once

#include "maicord/app/plugins/catalog_index.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/store/store_scene.hpp"

// The data and state of the Plugins, Themes and Effects pages (store-ui.md). The model is a plain snapshot the
// host fills from the catalog service, the package library and the effect module (store_source.cpp); the page
// state is what the user has done on a page. Projection, filters, search and actions here are pure: they read the
// model and answer with commands, so tests run them against hand-made models.
namespace maicord::app_detail {

inline constexpr u32 kStoreItemMaximum = 192;
inline constexpr u32 kStoreChipMaximum = 40;
inline constexpr u32 kStoreQueryBytes = 64;
inline constexpr u32 kStoreKnobMaximum = 28;
inline constexpr u32 kStoreHostMaximum = 8;

enum class StoreTab : u8 { browse, installed, updates, developer };
enum class StoreSort : u8 { name, newest, lightest };    // plugins: the third is "Least access"
enum class StoreFilter : u8 { all, on, off, updates };   // Installed segments
enum class StoreCatalogState : u8 { loading, ready, unreachable, stale, not_configured };

// One thing the pages show: a catalog listing, an installed item, or both.
struct StoreItem {
    CatalogKind kind;
    Utf8View slug, name, summary, authors, version, license, source, homepage;
    Utf8View categories[kCatalogIndexMaximumCategories];
    u32 category_count;
    u32 item;      // index into the catalog index, or ~0u when not listed
    u64 released;
    // Plugins: 0 low, 1 medium, 2 high access. Effects: the cost class. Themes: the base.
    u8 risk, cost, base;
    u32 swatches[4];
    bool has_swatches;
    Utf8View effect_slug; // a theme's effect
    u32 bytes;
    // Installed state.
    bool installed, enabled, running, auto_update, sideloaded, unreviewed, can_roll_back, incompatible;
    bool active;        // theme: applied; effect: the current background
    bool has_settings;  // a plugin with a settings page
    bool has_knobs;     // an installed effect
    bool installable;   // the catalog can install or update now (fresh signed index)
    u8 revoked;         // CatalogRevocationSeverity
    Utf8View reason, installed_version, update_version, previous_version, folder, changelog, sha, commit, contrast;
    u32 api_level;
    bool update, needs_review;
    // The operation running on this item: 0 none, else a phase (1 downloading, 2 verifying, 3 applying).
    u8 busy;
    u32 library;        // effects: the library index, else ~0u
};

struct StoreKnobChoice {
    Utf8View label;
};
// An effect's knob, current value included (an installed effect), or the catalog's declaration.
struct StoreKnob {
    Utf8View label, hint;
    u8 type; // CatalogKnobType
    float minimum, maximum, value, fallback;
    u32 rgb;
    u32 choice_count;
    Utf8View choices[16];
};

struct StoreUniversal {
    u8 resolution, frame_rate; // step + 1; 0 follows (shader rows) or automatic (global rows)
    u8 speed, opacity, blend;  // twentieths, percent, blend + 1
    u8 pointer, unfocused, budget;
    bool pointer_available;
    bool battery_cap;
    // The global defaults a per-effect row shows beside its override, and what the package asks for.
    u8 global_resolution, global_frame_rate, requested_frame_rate;
};

struct StorePermission {
    Utf8View sentence, name, note; // note: "It sends the text of messages you choose to translate."
    UiIcon glyph;
    u8 risk;
    bool added, critical;
};

struct StoreConsent {
    bool open;
    bool sideload, update;
    Utf8View name, version, author;
    u32 bytes;
    u32 permission_count;
    StorePermission permissions[16];
    Utf8View hosts[kStoreHostMaximum];
    u32 host_count;
    bool reviewed;
};

struct StoreLink {
    bool open;     // the install-from-link entry or the card
    u8 phase;      // 0 entry, 1 working, 2 card, 3 failed
    u8 error;      // PackageError
    Utf8View host, name, authors, license, based_on, size_text, source_note, cost_text;
    CatalogKind kind;
    bool replaces, has_effect, effect_installed, cost_known, cost_refused, previewing;
    u32 swatches[4];
    Utf8View effect_slug;
};

struct StoreModel {
    StoreItem items[kStoreItemMaximum];
    u32 count;
    StoreCatalogState catalog;
    bool developer_mode, installable, fetching, expired;
    u64 checked_unix, fetched_unix, now_unix, revision;
    Utf8View error_text;
    u32 operation_kind_item; // the item an operation runs on, or ~0u
    bool operation_failed;
    u32 protocol_state;  // 0 asked, 1 ask now
    bool auto_update_all;
    bool trying;         // a theme is being tried
    Utf8View trying_name;
    // The selected item's detail content.
    Utf8View details_markdown, changelog_markdown;
    bool details_loading;
    StoreKnob knobs[kStoreKnobMaximum];
    u32 knob_count;
    StoreUniversal universal, global_universal;
    u32 effect_cost_state; // 0 unknown, 1 known light/medium/heavy, 2 refused
    Utf8View effect_cost_text;
    StoreConsent consent;
    StoreLink link;
    Utf8View dev_folder;
    char text[24576];       // scratch the source writes display strings into
    u32 text_used;
};

enum class StoreAct : u8 {
    none,
    tab,          // arg: StoreTab
    header_link,  // Install from link...
    search,       // the field
    search_clear,
    sort,         // opens the sort menu
    chip,         // arg: chip index (0 = All)
    clear_filters,
    clear_search,
    segment,      // Installed filter: arg StoreFilter
    card,         // open the detail of items[arg]
    card_primary, // arg: item; install / update / review / apply / use / knobs by state
    card_switch,  // arg: item
    card_settings,
    card_overflow,
    row_open,
    row_primary,
    row_switch,
    row_settings,
    row_overflow,
    back,
    detail_primary,
    detail_secondary,
    detail_remove,
    detail_source,
    detail_copy_sha,
    detail_author,
    update_all,
    check_now,
    retry,
    show_saved,
    copy_details,
    banner_update,
    auto_update_all,
    open_folder,
    protocol_on,
    protocol_off,
    knob,         // arg: knob index; slider/stepped value, toggle, choice
    knob_menu,
    knob_color,
    knob_reset,   // one knob
    knobs_reset,
    universal,    // arg: UniversalRow; shader override
    universal_reset,
    universal_global,
    keep_theme,
    revert_theme,
    dialog_confirm,
    dialog_cancel,
    dialog_check,
    dialog_toggle_low,
    dialog_skip,
    dialog_open_file,
    dialog_close,
    dialog_source,
    dev_load,
    dev_reload,
    perf_battery,
    updates_expand,
    count
};
enum class StoreUniversalRow : u8 { resolution, frame_rate, speed, opacity, blend, unfocused, budget, pointer, count };

constexpr u32 store_action(StoreAct act, u32 arg = 0) noexcept {
    return (static_cast<u32>(act) << 16) | (arg & 0xffffu);
}
constexpr StoreAct store_act(u32 action) noexcept {
    return static_cast<StoreAct>(action >> 16);
}
constexpr u32 store_arg(u32 action) noexcept {
    return action & 0xffffu;
}

// What the user did, for the host to carry out.
enum class StoreCommandKind : u8 {
    none,
    menu,            // open a menu: arg = StoreMenu, item, anchor
    install, update, review, remove, rollback, enable, disable, set_auto_update, skip_update,
    update_all, check_now, set_auto_update_all,
    open_plugin_settings, open_folder, open_source, open_homepage, copy_text, open_author,
    apply_theme, try_theme, keep_theme, revert_theme, use_effect, unuse_effect,
    knob_set, knob_reset, knobs_reset, universal_set, universal_reset, global_set, knob_color,
    consent_answer,  // arg 0 cancel, 1 accept; flag: enable now
    link_begin, link_commit, link_cancel, link_open_file, link_preview,
    protocol_answer,
    dev_load, dev_reload,
    open_page,       // arg: CatalogKind
    request_details, // the open detail needs its details text
    request_preview,
    ledger,
    saved_catalog,
};
enum class StoreMenu : u8 { sort, overflow, knob_choice, universal_choice, installed_overflow_theme, installed_overflow_effect };
struct StoreCommand {
    StoreCommandKind kind;
    StoreMenu menu;
    bool flag;
    u16 item;
    u32 arg, value;
    UiRect anchor;
};

// What a page has done. Everything is small; one per kind.
struct StorePageState {
    CatalogKind kind;
    StoreTab tab;
    StoreSort sort;
    StoreFilter filter;
    u64 chips;                       // bit i = chip i selected (0 = All)
    char query[kStoreQueryBytes];
    u32 query_bytes;
    // The detail view: the item's slug, and where the user came from.
    bool detail, knobs;
    char slug[kCatalogSlugMaximumBytes + 1];
    float saved_scroll;
    StoreTab detail_from;
    u32 low_expanded;                // consent: bit 0 low-access group open
    bool dialog_check;               // consent "Turn it on now"; sideload acknowledgement
    bool dialog_acknowledged;
    bool entered;                    // the tab was chosen at least once (Installed first when there is something)
    bool updates_open[8];
    bool link_dialog;                // the "Install from link..." entry
    bool remove_confirm;
    u16 remove_item;
    bool link_text_focus;
    bool show_saved;                 // the unreachable catalog's saved copy was chosen
};

struct StoreChip {
    Utf8View label;
    u32 count;
    u8 group;         // 0 base/cost, 1 categories, 2 low access
    u8 type;          // 0 all, 1 base (value), 2 light cost, 3 category (label), 4 low access
    u8 value;
};
struct StoreChips {
    StoreChip chips[kStoreChipMaximum];
    u32 count;        // chips[0] is "All"
};

struct StoreList {
    u16 order[kStoreItemMaximum]; // item indexes in display order
    u32 count;
    u32 total;                    // before filters
    u32 counts[4];                // installed: all, on, off, updates
};

// ---- Projection (store_projection.cpp) --------------------------------------------------------------------
// Whether a listed item matches the words of `query` (word-prefix over name, summary, authors, categories).
bool store_item_matches(const StoreItem&, Utf8View query) noexcept;
// The filter chips of a kind over the listed items, with counts. Base/cost chips first, categories sorted by
// name, "Low access only" last for plugins.
void store_chips_build(const StoreModel&, CatalogKind, StoreChips*) noexcept;
// Chips an item lights.
u64 store_item_chips(const StoreItem&, const StoreChips&) noexcept;
// Within a group chips are OR, across groups AND.
bool store_chips_match(u64 selected, u64 item, const StoreChips&) noexcept;
u64 store_chip_toggle(u64 selected, const StoreChips&, u32 index) noexcept;
// The Browse list: listed items of the kind, filtered and sorted.
void store_browse_list(const StoreModel&, const StorePageState&, const StoreChips&, StoreList*) noexcept;
// The Installed list for the segment and query; counts always cover the whole installed set.
void store_installed_list(const StoreModel&, const StorePageState&, StoreList*) noexcept;
// The Updates list: installed plugins with an update.
void store_updates_list(const StoreModel&, StoreList*) noexcept;
u32 store_update_count(const StoreModel&, CatalogKind) noexcept;
u32 store_installed_count(const StoreModel&, CatalogKind) noexcept;
u32 store_find_item(const StoreModel&, CatalogKind, Utf8View slug) noexcept;
// The page opens on Installed when something is installed, else Browse.
StoreTab store_initial_tab(const StoreModel&, CatalogKind) noexcept;

// ---- Page (store_page.cpp) ---------------------------------------------------------------------------------
void store_page_reset(StorePageState*, CatalogKind) noexcept;
struct StoreLayoutInput {
    const StoreModel* model;
    StorePageState* state;
    const UiStoreSearch* search;
    float width;
    bool reduced_motion;
    // Retained markdown blocks (UiMarkdown*) of the open detail's About and What's new; null: plain text.
    const void* about;
    const void* changes;
};
// Builds the page and its dialog into the scene (between ui_store_scene_begin and commit).
void store_page_build(UiStoreScene*, const StoreLayoutInput&) noexcept;
// An activation of the scene: updates the page state and returns what the host has to do.
StoreCommand store_page_act(const StoreModel&, StorePageState*, u32 action, u32 value, const UiRect& anchor) noexcept;
// The popup rows of a menu, and the command a chosen row stands for.
// `labels` holds the text of rows that are composed ("Roll back to 0.9.2"); it must outlive the popup.
u32 store_menu_items(const StoreModel&, const StorePageState&, StoreMenu, u32 item, PopupItem* out, u32 capacity,
                     Slice<char> labels) noexcept;
StoreCommand store_menu_choose(const StoreModel&, StorePageState*, StoreMenu, u32 item, u32 row) noexcept;
// Escape: closes a dialog, reverts a trial, leaves a detail view. handled false: the page has nothing to undo.
StoreCommand store_page_escape(const StoreModel&, StorePageState*, bool* handled) noexcept;
u32 store_page_tab_count(const StoreModel&, const StorePageState&) noexcept;
// The sentences and risk of a permission bit (the site's table); false past the known bits.
bool store_permission_text(u32 bit, StorePermission*) noexcept;
u8 store_permission_risk(u64 permissions, bool has_hosts) noexcept;

} // namespace maicord::app_detail
