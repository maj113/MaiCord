#pragma once
#include "maicord/core/json_reader.hpp"

namespace maicord {
inline constexpr u32 kNativePluginManifestMaximumBytes = 4096;
inline constexpr u32 kNativePluginNameCapacity = 96;
inline constexpr u32 kNativePluginVersionCapacity = 32;
inline constexpr u32 kNativePluginDescriptionCapacity = 256;
inline constexpr u32 kNativePluginDllNameCapacity = 128;
inline constexpr u32 kNativePluginPublisherCapacity = 64;
// Manifest permission names in MC_PERM_* bit order (maicord_plugin.h).
inline constexpr u32 kNativePluginPermissionCount = 22;
inline constexpr u64 kNativePluginPermissionsAll = (1ull << kNativePluginPermissionCount) - 1;
enum class NativePluginManifestError : u8 {
    none, invalid_input, json, unknown_field, duplicate_field, missing_field,
    type, unsupported, text, dll_name, digest, capacity,
};
struct NativePluginManifestResult {
    NativePluginManifestError error;
    constexpr bool ok() const noexcept { return error == NativePluginManifestError::none; }
};
struct NativePluginManifest {
    u64 owner_id;
    u32 abi_version;
    char name[kNativePluginNameCapacity];
    char version[kNativePluginVersionCapacity];
    char description[kNativePluginDescriptionCapacity];
    char dll[kNativePluginDllNameCapacity];
    u32 name_bytes, version_bytes, description_bytes, dll_bytes;
    u8 sha256[32];
    u32 min_host_abi; // zero when the manifest does not name one
    u64 permissions;
    char publisher[kNativePluginPublisherCapacity];
    u32 publisher_bytes;
};
// "settings", "commands", ... for one MC_PERM_* bit index; empty when unknown.
Utf8View native_plugin_permission_name(u32 index) noexcept;
// The MC_PERM_* bit index of an exact manifest permission name, or
// kNativePluginPermissionCount when the name is unknown.
u32 native_plugin_permission_bit(Utf8View name) noexcept;
// Short sentence for the approval prompt and settings rows.
Utf8View native_plugin_permission_label(u32 index) noexcept;
// Requires exactly schema 3 and abi 5 with owner_id, name, version, description,
// dll, sha256 and a permissions array of names, plus optional min_host_abi and
// publisher. Integers use JSON numbers; sha256 is lowercase hexadecimal.
// Owned UTF-8 text excludes C0/C1 controls. No code is loaded.
// Output is unchanged on failure; document and output must not alias.
NativePluginManifestResult native_plugin_manifest_parse(Utf8View document,
    NativePluginManifest* output) noexcept;
// Also validates caller-created metadata before a declared module load.
bool native_plugin_manifest_valid(const NativePluginManifest& manifest) noexcept;
// A plugin DLL leaf: letters (any case), digits, '_', '-' and single dots, ending in ".dll"
// and not starting with a dot or naming a reserved device (CON, COM1, ...).
bool native_plugin_dll_name_valid(Utf8View name) noexcept;
// Case-insensitive DLL leaf equality.
bool native_plugin_manifest_same_dll(const NativePluginManifest& a, const NativePluginManifest& b) noexcept;
// One Windows path component naming an ordinary file or directory: nonempty,
// no leading or trailing dot, no trailing space, no reserved device name (CON,
// COM1, ...) and no control, delimiter, wildcard, '~' or surrogate unit.
bool native_plugin_path_component_valid(ConstSlice<wchar_t> part) noexcept;
// Canonical schema 3 JSON, preserving semantic UTF-8 metadata and the complete
// owner/ABI/digest identity. No terminating NUL is written. All outputs remain
// unchanged on invalid input, insufficient capacity, or range aliasing.
NativePluginManifestResult native_plugin_manifest_write(const NativePluginManifest&,
    Slice<char> output, u32* bytes) noexcept;
} // namespace maicord
