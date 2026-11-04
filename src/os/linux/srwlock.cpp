// The contended paths of the SRWLOCK twin (include/maicord/os/linux/srwlock.hpp). A thread that cannot take the lock
// sets the sleepers bit with a compare-exchange against the state it found blocking, then sleeps on the futex only
// while the state still has that value: a release that changes the state before the sleep makes the sleep return at
// once, and one that follows it sees the bit and wakes.
#include "maicord/os/linux/srwlock.hpp"
#include "maicord/sys/linux/calls.hpp"

namespace maicord {
namespace {

constexpr u32 kWakeAll = 0x7fffffffu;
// Critical sections are short: a contender spins this many times before it sleeps in the kernel.
constexpr u32 kSpins = 200;

u32 load(const volatile u32* word) noexcept {
    return __atomic_load_n(word, __ATOMIC_RELAXED);
}

// Marks `state` (the value just read, which blocks the caller) as having sleepers. True when the caller may sleep
// on that value; false when the state changed and the caller must look again.
bool announce_sleeper(SrwWords* lock, u32* state) noexcept {
    if ((*state & kSrwSleepers) != 0) {
        return true;
    }
    const u32 expected = *state;
    u32 current = expected;
    if (__atomic_compare_exchange_n(&lock->state, &current, expected | kSrwSleepers, false, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED)) {
        *state = expected | kSrwSleepers;
        return true;
    }
    *state = current;
    return false;
}

} // namespace

void srw_acquire_exclusive_slow(SrwWords* lock) noexcept {
    __atomic_fetch_add(&lock->writers, 1u, __ATOMIC_RELAXED);
    for (u32 spin = 0; spin < kSpins; ++spin) {
        u32 state = load(&lock->state);
        if ((state & (kSrwWriter | kSrwReaders)) == 0 &&
            __atomic_compare_exchange_n(&lock->state, &state, state | kSrwWriter, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            __atomic_fetch_sub(&lock->writers, 1u, __ATOMIC_RELAXED);
            return;
        }
        __builtin_ia32_pause();
    }
    for (;;) {
        u32 state = load(&lock->state);
        if ((state & (kSrwWriter | kSrwReaders)) == 0) {
            // Free; the sleepers bit, if set, stays for the release to see.
            if (__atomic_compare_exchange_n(&lock->state, &state, state | kSrwWriter, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                break;
            }
            continue;
        }
        if (announce_sleeper(lock, &state)) {
            sys_futex_wait(&lock->state, state, nullptr);
        }
    }
    __atomic_fetch_sub(&lock->writers, 1u, __ATOMIC_RELAXED);
}

void srw_acquire_shared_slow(SrwWords* lock) noexcept {
    for (u32 spin = 0; spin < kSpins; ++spin) {
        u32 state = load(&lock->state);
        if ((state & kSrwWriter) == 0 && load(&lock->writers) == 0 &&
            __atomic_compare_exchange_n(&lock->state, &state, state + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
        __builtin_ia32_pause();
    }
    for (;;) {
        u32 state = load(&lock->state);
        if ((state & kSrwWriter) == 0 && load(&lock->writers) == 0) {
            if (__atomic_compare_exchange_n(&lock->state, &state, state + 1, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                return;
            }
            continue;
        }
        if (announce_sleeper(lock, &state)) {
            sys_futex_wait(&lock->state, state, nullptr);
        }
    }
}

void srw_wake_sleepers(SrwWords* lock) noexcept {
    __atomic_fetch_and(&lock->state, ~kSrwSleepers, __ATOMIC_RELAXED);
    sys_futex_wake(&lock->state, kWakeAll);
}

} // namespace maicord
