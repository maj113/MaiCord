#pragma once

// Win32 is not part of the base: headers name its types through
// maicord/core/win32_types.hpp, and code that calls it includes <Windows.h>,
// which these keep lean.
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif

namespace maicord {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using i16 = short;
using i32 = int;
using i64 = long long;
using usize = unsigned long long;

static_assert(sizeof(void*) == 8, "MaiCord currently ships as x64 only.");
static_assert(sizeof(u64) == 8);
static_assert(sizeof(i16) == 2);

template <typename T> inline void com_release(T*& value) noexcept {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

template <typename T> constexpr T minimum(T a, T b) noexcept {
    return a < b ? a : b;
}

template <typename T> constexpr T maximum(T a, T b) noexcept {
    return a > b ? a : b;
}

constexpr void saturating_increment(u32& value) noexcept {
    if (value != ~u32{0}) {
        ++value;
    }
}

constexpr void saturating_increment_if(bool condition, u32& value) noexcept {
    if (condition) {
        saturating_increment(value);
    }
}

constexpr void saturating_accumulate(u32& value, u32 addend = 1) noexcept {
    value = addend > ~u32{0} - value ? ~u32{0} : value + addend;
}

constexpr void saturating_accumulate(u64& value, u64 addend) noexcept {
    value = addend > ~u64{0} - value ? ~u64{0} : value + addend;
}

} // namespace maicord
