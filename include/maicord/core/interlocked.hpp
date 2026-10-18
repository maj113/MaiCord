#pragma once

// Declared as <intrin.h> and <winnt.h> declare them, without parsing either.
extern "C" long _InterlockedCompareExchange(long volatile* destination, long exchange, long comparand);
extern "C" long _InterlockedExchange(long volatile* target, long value);
extern "C" long _InterlockedIncrement(long volatile* addend);
extern "C" long _InterlockedDecrement(long volatile* addend);
extern "C" long _InterlockedExchangeAdd(long volatile* addend, long value);
extern "C" long _InterlockedOr(long volatile* value, long mask);
extern "C" __int64 _InterlockedCompareExchange64(__int64 volatile* destination, __int64 exchange,
                                                 __int64 comparand);
extern "C" __int64 _InterlockedExchange64(__int64 volatile* target, __int64 value);
extern "C" __int64 _InterlockedIncrement64(__int64 volatile* addend);
extern "C" __int64 _InterlockedExchangeAdd64(__int64 volatile* addend, __int64 value);
#pragma intrinsic(_InterlockedCompareExchange, _InterlockedExchange, _InterlockedIncrement, _InterlockedDecrement,  \
                  _InterlockedExchangeAdd, _InterlockedOr, _InterlockedCompareExchange64, _InterlockedExchange64,  \
                  _InterlockedIncrement64, _InterlockedExchangeAdd64)

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
