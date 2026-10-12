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

bool file_persistence_read_complete(HANDLE file, Slice<u8> bytes, u32* native_error) noexcept;
FilePersistenceWriteResult file_persistence_write_atomic(const wchar_t* path, u32 path_characters,
                                                         ConstSlice<u8> bytes) noexcept;
void file_persistence_secure_release(u8* bytes, u32 count) noexcept;

} // namespace maicord
