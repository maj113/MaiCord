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

template <typename T> constexpr T clamp_value(T value, T low, T high) noexcept {
    return value < low ? low : value > high ? high : value;
}

template <typename T> constexpr T absolute_value(T value) noexcept {
    return value < 0 ? -value : value;
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

// alignment must be a power of two.
template <typename T> constexpr T align_up_pow2(T value, T alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Pays cost from a work budget; a budget that cannot pay is left unchanged.
constexpr bool budget_spend(u32& budget, u32 cost = 1) noexcept {
    if (cost > budget)
        return false;
    budget -= cost;
    return true;
}

constexpr bool budget_spend(u64& budget, u64 cost = 1) noexcept {
    if (cost > budget)
        return false;
    budget -= cost;
    return true;
}

// budget_spend that also adds what was paid to an operation count.
constexpr bool budget_spend_counted(u64& budget, u64& operations, u64 cost = 1) noexcept {
    if (cost > budget)
        return false;
    budget -= cost;
    operations += cost;
    return true;
}

// Float classification reads the bit pattern: /fp:fast may fold NaN and
// infinity comparisons, so these never compare floating-point values.
constexpr bool is_finite(float value) noexcept {
    return (__builtin_bit_cast(u32, value) & 0x7f800000u) != 0x7f800000u;
}

constexpr bool is_finite(double value) noexcept {
    return (__builtin_bit_cast(u64, value) & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

// Finite and greater than zero.
constexpr bool is_finite_positive(float value) noexcept {
    const u32 bits = __builtin_bit_cast(u32, value);
    return bits != 0 && bits < 0x7f800000u;
}

// Finite and not below zero; negative zero counts as zero.
constexpr bool is_finite_nonnegative(float value) noexcept {
    const u32 bits = __builtin_bit_cast(u32, value);
    return bits < 0x7f800000u || bits == 0x80000000u;
}

} // namespace maicord
