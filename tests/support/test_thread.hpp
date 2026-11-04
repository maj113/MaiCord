#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/interlocked.hpp"
#include "maicord/core/time.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_kernel.hpp"
#else
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/libc.hpp"
#endif

// Threads and processor affinity for the tests that race two threads, on Windows (CreateThread,
// SetThreadAffinityMask) and Linux (pthread_create, sched_setaffinity).

namespace maicord {

using TestThreadFunction = u32 (*)(void*);

struct TestThread {
#if defined(_WIN32)
    HANDLE handle;
#else
    unsigned long handle;
    TestThreadFunction function;
    void* argument;
    volatile Atomic32 finished;
    u32 result;
#endif
};

#if !defined(_WIN32)
inline void* test_thread_main(void* argument) {
    auto* thread = static_cast<TestThread*>(argument);
    thread->result = thread->function(thread->argument);
    write_release(&thread->finished, 1);
    return nullptr;
}
#endif

// Starts a thread running function(argument). The thread object must stay in place until test_thread_finish.
inline bool test_thread_start(TestThread* thread, TestThreadFunction function, void* argument) noexcept {
#if defined(_WIN32)
    thread->handle = CreateThread(nullptr, 64 * 1024, reinterpret_cast<LPTHREAD_START_ROUTINE>(function), argument,
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    return thread->handle != nullptr;
#else
    thread->function = function;
    thread->argument = argument;
    thread->finished = 0;
    thread->result = 1;
    return pthread_create(&thread->handle, nullptr, test_thread_main, thread) == 0;
#endif
}

inline void test_thread_yield() noexcept {
#if defined(_WIN32)
    SwitchToThread();
#else
    sys_sched_yield();
#endif
}

// Waits up to `milliseconds` for the thread to return, then reclaims it. True when it returned `*result`; a thread
// still running at the deadline is abandoned (detached) and false is returned.
inline bool test_thread_finish(TestThread* thread, u32 milliseconds, u32* result) noexcept {
#if defined(_WIN32)
    DWORD code = 1;
    const bool finished = WaitForSingleObject(thread->handle, milliseconds) == WAIT_OBJECT_0 &&
                          GetExitCodeThread(thread->handle, &code);
    CloseHandle(thread->handle);
    *result = code;
    return finished;
#else
    const u64 deadline = clock_monotonic_milliseconds() + milliseconds;
    while (!read_acquire(&thread->finished) && clock_monotonic_milliseconds() < deadline) {
        sys_sched_yield();
    }
    if (!read_acquire(&thread->finished)) {
        pthread_detach(thread->handle);
        *result = 1;
        return false;
    }
    pthread_join(thread->handle, nullptr);
    *result = thread->result;
    return true;
#endif
}

// The processors this process may run on, as a mask of the first 64.
inline u64 test_allowed_processors() noexcept {
#if defined(_WIN32)
    DWORD_PTR allowed = 0, system = 0;
    return GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) ? allowed : 0;
#else
    u64 mask[16]{};
    return syscall_failed(sys_sched_getaffinity(0, sizeof(mask), mask)) ? 0 : mask[0];
#endif
}

// Restricts the calling thread to `mask`; returns the mask it had, or 0 when that failed.
inline u64 test_pin_current_thread(u64 mask) noexcept {
#if defined(_WIN32)
    return SetThreadAffinityMask(GetCurrentThread(), mask);
#else
    u64 previous[16]{};
    if (syscall_failed(sys_sched_getaffinity(0, sizeof(previous), previous)) ||
        syscall_failed(sys_sched_setaffinity(0, sizeof(mask), &mask))) {
        return 0;
    }
    return previous[0];
#endif
}

} // namespace maicord
