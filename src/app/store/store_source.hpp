#pragma once

#include "store_model.hpp"

namespace maicord::app_detail {

// What the host knows that no service does.
struct StoreSourceHooks {
    bool trying;           // a theme is being tried
    Utf8View trying_name;
};
// Fills the model from the catalog service, the package library and the effect module (UI thread).
void store_model_fill(StoreModel*, const StoreSourceHooks&) noexcept;
// The open item's extras: details and changelog text (the host fetched and kept them), permissions of the shown
// version, an effect's knobs with their values, the universal settings and the measured cost.
struct StoreDetailHooks {
    Utf8View details, changelog;
    bool details_loading;
};
void store_model_fill_detail(StoreModel*, CatalogKind, const Utf8View& slug, const StoreDetailHooks&) noexcept;
// "Light here · 0.4 ms" (here) or "Light · 0.4 ms": a measured cost, written into the model's text.
struct EffectCost;
Utf8View store_cost_text(StoreModel*, const EffectCost&, bool here) noexcept;

} // namespace maicord::app_detail
