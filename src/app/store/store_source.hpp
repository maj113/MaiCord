#pragma once

#include "store_model.hpp"

namespace maicord::app_detail {

// What the host knows that no service does.
struct StoreSourceHooks {
    bool trying;           // a theme is being tried
    Utf8View trying_name;
};
// Fills the model from the catalog service, the package library and the effect module (UI thread). The listings
// borrow text from the catalog's current index (m->index): hold it (application_catalog_hold_index) for as long as the
// model is read.
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
// "WCAG AAA for text", "WCAG AA for text" or "Below WCAG AA for text": what a theme's text (swatch 2) keeps over its
// background (swatch 0); swatches are background, surface, text, accent.
Utf8View store_contrast_text(const u32 (&swatches)[4]) noexcept;

} // namespace maicord::app_detail
