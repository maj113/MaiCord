// The services behind the store pages, as a client without a catalog key and without a GPU sees them: no index,
// nothing installed, no install session and no picture uploads. The store host, page and scene stay real.
#include "shell/state.hpp"
#include "effects/effects.hpp"
#include "packages/packages.hpp"
#include "plugins/catalog_service.hpp"
#include "plugins/dev_console.hpp"
#include "plugins/settings_dialog.hpp"
#include "maicord/render/d3d11/renderer.hpp"

namespace maicord {
namespace {
constexpr MediaAtlasGpuResult kNoDevice{{}, E_NOTIMPL, MediaAtlasGpuError::device_failure};
} // namespace

MediaAtlasGpuResult renderer_upload_media_straight_rgba8(D3D11Renderer*, MediaAssetKey, u16, u16, u32,
                                                         ConstSlice<u8>) noexcept {
    return kNoDevice;
}
MediaAtlasGpuResult renderer_begin_apng(D3D11Renderer*, MediaAssetKey, u16, u16, const ApngFrame&, const PngImage&,
                                        ConstSlice<u8>) noexcept {
    return kNoDevice;
}
MediaAtlasGpuResult renderer_advance_apng(D3D11Renderer*, MediaAssetKey, const ApngFrame&, const ApngFrame&,
                                          const PngImage&, ConstSlice<u8>) noexcept {
    return kNoDevice;
}

namespace app_detail {
namespace {
// The first fetch is on its way: the pages show their toolbar (search, sort) over skeleton cards.
CatalogView g_catalog{1, nullptr, nullptr, CatalogStatus::none, CatalogError::none, true};
} // namespace

// ---- catalog service ----
Utf8View application_catalog_error_text(CatalogError) noexcept {
    return utf8_literal("Not configured.");
}
const CatalogView& application_catalog_view() noexcept {
    return g_catalog;
}
// The index the next store_model_fill lists (null: none); the test owns it.
void store_stub_catalog_index(const CatalogIndex* index) noexcept {
    g_catalog.index = index;
}
// The operation the catalog runs (an item is busy while its phase is active); the store sees it as a new revision.
void store_stub_catalog_operation(const CatalogOperation& operation) noexcept {
    g_catalog.operation = operation;
    ++g_catalog.revision;
}
CatalogItemState application_catalog_item_state(CatalogKind, const Utf8View&) noexcept {
    return {nullptr, kCatalogNone, kCatalogNone};
}
void application_catalog_browse() noexcept {
}
bool application_catalog_check_now() noexcept {
    return false;
}
bool application_catalog_install(CatalogKind, const Utf8View&) noexcept {
    return false;
}
bool application_catalog_update(CatalogKind, const Utf8View&) noexcept {
    return false;
}
u32 application_catalog_update_all() noexcept {
    return 0;
}
bool application_catalog_rollback(CatalogKind, const Utf8View&) noexcept {
    return false;
}
bool application_catalog_remove(CatalogKind, const Utf8View&, bool) noexcept {
    return false;
}
bool application_catalog_set_enabled(CatalogKind, const Utf8View&, bool) noexcept {
    return false;
}
bool application_catalog_set_auto_update(CatalogKind, const Utf8View&, bool) noexcept {
    return false;
}
bool application_catalog_set_auto_update_all(bool) noexcept {
    return false;
}
bool application_catalog_skip_update(CatalogKind, const Utf8View&) noexcept {
    return false;
}
bool application_catalog_sideload(ConstSlice<wchar_t>) noexcept {
    return false;
}
bool application_catalog_dev_reload() noexcept {
    return false;
}
Utf8View application_catalog_dev_folder(u32) noexcept {
    return {};
}
const CatalogConsent* application_catalog_consent() noexcept {
    return nullptr;
}
bool application_catalog_consent_answer(bool, bool) noexcept {
    return false;
}
bool application_catalog_asset_request(CatalogAssetKind, const CatalogBlob&) noexcept {
    return false;
}
bool application_catalog_asset_take(const u8 (&)[32], CatalogAsset*) noexcept {
    return false;
}
void application_catalog_asset_release(CatalogAsset*) noexcept {
}
PluginRuntime* application_catalog_runtime() noexcept {
    return nullptr;
}
bool application_plugin_settings_open(PluginBinding*) noexcept {
    return false;
}

// ---- developer console ----
u32 application_dev_console_count() noexcept {
    return 0;
}
const DevLine* application_dev_console_line(u32) noexcept {
    return nullptr;
}
u32 application_dev_console_revision() noexcept {
    return 0;
}
void application_dev_console_clear() noexcept {
}

// ---- link, file and drop installs ----
bool application_packages_begin_link(Utf8View) noexcept {
    return false;
}
bool application_packages_begin_file(ConstSlice<wchar_t>) noexcept {
    return false;
}
PackageStatus application_packages_status() noexcept {
    return {};
}
const PackageCard* application_packages_card() noexcept {
    return nullptr;
}
bool application_packages_preview_start() noexcept {
    return false;
}
bool application_packages_previewing() noexcept {
    return false;
}
EffectCost application_packages_preview_cost() noexcept {
    return {};
}
PackageError application_packages_commit() noexcept {
    return PackageError::busy;
}
void application_packages_cancel() noexcept {
}
bool application_packages_take_deep_link(PackageDeepLink*) noexcept {
    return false;
}
bool application_packages_protocol_asked() noexcept {
    return true;
}
bool application_packages_protocol_answer(bool) noexcept {
    return false;
}
void application_packages_drop_enable(bool) noexcept {
}

} // namespace app_detail
} // namespace maicord
