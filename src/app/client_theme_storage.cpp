#include "maicord/app/client_theme_storage.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/win32_process.hpp"

#include <Windows.h>

namespace maicord {
static_assert(kClientThemeCatalogFileUnits == MAX_PATH);
namespace {
constexpr u8 kMagic[8]{'M', 'A', 'I', 'C', 'T', 'H', 'M', 'E'};
constexpr u32 kChecksumOffset = 80;
struct Workspace {
    u8 record[kClientThemeStorageMaximumBytes];
    u32 structurals[kClientThemeImportMaximumBytes];
};
ClientThemeStorageResult failure(ClientThemeStorageError error, u32 native = 0) noexcept {
    return {native, 0, 0, error};
}
ClientThemeStorageResult document_failure(ClientThemeDocumentResult result) noexcept {
    return {0, result.error_offset, result.unsupported_fields, ClientThemeStorageError::document};
}
bool span(const void* p, u64 bytes) noexcept {
    return (!bytes || p) && bytes <= ~usize{0} - reinterpret_cast<usize>(p);
}
bool overlap(const void* a, u64 an, const void* b, u64 bn) noexcept {
    if (!an || !bn)
        return false;
    const auto av = reinterpret_cast<usize>(a), bv = reinterpret_cast<usize>(b);
    return !span(a, an) || !span(b, bn) || (av < bv + bn && bv < av + an);
}
bool path_valid(ConstSlice<wchar_t> path) noexcept {
    if (!path.data || !path.count || path.count >= kClientThemeIoMaximumPathUnits - 4)
        return false;
    const bool drive = path.count >= 3 &&
                       ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
                       path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    const bool unc =
        path.count >= 5 && path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' && path[2] != L'.';
    if (!drive && !unc)
        return false;
    for (u32 i = 0; i < path.count; ++i)
        if (path[i] < 32 || path[i] == L'"' || path[i] == L'<' || path[i] == L'>' || path[i] == L'|' ||
            path[i] == L'*' || path[i] == L'?' || (path[i] == L':' && i != 1))
            return false;
    return true;
}
bool path_length(const wchar_t* path, u32* length) noexcept {
    *length = wide_length(path, kClientThemeIoMaximumPathUnits);
    return path_valid({path, *length});
}
u64 checksum(ConstSlice<u8> record) noexcept {
    return hash64_bytes(hash64_bytes(kHash64Seed, record.data, kChecksumOffset),
                        record.data + kChecksumOffset + 8, record.count - kChecksumOffset - 8);
}
void gradient_write(u8* record, const BackgroundTheme& value) noexcept {
    for (u32 i = 0; i < 5; ++i)
        byte_store_le32(record + 20 + i * 4, value.colors[i]);
    record[40] = static_cast<u8>(value.angle_degrees);
    record[41] = static_cast<u8>(value.angle_degrees >> 8);
    record[42] = value.color_count;
    record[43] = value.intensity;
    record[44] = static_cast<u8>(value.effect);
    record[45] = value.speed;
    record[46] = value.strength;
    record[47] = value.density;
}
bool reserved_zero(ConstSlice<u8> record, u32 start, u32 end) noexcept {
    for (u32 i = start; i < end; ++i)
        if (record[i])
            return false;
    return true;
}
ClientThemeStorageResult write_result(FilePersistenceWriteResult result) noexcept {
    switch (result.error) {
    case FilePersistenceWriteError::none:
        return {};
    case FilePersistenceWriteError::open:
        return failure(ClientThemeStorageError::open, result.native_error);
    case FilePersistenceWriteError::write:
        return failure(ClientThemeStorageError::write, result.native_error);
    case FilePersistenceWriteError::flush:
        return failure(ClientThemeStorageError::flush, result.native_error);
    case FilePersistenceWriteError::commit:
        return failure(ClientThemeStorageError::commit, result.native_error);
    }
    return failure(ClientThemeStorageError::write, result.native_error);
}
HANDLE open_read(const wchar_t* path, ClientThemeStorageResult* result) noexcept {
    const auto file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const u32 native = thread_last_error();
        *result = failure(native == ERROR_FILE_NOT_FOUND || native == ERROR_PATH_NOT_FOUND
                              ? ClientThemeStorageError::missing
                              : ClientThemeStorageError::open,
                          native);
    }
    return file;
}
ClientThemeStorageResult read_bytes(HANDLE file, Slice<u8> buffer, const FileSnapshot* expected,
                                    u32* bytes) noexcept {
    FileSnapshot before{};
    u32 native = 0;
    if (!file_snapshot_read(file, &before, &native))
        return failure(ClientThemeStorageError::read, native);
    if (expected &&
        (!file_identity_equal(before.identity, expected->identity) || before.bytes != expected->bytes))
        return failure(ClientThemeStorageError::changed);
    if (!before.bytes || before.bytes > buffer.count)
        return failure(ClientThemeStorageError::size);
    if (!file_persistence_read_complete(file, {buffer.data, static_cast<u32>(before.bytes)}, &native))
        return failure(ClientThemeStorageError::read, native);
    FileSnapshot after{};
    if (!file_snapshot_read(file, &after, &native))
        return failure(ClientThemeStorageError::read, native);
    if (!file_identity_equal(before.identity, after.identity) || before.bytes != after.bytes)
        return failure(ClientThemeStorageError::changed);
    *bytes = static_cast<u32>(before.bytes);
    return {};
}
ClientThemeStorageResult import_file(const wchar_t* path, const FileSnapshot& expected,
                                     const ClientThemeStorageSnapshot& baseline,
                                     ClientThemeStorageSnapshot* output) noexcept {
    ClientThemeStorageResult result{};
    const HANDLE file = open_read(path, &result);
    if (file == INVALID_HANDLE_VALUE)
        return result;
    auto* workspace = static_cast<Workspace*>(
        VirtualAlloc(nullptr, sizeof(Workspace), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!workspace) {
        CloseHandle(file);
        return failure(ClientThemeStorageError::memory, ERROR_NOT_ENOUGH_MEMORY);
    }
    u32 bytes = 0;
    result = read_bytes(file, {workspace->record, kClientThemeImportMaximumBytes}, &expected, &bytes);
    CloseHandle(file);
    if (result.ok()) {
        auto candidate = baseline;
        const auto defaults = client_theme_document_defaults();
        const auto parsed = client_theme_document_parse(
            {reinterpret_cast<const char*>(workspace->record), bytes}, defaults,
            {workspace->structurals, kClientThemeImportMaximumBytes}, &candidate.custom);
        if (parsed.ok()) {
            candidate.custom_present = true;
            candidate.custom_active = false;
            *output = candidate;
            result.unsupported_fields = parsed.unsupported_fields;
        } else
            result = document_failure(parsed);
    }
    VirtualFree(workspace, 0, MEM_RELEASE);
    return result;
}
} // namespace
bool client_theme_storage_valid(const ClientThemeStorageSnapshot& value) noexcept {
    if (static_cast<u8>(value.builtin_theme) > static_cast<u8>(ClientTheme::custom_light_gradient) ||
        !background_theme_valid(value.gradient) || (value.custom_active && !value.custom_present) ||
        (value.custom_present && !client_theme_document_valid(value.custom)))
        return false;
    for (const auto profile : value.effect_profiles)
        if (!background_effect_settings_valid(profile))
            return false;
    return true;
}
bool client_theme_storage_capture(ClientThemeStorageSnapshot* output, const ClientPreferences& preferences,
                                  const ClientThemeDocument& document, bool present, bool active) noexcept {
    if (!output)
        return false;
    ClientThemeStorageSnapshot candidate{};
    candidate.custom = document;
    candidate.gradient = preferences.gradient;
    candidate.builtin_theme = preferences.theme;
    candidate.custom_present = present;
    candidate.custom_active = active;
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i)
        candidate.effect_profiles[i] = preferences.effect_profiles[i];
    if (!client_theme_storage_valid(candidate))
        return false;
    *output = candidate;
    return true;
}
bool client_theme_storage_apply_theme_fields(const ClientThemeStorageSnapshot& snapshot,
                                             ClientPreferences* output) noexcept {
    if (!output || !client_theme_storage_valid(snapshot))
        return false;
    output->theme = snapshot.builtin_theme;
    output->gradient = snapshot.gradient;
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i)
        output->effect_profiles[i] = snapshot.effect_profiles[i];
    return true;
}
ClientThemeStorageResult client_theme_storage_encode(const ClientThemeStorageSnapshot& snapshot,
                                                     Slice<u8> record, u32* bytes) noexcept {
    if (!bytes || !span(record.data, record.count) || !client_theme_storage_valid(snapshot) ||
        overlap(record.data, record.count, &snapshot, sizeof(snapshot)) ||
        overlap(record.data, record.count, bytes, sizeof(*bytes)) ||
        overlap(bytes, sizeof(*bytes), &snapshot, sizeof(snapshot)))
        return failure(ClientThemeStorageError::invalid_input);
    u32 document_bytes = 0;
    if (snapshot.custom_present) {
        const auto sized = client_theme_document_write(snapshot.custom, {}, &document_bytes);
        if (!sized.ok())
            return document_failure(sized);
        if (!document_bytes || document_bytes > kClientThemeImportMaximumBytes)
            return failure(ClientThemeStorageError::size);
    }
    const u32 total = kClientThemeStorageHeaderBytes + document_bytes;
    if (record.count < total)
        return failure(ClientThemeStorageError::size);
    if (document_bytes) {
        const auto written = client_theme_document_write(
            snapshot.custom,
            {reinterpret_cast<char*>(record.data + kClientThemeStorageHeaderBytes), document_bytes},
            &document_bytes);
        if (!written.ok())
            return document_failure(written);
    }
    u8 header[kClientThemeStorageHeaderBytes]{};
    copy_bytes(header, kMagic, sizeof(kMagic));
    byte_store_le32(header + 8, kClientThemeStorageVersion);
    byte_store_le32(header + 12, total);
    header[16] = static_cast<u8>((snapshot.custom_present ? 1 : 0) | (snapshot.custom_active ? 2 : 0));
    header[17] = static_cast<u8>(snapshot.builtin_theme);
    gradient_write(header, snapshot.gradient);
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i) {
        header[48 + i * 3] = snapshot.effect_profiles[i].speed;
        header[49 + i * 3] = snapshot.effect_profiles[i].strength;
        header[50 + i * 3] = snapshot.effect_profiles[i].density;
    }
    byte_store_le32(header + 68, document_bytes);
    copy_bytes(record.data, header, sizeof(header));
    byte_store_le64(record.data + kChecksumOffset, checksum({record.data, total}));
    *bytes = total;
    return {};
}
ClientThemeStorageResult client_theme_storage_decode(ConstSlice<u8> record, Slice<u32> scratch,
                                                     ClientThemeStorageSnapshot* output) noexcept {
    const u64 scratch_bytes = u64{scratch.count} * sizeof(u32);
    if (!output || !span(record.data, record.count) || !span(scratch.data, scratch_bytes) ||
        overlap(output, sizeof(*output), record.data, record.count) ||
        overlap(output, sizeof(*output), scratch.data, scratch_bytes) ||
        overlap(record.data, record.count, scratch.data, scratch_bytes))
        return failure(ClientThemeStorageError::invalid_input);
    if (record.count < kClientThemeStorageHeaderBytes || record.count > kClientThemeStorageMaximumBytes)
        return failure(ClientThemeStorageError::size);
    if (!bytes_equal(record.data, kMagic, sizeof(kMagic)) ||
        byte_load_le32(record.data + 12) != record.count || (record[16] & ~3u) ||
        !reserved_zero(record, 18, 20) || !reserved_zero(record, 66, 68) ||
        !reserved_zero(record, 72, 80) || !reserved_zero(record, 88, 96))
        return failure(ClientThemeStorageError::format);
    if (byte_load_le32(record.data + 8) != kClientThemeStorageVersion)
        return failure(ClientThemeStorageError::version);
    if (byte_load_le64(record.data + kChecksumOffset) != checksum(record))
        return failure(ClientThemeStorageError::checksum);
    const u32 json_bytes = byte_load_le32(record.data + 68);
    if (json_bytes != record.count - kClientThemeStorageHeaderBytes ||
        ((record[16] & 1) != 0) != (json_bytes != 0))
        return failure(ClientThemeStorageError::format);
    ClientThemeStorageSnapshot candidate{};
    candidate.custom = client_theme_document_defaults();
    candidate.custom_present = (record[16] & 1) != 0;
    candidate.custom_active = (record[16] & 2) != 0;
    candidate.builtin_theme = static_cast<ClientTheme>(record[17]);
    for (u32 i = 0; i < 5; ++i)
        candidate.gradient.colors[i] = byte_load_le32(record.data + 20 + i * 4);
    candidate.gradient.angle_degrees = byte_load_le16(record.data + 40);
    candidate.gradient.color_count = record[42];
    candidate.gradient.intensity = record[43];
    if (background_effect_known(record[44])) {
        candidate.gradient.effect = static_cast<BackgroundEffect>(record[44]);
        candidate.gradient.speed = record[45];
        candidate.gradient.strength = record[46];
        candidate.gradient.density = record[47];
    } else {
        // A removed effect restores "none" with its default tuning.
        const auto defaults = background_effect_settings_defaults(BackgroundEffect::none);
        candidate.gradient.effect = BackgroundEffect::none;
        candidate.gradient.speed = defaults.speed;
        candidate.gradient.strength = defaults.strength;
        candidate.gradient.density = defaults.density;
    }
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i)
        candidate.effect_profiles[i] = {record[48 + i * 3], record[49 + i * 3], record[50 + i * 3]};
    ClientThemeStorageResult result{};
    if (json_bytes) {
        const auto parsed = client_theme_document_parse(
            {reinterpret_cast<const char*>(record.data + kClientThemeStorageHeaderBytes), json_bytes},
            candidate.custom, scratch, &candidate.custom);
        if (!parsed.ok())
            return document_failure(parsed);
        result.unsupported_fields = parsed.unsupported_fields;
    }
    if (!client_theme_storage_valid(candidate))
        return failure(ClientThemeStorageError::format);
    *output = candidate;
    return result;
}
ClientThemeStorageResult client_theme_storage_load_file(const wchar_t* path,
                                                        ClientThemeStorageSnapshot* output) noexcept {
    u32 length = 0;
    if (!output || !path_length(path, &length))
        return failure(ClientThemeStorageError::invalid_input);
    ClientThemeStorageResult result{};
    const HANDLE file = open_read(path, &result);
    if (file == INVALID_HANDLE_VALUE)
        return result;
    auto* workspace = static_cast<Workspace*>(
        VirtualAlloc(nullptr, sizeof(Workspace), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!workspace) {
        CloseHandle(file);
        return failure(ClientThemeStorageError::memory, ERROR_NOT_ENOUGH_MEMORY);
    }
    u32 bytes = 0;
    result = read_bytes(file, {workspace->record, kClientThemeStorageMaximumBytes}, nullptr, &bytes);
    CloseHandle(file);
    if (result.ok())
        result = client_theme_storage_decode(
            {workspace->record, bytes}, {workspace->structurals, kClientThemeImportMaximumBytes}, output);
    VirtualFree(workspace, 0, MEM_RELEASE);
    return result;
}
ClientThemeStorageResult client_theme_storage_save_file(
    const wchar_t* path, const ClientThemeStorageSnapshot& snapshot) noexcept {
    u32 length = 0;
    if (!path_length(path, &length) || !client_theme_storage_valid(snapshot))
        return failure(ClientThemeStorageError::invalid_input);
    auto* record = static_cast<u8*>(
        VirtualAlloc(nullptr, kClientThemeStorageMaximumBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!record)
        return failure(ClientThemeStorageError::memory, ERROR_NOT_ENOUGH_MEMORY);
    u32 bytes = 0;
    auto result = client_theme_storage_encode(snapshot, {record, kClientThemeStorageMaximumBytes}, &bytes);
    if (result.ok())
        result = write_result(file_persistence_write_atomic(path, length, {record, bytes}));
    VirtualFree(record, 0, MEM_RELEASE);
    return result;
}
ClientThemeStorageResult client_theme_storage_user_path(Slice<wchar_t> output,
                                                        bool create_directory) noexcept {
    if (!output.data || output.count < kClientThemeIoMaximumPathUnits)
        return failure(ClientThemeStorageError::invalid_input);
    // Leaves path_valid()'s four spare units.
    const DataPathResult built =
        data_path(output.data, kClientThemeIoMaximumPathUnits - 4, L"themes.bin", create_directory);
    if (!built.ok())
        return failure(built.error == DataPathError::directory ? ClientThemeStorageError::directory
                                                               : ClientThemeStorageError::path,
                       built.native_error);
    return {};
}

namespace {
// <directory of storage file>\themes
bool catalog_directory_beside(const wchar_t* storage, wchar_t* output) noexcept {
    u32 length = 0;
    if (!path_length(storage, &length))
        return false;
    while (length && storage[length - 1] != L'\\' && storage[length - 1] != L'/')
        --length;
    constexpr wchar_t themes[] = L"themes";
    constexpr u32 units = sizeof(themes) / sizeof(wchar_t);
    if (length < 3 || length + units >= kClientThemeIoMaximumPathUnits - kClientThemeCatalogFileUnits)
        return false;
    copy_bytes(output, storage, length * sizeof(wchar_t));
    copy_bytes(output + length, themes, sizeof(themes));
    return true;
}
bool wide_less(const wchar_t* a, u32 an, const wchar_t* b, u32 bn) noexcept {
    for (u32 i = 0; i < an && i < bn; ++i) {
        const wchar_t x = a[i] >= L'A' && a[i] <= L'Z' ? a[i] + 32 : a[i];
        const wchar_t y = b[i] >= L'A' && b[i] <= L'Z' ? b[i] + 32 : b[i];
        if (x != y)
            return x < y;
    }
    return an < bn;
}
} // namespace
ClientThemeStorageResult client_theme_catalog_directory(Slice<wchar_t> output, bool create_directory) noexcept {
    if (!output.data || output.count < kClientThemeIoMaximumPathUnits)
        return failure(ClientThemeStorageError::invalid_input);
    wchar_t storage[kClientThemeIoMaximumPathUnits]{};
    auto result = client_theme_storage_user_path({storage, kClientThemeIoMaximumPathUnits}, create_directory);
    if (!result.ok())
        return result;
    wchar_t directory[kClientThemeIoMaximumPathUnits]{};
    if (!catalog_directory_beside(storage, directory))
        return failure(ClientThemeStorageError::path, ERROR_BAD_PATHNAME);
    u32 native_error = 0;
    if (create_directory && !directory_ensure(directory, &native_error))
        return failure(ClientThemeStorageError::directory, native_error);
    copy_bytes(output.data, directory, sizeof(directory));
    return {};
}
bool client_theme_catalog_path(const wchar_t* directory, const ClientThemeCatalogEntry& entry,
                               Slice<wchar_t> output, u32* units) noexcept {
    u32 length = 0;
    if (!units || !output.data || !path_length(directory, &length) || !entry.file_units ||
        entry.file_units >= kClientThemeCatalogFileUnits || length + 1 + entry.file_units >= output.count)
        return false;
    copy_bytes(output.data, directory, length * sizeof(wchar_t));
    output.data[length] = L'\\';
    copy_bytes(output.data + length + 1, entry.file, entry.file_units * sizeof(wchar_t));
    *units = length + 1 + entry.file_units;
    output.data[*units] = 0;
    return path_valid({output.data, *units});
}
ClientThemeStorageResult client_theme_catalog_scan(const wchar_t* directory, ClientThemeCatalog* output) noexcept {
    u32 length = 0;
    if (!output || !path_length(directory, &length) ||
        length + 8 + kClientThemeCatalogFileUnits >= kClientThemeIoMaximumPathUnits)
        return failure(ClientThemeStorageError::invalid_input);
    auto* workspace = static_cast<Workspace*>(
        VirtualAlloc(nullptr, sizeof(Workspace) + sizeof(ClientThemeCatalog), MEM_RESERVE | MEM_COMMIT,
                     PAGE_READWRITE));
    if (!workspace)
        return failure(ClientThemeStorageError::memory, ERROR_NOT_ENOUGH_MEMORY);
    auto& catalog = *reinterpret_cast<ClientThemeCatalog*>(workspace + 1);
    wchar_t pattern[kClientThemeIoMaximumPathUnits]{};
    copy_bytes(pattern, directory, length * sizeof(wchar_t));
    copy_bytes(pattern + length, L"\\*.json", 8 * sizeof(wchar_t));
    WIN32_FIND_DATAW data{};
    const HANDLE scan = FindFirstFileW(pattern, &data);
    ClientThemeStorageResult result{};
    if (scan == INVALID_HANDLE_VALUE) {
        const u32 error = thread_last_error();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            result = failure(ClientThemeStorageError::directory, error);
    } else {
        const auto defaults = client_theme_document_defaults();
        do {
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            ClientThemeCatalogEntry entry{};
            while (entry.file_units < kClientThemeCatalogFileUnits && data.cFileName[entry.file_units])
                ++entry.file_units;
            wchar_t path[kClientThemeIoMaximumPathUnits]{};
            u32 units = 0;
            if (entry.file_units >= kClientThemeCatalogFileUnits || catalog.count == kClientThemeCatalogCapacity) {
                ++catalog.skipped;
                continue;
            }
            copy_bytes(entry.file, data.cFileName, entry.file_units * sizeof(wchar_t));
            if (!client_theme_catalog_path(directory, entry, {path, kClientThemeIoMaximumPathUnits}, &units)) {
                ++catalog.skipped;
                continue;
            }
            ClientThemeStorageResult read{};
            const HANDLE file = open_read(path, &read);
            u32 bytes = 0;
            u32 native = 0;
            bool parsed = false;
            if (file != INVALID_HANDLE_VALUE) {
                read = read_bytes(file, {workspace->record, kClientThemeImportMaximumBytes}, nullptr, &bytes);
                parsed = read.ok() && file_snapshot_read(file, &entry.identity, &native);
                CloseHandle(file);
            }
            ClientThemeDocument document{};
            if (parsed)
                parsed = client_theme_document_parse({reinterpret_cast<const char*>(workspace->record), bytes},
                                                     defaults, {workspace->structurals, kClientThemeImportMaximumBytes},
                                                     &document)
                             .ok();
            if (!parsed) {
                ++catalog.skipped;
                continue;
            }
            copy_bytes(entry.name, document.name, document.name_bytes + 1);
            entry.name_bytes = document.name_bytes;
            if (bytes_equal(document.name, defaults.name, defaults.name_bytes + 1)) {
                // Unnamed documents are listed by their file name without ".json".
                const int converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry.file,
                                                          static_cast<int>(entry.file_units - 5), entry.name,
                                                          kClientThemeNameMaximumBytes, nullptr, nullptr);
                if (converted > 0) {
                    entry.name_bytes = static_cast<u32>(converted);
                    entry.name[entry.name_bytes] = 0;
                }
            }
            entry.base = document.base;
            u32 at = catalog.count++;
            while (at && wide_less(entry.file, entry.file_units, catalog.entries[at - 1].file,
                                   catalog.entries[at - 1].file_units)) {
                catalog.entries[at] = catalog.entries[at - 1];
                --at;
            }
            catalog.entries[at] = entry;
        } while (FindNextFileW(scan, &data));
        const u32 error = thread_last_error();
        FindClose(scan);
        if (error != ERROR_NO_MORE_FILES)
            result = failure(ClientThemeStorageError::read, error);
    }
    if (result.ok())
        copy_bytes(output, &catalog, sizeof(catalog));
    VirtualFree(workspace, 0, MEM_RELEASE);
    return result;
}

struct ClientThemeIoRequest {
    ClientThemeStorageSnapshot snapshot;
    FileSnapshot expected;
    wchar_t path[kClientThemeIoMaximumPathUnits];
    u64 generation;
    ClientThemeIoOperation operation;
};
// The worker starts with a submission and exits once nothing is pending, so
// no thread (or its stack) stays parked between the rare theme loads/saves.
struct ClientThemeIo {
    SRWLOCK lock;
    HANDLE completion_event, thread; // thread: the current or last worker
    ClientThemeIoRequest pending;
    ClientThemeIoCompletion completion;
    wchar_t storage_path[kClientThemeIoMaximumPathUnits];
    u64 next_generation, active_generation;
    bool pending_ready, completion_ready, active_cancelled, stopping, explicit_path, running;
};
namespace {
bool reject(u32 error) noexcept {
    SetLastError(error);
    return false;
}
void release_io(ClientThemeIo* io) noexcept {
    if (io->thread)
        CloseHandle(io->thread);
    if (io->completion_event)
        CloseHandle(io->completion_event);
    HeapFree(process_heap(), 0, io);
}
DWORD WINAPI io_work(void* opaque) noexcept {
    auto* io = static_cast<ClientThemeIo*>(opaque);
    for (;;) {
        ClientThemeIoRequest request{};
        AcquireSRWLockExclusive(&io->lock);
        if (!io->pending_ready) {
            io->running = false; // the next submission starts a new worker
            ReleaseSRWLockExclusive(&io->lock);
            return 0;
        }
        request = io->pending;
        io->pending_ready = false;
        io->active_generation = request.generation;
        io->active_cancelled = false;
        ReleaseSRWLockExclusive(&io->lock);
        ClientThemeIoCompletion completed{};
        completed.generation = request.generation;
        completed.operation = request.operation;
        if (request.operation == ClientThemeIoOperation::import_document)
            completed.result =
                import_file(request.path, request.expected, request.snapshot, &completed.snapshot);
        else {
            wchar_t path[kClientThemeIoMaximumPathUnits]{};
            if (io->explicit_path)
                copy_bytes(path, io->storage_path, sizeof(path));
            else
                completed.result =
                    client_theme_storage_user_path({path, kClientThemeIoMaximumPathUnits},
                                                   request.operation == ClientThemeIoOperation::save);
            if (completed.result.ok() && request.operation == ClientThemeIoOperation::catalog) {
                wchar_t directory[kClientThemeIoMaximumPathUnits]{};
                completed.result = catalog_directory_beside(path, directory)
                                       ? client_theme_catalog_scan(directory, &completed.catalog)
                                       : failure(ClientThemeStorageError::path, ERROR_BAD_PATHNAME);
            } else if (completed.result.ok()) {
                if (request.operation == ClientThemeIoOperation::save) {
                    completed.snapshot = request.snapshot;
                    completed.result = client_theme_storage_save_file(path, request.snapshot);
                } else
                    completed.result = client_theme_storage_load_file(path, &completed.snapshot);
            }
        }
        AcquireSRWLockExclusive(&io->lock);
        if (!io->active_cancelled) {
            io->completion = completed;
            io->completion_ready = true;
            SetEvent(io->completion_event);
        }
        io->active_generation = 0;
        io->active_cancelled = false;
        ReleaseSRWLockExclusive(&io->lock);
    }
}
bool submit(ClientThemeIo* io, const ClientThemeIoRequest& request, u64* generation) noexcept {
    if (!io || !generation || overlap(generation, sizeof(*generation), io, sizeof(*io)))
        return reject(ERROR_INVALID_PARAMETER);
    AcquireSRWLockExclusive(&io->lock);
    if (io->stopping || io->next_generation == ~u64{0}) {
        const u32 error = io->stopping ? ERROR_OPERATION_ABORTED : ERROR_ARITHMETIC_OVERFLOW;
        ReleaseSRWLockExclusive(&io->lock);
        return reject(error);
    }
    if (!io->running) {
        // The previous worker (if any) has exited or is returning.
        const HANDLE thread =
            CreateThread(nullptr, 256u * 1024u, io_work, io, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!thread) {
            const u32 error = thread_last_error();
            ReleaseSRWLockExclusive(&io->lock);
            return reject(error);
        }
        if (io->thread)
            CloseHandle(io->thread);
        io->thread = thread;
        io->running = true;
    }
    io->pending = request;
    io->pending.generation = ++io->next_generation;
    io->pending_ready = true;
    *generation = io->next_generation;
    ReleaseSRWLockExclusive(&io->lock);
    return true;
}
} // namespace
bool client_theme_io_create(ClientThemeIo** output, const wchar_t* path) noexcept {
    if (!output || *output)
        return reject(ERROR_INVALID_PARAMETER);
    u32 length = 0;
    if (path && !path_length(path, &length))
        return reject(ERROR_BAD_PATHNAME);
    auto* io =
        static_cast<ClientThemeIo*>(HeapAlloc(process_heap(), HEAP_ZERO_MEMORY, sizeof(ClientThemeIo)));
    if (!io)
        return reject(ERROR_NOT_ENOUGH_MEMORY);
    io->explicit_path = path != nullptr;
    if (path)
        copy_bytes(io->storage_path, path, (length + 1) * sizeof(wchar_t));
    io->completion_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!io->completion_event) {
        const u32 error = thread_last_error();
        release_io(io);
        return reject(error);
    }
    *output = io;
    return true;
}
bool client_theme_io_submit_load(ClientThemeIo* io, u64* generation) noexcept {
    ClientThemeIoRequest request{};
    request.operation = ClientThemeIoOperation::load;
    return submit(io, request, generation);
}
bool client_theme_io_submit_import(ClientThemeIo* io, ConstSlice<wchar_t> path,
                                   const FileSnapshot& expected, const ClientThemeStorageSnapshot& baseline,
                                   u64* generation) noexcept {
    if (!path_valid(path) || !expected.bytes || expected.bytes > kClientThemeImportMaximumBytes ||
        expected.identity.reserved || !client_theme_storage_valid(baseline))
        return reject(ERROR_INVALID_PARAMETER);
    ClientThemeIoRequest request{};
    request.snapshot = baseline;
    request.expected = expected;
    request.operation = ClientThemeIoOperation::import_document;
    copy_bytes(request.path, path.data, path.count * sizeof(wchar_t));
    return submit(io, request, generation);
}
bool client_theme_io_submit_save(ClientThemeIo* io, const ClientThemeStorageSnapshot& snapshot,
                                 u64* generation) noexcept {
    if (!client_theme_storage_valid(snapshot))
        return reject(ERROR_INVALID_PARAMETER);
    ClientThemeIoRequest request{};
    request.snapshot = snapshot;
    request.operation = ClientThemeIoOperation::save;
    return submit(io, request, generation);
}
bool client_theme_io_submit_catalog(ClientThemeIo* io, u64* generation) noexcept {
    ClientThemeIoRequest request{};
    request.operation = ClientThemeIoOperation::catalog;
    return submit(io, request, generation);
}
HANDLE client_theme_io_completion_event(const ClientThemeIo* io) noexcept {
    return io ? io->completion_event : nullptr;
}
bool client_theme_io_poll(ClientThemeIo* io, ClientThemeIoCompletion* completion, bool* ready) noexcept {
    if (!io || !completion || !ready || overlap(completion, sizeof(*completion), io, sizeof(*io)) ||
        overlap(ready, sizeof(*ready), io, sizeof(*io)) ||
        overlap(ready, sizeof(*ready), completion, sizeof(*completion)))
        return reject(ERROR_INVALID_PARAMETER);
    AcquireSRWLockExclusive(&io->lock);
    *ready = io->completion_ready;
    if (io->completion_ready) {
        *completion = io->completion;
        io->completion_ready = false;
        ResetEvent(io->completion_event);
    }
    ReleaseSRWLockExclusive(&io->lock);
    return true;
}
void client_theme_io_cancel(ClientThemeIo* io, u64 generation) noexcept {
    if (!io || !generation)
        return;
    AcquireSRWLockExclusive(&io->lock);
    if (io->pending_ready && io->pending.generation == generation)
        io->pending_ready = false;
    if (io->active_generation == generation)
        io->active_cancelled = true;
    if (io->completion_ready && io->completion.generation == generation) {
        io->completion_ready = false;
        ResetEvent(io->completion_event);
    }
    ReleaseSRWLockExclusive(&io->lock);
}
bool client_theme_io_stop(ClientThemeIo** output, u32 timeout) noexcept {
    if (!output)
        return reject(ERROR_INVALID_PARAMETER);
    auto* io = *output;
    if (!io)
        return true;
    AcquireSRWLockExclusive(&io->lock);
    io->stopping = true; // no new worker starts; a running one drains and exits
    const HANDLE thread = io->thread;
    ReleaseSRWLockExclusive(&io->lock);
    const DWORD waited = thread ? WaitForSingleObject(thread, timeout) : WAIT_OBJECT_0;
    if (waited != WAIT_OBJECT_0)
        return reject(waited == WAIT_FAILED ? thread_last_error() : WAIT_TIMEOUT);
    release_io(io);
    *output = nullptr;
    return true;
}
u32 client_theme_io_owner_bytes(const ClientThemeIo* io) noexcept {
    return io ? sizeof(ClientThemeIo) : 0;
}
} // namespace maicord
