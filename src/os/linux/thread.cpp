// maicord/os/thread.hpp on Linux: glibc pthreads (the thread control block, TLS and errno that Mesa, libEGL and
// libwayland need exist only for threads pthread_create made) over a fixed table of 64 records, so starting a
// thread allocates nothing.
//
// A record's `state` runs 0 (the procedure runs) -> 1 (it returned) when somebody may still wait for it, or
// 0 -> 2 (detached: thread_close came first) when the thread cleans up after itself. Exactly one side
// frees the record: thread_close after state 1 (joining the finished thread), the thread itself after state 2.
#include "maicord/os/thread.hpp"
#include "maicord/os/linux/error.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "monotonic.hpp"

namespace maicord {

// Defined here so the public header leaves it opaque.
struct Thread {
    unsigned long id;
    ThreadProc procedure;
    void* argument;
    volatile u32 state;
    Waitable done;
    volatile u32 in_use;
};

namespace {

constexpr u32 kRecordCount = 64;
constexpr u32 kRunning = 0;
constexpr u32 kFinished = 1;
constexpr u32 kDetached = 2;
constexpr u32 kInfinite = 0xFFFFFFFFu;
// glibc keeps the static TLS block in the thread's stack mapping, and libraries the image dlopens (Mesa, PulseAudio)
// run on these threads, so a Windows-sized 64 KiB reservation is not enough.
constexpr u32 kMinimumStackBytes = 128u * 1024u;
constexpr u32 kPageBytes = 4096;

Thread g_records[kRecordCount];
SRWLOCK g_records_lock = SRWLOCK_INIT;

Thread* acquire_record() noexcept {
    Thread* found = nullptr;
    AcquireSRWLockExclusive(&g_records_lock);
    for (Thread& record : g_records) {
        if (__atomic_load_n(&record.in_use, __ATOMIC_RELAXED) == 0) {
            __atomic_store_n(&record.in_use, 1u, __ATOMIC_RELAXED);
            found = &record;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_records_lock);
    return found;
}

void release_record(Thread* record) noexcept {
    waitable_close(record->done);
    record->done = nullptr;
    __atomic_store_n(&record->in_use, 0u, __ATOMIC_RELEASE);
}

void* thread_main(void* argument) noexcept {
    auto* record = static_cast<Thread*>(argument);
    record->procedure(record->argument);
    event_set(record->done);
    u32 expected = kRunning;
    if (__atomic_compare_exchange_n(&record->state, &expected, kFinished, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        // Nothing of the record is touched after this wake: its waiter or closer may free it at once.
        sys_futex_wake(&record->state, 0x7fffffffu);
    } else {
        release_record(record);
    }
    return nullptr;
}

bool finished(const Thread* thread) noexcept {
    return __atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) == kFinished;
}

// Waits until `deadline` (monotonic nanoseconds; 0 = never) for the thread to return.
bool wait_until(Thread* thread, i64 deadline) noexcept {
    for (;;) {
        if (finished(thread)) {
            return true;
        }
        TimeSpec timeout{};
        const TimeSpec* limit = nullptr;
        if (deadline != 0) {
            const i64 remaining = deadline - monotonic_nanoseconds();
            if (remaining <= 0) {
                return false;
            }
            timeout = timespec_from_nanoseconds(remaining);
            limit = &timeout;
        }
        sys_futex_wait(&thread->state, kRunning, limit);
    }
}

i64 deadline_after(u32 milliseconds) noexcept {
    return milliseconds == kInfinite ? 0 : monotonic_nanoseconds() + static_cast<i64>(milliseconds) * 1'000'000;
}

} // namespace

// Creation and the processor count run once per thread or worker pool: compiled for size.
__attribute__((minsize)) Thread* thread_start(ThreadProc procedure, void* argument, u32 stack_reserve_bytes,
                                              u32* native_error) noexcept {
    Thread* record = acquire_record();
    if (record == nullptr) {
        if (native_error) {
            *native_error = ERROR_NOT_ENOUGH_MEMORY;
        }
        return nullptr;
    }
    record->procedure = procedure;
    record->argument = argument;
    record->state = kRunning;
    record->done = event_create(true, false);
    const u32 stack = stack_reserve_bytes < kMinimumStackBytes ? kMinimumStackBytes : stack_reserve_bytes;
    PthreadAttribute attribute;
    int error = pthread_attr_init(&attribute);
    if (error == 0) {
        error = pthread_attr_setstacksize(&attribute, (stack + kPageBytes - 1) & ~(kPageBytes - 1));
        if (error == 0) {
            error = pthread_create(&record->id, &attribute, thread_main, record);
        }
        pthread_attr_destroy(&attribute);
    }
    if (error != 0) {
        release_record(record);
        if (native_error) {
            *native_error = win32_error_from_errno(error);
        }
        return nullptr;
    }
    return record;
}

bool thread_wait(Thread* thread, u32 timeout_ms) noexcept {
    return wait_until(thread, deadline_after(timeout_ms));
}

bool threads_wait_all(Thread* const* threads, u32 count, u32 timeout_ms) noexcept {
    if (threads == nullptr || count == 0 || count > kRecordCount) {
        return false;
    }
    const i64 deadline = deadline_after(timeout_ms);
    for (u32 index = 0; index < count; ++index) {
        if (!wait_until(threads[index], deadline)) {
            return false;
        }
    }
    return true;
}

void thread_close(Thread* thread) noexcept {
    // Read before the state changes: a detached thread may free its record, and a new thread reuse it, at once.
    const unsigned long id = thread->id;
    u32 expected = kRunning;
    if (__atomic_compare_exchange_n(&thread->state, &expected, kDetached, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        // Still running: it frees its own record when the procedure returns.
        pthread_detach(id);
        return;
    }
    // Finished: reclaim the thread's stack, then the record.
    pthread_join(thread->id, nullptr);
    release_record(thread);
}

// The record's own manual-reset event, set by thread_main just before it publishes the end: it lives until the
// record is released, so a waiter holds it exactly as long as it holds the Thread.
Waitable thread_waitable(Thread* thread) noexcept {
    return thread->done;
}

void thread_cancel_synchronous_io(Thread*) noexcept {}

bool thread_com_enter() noexcept {
    return true;
}

void thread_com_leave() noexcept {}

void thread_boost_realtime() noexcept {
    const SchedParam parameter{1};
    sys_sched_setscheduler(0, kSchedRoundRobin, &parameter);
}

void thread_sleep(u32 milliseconds) noexcept {
    const i64 deadline = monotonic_nanoseconds() + static_cast<i64>(milliseconds) * 1'000'000;
    for (;;) {
        const i64 remaining = deadline - monotonic_nanoseconds();
        if (remaining <= 0) {
            return;
        }
        const TimeSpec request = timespec_from_nanoseconds(remaining);
        sys_nanosleep(&request);
    }
}

__attribute__((minsize)) u32 processor_count() noexcept {
    u64 mask[16]{};
    if (syscall_failed(sys_sched_getaffinity(0, sizeof(mask), mask))) {
        return 1;
    }
    u32 count = 0;
    for (const u64 word : mask) {
        count += static_cast<u32>(__builtin_popcountll(word));
    }
    return count == 0 ? 1 : count;
}

} // namespace maicord
