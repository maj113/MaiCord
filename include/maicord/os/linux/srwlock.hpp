#pragma once

#include "maicord/core/base.hpp"
#include "maicord/os/linux/types.hpp"

// Slim reader/writer locks on Linux (D17): InitializeSRWLock, AcquireSRWLock{Exclusive,Shared} and
// ReleaseSRWLock{Exclusive,Shared} over a futex, with the Windows contract: zero-initialized is unlocked, locks
// are not recursive, a thread that waits for the lock sleeps in the kernel, and a waiting writer keeps new
// readers out. The 8 bytes of an SRWLOCK hold two words:
//
//   state    bit 31 a writer holds the lock, bit 30 a thread may be asleep on `state`, bits 0..29 the readers
//   writers  the number of writers inside AcquireSRWLockExclusive's slow path (readers wait while it is nonzero)
//
// The uncontended acquire is one compare-exchange and the uncontended release one atomic operation, without a
// system call; every release that sees the sleepers bit wakes all sleepers, who compete again.

namespace maicord {

// An SRWLOCK's two words. may_alias: the lock is declared as a pointer, and zero-initialized as one.
struct __attribute__((may_alias)) SrwWords {
    volatile u32 state;
    volatile u32 writers;
};
static_assert(sizeof(SrwWords) == sizeof(RTL_SRWLOCK));

inline constexpr u32 kSrwWriter = 0x80000000u;
inline constexpr u32 kSrwSleepers = 0x40000000u;
inline constexpr u32 kSrwReaders = 0x3fffffffu;

// The contended paths (src/os/linux/srwlock.cpp).
void srw_acquire_exclusive_slow(SrwWords* lock) noexcept;
void srw_acquire_shared_slow(SrwWords* lock) noexcept;
void srw_wake_sleepers(SrwWords* lock) noexcept;

} // namespace maicord

inline void InitializeSRWLock(PSRWLOCK lock) noexcept {
    auto* words = reinterpret_cast<maicord::SrwWords*>(lock);
    __atomic_store_n(&words->state, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&words->writers, 0u, __ATOMIC_RELAXED);
}

inline void AcquireSRWLockExclusive(PSRWLOCK lock) noexcept {
    auto* words = reinterpret_cast<maicord::SrwWords*>(lock);
    maicord::u32 expected = 0;
    if (!__atomic_compare_exchange_n(&words->state, &expected, maicord::kSrwWriter, false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)) {
        maicord::srw_acquire_exclusive_slow(words);
    }
}

inline void AcquireSRWLockShared(PSRWLOCK lock) noexcept {
    auto* words = reinterpret_cast<maicord::SrwWords*>(lock);
    maicord::u32 state = __atomic_load_n(&words->state, __ATOMIC_RELAXED);
    if ((state & maicord::kSrwWriter) != 0 || __atomic_load_n(&words->writers, __ATOMIC_RELAXED) != 0 ||
        !__atomic_compare_exchange_n(&words->state, &state, state + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        maicord::srw_acquire_shared_slow(words);
    }
}

inline void ReleaseSRWLockExclusive(PSRWLOCK lock) noexcept {
    auto* words = reinterpret_cast<maicord::SrwWords*>(lock);
    if ((__atomic_exchange_n(&words->state, 0u, __ATOMIC_RELEASE) & maicord::kSrwSleepers) != 0) {
        maicord::srw_wake_sleepers(words);
    }
}

inline void ReleaseSRWLockShared(PSRWLOCK lock) noexcept {
    auto* words = reinterpret_cast<maicord::SrwWords*>(lock);
    const maicord::u32 before = __atomic_fetch_sub(&words->state, 1u, __ATOMIC_RELEASE);
    // The last reader out wakes whoever waited for the lock to drain.
    if ((before & maicord::kSrwSleepers) != 0 && (before & maicord::kSrwReaders) == 1) {
        maicord::srw_wake_sleepers(words);
    }
}
