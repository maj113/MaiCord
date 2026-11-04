#pragma once

#if defined(_WIN32)
#include "maicord/sys/intrin.hpp"
#endif

namespace maicord {

// The signed integers the sequence counters and interlocked operations act on: the SDK's LONG (long) and LONG64
// (__int64) on Windows; int and long long elsewhere, where long is 64 bits wide.
#if defined(_WIN32)
using Atomic32 = long;
#else
using Atomic32 = int;
#endif
using Atomic64 = long long;

// ReadAcquire, ReadAcquire64 and WriteRelease without <Windows.h>: on x64 the
// SDK's are these same plain volatile accesses, ordered by /volatile:ms.
__forceinline Atomic32 read_acquire(const volatile Atomic32* source) noexcept {
    return *source;
}
__forceinline Atomic64 read_acquire(const volatile Atomic64* source) noexcept {
    return *source;
}
__forceinline void write_release(volatile Atomic32* destination, Atomic32 value) noexcept {
    *destination = value;
}
__forceinline void write_release(volatile Atomic64* destination, Atomic64 value) noexcept {
    *destination = value;
}
// Increments a counter that exactly one thread writes and others only read (through read_acquire): no lock prefix, and
// a reader sees the old or the new value. Concurrent writers need InterlockedIncrement64 instead.
__forceinline void increment_single_writer(volatile Atomic64* counter) noexcept {
    *counter = *counter + 1;
}

} // namespace maicord
