#include "application_plugin_browser.hpp"
#include "maicord/core/memory.hpp"

// Build inputs from CMake: MAICORD_PLUGIN_CATALOG_REPOSITORY and the official
// catalog keys (current, and the next one held offline for rotation). Only
// this translation unit sees them. No key: the official catalog is not
// configured for this build.
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
constexpr char repository[] = MAICORD_PLUGIN_CATALOG_REPOSITORY;
constexpr char current_key[] = MAICORD_PLUGIN_CATALOG_PUBLIC_KEY;
constexpr char next_key[] = MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY;
static_assert(sizeof(current_key) == 1 || sizeof(current_key) == 131, "MAICORD_PLUGIN_CATALOG_PUBLIC_KEY is 130 hex digits.");
static_assert(sizeof(next_key) == 1 || sizeof(next_key) == 131, "MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY is 130 hex digits.");
static_assert(sizeof(next_key) == 1 || sizeof(current_key) == 131, "A next catalog key needs a current one.");
static_assert(sizeof(current_key) == 1 || sizeof(repository) > 1, "A catalog key needs MAICORD_PLUGIN_CATALOG_REPOSITORY.");
} // namespace

PluginBrowserConfig application_plugin_browser_build_config() noexcept {
    PluginBrowserConfig config{};
    const Utf8View name{repository, sizeof(repository) - 1};
    if (!name.bytes || name.bytes > kUpdateRepositoryMaximumBytes || !update_repository_valid(name)) return config;
    config.repository_bytes = name.bytes;
    copy_bytes(config.repository, name.data, name.bytes);
    const Utf8View keys[]{{current_key, sizeof(current_key) - 1}, {next_key, sizeof(next_key) - 1}};
    for (const Utf8View& key : keys)
        if (key.bytes && update_public_key_from_hex(key, {config.keys[config.key_count], 65})) ++config.key_count;
    return config;
}

} // namespace maicord::app_detail
