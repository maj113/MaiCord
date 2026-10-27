#pragma once

#include "maicord/app/client_theme_document.hpp"
#include "maicord/app/client_preferences.hpp"
#include "maicord/platform/file_identity.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {
inline constexpr u32 kClientThemeStorageVersion = 1;
inline constexpr u32 kClientThemeStorageHeaderBytes = 96;
inline constexpr u32 kClientThemeImportMaximumBytes = 64u * 1024u;
inline constexpr u32 kClientThemeStorageMaximumBytes =
    kClientThemeStorageHeaderBytes + kClientThemeImportMaximumBytes;
inline constexpr u32 kClientThemeIoMaximumPathUnits = 1024;
struct ClientThemeStorageSnapshot {
    ClientThemeDocument custom;
    BackgroundTheme gradient;
    BackgroundEffectSettings effect_profiles[kBackgroundEffectProfileCount];
    ClientTheme builtin_theme;
    bool custom_present, custom_active;
};
enum class ClientThemeStorageError : u8 {
    none,
    invalid_input,
    path,
    directory,
    missing,
    open,
    size,
    read,
    format,
    version,
    checksum,
    document,
    changed,
    memory,
    write,
    flush,
    commit
};
struct ClientThemeStorageResult {
    u32 native_error{}, document_offset{}, unsupported_fields{};
    ClientThemeStorageError error{};
    bool ok() const noexcept {
        return error == ClientThemeStorageError::none;
    }
};
bool client_theme_storage_valid(const ClientThemeStorageSnapshot&) noexcept;
bool client_theme_storage_capture(ClientThemeStorageSnapshot*, const ClientPreferences&,
                                  const ClientThemeDocument&, bool custom_present,
                                  bool custom_active) noexcept;
// Only theme, gradient, and effect_profiles are authoritative in this file.
// Every notification, accessibility, navigation, and other preference is preserved.
bool client_theme_storage_apply_theme_fields(const ClientThemeStorageSnapshot&,
                                             ClientPreferences*) noexcept;
// Portable 96-byte envelope plus canonical JSON: explicit fields, zeroed reserved bytes, checksum.
// Missing/corrupt/invalid files and decode errors preserve caller output.
// Encode requires enough caller storage for the resolved record; failure preserves output/bytes.
ClientThemeStorageResult client_theme_storage_encode(const ClientThemeStorageSnapshot&, Slice<u8>,
                                                     u32* bytes) noexcept;
ClientThemeStorageResult client_theme_storage_decode(ConstSlice<u8>, Slice<u32> document_scratch,
                                                     ClientThemeStorageSnapshot*) noexcept;
// Blocking operations are worker/test boundaries, never frame/poll calls.
ClientThemeStorageResult client_theme_storage_load_file(const wchar_t*,
                                                        ClientThemeStorageSnapshot*) noexcept;
ClientThemeStorageResult client_theme_storage_save_file(const wchar_t*,
                                                        const ClientThemeStorageSnapshot&) noexcept;
// Resolve %LOCALAPPDATA%\MaiCord\themes.bin; directory creation only when requested.
ClientThemeStorageResult client_theme_storage_user_path(Slice<wchar_t>,
                                                        bool create_directory = false) noexcept;

// Installed themes: every *.json document in %LOCALAPPDATA%\MaiCord\themes.
// Invalid, oversized or unreadable files are skipped and counted.
inline constexpr u32 kClientThemeCatalogCapacity = 16;
inline constexpr u32 kClientThemeCatalogFileUnits = 260; // MAX_PATH, checked in client_theme_storage.cpp
struct ClientThemeCatalogEntry {
    char name[kClientThemeNameMaximumBytes + 1];
    u32 name_bytes;
    wchar_t file[kClientThemeCatalogFileUnits];
    u32 file_units;
    FileSnapshot identity;
    ClientThemeBase base;
};
struct ClientThemeCatalog {
    ClientThemeCatalogEntry entries[kClientThemeCatalogCapacity];
    u32 count, skipped;
};
ClientThemeStorageResult client_theme_catalog_directory(Slice<wchar_t>, bool create_directory = false) noexcept;
// Blocking worker/test boundary. Entries are sorted by file name; a missing
// directory is an empty catalog. Output is replaced only on success.
ClientThemeStorageResult client_theme_catalog_scan(const wchar_t* directory, ClientThemeCatalog*) noexcept;
// Full path of an entry inside directory; false when it does not fit.
bool client_theme_catalog_path(const wchar_t* directory, const ClientThemeCatalogEntry&, Slice<wchar_t>,
                               u32* units) noexcept;

enum class ClientThemeIoOperation : u8 {
    load,
    import_document,
    save,
    catalog
};
struct ClientThemeIoCompletion {
    ClientThemeStorageSnapshot snapshot;
    ClientThemeCatalog catalog;
    ClientThemeStorageResult result;
    u64 generation{};
    ClientThemeIoOperation operation{};
};
struct ClientThemeIo;
// Lazy thread owner. Null storage_path selects user-global themes.bin. Creation
// copies path and creates handles only; disk reads/writes/parser run on a
// worker that a submission starts and that exits once nothing is pending.
bool client_theme_io_create(ClientThemeIo**, const wchar_t* storage_path = nullptr) noexcept;
bool client_theme_io_submit_load(ClientThemeIo*, u64* generation) noexcept;
bool client_theme_io_submit_import(ClientThemeIo*, ConstSlice<wchar_t> path, const FileSnapshot&,
                                   const ClientThemeStorageSnapshot& baseline, u64* generation) noexcept;
bool client_theme_io_submit_save(ClientThemeIo*, const ClientThemeStorageSnapshot&,
                                 u64* generation) noexcept;
// Scans the installed theme directory (beside the storage file's directory).
bool client_theme_io_submit_catalog(ClientThemeIo*, u64* generation) noexcept;
HANDLE client_theme_io_completion_event(const ClientThemeIo*) noexcept;
// One active job plus replaceable latest request/completion; no growing queue.
// Success copies complete owning snapshot. ready=false leaves completion untouched.
bool client_theme_io_poll(ClientThemeIo*, ClientThemeIoCompletion*, bool* ready) noexcept;
// Cancels publication of this exact generation; cancels it if still queued.
// An admitted atomic write may finish and cannot be rolled back by cancellation.
void client_theme_io_cancel(ClientThemeIo*, u64 generation) noexcept;
// Stop drains accepted work then joins. Timeout/failure retains all resources
// and rejects future submissions; retry stop later. Joined success frees owner.
bool client_theme_io_stop(ClientThemeIo**, u32 timeout_ms) noexcept;
} // namespace maicord
