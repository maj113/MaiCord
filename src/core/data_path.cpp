#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {

constexpr u32 kRootCapacity = 1024;

bool separator(wchar_t value) noexcept {
    return value == L'\\' || value == L'/';
}

// An absolute drive or UNC path (not a \\?\ or \\.\ device path) without
// control or reserved characters.
bool root_valid(const wchar_t* root, u32 length) noexcept {
    const bool drive = length >= 3 &&
                       ((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z')) &&
                       root[1] == L':' && separator(root[2]);
    const bool unc = length >= 5 && root[0] == L'\\' && root[1] == L'\\' && root[2] != L'?' && root[2] != L'.';
    if (!drive && !unc)
        return false;
    for (u32 i = 0; i < length; ++i)
        if (root[i] < 32 || root[i] == L'"' || root[i] == L'<' || root[i] == L'>' || root[i] == L'|' ||
            root[i] == L'*' || root[i] == L'?' || (root[i] == L':' && i != 1))
            return false;
    return true;
}

DataPathResult failure(DataPathError error, u32 native_error) noexcept {
    return {0, native_error, error};
}

} // namespace

u32 wide_length(const wchar_t* text, u32 limit) noexcept {
    u32 length = 0;
    if (text)
        while (length < limit && text[length])
            ++length;
    return length;
}

bool wide_append(wchar_t* output, u32 capacity, u32* length, const wchar_t* text) noexcept {
    const u32 count = wide_length(text, capacity);
    if (!output || !length || *length + count >= capacity)
        return false;
    copy_bytes(output + *length, text, count * static_cast<u32>(sizeof(wchar_t)));
    *length += count;
    output[*length] = 0;
    return true;
}

bool wide_append_hex(wchar_t* output, u32 capacity, u32* length, u64 value) noexcept {
    if (!output || !length || *length + 16 >= capacity)
        return false;
    for (u32 shift = 64; shift;) {
        shift -= 4;
        output[(*length)++] = L"0123456789abcdef"[(value >> shift) & 15];
    }
    output[*length] = 0;
    return true;
}

u32 data_path_build(const wchar_t* root, wchar_t* output, u32 capacity, const wchar_t* relative) noexcept {
    if (!root || !output)
        return 0;
    u32 length = wide_length(root, capacity);
    while (length && separator(root[length - 1]))
        --length;
    if (length == 0 || length >= capacity)
        return 0;
    copy_bytes(output, root, length * static_cast<u32>(sizeof(wchar_t)));
    output[length] = 0;
    if (!wide_append(output, capacity, &length, L"\\MaiCord"))
        return 0;
    if (relative && relative[0] &&
        (!wide_append(output, capacity, &length, L"\\") || !wide_append(output, capacity, &length, relative)))
        return 0;
    return length;
}

DataPathResult data_path(wchar_t* output, u32 capacity, const wchar_t* relative, bool create_parents) noexcept {
    wchar_t root[kRootCapacity];
    const u32 root_length = GetEnvironmentVariableW(L"LOCALAPPDATA", root, kRootCapacity);
    if (root_length == 0)
        return failure(DataPathError::path, GetLastError());
    if (root_length >= kRootCapacity || !root_valid(root, root_length))
        return failure(DataPathError::path, ERROR_BAD_PATHNAME);
    const u32 length = data_path_build(root, output, capacity, relative);
    if (length == 0)
        return failure(DataPathError::path, ERROR_FILENAME_EXCED_RANGE);
    if (create_parents) {
        // Every separator from the end of <root>\MaiCord on closes a directory.
        u32 root_end = root_length;
        while (root_end && separator(root[root_end - 1]))
            --root_end;
        for (u32 i = root_end + 8; i < length; ++i) {
            if (!separator(output[i]))
                continue;
            const wchar_t saved = output[i];
            output[i] = 0;
            u32 native_error = 0;
            const bool created = directory_ensure(output, &native_error);
            output[i] = saved;
            if (!created)
                return failure(DataPathError::directory, native_error);
        }
    }
    return {length, 0, DataPathError::none};
}

bool directory_ensure(const wchar_t* path, u32* native_error) noexcept {
    if (CreateDirectoryW(path, nullptr))
        return true;
    u32 error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        const DWORD attributes = GetFileAttributesW(path);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
            return true;
        error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY;
    }
    if (native_error)
        *native_error = error;
    return false;
}

} // namespace maicord
