#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// The RFC 1950 Adler-32 of `bytes` continued from `adler`: 1 starts a checksum and the result of one call is the
// `adler` of the next. zlib streams (inflate, the PNG encoders) check and write it.
u32 adler32_update(u32 adler, ConstSlice<u8> bytes) noexcept;

} // namespace maicord
