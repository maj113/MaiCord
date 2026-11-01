#pragma once
#include "maicord/text/font_catalog.hpp"

#include "maicord/text/base.hpp"
namespace maicord::font_path_stat {
// Optional one-query metadata read from an already read-locked disk-file handle.
// False leaves the output untouched and the caller falls back to the public query
// path. No pathname lookup, handle ownership or cache.
bool read_handle(HANDLE,NativeFontFileStamp*) noexcept;
}
