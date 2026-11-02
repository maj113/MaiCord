#include "catalog_service.hpp"
#include "ui_host.hpp"
#include "../packages/packages.hpp"
#include "../shell/state.hpp"
#include "../shell/toast.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/win32_kernel.hpp"

// The client's catalog configuration and the hosts the service calls. Build inputs from CMake:
// MAICORD_PLUGIN_CATALOG_REPOSITORY and the catalog keys (current, and the next one held offline for
// rotation). Only this translation unit sees them. No key: the catalog is not configured for this build.
#if !defined(MAICORD_PLUGIN_CATALOG_REPOSITORY)
#define MAICORD_PLUGIN_CATALOG_REPOSITORY ""
#endif
#if !defined(MAICORD_PLUGIN_CATALOG_PUBLIC_KEY)
#define MAICORD_PLUGIN_CATALOG_PUBLIC_KEY ""
#endif
#if !defined(MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY)
#define MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY ""
#endif

namespace maicord::app_detail {
namespace {
constexpr char kRepository[] = MAICORD_PLUGIN_CATALOG_REPOSITORY;
constexpr char kCurrentKey[] = MAICORD_PLUGIN_CATALOG_PUBLIC_KEY;
constexpr char kNextKey[] = MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY;
static_assert(sizeof(kCurrentKey) == 1 || sizeof(kCurrentKey) == 131, "MAICORD_PLUGIN_CATALOG_PUBLIC_KEY is 130 hex digits.");
static_assert(sizeof(kNextKey) == 1 || sizeof(kNextKey) == 131, "MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY is 130 hex digits.");
static_assert(sizeof(kNextKey) == 1 || sizeof(kCurrentKey) == 131, "A next catalog key needs a current one.");
static_assert(sizeof(kCurrentKey) == 1 || sizeof(kRepository) > 1, "A catalog key needs MAICORD_PLUGIN_CATALOG_REPOSITORY.");

bool developer_mode() noexcept { return g_app.preferences.developer_mode; }
void toast(Utf8View text, bool failure) noexcept {
    application_toast_show(failure ? UiToastKind::failure : UiToastKind::message, text);
}
PackageLinkKind link_kind(CatalogKind kind) noexcept { return static_cast<PackageLinkKind>(kind); }
static_assert(static_cast<u32>(PackageLinkKind::theme) == static_cast<u32>(CatalogKind::theme) &&
              static_cast<u32>(PackageLinkKind::effect) == static_cast<u32>(CatalogKind::effect));
bool validate_package(CatalogKind kind, ConstSlice<u8> bytes, Utf8View slug) noexcept {
    PackageInfo info{};
    if (package_validate(link_kind(kind), bytes, &info) != PackageError::none) return false;
    // An effect names itself; it must be the item the catalog lists.
    return kind != CatalogKind::effect || utf8_equal({info.slug, c_string_length(info.slug)}, slug);
}
void package_removed(CatalogKind kind, Utf8View slug, bool keep_file) noexcept {
    if (keep_file) application_packages_deactivate(link_kind(kind), slug);
    else application_packages_remove(link_kind(kind), slug);
}
} // namespace

namespace {
// The plugin list and call marker of the retired first plugin system. Nothing reads them now and no
// install depends on them, so every start removes what an earlier version left behind; a missing file is the
// normal case.
void remove_retired_files() noexcept {
    const wchar_t* const names[]{L"plugins.bin", L"plugin-call.bin"};
    for (const wchar_t* name : names) {
        wchar_t path[480];
        if (data_path(path, 480, name).ok()) DeleteFileW(path);
    }
}
} // namespace

bool application_catalog_initialize_default() noexcept {
    remove_retired_files();
    u8 keys[2][65];
    const u8* key_list[2];
    u32 key_count = 0;
    const Utf8View repository{kRepository, sizeof(kRepository) - 1};
    const Utf8View texts[]{{kCurrentKey, sizeof(kCurrentKey) - 1}, {kNextKey, sizeof(kNextKey) - 1}};
    for (const Utf8View& text : texts)
        if (text.bytes && update_public_key_from_hex(text, {keys[key_count], 65})) key_list[key_count] = keys[key_count], ++key_count;
    wchar_t base[480];
    const DataPathResult located = data_path(base, 480, L"");
    if (!located.ok()) return false;
    CatalogServiceOptions options{};
    options.base = {base, located.length};
    options.repository = repository;
    options.keys = {key_list, key_count};
    options.host = {developer_mode, toast, validate_package, application_packages_library_changed, package_removed,
                    application_plugins_ui_toast};
    return application_catalog_initialize(options);
}

} // namespace maicord::app_detail
