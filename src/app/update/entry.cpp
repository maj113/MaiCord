#include "updates.hpp"
#include "maicord/app/update/log.hpp"
#if !defined(_WIN32)
#include "maicord/app/update/portable.hpp"
#include "maicord/core/data_path.hpp"
#endif

// Build inputs from CMake (MAICORD_UPDATE_REPOSITORY, MAICORD_UPDATE_PUBLIC_KEY,
// MAICORD_UPDATE_DEV, MAICORD_UPDATE_COMMIT and the version). Only this
// translation unit sees them, so changing them rebuilds one file.
#if !defined(MAICORD_UPDATE_REPOSITORY)
#define MAICORD_UPDATE_REPOSITORY ""
#endif
#if !defined(MAICORD_UPDATE_PUBLIC_KEY)
#define MAICORD_UPDATE_PUBLIC_KEY ""
#endif
#if !defined(MAICORD_UPDATE_DEV)
#define MAICORD_UPDATE_DEV 0
#endif
#if !defined(MAICORD_UPDATE_COMMIT)
#define MAICORD_UPDATE_COMMIT ""
#endif
#if !defined(MAICORD_VERSION_MAJOR)
#define MAICORD_VERSION_MAJOR 0
#define MAICORD_VERSION_MINOR 0
#define MAICORD_VERSION_PATCH 0
#endif

namespace maicord::app_detail {
namespace {
constexpr char repository[] = MAICORD_UPDATE_REPOSITORY;
constexpr char public_key[] = MAICORD_UPDATE_PUBLIC_KEY;
static_assert((sizeof(repository) == 1) == (sizeof(public_key) == 1),
    "Set both MAICORD_UPDATE_REPOSITORY and MAICORD_UPDATE_PUBLIC_KEY, or neither.");
static_assert(sizeof(public_key) == 1 || sizeof(public_key) == 131, "MAICORD_UPDATE_PUBLIC_KEY is 130 hex digits.");
constexpr char commit[] = MAICORD_UPDATE_COMMIT;
static_assert(sizeof(commit) == 1 || sizeof(commit) == 41, "MAICORD_UPDATE_COMMIT is 40 hex digits.");
static_assert(update_version_components_valid(MAICORD_VERSION_MAJOR, MAICORD_VERSION_MINOR, MAICORD_VERSION_PATCH, MAICORD_UPDATE_DEV));
constexpr u64 installed_version =
    update_version_number(MAICORD_VERSION_MAJOR, MAICORD_VERSION_MINOR, MAICORD_VERSION_PATCH, MAICORD_UPDATE_DEV);
#if !defined(_WIN32)
// Whether the release channels carry the Linux tarball (the Linux manifests of maicord_release.py linux): the build
// option MAICORD_LINUX_UPDATES_PUBLISHED, which the release workflow sets from its MAICORD_LINUX_RELEASES variable. A
// build that is not told so never stages anything, so no Linux install replaces itself with a package nobody signed
// for it; the marker check in update_portable_gate is what then enables the tarball install and no other.
#if !defined(MAICORD_LINUX_UPDATES_PUBLISHED)
#define MAICORD_LINUX_UPDATES_PUBLISHED 0
#endif
constexpr bool kLinuxUpdatesPublished = MAICORD_LINUX_UPDATES_PUBLISHED != 0;
#endif
} // namespace

UpdateServiceConfig application_update_build_config() noexcept {
    // Every updater path starts here, so this routes its diagnostics.
    g_update_log_sink = log_write;
    UpdateServiceConfig config{};
    if (!update_service_config({repository, sizeof(repository) - 1}, {public_key, sizeof(public_key) - 1}, installed_version, &config))
        update_service_config({}, {}, installed_version, &config);
#if !defined(_WIN32)
    // Only the tarball install (the .maicord-portable marker beside the real executable, in a directory this user can
    // write) may replace its own files; a package manager owns every other install (packaging/README.md).
    if (config.configured) {
        wchar_t executable[4096];
        const u32 count = executable_path(executable, 4096);
        update_portable_gate(&config, kLinuxUpdatesPublished && count && count < 4096, {executable, count});
    }
#endif
    return config;
}

Utf8View application_update_build_commit() noexcept {
    return {commit, sizeof(commit) == 41 ? kApplicationUpdatesCommitChars : 0u};
}

} // namespace maicord::app_detail
