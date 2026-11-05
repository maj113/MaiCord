#include "maicord/app/account/login/acceptance_record.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/path.hpp"

namespace maicord {
namespace {
AcceptanceRecordResult fail(AcceptanceRecordError error, DWORD native = 0) noexcept { return {error, native}; }
} // namespace

#if defined(_WIN32)
// A Windows reserved device name (CON, PRN, AUX, NUL, COM0-9, LPT0-9) before any extension.
static bool acceptance_record_reserved_name(const wchar_t* name, u32 count) noexcept {
    u32 stem = 0;
    while (stem < count && name[stem] != L'.') ++stem;
    const bool numbered = stem == 4 && name[3] >= L'0' && name[3] <= L'9';
    if (stem != 3 && !numbered)
        return false;
    const wchar_t a = ascii_lower(name[0]), b = ascii_lower(name[1]), c = ascii_lower(name[2]);
    if (numbered)
        return (a == L'c' && b == L'o' && c == L'm') || (a == L'l' && b == L'p' && c == L't');
    return (a == L'c' && b == L'o' && c == L'n') || (a == L'p' && b == L'r' && c == L'n') ||
        (a == L'a' && b == L'u' && c == L'x') || (a == L'n' && b == L'u' && c == L'l');
}
#endif

bool acceptance_record_path_valid(const ConstSlice<wchar_t>& path) noexcept {
#if defined(_WIN32)
    if (!path.data || path.count < 4 || path.count > kAcceptanceRecordMaximumPathCharacters - 5 ||
        !byte_range_valid({reinterpret_cast<const u8*>(path.data), path.count * static_cast<u32>(sizeof(wchar_t))}) ||
        path_root(path) != PathRoot::drive) return false;
    u32 component = 3;
    for (u32 i = 3; i <= path.count; ++i) {
        if (i == path.count || path[i] == L'\\') {
            if (i == component || path[i - 1] == L'.' || path[i - 1] == L' ' ||
                acceptance_record_reserved_name(path.data + component, i - component)) return false;
            component = i + 1;
        } else if (path[i] < 32 || path[i] == L'/' || path[i] == L':' || path[i] == L'*' ||
                   path[i] == L'?' || path[i] == L'"' || path[i] == L'<' || path[i] == L'>' || path[i] == L'|') return false;
    }
    return true;
#else
    // A rooted path of plain components: no empty, "." or ".." ones, no backslash, no control character.
    if (!path.data || path.count < 2 || path.count > kAcceptanceRecordMaximumPathCharacters - 5 ||
        !byte_range_valid({reinterpret_cast<const u8*>(path.data), path.count * static_cast<u32>(sizeof(wchar_t))}) ||
        path_root(path) != PathRoot::rooted) return false;
    u32 component = 1;
    for (u32 i = 1; i <= path.count; ++i) {
        if (i == path.count || path[i] == L'/') {
            const u32 length = i - component;
            if (length == 0 || (length == 1 && path[component] == L'.') ||
                (length == 2 && path[component] == L'.' && path[component + 1] == L'.')) return false;
            component = i + 1;
        } else if (path[i] < 32 || path[i] == L'\\') return false;
    }
    return true;
#endif
}

void acceptance_record_terminate(const ConstSlice<wchar_t>& path_view, wchar_t* output) noexcept {
    const ConstSlice<wchar_t> path = path_view; // the stores below must not force reloads
    for (u32 i = 0; i < path.count; ++i) output[i] = path[i];
    output[path.count] = 0;
}

// Copies the first count characters of path followed by the NUL-terminated ending.
static void acceptance_record_sibling(const wchar_t* path, u32 count, const wchar_t* ending, wchar_t* output) noexcept {
    for (u32 i = 0; i < count; ++i) output[i] = path[i];
    u32 i = 0;
    do { output[count + i] = ending[i]; } while (ending[i++]);
}

AcceptanceRecordResult acceptance_record_ancestors(wchar_t* path, u32 count) noexcept {
#if defined(_WIN32)
    wchar_t root[4]{path[0], L':', L'\\', 0};
    if (GetDriveTypeW(root) != DRIVE_FIXED) return fail(AcceptanceRecordError::path, ERROR_NOT_SUPPORTED);
    constexpr wchar_t kSeparator = L'\\';
    constexpr u32 kFirst = 3;
#else
    constexpr wchar_t kSeparator = L'/';
    constexpr u32 kFirst = 1;
#endif
    for (u32 i = kFirst; i < count; ++i) if (path[i] == kSeparator) {
        path[i] = 0;
        FileStat stat{};
        const bool found = file_path_stat(path, &stat);
        const u32 native = found ? 0 : thread_last_error();
        path[i] = kSeparator;
        if (!found) return fail(AcceptanceRecordError::path, native);
        if (stat.kind != FileKind::directory) return fail(AcceptanceRecordError::path, ERROR_BAD_PATHNAME);
    }
    return {};
}

AcceptanceRecordResult acceptance_record_disk_file(HANDLE file) noexcept {
    FileStat stat{};
    if (!file_stat(file, &stat)) return fail(AcceptanceRecordError::file, thread_last_error());
    if (stat.kind == FileKind::other) return fail(AcceptanceRecordError::file);
    if (stat.kind != FileKind::regular) return fail(AcceptanceRecordError::file, ERROR_BAD_PATHNAME);
    return {};
}

AcceptanceRecordResult acceptance_record_read(const wchar_t* path, const Slice<u8>& record) noexcept {
    HANDLE file = file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead, kFileNoFollow});
    if (file == INVALID_HANDLE_VALUE) {
        const u32 native = thread_last_error();
        return fail(native == ERROR_FILE_NOT_FOUND ? AcceptanceRecordError::missing : AcceptanceRecordError::open, native);
    }
    auto result = acceptance_record_disk_file(file);
    u64 size = 0;
    if (result.ok() && !file_size(file, &size)) result = fail(AcceptanceRecordError::size, thread_last_error());
    if (result.ok() && size != record.count) result = fail(AcceptanceRecordError::size);
    u32 native = 0;
    if (result.ok() && !file_persistence_read_complete(file, record, &native))
        result = fail(AcceptanceRecordError::read, native);
    if (!file_close(file) && result.ok()) result = fail(AcceptanceRecordError::release, thread_last_error());
    return result;
}

