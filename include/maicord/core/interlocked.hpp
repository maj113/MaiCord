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
__forceinline void write_release(volatile __int64* destination, __int64 value) noexcept {
    *destination = value;
}
// Increments a counter that exactly one thread writes and others only read (through read_acquire): no lock prefix, and
// a reader sees the old or the new value. Concurrent writers need InterlockedIncrement64 instead.
__forceinline void increment_single_writer(volatile __int64* counter) noexcept {
    *counter = *counter + 1;
}

} // namespace maicord
