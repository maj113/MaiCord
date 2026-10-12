#include "maicord/app/acceptance_record.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_range.hpp"

#include <Windows.h>

namespace maicord {
namespace {
AcceptanceRecordResult fail(AcceptanceRecordError error, DWORD native = 0) noexcept { return {error, native}; }
wchar_t lower(wchar_t value) noexcept {
    return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + L'a' - L'A') : value;
}
bool reserved_component(const wchar_t* name, u32 count) noexcept {
    u32 stem = 0;
    while (stem < count && name[stem] != L'.') ++stem;
    if (stem == 3) {
        const wchar_t a = lower(name[0]), b = lower(name[1]), c = lower(name[2]);
        return (a == L'c' && b == L'o' && c == L'n') || (a == L'p' && b == L'r' && c == L'n') ||
            (a == L'a' && b == L'u' && c == L'x') || (a == L'n' && b == L'u' && c == L'l');
    }
    if (stem == 4 && name[3] >= L'0' && name[3] <= L'9')
        return (lower(name[0]) == L'c' && lower(name[1]) == L'o' && lower(name[2]) == L'm') ||
            (lower(name[0]) == L'l' && lower(name[1]) == L'p' && lower(name[2]) == L't');
    return false;
}
} // namespace

bool acceptance_record_path_valid(ConstSlice<wchar_t> path) noexcept {
    if (!path.data || path.count < 4 || path.count > kAcceptanceRecordMaximumPathCharacters - 5 ||
        !byte_range_valid({reinterpret_cast<const u8*>(path.data), path.count * static_cast<u32>(sizeof(wchar_t))}) ||
        lower(path[0]) < L'a' || lower(path[0]) > L'z' || path[1] != L':' || path[2] != L'\\') return false;
    u32 component = 3;
    for (u32 i = 3; i <= path.count; ++i) {
        if (i == path.count || path[i] == L'\\') {
            if (i == component || path[i - 1] == L'.' || path[i - 1] == L' ' ||
                reserved_component(path.data + component, i - component)) return false;
            component = i + 1;
        } else if (path[i] < 32 || path[i] == L'/' || path[i] == L':' || path[i] == L'*' ||
                   path[i] == L'?' || path[i] == L'"' || path[i] == L'<' || path[i] == L'>' || path[i] == L'|') return false;
    }
    return true;
}

void acceptance_record_terminate(ConstSlice<wchar_t> path, wchar_t* output) noexcept {
    for (u32 i = 0; i < path.count; ++i) output[i] = path[i];
    output[path.count] = 0;
}

void acceptance_record_sibling(const wchar_t* path, u32 count, const wchar_t* ending, wchar_t* output) noexcept {
    for (u32 i = 0; i < count; ++i) output[i] = path[i];
    u32 i = 0;
    do { output[count + i] = ending[i]; } while (ending[i++]);
}

AcceptanceRecordResult acceptance_record_ancestors(wchar_t* path, u32 count) noexcept {
    wchar_t root[4]{path[0], L':', L'\\', 0};
    if (GetDriveTypeW(root) != DRIVE_FIXED) return fail(AcceptanceRecordError::path, ERROR_NOT_SUPPORTED);
    for (u32 i = 3; i < count; ++i) if (path[i] == L'\\') {
        path[i] = 0;
        const DWORD attributes = GetFileAttributesW(path);
        const DWORD native = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
        path[i] = L'\\';
        if (attributes == INVALID_FILE_ATTRIBUTES) return fail(AcceptanceRecordError::path, native);
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return fail(AcceptanceRecordError::path, ERROR_BAD_PATHNAME);
    }
    return {};
}

AcceptanceRecordResult acceptance_record_disk_file(HANDLE file) noexcept {
    if (GetFileType(file) != FILE_TYPE_DISK) return fail(AcceptanceRecordError::file);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) return fail(AcceptanceRecordError::file, GetLastError());
    if (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        return fail(AcceptanceRecordError::file, ERROR_BAD_PATHNAME);
    return {};
}

AcceptanceRecordResult acceptance_record_read(const wchar_t* path, Slice<u8> record) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD native = GetLastError();
        return fail(native == ERROR_FILE_NOT_FOUND ? AcceptanceRecordError::missing : AcceptanceRecordError::open, native);
    }
    auto result = acceptance_record_disk_file(file);
    LARGE_INTEGER size{};
    if (result.ok() && !GetFileSizeEx(file, &size)) result = fail(AcceptanceRecordError::size, GetLastError());
    if (result.ok() && size.QuadPart != record.count) result = fail(AcceptanceRecordError::size);
    u32 native = 0;
    if (result.ok() && !file_persistence_read_complete(file, record, &native))
        result = fail(AcceptanceRecordError::read, native);
    if (!CloseHandle(file) && result.ok()) result = fail(AcceptanceRecordError::release, GetLastError());
    return result;
}

AcceptanceRecordResult acceptance_record_write(const wchar_t* path, u32 count, ConstSlice<u8> record) noexcept {
    wchar_t temporary[kAcceptanceRecordMaximumPathCharacters + 1];
    acceptance_record_sibling(path, count, L".new", temporary);
    // CREATE_NEW never truncates or follows an attacker-controlled/stale sibling.
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail(AcceptanceRecordError::open, GetLastError());
    auto result = acceptance_record_disk_file(file);
    DWORD written = 0;
    if (result.ok()) {
        if (!WriteFile(file, record.data, record.count, &written, nullptr)) result = fail(AcceptanceRecordError::write, GetLastError());
        else if (written != record.count) result = fail(AcceptanceRecordError::write, ERROR_WRITE_FAULT);
    }
    if (result.ok() && !FlushFileBuffers(file)) result = fail(AcceptanceRecordError::flush, GetLastError());
    if (!CloseHandle(file) && result.ok()) result = fail(AcceptanceRecordError::release, GetLastError());
    if (result.ok() && !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        result = fail(AcceptanceRecordError::commit, GetLastError());
    if (!result.ok()) DeleteFileW(temporary); // only our successfully CREATE_NEW-owned sibling
    return result;
}

AcceptanceRecordResult acceptance_record_lock(const wchar_t* path, u32 count, HANDLE* lock) noexcept {
    wchar_t locked[kAcceptanceRecordMaximumPathCharacters + 1];
    acceptance_record_sibling(path, count, L".lock", locked);
    const HANDLE handle = CreateFileW(locked, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return fail(AcceptanceRecordError::lock, GetLastError());
    const auto result = acceptance_record_disk_file(handle);
    if (!result.ok()) { CloseHandle(handle); return result; }
    *lock = handle;
    return {};
}

} // namespace maicord
