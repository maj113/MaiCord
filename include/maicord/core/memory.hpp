#pragma once

#include "maicord/core/base.hpp"
#include "maicord/sys/crt.hpp"
#include "maicord/sys/intrin.hpp"

namespace maicord {

// The runtime's memmove and memset accept a zero count with any pointers.
inline void copy_bytes(void* destination, const void* source, u32 bytes) noexcept {
    memmove(destination, source, bytes);
}

inline bool copy_bytes(void* destination, u32 capacity, const void* source, u32 bytes) noexcept {
    if (bytes > capacity || (!destination && bytes != 0) || (!source && bytes != 0)) {
        return false;
    }
    copy_bytes(destination, source, bytes);
    return true;
}

// Eight bytes per step, then a byte tail; the damage diff compares whole quad
// ranges with it.
__declspec(noinline) inline bool bytes_equal(const void* left, const void* right, usize bytes) noexcept {
    if ((!left || !right) && bytes != 0) {
        return false;
    }
    const auto* left_bytes = static_cast<const u8*>(left);
    const auto* right_bytes = static_cast<const u8*>(right);
    usize index = 0;
    for (; bytes - index >= 8; index += 8) {
        if (*reinterpret_cast<const u64 __unaligned*>(left_bytes + index) !=
            *reinterpret_cast<const u64 __unaligned*>(right_bytes + index)) {
            return false;
        }
    }
    for (; index < bytes; ++index) {
        if (left_bytes[index] != right_bytes[index]) {
            return false;
        }
    }
    return true;
}

inline bool bytes_zero(const void* data, usize bytes) noexcept {
    const auto* p = static_cast<const u8*>(data);
    for (usize i = 0; i < bytes; ++i)
        if (p[i])
            return false;
    return true;
}

inline void clear_bytes(void* destination, u32 bytes) noexcept {
    memset(destination, 0, bytes);
}

// `*object = T{}` without the zeroed temporary MSVC copies in for a large T:
// value-initializes in place (memset plus any nonzero member defaults). A type whose every member
// default (recursively) is zero declares `static constexpr bool kZeroDefault = true;` and is cleared
// with one fill instead; never declare it on a type with a nonzero default.
template <typename T>
inline void reset_value(T* object) noexcept {
    static_assert(__is_trivially_destructible(T));
    if constexpr (requires { T::kZeroDefault; })
        clear_bytes(object, static_cast<u32>(sizeof(T)));
    else
        ::new (static_cast<void*>(object)) T{};
}

// A memmove the compiler cannot expand inline at a constant size.
__declspec(noinline) inline void copy_bytes_call(void* destination, const void* source, usize bytes) noexcept {
    memmove(destination, source, bytes);
}

// `*destination = source` for a large T as one call instead of the inline
// vector loop MSVC emits at every site.
template <typename T>
inline void copy_value(T* destination, const T& source) noexcept {
    static_assert(__is_trivially_copyable(T));
    if constexpr (sizeof(T) > 128)
        copy_bytes_call(destination, &source, sizeof(T));
    else
        *destination = source;
}

// SecureZeroMemory without <Windows.h>: on x64 the SDK's RtlSecureZeroMemory
// is this same rep stosb, which the optimizer never removes as a dead store.
__forceinline void secure_clear_bytes(void* destination, usize bytes) noexcept {
    __stosb(static_cast<unsigned char*>(destination), 0, bytes);
}

} // namespace maicord
