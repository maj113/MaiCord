#pragma once

#include "maicord/app/plugins/catalog_index.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/ui/core/input_router.hpp"
#include "maicord/ui/settings/scene.hpp"

// Settings > Plugins, Themes and Effects (store-ui.md): the host side. The three pages are host pages of the
// settings scene; their column is drawn by UiStoreScene (src/ui/store) from a page built out of the catalog and
// package models (store_page.cpp). This module owns the page state, the search editor, the pop-up menus, the
// install dialogs' bookkeeping and the commands the pages issue. UI thread only.
namespace maicord {
struct UiStoreScene;
} // namespace maicord

namespace maicord::app_detail {

// A SettingsPanel page id that is a store page.
bool application_store_kind(u64 page_id, CatalogKind* kind) noexcept;
// The selected settings page is (or stopped being) a store page.
void application_store_visible(bool visible, CatalogKind kind) noexcept;
// Frame step after the settings scene is updated: lays the page out in its host rectangle and publishes it to
// the renderer. True while it animates.
bool application_store_synchronize(UiSettingsScene* scene) noexcept;
bool application_store_active() noexcept;
// The shown store page while it paints a spinner (a busy button, the install dialog's check), else null. The frame
// clock samples the shared spinner phase into it and arms the next spinner step from it.
UiStoreScene* application_store_spinner_scene() noexcept;
// Pointer, wheel and character input of the settings modal; true when the store took the message.
bool application_store_window_event(const WindowEvent& input) noexcept;
bool application_store_input(UiInputKind kind, u32 code, bool repeated) noexcept;
bool application_store_character(u32 code) noexcept;
// Settings > Effects on an installed effect's knobs (the theme editor's Knobs button).
bool application_store_open_effect_knobs(const Utf8View& slug) noexcept;
// Settings closed.
void application_store_release() noexcept;

} // namespace maicord::app_detail