AcceptanceRecordResult acceptance_record_write(const wchar_t* path, u32 count,
                                               const ConstSlice<u8>& record) noexcept {
    wchar_t temporary[kAcceptanceRecordMaximumPathCharacters + 1];
    acceptance_record_sibling(path, count, L".new", temporary);
    // CREATE_NEW never truncates or follows an attacker-controlled/stale sibling.
    HANDLE file = file_open(temporary, {FileAccess::write, FileCreate::create_new, 0, kFileWriteThrough | kFileNoFollow});
    if (file == INVALID_HANDLE_VALUE) return fail(AcceptanceRecordError::open, thread_last_error());
    auto result = acceptance_record_disk_file(file);
    u32 written = 0;
    if (result.ok()) {
        if (!file_write(file, record.data, record.count, &written)) result = fail(AcceptanceRecordError::write, thread_last_error());
        else if (written != record.count) result = fail(AcceptanceRecordError::write, ERROR_WRITE_FAULT);
    }
    if (result.ok() && !file_flush(file)) result = fail(AcceptanceRecordError::flush, thread_last_error());
    if (!file_close(file) && result.ok()) result = fail(AcceptanceRecordError::release, thread_last_error());
    if (result.ok() && !file_replace(temporary, path, true))
        result = fail(AcceptanceRecordError::commit, thread_last_error());
    if (!result.ok()) file_remove(temporary); // only our successfully CREATE_NEW-owned sibling
    return result;
}

AcceptanceRecordResult acceptance_record_lock(const wchar_t* path, u32 count, HANDLE* lock) noexcept {
    wchar_t locked[kAcceptanceRecordMaximumPathCharacters + 1];
    acceptance_record_sibling(path, count, L".lock", locked);
    const HANDLE handle = file_open(locked, {FileAccess::read_write, FileCreate::open_always, 0, kFileNoFollow});
    if (handle == INVALID_HANDLE_VALUE) return fail(AcceptanceRecordError::lock, thread_last_error());
    const auto result = acceptance_record_disk_file(handle);
    if (!result.ok()) { file_close(handle); return result; }
    *lock = handle;
    return {};
}

} // namespace maicord
