#pragma once
#include "maicord/app/plugin_manifest.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {
inline constexpr u32 kNativePluginCatalogMaximumEntries = 32;
inline constexpr u32 kNativePluginCatalogMaximumDirectoryEntries = 256;
inline constexpr u32 kNativePluginCatalogPathCapacity = 1024;
inline constexpr u32 kNativePluginCatalogFilenameCapacity = 128;
inline constexpr u64 kNativePluginDllMaximumBytes = 64u * 1024u * 1024u;
inline constexpr u32 kNativePluginPathMaximumAncestors = 64;
inline constexpr wchar_t kNativePluginManifestSuffix[] = L".maicord-plugin.json";
enum class NativePluginCatalogError : u8 {
    none, invalid_input, path, directory, scan, capacity, open, file, size,
    read, manifest, duplicate_owner, duplicate_dll, hash, digest, release,
};
struct NativePluginCatalogResult {
    NativePluginCatalogError error;
    DWORD win32_error;
    i32 crypto_status;
    constexpr bool ok() const noexcept { return error == NativePluginCatalogError::none; }
};
struct NativePluginCatalogEntry {
    NativePluginManifest manifest;
    wchar_t filename[kNativePluginCatalogFilenameCapacity];
    u32 filename_characters;
    NativePluginCatalogError status;
    NativePluginManifestError manifest_error;
    DWORD win32_error;
};
struct NativePluginVerifiedFile;
struct NativePluginCatalog {
    Slice<NativePluginCatalogEntry> entries;
    wchar_t directory[kNativePluginCatalogPathCapacity];
    u32 directory_characters;
    u32 count;
    NativePluginVerifiedFile* cleanup;
};
// Caller-owned, stable storage; explicit scans only. Drive-rooted canonical
// paths only, with no device namespace, dot/alias components or reparse points.
// Refresh reads only direct *.maicord-plugin.json files, never DLL code.
// Scan capacity/error clears count, so a truncated inventory cannot be enabled.
// Zero initialization is required. A transient cleanup handle is allocated before
// scan handles and freed normally; a failed release keeps it for retry.
NativePluginCatalogResult native_plugin_catalog_initialize(NativePluginCatalog* catalog,
    ConstSlice<wchar_t> absolute_directory, Slice<NativePluginCatalogEntry> storage) noexcept;
NativePluginCatalogResult native_plugin_catalog_refresh(NativePluginCatalog* catalog) noexcept;
NativePluginCatalogResult native_plugin_catalog_release(NativePluginCatalog* catalog) noexcept;

// Zero-initialized file gate. Handles deny writes/deletes and ancestor
// replacement through verification and DLL load/query/start. This checks content
// integrity against a locally declared hash; it does not authenticate a publisher.
// User trust/Enable remains a caller requirement. No load occurs here.
// On release failure, nonzero output still holds the handles for retry even
// though verify returned an error. The caller must release it before reusing storage.
struct NativePluginVerifiedFile {
    void* hash;
    void* algorithm;
    HANDLE scan;
    HANDLE file;
    HANDLE ancestors[kNativePluginPathMaximumAncestors];
    u32 ancestor_count;
    wchar_t path[kNativePluginCatalogPathCapacity];
    u32 path_characters;
    u64 bytes;
};
NativePluginCatalogResult native_plugin_declared_file_verify(ConstSlice<wchar_t> absolute_directory,
    const NativePluginManifest& manifest, NativePluginVerifiedFile* output) noexcept;
NativePluginCatalogResult native_plugin_verified_file_release(NativePluginVerifiedFile* file) noexcept;
// Hash an already pinned read handle from offset zero with the same
// walker used by declared loading. Failed crypto cleanup remains in this gate.
NativePluginCatalogResult native_plugin_verified_file_hash(NativePluginVerifiedFile*, ConstSlice<u8> expected_sha256) noexcept;
// Shared hardened filesystem gates for local bundle installation. Directory
// pins hold all ancestors. create_tail is zero normally, or <=2 requested
// final components beneath existing pinned ancestors. Failed calls keep
// acquired handles in output until release succeeds.
NativePluginCatalogResult native_plugin_directory_pin(ConstSlice<wchar_t>,
    NativePluginVerifiedFile*, u32 create_tail = 0) noexcept;
// The <Windows.h> CreateFileW values of the defaults below (checked in plugin_catalog.cpp).
inline constexpr DWORD kNativePluginFileRead = 0x80000000;           // GENERIC_READ
inline constexpr DWORD kNativePluginFileShareRead = 0x00000001;      // FILE_SHARE_READ
inline constexpr DWORD kNativePluginFileOpenExisting = 3;            // OPEN_EXISTING
inline constexpr DWORD kNativePluginFileSequentialScan = 0x08000000; // FILE_FLAG_SEQUENTIAL_SCAN
// File gates borrow the directory pin's lifetime, own their file/crypto handles,
// and never overwrite existing files: only OPEN_EXISTING or CREATE_NEW. Release
// every file gate before its directory pin. Existing files cannot request write.
NativePluginCatalogResult native_plugin_pinned_file_open(const NativePluginVerifiedFile& directory,
    ConstSlice<wchar_t> basename, NativePluginVerifiedFile* output,
    DWORD access = kNativePluginFileRead, DWORD sharing = kNativePluginFileShareRead,
    DWORD creation = kNativePluginFileOpenExisting, DWORD flags = kNativePluginFileSequentialScan) noexcept;
// READ|DELETE sharing is reserved for update-target identity pins;
// ordinary reads use READ-only sharing. Write sharing is never allowed.
NativePluginCatalogResult native_plugin_pinned_manifest_read(const NativePluginVerifiedFile& directory,
    ConstSlice<wchar_t> basename, NativePluginManifest* manifest, NativePluginVerifiedFile* output,
    DWORD sharing=kNativePluginFileShareRead) noexcept;
NativePluginCatalogResult native_plugin_pinned_dll_verify(const NativePluginVerifiedFile& directory,
    const NativePluginManifest& manifest, NativePluginVerifiedFile* output) noexcept;
} // namespace maicord
