#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Bounded wide-string helpers for Win32 paths. An append keeps the output
// NUL-terminated and leaves it unchanged when the text does not fit.
u32 wide_length(const wchar_t* text, u32 limit) noexcept;
bool wide_append(wchar_t* output, u32 capacity, u32* length, const wchar_t* text) noexcept;
// Appends value as 16 lowercase hexadecimal digits.
bool wide_append_hex(wchar_t* output, u32 capacity, u32* length, u64 value) noexcept;

// Builds <root>\MaiCord, or <root>\MaiCord\<relative> when relative is not
// empty; relative may name nested components ("plugin-data\\effect.bin").
// Trailing separators on root are dropped, so a drive root yields X:\MaiCord.
// Returns the length, or 0 when root is empty or the path and its terminator
// do not fit in capacity.
u32 data_path_build(const wchar_t* root, wchar_t* output, u32 capacity, const wchar_t* relative) noexcept;

enum class DataPathError : u8 {
    none,
    path,
    directory,
};

struct DataPathResult {
    u32 length;
    u32 native_error;
    DataPathError error;

    constexpr bool ok() const noexcept {
        return error == DataPathError::none;
    }
};

// %LOCALAPPDATA%\MaiCord[\relative], the per-user data directory. LOCALAPPDATA
// must be an absolute drive or UNC path without reserved characters. With
// create_parents, the MaiCord directory and every directory of relative above
// its last component are created (existing ones must be directories).
DataPathResult data_path(wchar_t* output, u32 capacity, const wchar_t* relative,
                         bool create_parents = false) noexcept;

// Creates path as a directory when it is missing. True when it exists as a
// directory; otherwise *native_error (when given) receives the Win32 error.
bool directory_ensure(const wchar_t* path, u32* native_error = nullptr) noexcept;

} // namespace maicord
