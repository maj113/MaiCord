#pragma once

// Win32 is not part of the base: headers name its types through
// maicord/core/win32_types.hpp, and code that calls it includes the
// declarations it needs (maicord/sys). These keep <Windows.h> lean where the
// SDK's own headers are used (MAICORD_SDK_INCLUDES).
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif

// For the code that runs before the processor check (maicord/app/shell/processor_check.hpp) returns: it runs on a
// processor without AVX, so the compiler uses only the instructions of the first x86-64 there.
#define MAICORD_ANY_PROCESSOR __attribute__((__target__("arch=x86-64")))

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

// <type_traits> equivalents, on the compiler builtins the STL uses.
template <typename T> inline constexpr bool is_trivially_default_constructible_v = __is_trivially_constructible(T);
template <typename T> inline constexpr bool is_trivially_copyable_v = __is_trivially_copyable(T);
template <typename T>
inline constexpr bool is_trivial_v = __is_trivially_constructible(T) && __is_trivially_copyable(T);
template <typename T> inline constexpr bool is_standard_layout_v = __is_standard_layout(T);

// Releases the COM object in *slot and clears it. IUnknown::Release is the third
// vtable slot of every COM interface, so one out-of-line body serves all of them.
__declspec(noinline) inline void com_release_slot(void** slot) noexcept {
    void* object = *slot;
    if (object) {
        reinterpret_cast<unsigned long (*)(void*)>((*static_cast<void***>(object))[2])(object);
        *slot = nullptr;
    }
}

template <typename T> inline void com_release(T*& value) noexcept {
    com_release_slot(reinterpret_cast<void**>(&value));
}

template <typename T> constexpr T minimum(T a, T b) noexcept {
    return a < b ? a : b;
}

template <typename T> constexpr T maximum(T a, T b) noexcept {
    return a > b ? a : b;
}

// A member offset or a size that a constant table keeps in a narrower field. The build fails when the value does not
// fit, so a struct that grows cannot make the table point at the wrong bytes.
template <typename Field, usize Value> consteval Field narrow_constant() noexcept {
    static_assert(Value <= static_cast<Field>(~Field{}), "the table field is too narrow for this offset or size");
    return static_cast<Field>(Value);
}

// The generation after this one: zero is skipped so a stale handle never matches an empty slot.
template <typename T> constexpr T next_generation(T generation) noexcept {
    ++generation;
    return generation == 0 ? T{1} : generation;
}

template <typename T> constexpr T clamp_value(T value, T low, T high) noexcept {
    return value < low ? low : value > high ? high : value;
}

// 0..1: what an opacity or a progress is held to.
constexpr float clamp_unit(float value) noexcept {
    return clamp_value(value, 0.f, 1.f);
}

// Nonzero with one bit set.
constexpr bool is_power_of_two(u64 value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
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
    const u32 sum = value + addend;
    value = sum < value ? ~u32{0} : sum;
}

constexpr void saturating_accumulate(u64& value, u64 addend) noexcept {
    const u64 sum = value + addend;
    value = sum < value ? ~u64{0} : sum;
}

// A byte count and the error that ended the read or write; Error::none means it succeeded.
template <typename Error> struct ByteResult {
    u32 bytes;
    Error error;
    constexpr bool ok() const noexcept { return error == Error::none; }
};

// Float classification reads the bit pattern: /fp:fast may fold NaN and
// infinity comparisons, so these never compare floating-point values.
constexpr bool is_finite(float value) noexcept {
    return (__builtin_bit_cast(u32, value) & 0x7f800000u) != 0x7f800000u;
}

constexpr bool is_finite(double value) noexcept {
    return (__builtin_bit_cast(u64, value) & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

} // namespace maicord
