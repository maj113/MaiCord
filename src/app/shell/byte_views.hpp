#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// A string literal without its terminator.
template <u32 N> constexpr ConstSlice<u8> text_bytes(const char (&value)[N]) noexcept {
    return {reinterpret_cast<const u8*>(value), N - 1};
}
} // namespace maicord
