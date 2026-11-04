#pragma once
#include "maicord/text/fonts/catalog.hpp"

#include "maicord/os/file.hpp"
#include "maicord/text/base.hpp"
namespace maicord::font_path_stat {
// Optional one-query metadata read from an already read-locked disk-file handle.
// False leaves the output untouched and the caller falls back to read_fallback.
// No pathname lookup, handle ownership or cache.
bool read_handle(FileHandle,NativeFontFileStamp*) noexcept;
// The portable read for a handle read_handle declined: size and identity, and no times. False sets *error to the
// Win32 error that says why (a handle that is not a disk file is ERROR_FILE_INVALID).
bool read_fallback(FileHandle,NativeFontFileStamp*,u32* error) noexcept;
}
