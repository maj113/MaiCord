#include "maicord/core/win32_process.hpp"
#include "maicord/app/net/file_persistence.hpp"

#include "maicord/app/account/account_profile.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/file.hpp"

namespace maicord {
namespace {

__declspec(noinline) bool transfer_complete(HANDLE file, u8* bytes, u32 count, bool write,
                                             u32* native_error) noexcept {
    u32 offset = 0;
    while (offset < count) {
        u32 transferred = 0;
        const bool ok = write ? file_write(file, bytes + offset, count - offset, &transferred)
                              : file_read(file, bytes + offset, count - offset, &transferred);
        if (!ok || transferred == 0) {
            *native_error = ok ? ERROR_HANDLE_EOF : thread_last_error();
            return false;
        }
        offset += transferred;
    }
    return true;
}

} // namespace

bool file_persistence_read_complete(HANDLE file, const Slice<u8>& bytes, u32* native_error) noexcept {
    return transfer_complete(file, bytes.data, bytes.count, false, native_error);
}

FileReadResult file_persistence_read_file(const wchar_t* path, const Slice<u8>& buffer,
                                          u32 minimum_bytes) noexcept {
    HANDLE file = file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead | kFileShareDelete, 0});
    if (file == INVALID_HANDLE_VALUE) {
        const u32 error = thread_last_error();
        return {0, error,
                error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? FileReadError::missing
                                                                               : FileReadError::open};
    }
    u64 size = 0;
    FileReadResult result{};
    if (!file_size(file, &size))
        result = {0, thread_last_error(), FileReadError::size};
    else if (size < minimum_bytes || size > buffer.count)
        result = {0, 0, FileReadError::size};
    else {
        result.bytes = static_cast<u32>(size);
        if (!transfer_complete(file, buffer.data, result.bytes, false, &result.native_error))
            result = {0, result.native_error, FileReadError::read};
    }
    file_close(file);
    return result;
}

__declspec(noinline) FilePersistenceWriteResult
file_persistence_write_atomic(const wchar_t* path, u32 path_characters, const ConstSlice<u8>& bytes,
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
    HANDLE file = file_open(temporary, {FileAccess::write, FileCreate::create_always, 0, kFileTemporary});
    if (file == INVALID_HANDLE_VALUE) {
        return {thread_last_error(), FilePersistenceWriteError::open};
    }
    u32 native_error = 0;
    const bool written =
        transfer_complete(file, const_cast<u8*>(bytes.data), bytes.count, true, &native_error);
    const bool flushed = written && (!durable || file_flush(file));
    if (written && !flushed) {
        native_error = thread_last_error();
    }
    file_close(file);
    if (!written || !flushed) {
        file_remove(temporary);
        return {native_error,
                written ? FilePersistenceWriteError::flush : FilePersistenceWriteError::write};
    }
    if (!file_replace(temporary, path, durable)) {
        native_error = thread_last_error();
        file_remove(temporary);
        return {native_error, FilePersistenceWriteError::commit};
    }
    return {0, FilePersistenceWriteError::none};
}

__declspec(noinline) void file_persistence_secure_release(u8* bytes, u32 count) noexcept {
    if (bytes) {
        secure_clear_bytes(bytes, count);
        pages_release(bytes);
    }
}

} // namespace maicord
