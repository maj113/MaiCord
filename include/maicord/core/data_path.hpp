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

// The cache and state directories (D19): for what can be created again, and for logs and state. Linux keeps them
// apart ($XDG_CACHE_HOME/maicord, else ~/.cache/maicord; $XDG_STATE_HOME/maicord, else ~/.local/state/maicord);
// Windows keeps everything under data_path, so these are that. Same arguments and result as data_path.
#if defined(_WIN32)
inline DataPathResult cache_path(wchar_t* output, u32 capacity, const wchar_t* relative,
                                 bool create_parents = false) noexcept {
    return data_path(output, capacity, relative, create_parents);
}
inline DataPathResult state_path(wchar_t* output, u32 capacity, const wchar_t* relative,
                                 bool create_parents = false) noexcept {
    return data_path(output, capacity, relative, create_parents);
}
#else
DataPathResult cache_path(wchar_t* output, u32 capacity, const wchar_t* relative,
                          bool create_parents = false) noexcept;
DataPathResult state_path(wchar_t* output, u32 capacity, const wchar_t* relative,
                          bool create_parents = false) noexcept;
#endif

// data_path(stem), then the profile id as 16 lowercase hex digits and extension: one account's file, such as
// profile_data_path(id, path, capacity, L"accounts\\links-", L".trust"). False for profile 0 or when it does not fit.
bool profile_data_path(u64 profile_id, wchar_t* path, u32 capacity, const wchar_t* stem,
                       const wchar_t* extension) noexcept;

// Creates path as a directory when it is missing. True when it exists as a
// directory; otherwise *native_error (when given) receives the Win32 error.
bool directory_ensure(const wchar_t* path, u32* native_error = nullptr) noexcept;

// The path of the running executable (GetModuleFileNameW(nullptr), /proc/self/exe). The length excluding the
// terminator, or 0 when it fails or does not fit in capacity.
u32 executable_path(wchar_t* output, u32 capacity) noexcept;

} // namespace maicord
