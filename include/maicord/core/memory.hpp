#pragma once

#include "maicord/core/base.hpp"

extern "C" void* __cdecl memmove(void* destination, const void* source, unsigned long long count);
extern "C" void* __cdecl memset(void* destination, int value, unsigned long long count);
// Declared as <intrin.h> declares it, without parsing it.
extern "C" void __stosb(unsigned char* destination, unsigned char value, unsigned long long count);
#pragma intrinsic(__stosb)

namespace maicord {

inline void copy_bytes(void* destination, const void* source, u32 bytes) noexcept {
    if (bytes != 0) {
        memmove(destination, source, bytes);
    }
}

inline bool copy_bytes(void* destination, u32 capacity, const void* source, u32 bytes) noexcept {
    if (bytes > capacity || (!destination && bytes != 0) || (!source && bytes != 0)) {
        return false;
    }
    copy_bytes(destination, source, bytes);
    return true;
}

inline bool bytes_equal(const void* left, const void* right, u32 bytes) noexcept {
    if ((!left || !right) && bytes != 0) {
        return false;
    }
    const auto* left_bytes = static_cast<const u8*>(left);
    const auto* right_bytes = static_cast<const u8*>(right);
    for (u32 index = 0; index < bytes; ++index) {
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
    if (bytes != 0) {
        memset(destination, 0, bytes);
    }
}

// SecureZeroMemory without <Windows.h>: on x64 the SDK's RtlSecureZeroMemory
// is this same rep stosb, which the optimizer never removes as a dead store.
__forceinline void secure_clear_bytes(void* destination, usize bytes) noexcept {
    __stosb(static_cast<unsigned char*>(destination), 0, bytes);
}

} // namespace maicord
