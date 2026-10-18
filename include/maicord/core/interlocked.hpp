#pragma once

#include "maicord/sys/intrin.hpp"

namespace maicord {

// ReadAcquire, ReadAcquire64 and WriteRelease without <Windows.h>: on x64 the
// SDK's are these same plain volatile accesses, ordered by /volatile:ms.
__forceinline long read_acquire(const volatile long* source) noexcept {
    return *source;
}
__forceinline __int64 read_acquire(const volatile __int64* source) noexcept {
    return *source;
}
__forceinline void write_release(volatile long* destination, long value) noexcept {
    *destination = value;
}

} // namespace maicord
