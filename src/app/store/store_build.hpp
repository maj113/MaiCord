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

inline constexpr Utf8View kNoView{};
Utf8View kind_plural(CatalogKind) noexcept;   // "plugins"
Utf8View kind_title(CatalogKind) noexcept;    // "Plugins"
Utf8View kind_noun(CatalogKind) noexcept;     // "plugin"
UiIcon kind_icon(CatalogKind) noexcept;
UiIcon category_icon(const StoreItem&) noexcept;
Utf8View risk_label(u8 risk) noexcept;
UiStoreTone risk_tone(u8 risk) noexcept;
Utf8View cost_label(u8 cost) noexcept;
// "Oct 28, 2026" and "9.0 KB" into the scene arena.
Utf8View date_text(UiStoreBuilder&, u64 unix) noexcept;
Utf8View size_text(UiStoreBuilder&, u32 bytes) noexcept;
Utf8View byline(UiStoreBuilder&, const StoreItem&) noexcept;
// "Updated 1.1 / Update 1.2.0" style label pieces.
Utf8View join(UiStoreBuilder&, Utf8View a, Utf8View b, Utf8View c = {}, Utf8View d = {}) noexcept;

// Leaves.
UiStoreNode& text(Ctx&, Utf8View value, UiStoreType type = UiStoreType::body, UiStoreInk ink = UiStoreInk::normal,
                  u16 flags = 0) noexcept;
UiStoreNode& subtle(Ctx&, Utf8View value, u16 flags = 0) noexcept;
UiStoreNode& chip_for(Ctx&, const StoreItem&) noexcept;
void card_open(Ctx&, UiStoreFill fill = UiStoreFill::subtle, u32 action = 0) noexcept;
UiStoreNode& hairline_row(Ctx&, bool last, float pad_x = 16, float pad_y = 12) noexcept;

// Screens.
void build_header(Ctx&) noexcept;
void build_tabs(Ctx&) noexcept;
void build_empty(Ctx&, UiIcon icon, Utf8View title, Utf8View body, bool error) noexcept;
void build_skeleton(Ctx&) noexcept;
void build_browse(Ctx&) noexcept;
void build_installed(Ctx&) noexcept;
void build_updates(Ctx&) noexcept;
void build_developer(Ctx&) noexcept;
void build_detail(Ctx&, const StoreItem&, u32 index) noexcept;
void build_knobs(Ctx&, const StoreItem&, u32 index) noexcept;
void build_perf_defaults(Ctx&) noexcept;
void build_universal(Ctx&, const StoreItem*) noexcept;
void build_dialog(Ctx&) noexcept;
void build_trial_bar(Ctx&) noexcept;

// Facts and permissions (detail rail and consent).
void fact(Ctx&, Utf8View label, Utf8View value, bool mono = false, bool link = false, u32 action = 0) noexcept;
void notice_card(Ctx&, UiIcon icon, Utf8View title, Utf8View body, UiStoreFill fill, UiStoreInk ink) noexcept;
void permission_group(Ctx&, u8 risk, const StorePermission* rows, u32 count, ConstSlice<Utf8View> hosts) noexcept;

} // namespace maicord::app_detail::store_build
