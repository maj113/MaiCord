#pragma once

#include "maicord/os/linux/types.hpp"

// The Interlocked* family on Linux (D17), with the Windows contract: every operation is a full barrier, and each
// returns what the Windows function does (the initial value, except Increment, Decrement and InterlockedAdd64,
// which return the new one). Windows takes these from compiler intrinsics (win32_kernel.hpp); here they are
// clang's __atomic builtins, which on x86-64 are the same lock-prefixed instructions.

inline LONG InterlockedExchange(volatile LONG* target, LONG value) noexcept {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}
inline LONG InterlockedCompareExchange(volatile LONG* destination, LONG exchange, LONG comparand) noexcept {
    __atomic_compare_exchange_n(destination, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
inline LONG InterlockedIncrement(volatile LONG* addend) noexcept {
    return __atomic_add_fetch(addend, 1, __ATOMIC_SEQ_CST);
}
inline LONG InterlockedDecrement(volatile LONG* addend) noexcept {
    return __atomic_sub_fetch(addend, 1, __ATOMIC_SEQ_CST);
}
inline LONG InterlockedExchangeAdd(volatile LONG* addend, LONG value) noexcept {
    return __atomic_fetch_add(addend, value, __ATOMIC_SEQ_CST);
}
inline LONG InterlockedOr(volatile LONG* destination, LONG mask) noexcept {
    return __atomic_fetch_or(destination, mask, __ATOMIC_SEQ_CST);
}

inline LONG64 InterlockedExchange64(volatile LONG64* target, LONG64 value) noexcept {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}
inline LONG64 InterlockedCompareExchange64(volatile LONG64* destination, LONG64 exchange, LONG64 comparand) noexcept {
    __atomic_compare_exchange_n(destination, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
inline LONG64 InterlockedIncrement64(volatile LONG64* addend) noexcept {
    return __atomic_add_fetch(addend, 1, __ATOMIC_SEQ_CST);
}
inline LONG64 InterlockedExchangeAdd64(volatile LONG64* addend, LONG64 value) noexcept {
    return __atomic_fetch_add(addend, value, __ATOMIC_SEQ_CST);
}
inline LONG64 InterlockedAdd64(volatile LONG64* addend, LONG64 value) noexcept {
    return __atomic_add_fetch(addend, value, __ATOMIC_SEQ_CST);
}

inline PVOID InterlockedExchangePointer(PVOID volatile* target, PVOID value) noexcept {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}
inline PVOID InterlockedCompareExchangePointer(PVOID volatile* destination, PVOID exchange, PVOID comparand) noexcept {
    __atomic_compare_exchange_n(destination, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
