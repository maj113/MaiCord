#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "../support/test_output.hpp"

// SRW locks (AcquireSRWLock*, ReleaseSRWLock*): the Windows ones and the Linux futex twin
// (include/maicord/os/linux/srwlock.hpp) under contention. Everything asserted is a count or an invariant, never a
// time: writers keep two counters equal, readers check that they never see them differ, and the totals are exact.

namespace maicord {
namespace {

constexpr u32 kLimit = 10000;
constexpr u32 kWriters = 3;
constexpr u32 kReaders = 3;
constexpr u32 kRounds = 5000;

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

struct Shared {
    SRWLOCK lock;
    // Both are changed only under the exclusive lock, one after the other, so a reader sees them equal.
    u64 first;
    u64 second;
    volatile Atomic32 torn;
};

u32 write_rounds(void* argument) noexcept {
    auto* shared = static_cast<Shared*>(argument);
    for (u32 round = 0; round < kRounds; ++round) {
        AcquireSRWLockExclusive(&shared->lock);
        ++shared->first;
        ++shared->second;
        ReleaseSRWLockExclusive(&shared->lock);
    }
    return 0;
}

u32 read_rounds(void* argument) noexcept {
    auto* shared = static_cast<Shared*>(argument);
    for (u32 round = 0; round < kRounds; ++round) {
        AcquireSRWLockShared(&shared->lock);
        if (shared->first != shared->second) {
            InterlockedExchange(&shared->torn, 1);
        }
        ReleaseSRWLockShared(&shared->lock);
    }
    return 0;
}

void test_contention() noexcept {
    Shared shared{};
    InitializeSRWLock(&shared.lock);
    Thread* threads[kWriters + kReaders]{};
    bool started = true;
    for (u32 index = 0; index < kWriters + kReaders; ++index) {
        threads[index] = thread_start(index < kWriters ? write_rounds : read_rounds, &shared, 64 * 1024, nullptr);
        started = started && threads[index] != nullptr;
    }
    check(started, "contention: threads start");
    check(threads_wait_all(threads, kWriters + kReaders, kLimit), "contention: every thread finished");
    for (Thread* thread : threads) {
        if (thread != nullptr) {
            thread_close(thread);
        }
    }
    check(shared.first == u64{kWriters} * kRounds && shared.second == shared.first, "contention: no lost update");
    check(read_acquire(&shared.torn) == 0, "contention: a reader never saw a half-done update");
}

// Main holds the lock; `entered` says whether the other thread got in, `done` that it finished.
struct Gate {
    SRWLOCK lock;
    bool exclusive;
    volatile Atomic32 entered;
    Waitable finished;
};

u32 acquire_once(void* argument) noexcept {
    auto* gate = static_cast<Gate*>(argument);
    if (gate->exclusive) {
        AcquireSRWLockExclusive(&gate->lock);
        write_release(&gate->entered, 1);
        ReleaseSRWLockExclusive(&gate->lock);
    } else {
        AcquireSRWLockShared(&gate->lock);
        write_release(&gate->entered, 1);
        ReleaseSRWLockShared(&gate->lock);
    }
    event_set(gate->finished);
    return 0;
}

// What blocks what: the lock is held in one mode and a thread asks for the same or the other.
void run_gate(bool hold_exclusive, bool ask_exclusive, bool expect_blocked, const char* label) noexcept {
    Gate gate{};
    InitializeSRWLock(&gate.lock);
    gate.exclusive = ask_exclusive;
    gate.finished = event_create(false, false);
    if (hold_exclusive) {
        AcquireSRWLockExclusive(&gate.lock);
    } else {
        AcquireSRWLockShared(&gate.lock);
    }
    Thread* thread = thread_start(acquire_once, &gate, 64 * 1024, nullptr);
    check(thread != nullptr, label);
    if (thread != nullptr) {
        const u32 waited = wait_one(gate.finished, 50);
        if (expect_blocked) {
            check(waited == WAIT_TIMEOUT && read_acquire(&gate.entered) == 0, label);
        } else {
            check(waited == WAIT_OBJECT_0 && read_acquire(&gate.entered) == 1, label);
        }
    }
    if (hold_exclusive) {
        ReleaseSRWLockExclusive(&gate.lock);
    } else {
        ReleaseSRWLockShared(&gate.lock);
    }
    if (thread != nullptr) {
        // A blocked thread signals once it has the lock; one that got in earlier signaled before.
        check(!expect_blocked || wait_one(gate.finished, kLimit) == WAIT_OBJECT_0, label);
        check(thread_wait(thread, kLimit) && read_acquire(&gate.entered) == 1, label);
        thread_close(thread);
    }
    waitable_close(gate.finished);
}

void test_modes() noexcept {
    run_gate(false, false, false, "modes: readers share the lock");
    run_gate(false, true, true, "modes: a writer waits for a reader, then gets in");
    run_gate(true, false, true, "modes: a reader waits for a writer, then gets in");
    run_gate(true, true, true, "modes: a writer waits for a writer, then gets in");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_os_srwlock_entry) {
    using namespace maicord;
    test_modes();
    test_contention();
    test_exit(g_failures, "os_srwlock");
}
