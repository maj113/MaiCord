#pragma once
#include "maicord/app/plugin_manifest.hpp"
#include <stddef.h>
#include "maicord/core/win32_types.hpp"

namespace maicord {
inline constexpr u32 kNativePluginPreferencesMaximumEntries = 8;
inline constexpr u32 kNativePluginPreferencesVersion = 2;
inline constexpr u32 kNativePluginPreferencesEntryBytes = 184;
inline constexpr u32 kNativePluginPreferencesRecordBytes = 32 +
    kNativePluginPreferencesMaximumEntries * kNativePluginPreferencesEntryBytes;
// Why a remembered plugin stays off until the user enables it again.
inline constexpr u32 native_plugin_preference_crashed = 1u << 0;
inline constexpr u32 native_plugin_preference_overran = 1u << 1;
inline constexpr u32 native_plugin_preference_known_flags =
    native_plugin_preference_crashed | native_plugin_preference_overran;

// Manifest identity plus the user's choices for it. DLL leaves use the
// manifest's ASCII grammar, folded to lowercase. Names/descriptions/versions are
// presentation metadata. A changed owner, ABI, DLL leaf or SHA256 always needs
// the user to Enable it again. Entries record the approved MC_PERM_* set.
struct NativePluginPreference {
    u64 owner_id;
    u32 abi_version;
    u32 dll_bytes;
    u8 sha256[32];
    char dll[kNativePluginDllNameCapacity];
    u32 permissions;
    u32 flags;
};
inline constexpr u32 kNativePluginPreferenceIdentityBytes = 176;
struct NativePluginPreferences {
    NativePluginPreference entries[kNativePluginPreferencesMaximumEntries];
    u32 count;
};
struct NativePluginPreferencesCleanup;
// Zero-initialized, stable-address, caller-owned. UI-thread only. IO allocates one
// transient record/handle block of at most 4KiB. A close/delete/free failure keeps
// it for retry; do not copy, reuse or destroy this object until release succeeds.
struct NativePluginPreferencesIo { NativePluginPreferencesCleanup* cleanup; };
enum class NativePluginPreferencesError : u8 {
    none, invalid_input, capacity, path, directory, open, file, size, read,
    format, version, checksum, allocation, write, flush, commit, release, busy,
};
struct NativePluginPreferencesResult {
    NativePluginPreferencesError error;
    DWORD win32_error;
    bool missing;
    // True once the destination has been atomically replaced, including when
    // subsequent flush/release fails. Use the saved candidate in that case.
    bool committed;
    constexpr bool ok() const noexcept { return error == NativePluginPreferencesError::none; }
};
bool native_plugin_preferences_valid(const NativePluginPreferences& preferences) noexcept;
bool native_plugin_preferences_has(const NativePluginPreferences& preferences,
    const NativePluginManifest& manifest) noexcept;
// The entry whose identity matches manifest, or null.
const NativePluginPreference* native_plugin_preferences_find(const NativePluginPreferences& preferences,
    const NativePluginManifest& manifest) noexcept;
// Startup restore: identity matches, no off flag, and the manifest asks
// for nothing beyond what the user approved.
bool native_plugin_preferences_restorable(const NativePluginPreferences& preferences,
    const NativePluginManifest& manifest) noexcept;
// Marks an owner as turned off (crash or time budget). False when absent.
bool native_plugin_preferences_set_flags(NativePluginPreferences* preferences, u64 owner_id, u32 flags) noexcept;
// Call only after a successful explicit trusted catalog Enable. Owner updates
// replace the old digest; conflicting DLL leaves and a ninth owner are refused.
// No function here loads code. A DLL picked with Choose DLL is never persisted as trusted.
// Enabling clears off flags and records the approved permissions.
NativePluginPreferencesResult native_plugin_preferences_enable(NativePluginPreferences* preferences,
    const NativePluginManifest& manifest, u64 permissions = 0) noexcept;
// Removing an owner removes its trust even if the current manifest changed.
bool native_plugin_preferences_remove_owner(NativePluginPreferences* preferences, u64 owner_id) noexcept;
// Exact 1504-byte portable encoding: 8-byte magic, LE version/size/count,
// zero reserved u32, LE checksum, eight zero-padded 184-byte entries containing
// LE owner/ABI/DLL length, 32 SHA256 bytes, 128 DLL bytes, LE permissions and
// LE flags. Unused slots are zero. Any other size or version is rejected.
// Checksum detects accidents; it does not authenticate this user's trust file.
// Duplicate owners/leaves, unsupported ABI, reserved bytes and trailing data
// fail closed. Encode/decode preserve output on failure and refuse overlap.
NativePluginPreferencesResult native_plugin_preferences_encode(const NativePluginPreferences& preferences,
    Slice<u8> record) noexcept;
NativePluginPreferencesResult native_plugin_preferences_decode(ConstSlice<u8> record,
    NativePluginPreferences* preferences) noexcept;
// Canonical drive-rooted paths only, max 1003 characters, no dot/DOS/device/
// short-name aliases, reparse points or >64 ancestors. All ancestors are pinned
// root-to-leaf. Saves create a new sibling exclusively, flush it, then rename
// its handle over the destination. Unique .new+16-hex siblings use up
// to sixteen CREATE_NEW attempts; no preexisting sibling is touched.
// Load clears choices before IO: missing, or a file of another format version
// (an older MaiCord's), is success+missing; corruption/error
// never leaves autoload trust. Save never mutates caller choices; callers
// may save a candidate and apply it only when save succeeds. A release error
// after commit can mean the record was committed; retry release, then reload.
NativePluginPreferencesResult native_plugin_preferences_load_file(NativePluginPreferencesIo* io,
    const wchar_t* path, NativePluginPreferences* preferences) noexcept;
NativePluginPreferencesResult native_plugin_preferences_save_file(NativePluginPreferencesIo* io,
    const wchar_t* path, const NativePluginPreferences& preferences) noexcept;
// %LOCALAPPDATA%\MaiCord\plugins.bin, startup/edit only. Save creates only the
// final MaiCord directory under the pinned existing LOCALAPPDATA ancestry.
NativePluginPreferencesResult native_plugin_preferences_load(NativePluginPreferencesIo* io,
    NativePluginPreferences* preferences) noexcept;
NativePluginPreferencesResult native_plugin_preferences_save(NativePluginPreferencesIo* io,
    const NativePluginPreferences& preferences) noexcept;
NativePluginPreferencesResult native_plugin_preferences_release(NativePluginPreferencesIo* io) noexcept;
static_assert(sizeof(NativePluginPreference) == kNativePluginPreferencesEntryBytes);
static_assert(offsetof(NativePluginPreference, permissions) == kNativePluginPreferenceIdentityBytes);
static_assert(sizeof(NativePluginPreferences) == 1480);
} // namespace maicord
