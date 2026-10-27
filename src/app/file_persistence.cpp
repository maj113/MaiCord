#include "maicord/core/win32_process.hpp"
#include "maicord/app/file_persistence.hpp"

#include "maicord/app/account_profile.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

__declspec(noinline) bool transfer_complete(HANDLE file, u8* bytes, u32 count, bool write,
                                             u32* native_error) noexcept {
    u32 offset = 0;
    while (offset < count) {
        DWORD transferred = 0;
        const BOOL ok = write ? WriteFile(file, bytes + offset, count - offset, &transferred, nullptr)
                              : ReadFile(file, bytes + offset, count - offset, &transferred, nullptr);
        if (!ok || transferred == 0) {
            *native_error = ok ? ERROR_HANDLE_EOF : thread_last_error();
            return false;
        }
        offset += transferred;
    }
    return true;
}

} // namespace

bool file_persistence_read_complete(HANDLE file, Slice<u8> bytes, u32* native_error) noexcept {
    return transfer_complete(file, bytes.data, bytes.count, false, native_error);
}

FileReadResult file_persistence_read_file(const wchar_t* path, Slice<u8> buffer, u32 minimum_bytes) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const u32 error = thread_last_error();
        return {0, error,
                error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? FileReadError::missing
                                                                               : FileReadError::open};
    }
    Win32LargeInteger size{};
    FileReadResult result{};
    if (!GetFileSizeEx(file, size.native()))
        result = {0, thread_last_error(), FileReadError::size};
    else if (size.QuadPart < minimum_bytes || size.QuadPart > buffer.count)
        result = {0, 0, FileReadError::size};
    else {
        result.bytes = static_cast<u32>(size.QuadPart);
        if (!transfer_complete(file, buffer.data, result.bytes, false, &result.native_error))
            result = {0, result.native_error, FileReadError::read};
    }
    CloseHandle(file);
    return result;
}

__declspec(noinline) FilePersistenceWriteResult
file_persistence_write_atomic(const wchar_t* path, u32 path_characters, ConstSlice<u8> bytes,
                              bool durable) noexcept {
    if (!path || path_characters == 0 ||
        path_characters + 4 >= kAccountProfileMaximumPathCharacters || !bytes.data || bytes.count == 0) {
        return {ERROR_INVALID_PARAMETER, FilePersistenceWriteError::open};
    }
    wchar_t temporary[kAccountProfileMaximumPathCharacters]{};
    for (u32 index = 0; index < path_characters; ++index) {
        temporary[index] = path[index];
    }
    temporary[path_characters] = L'.';
    temporary[path_characters + 1] = L'n';
    temporary[path_characters + 2] = L'e';
    temporary[path_characters + 3] = L'w';

    // No FILE_FLAG_WRITE_THROUGH: the flush below makes the data durable, and the flag made every
    // write wait on the device on its own.
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {thread_last_error(), FilePersistenceWriteError::open};
    }
    u32 native_error = 0;
    const bool written =
        transfer_complete(file, const_cast<u8*>(bytes.data), bytes.count, true, &native_error);
    const bool flushed = written && (!durable || FlushFileBuffers(file));
    if (written && !flushed) {
        native_error = thread_last_error();
    }
    CloseHandle(file);
    if (!written || !flushed) {
        DeleteFileW(temporary);
        return {native_error,
                written ? FilePersistenceWriteError::flush : FilePersistenceWriteError::write};
    }
    if (!MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0))) {
        native_error = thread_last_error();
        DeleteFileW(temporary);
        return {native_error, FilePersistenceWriteError::commit};
    }
    return {0, FilePersistenceWriteError::none};
}

__declspec(noinline) void file_persistence_secure_release(u8* bytes, u32 count) noexcept {
    if (bytes) {
        secure_clear_bytes(bytes, count);
        VirtualFree(bytes, 0, MEM_RELEASE);
    }
}

} // namespace maicord
