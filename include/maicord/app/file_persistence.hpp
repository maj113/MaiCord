#pragma once

#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {

enum class FilePersistenceWriteError : u8 {
    none,
    open,
    write,
    flush,
    commit,
};

struct FilePersistenceWriteResult {
    u32 native_error;
    FilePersistenceWriteError error;

    constexpr bool ok() const noexcept {
        return error == FilePersistenceWriteError::none;
    }
};

enum class FileReadError : u8 {
    none,
    missing,
    open,
    size,
    read,
};

struct FileReadResult {
    u32 bytes;
    u32 native_error;
    FileReadError error;

    constexpr bool ok() const noexcept {
        return error == FileReadError::none;
    }
};

bool file_persistence_read_complete(HANDLE file, Slice<u8> bytes, u32* native_error) noexcept;
// Reads a whole file of minimum_bytes..buffer.count bytes into buffer. A file that does
// not exist reports `missing`; any other failure reports its stage and native error.
FileReadResult file_persistence_read_file(const wchar_t* path, Slice<u8> buffer,
                                          u32 minimum_bytes = 1) noexcept;
// Replaces path through a sibling ".new" file. durable (the default) flushes the data and
// writes the rename through before returning; otherwise neither waits on the device.
FilePersistenceWriteResult file_persistence_write_atomic(const wchar_t* path, u32 path_characters,
                                                         ConstSlice<u8> bytes,
                                                         bool durable = true) noexcept;
void file_persistence_secure_release(u8* bytes, u32 count) noexcept;

} // namespace maicord
