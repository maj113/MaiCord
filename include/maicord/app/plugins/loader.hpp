#pragma once

#include "maicord/core/win32_types.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/sdk/maicord_plugin.h"

// Loads plugin DLLs: a pinned plugins directory, the PE header checks that make
// LoadLibrary run no plugin code, and the one-time validation of the exported
// descriptor. UI thread only. Nothing here calls into a plugin.
namespace maicord {

inline constexpr u32 kPluginDllMaximumBytes = 8u * 1024u * 1024u;
inline constexpr u32 kPluginImageMaximumBytes = 32u * 1024u * 1024u;
inline constexpr u32 kPluginMaximumSections = 16;

enum class PluginLoadError : u8 {
    none,
    invalid_argument,
    already_loaded,
    directory,    // pinning the plugins directory failed
    open,         // the DLL could not be opened beside the pinned directory
    read,
    not_pe,       // truncated headers or an image this loader doesn't read
    machine,      // not an x64 DLL
    entry_point,  // AddressOfEntryPoint is not zero
    tls,          // a TLS directory is present
    sections,     // section table out of bounds, overlapping or writable and executable
    exports,      // not exactly one named export, or it is code or forwarded
    load,         // LoadLibraryExW failed
    descriptor,   // the exported descriptor failed validation (see PluginLoadResult::problem)
    api_level,    // the plugin needs a newer host
    permissions,  // the plugin needs permissions that were not approved
    release,      // FreeLibrary failed; the image stays owned for a retry
    digest,       // the file is not the SHA-256 the caller expects
};

// Which descriptor rule failed.
enum class PluginDescriptorProblem : u8 {
    none,
    location,  // a pointer or the table leaves the image or the executable sections
    size,
    abi,
    flags,     // flags or a reserved field is not zero
    owner,
    text,      // a string is empty, too long, not UTF-8 or has control characters
    permission,
    reserved_capability, // a reserved field (theme) is in use
    state,
    settings,
    hooks,
    commands,
    menus,
    buttons,
    network,
};

struct PluginLoadResult {
    PluginLoadError error;
    PluginDescriptorProblem problem;
    DWORD win32_error;
    // descriptor errors: the 1-based setting, hook, command, menu, button or host index, else 0.
    u32 index;
    constexpr bool ok() const noexcept { return error == PluginLoadError::none; }
};

// The plugins directory, pinned once with every ancestor (no reparse points, no
// replacement while open). Zero-initialized, caller-owned, stable address.
inline constexpr u32 kPluginPathCapacity = 1024;
inline constexpr u32 kPluginPathMaximumAncestors = 64;
struct PluginDirectory {
    HANDLE ancestors[kPluginPathMaximumAncestors];
    u32 ancestor_count;
    wchar_t path[kPluginPathCapacity];
    u32 path_characters;
};

PluginLoadResult plugin_directory_open(PluginDirectory*, ConstSlice<wchar_t> absolute_path) noexcept;
// Fails with release and keeps the handles when a close fails; retry later.
PluginLoadResult plugin_directory_close(PluginDirectory*) noexcept;

// What the host keeps of the descriptor's text. The descriptor itself stays
// readable in the mapped image for as long as the image is loaded.
struct PluginInfo {
    char name[MC_NAME_MAX];
    char author[MC_NAME_MAX];
    char version[MC_VERSION_MAX];
    char summary[MC_SUMMARY_MAX];
    char homepage[MC_SUMMARY_MAX];
    u8 name_bytes, author_bytes, version_bytes;
    u16 summary_bytes, homepage_bytes;
};

struct PluginExecutableRange {
    u32 begin, end; // RVAs
};

// A mapped, validated plugin. Zero-initialized, caller-owned, stable address.
struct PluginImage {
    HMODULE module;
    const mc_descriptor* descriptor; // inside module
    PluginInfo info;
    u64 owner_id;
    // The descriptor's permissions plus those its hooks imply.
    u64 permissions;
    u32 image_bytes;
    u32 executable_count;
    PluginExecutableRange executable[kPluginMaximumSections];
};

// Opens leaf (a file name in the pinned directory) without following links,
// checks its PE headers, maps it and validates the descriptor. The file is held
// open against writes until the call returns, so a nonnull sha256 is checked on
// exactly the bytes that are mapped. approved must contain every permission the
// descriptor needs; on any failure nothing stays loaded.
PluginLoadResult plugin_image_load(PluginImage*, const PluginDirectory&, ConstSlice<wchar_t> leaf,
                                   u64 approved_permissions, const u8* sha256 = nullptr) noexcept;
// FreeLibrary and reset. The caller stopped the plugin and removed its hooks first.
PluginLoadResult plugin_image_unload(PluginImage*) noexcept;

// The checks plugin_image_load applies, on their own, for tests and tooling:
// headers (bytes of the file's first kilobytes are enough for sections and the
// export names are read through file), then the descriptor inside a mapped image.
PluginLoadResult plugin_pe_check(HANDLE file, u64 file_bytes, PluginImage* output) noexcept;
PluginLoadResult plugin_descriptor_check(const u8* image, const PluginImage& pe, const mc_descriptor* descriptor,
                                         PluginInfo* info, u64* permissions) noexcept;
// The permissions a validated descriptor's hooks, buttons and network hosts imply
// (plugin.hpp computes the same at compile time); *permissions above includes them.
u64 plugin_implied_permissions(const mc_descriptor& descriptor) noexcept;

} // namespace maicord
