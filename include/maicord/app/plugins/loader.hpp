#pragma once

#include "maicord/core/win32_types.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/sdk/maicord_plugin.h"

// Loads native plugins: a DLL on Windows (the PE header checks that make LoadLibrary run no plugin code) and a
// freestanding shared object on Linux (the ELF checks that make dlopen run none), both through a pinned plugins
// directory and the one-time validation of the exported descriptor. UI thread only. Nothing here calls into a plugin.
namespace maicord {

inline constexpr u32 kPluginDllMaximumBytes = 8u * 1024u * 1024u;
inline constexpr u32 kPluginImageMaximumBytes = 32u * 1024u * 1024u;
inline constexpr u32 kPluginMaximumSections = 16;

enum class PluginLoadError : u8 {
    none,
    invalid_argument,
    already_loaded,
    directory,    // pinning the plugins directory failed
    open,         // the plugin file could not be opened beside the pinned directory
    read,
    not_pe,       // truncated headers or an image this loader doesn't read (not a PE or an ELF shared object)
    machine,      // not an x64 DLL / x86-64 shared object
    entry_point,  // PE: AddressOfEntryPoint is not zero; ELF: an entry point, an initializer or a finalizer
    tls,          // a TLS directory (PE) or segment (ELF) is present
    sections,     // section table / program headers out of bounds, overlapping, or writable and executable (ELF: or an executable stack)
    exports,      // not exactly one named export, or it is code or forwarded
    load,         // LoadLibraryExW or dlopen failed
    descriptor,   // the exported descriptor failed validation (see PluginLoadResult::problem)
    api_level,    // the plugin needs a newer host
    permissions,  // the plugin needs permissions that were not approved
    release,      // FreeLibrary or dlclose failed; the image stays owned for a retry
    digest,       // the file is not the SHA-256 the caller expects
    imports,      // ELF: a needed library, an undefined symbol, or a symbol or indirect-function relocation
    relocations,  // ELF: a relocation other than relative ones inside the image
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

// Eight bytes, so a result returns in a register.
struct PluginLoadResult {
    PluginLoadError error;
    PluginDescriptorProblem problem;
    // descriptor errors: the 1-based setting, hook, command, menu, button or host index, else 0.
    u16 index;
    DWORD win32_error; // the system's error number: Win32 on Windows, errno on Linux
    constexpr bool ok() const noexcept { return error == PluginLoadError::none; }
};
static_assert(sizeof(PluginLoadResult) == 8);

// The plugins directory, pinned once with every ancestor (Windows: no reparse points, no replacement while open;
// Linux: each ancestor opened without following a link and held, owned by root or the user and not writable by
// others, and the directory itself the user's alone). Zero-initialized, caller-owned, stable address. On Linux the
// ancestors are descriptors plus one.
inline constexpr u32 kPluginPathCapacity = 1024;
inline constexpr u32 kPluginPathMaximumAncestors = 64;
struct PluginDirectory {
    HANDLE ancestors[kPluginPathMaximumAncestors];
    u32 ancestor_count;
    wchar_t path[kPluginPathCapacity];
    u32 path_characters;
#if !defined(_WIN32)
    char utf8[3 * kPluginPathCapacity];
    u32 utf8_bytes;
#endif
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
    u8 name_bytes, author_bytes, version_bytes;
    u16 summary_bytes;
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
#if !defined(_WIN32)
    // Linux: the descriptor of the sealed copy the plugin was loaded from, plus one; open until the plugin is unloaded.
    u32 sealed_token;
#endif
};

// Opens leaf (a file name in the pinned directory) without following links and checks it. The file's bytes are read
// once, hashed (sha256) and checked from memory: Windows checks the PE headers on them, holds the file open against
// writes and maps it; Linux moves them into a sealed memory file, checks the ELF headers, dynamic section and symbols
// on that copy and dlopen()s the copy, so what was hashed and checked is exactly what is mapped. Either way the
// descriptor is then validated. approved must contain every permission the descriptor needs; on any failure nothing
// stays loaded. The pin's directory on Linux must be the file's parent still (its final path is compared), the file
// regular, the user's and not writable by others.
PluginLoadResult plugin_image_load(PluginImage*, const PluginDirectory&, const ConstSlice<wchar_t>& leaf,
                                   u64 approved_permissions, const u8* sha256 = nullptr) noexcept;
// FreeLibrary or dlclose, and reset. The caller stopped the plugin and removed its hooks first.
PluginLoadResult plugin_image_unload(PluginImage*) noexcept;

// A keybind setting value: 0 (unbound), or a virtual-key code with modifier bits that cannot be typing (Ctrl or
// Alt held, or a function key).
bool plugin_keybind_valid(u32 key) noexcept;

} // namespace maicord
