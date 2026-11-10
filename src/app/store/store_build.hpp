#pragma once

#include "store_model.hpp"

// Shared pieces of the page builders (store_page.cpp, store_detail.cpp, store_dialogs.cpp).
namespace maicord::app_detail::store_build {

struct Ctx {
    UiStoreBuilder& b;
    const StoreModel& m;
    StorePageState& st;
    const StoreLayoutInput& in;
};

Utf8View kind_plural(CatalogKind) noexcept;   // "plugins"
Utf8View kind_title(CatalogKind) noexcept;    // "Plugins"
Utf8View kind_noun(CatalogKind) noexcept;     // "plugin"
UiIcon kind_icon(CatalogKind) noexcept;
UiIcon category_icon(const StoreItem&) noexcept;
Utf8View risk_label(u8 risk) noexcept;
UiStoreTone risk_tone(u8 risk) noexcept;
Utf8View cost_label(u8 cost) noexcept;
Utf8View base_label(u32 base) noexcept;       // "Dark"; a base past the last reads as the first
// The index-th of a packed list of names (core/name_list.hpp); out of line, so a call site is one call.
Utf8View name_at(const char* names, u32 index) noexcept;
// The label of sort choice `sort` of a kind's Sort control ("Least access", or "Lightest" for effects).
Utf8View sort_name(CatalogKind, u32 sort) noexcept;
// "Oct 28, 2026" and "9.0 KB" into the scene arena.
Utf8View date_text(UiStoreBuilder&, u64 unix) noexcept;
Utf8View size_text(UiStoreBuilder&, u32 bytes) noexcept;
Utf8View byline(UiStoreBuilder&, const StoreItem&, bool listed = false) noexcept;
// `pattern` with each "{}" replaced by the next view, in the scene arena. At most four pieces in all (the literals
// between the marks count too); nest a compose for more.
Utf8View compose(UiStoreBuilder&, const char* pattern, const Utf8View& a) noexcept;
Utf8View compose(UiStoreBuilder&, const char* pattern, const Utf8View& a, const Utf8View& b) noexcept;
Utf8View compose(UiStoreBuilder&, const char* pattern, const Utf8View& a, const Utf8View& b, const Utf8View& c) noexcept;

// Leaves.
UiStoreNode& subtle(Ctx&, const Utf8View& value, u16 flags = 0) noexcept;
UiStoreNode& subtle(Ctx&, const char* value, u16 flags = 0) noexcept;
void card_open(Ctx&, UiStoreFill fill = UiStoreFill::subtle, u32 action = 0) noexcept;
UiStoreNode& hairline_row(Ctx&, bool last, float pad_x = 16, float pad_y = 12) noexcept;
// The two most common containers in one call: a column of text lines (gap 2, takes the row's free space) and a
// row that centres its children vertically.
UiStoreNode& text_column(Ctx&) noexcept;
UiStoreNode& center_row(Ctx&, u32 gap) noexcept;

// Screens.
void build_header(Ctx&) noexcept;
void build_tabs(Ctx&) noexcept;
void build_empty(Ctx&, UiIcon icon, const Utf8View& title, const Utf8View& body) noexcept;
void build_skeleton(Ctx&) noexcept;
void build_browse(Ctx&) noexcept;
void build_installed(Ctx&) noexcept;
void build_updates(Ctx&) noexcept;
void build_developer(Ctx&) noexcept;
void build_detail(Ctx&, const StoreItem&, u32 index) noexcept;
void build_knobs(Ctx&, const StoreItem&, u32 index) noexcept;
// The slider's step count for a number knob (2..101); never below 2, so a step index divides by steps - 1.
u32 knob_steps(const StoreKnob&) noexcept;
void build_perf_defaults(Ctx&) noexcept;
void build_dialog(Ctx&) noexcept;
void build_trial_bar(Ctx&) noexcept;

// Facts and permissions (detail rail and consent).
void access_eyebrow(Ctx&, u8 risk, float pad_y) noexcept; // the dot and "LOW ACCESS" heading of a permission group
void host_chips(Ctx&, const StorePermission&, ConstSlice<Utf8View> hosts) noexcept; // a `network` permission's hosts
void fact(Ctx&, const char* label, const Utf8View& value, u32 action = 0, u8 dot = 0) noexcept;
void notice_card(Ctx&, UiIcon icon, const Utf8View& title, const Utf8View& body, UiStoreFill fill, UiStoreInk ink) noexcept;
void permission_group(Ctx&, u8 risk, const StorePermission* rows, u32 count, ConstSlice<Utf8View> hosts) noexcept;

} // namespace maicord::app_detail::store_build
