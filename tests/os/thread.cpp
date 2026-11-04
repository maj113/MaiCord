#include "maicord/core/interlocked.hpp"
#include "maicord/core/intrinsics.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "../support/test_output.hpp"

// Threads (maicord/os/thread.hpp): start, wait with a timeout, close a thread that is still running, wait for
// several, record reuse. Waits that must expire use a short timeout on a thread known to be blocked; waits that
// must succeed use a generous limit.

namespace maicord {
namespace {

constexpr u32 kLimit = 5000;
u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

u32 return_argument(void* argument) noexcept {
    return static_cast<u32>(reinterpret_cast<usize>(argument));
}

// Waits for `release`, then raises `finished` (when given) and returns 42.
struct Blocked {
    Waitable release;
    Waitable finished;
    volatile Atomic32 ran;
};

u32 block_until_released(void* argument) noexcept {
    auto* blocked = static_cast<Blocked*>(argument);
    wait_one(blocked->release, kLimit);
    write_release(&blocked->ran, 1);
    if (blocked->finished != nullptr) {
        event_set(blocked->finished);
    }
    return 42;
}

void test_start_and_wait() noexcept {
    u32 error = 0xFFFFFFFFu;
    Thread* thread = thread_start(return_argument, reinterpret_cast<void*>(usize{7}), 64 * 1024, &error);
    check(thread != nullptr, "start");
    if (thread == nullptr) {
        return;
    }
    check(thread_wait(thread, kLimit), "wait returns once the thread returned");
    u32 code = 0;
    check(thread_exit_code(thread, &code) && code == 7, "exit code");
    check(thread_wait(thread, 0), "a finished thread stays waited for");
    thread_close(thread);
    Thread* plain = thread_start(return_argument, nullptr, 0, nullptr);
    check(plain != nullptr && thread_wait(plain, kLimit), "start without an error out or a stack size");
    if (plain != nullptr) {
        thread_close(plain);
    }
}

void test_wait_times_out_while_running() noexcept {
    Blocked blocked{event_create(false, false), event_create(false, false), 0};
    Thread* thread = thread_start(block_until_released, &blocked, 64 * 1024, nullptr);
    check(thread != nullptr, "blocked: start");
    if (thread != nullptr) {
        check(!thread_wait(thread, 20), "blocked: a running thread times out");
        u32 code = 0;
        check(!thread_exit_code(thread, &code), "blocked: no exit code yet");
        event_set(blocked.release);
        check(thread_wait(thread, kLimit) && read_acquire(&blocked.ran) == 1, "blocked: released thread returns");
        check(thread_exit_code(thread, &code) && code == 42, "blocked: its code");
        thread_close(thread);
    }
    waitable_close(blocked.release);
    waitable_close(blocked.finished);
}

void test_close_running_thread() noexcept {
    Blocked blocked{event_create(false, false), event_create(false, false), 0};
    Thread* thread = thread_start(block_until_released, &blocked, 64 * 1024, nullptr);
    check(thread != nullptr, "close: start");
    if (thread != nullptr) {
        thread_close(thread);
        check(read_acquire(&blocked.ran) == 0, "close: the thread keeps running");
        event_set(blocked.release);
        check(wait_one(blocked.finished, kLimit) == WAIT_OBJECT_0 && read_acquire(&blocked.ran) == 1,
              "close: the closed thread finishes by itself");
    }
    waitable_close(blocked.release);
    waitable_close(blocked.finished);
}

void test_wait_all() noexcept {
    Blocked blocked[3]{};
    Thread* threads[3]{};
    bool started = true;
    for (u32 index = 0; index < 3; ++index) {
        blocked[index].release = event_create(false, false);
        threads[index] = thread_start(block_until_released, &blocked[index], 64 * 1024, nullptr);
        started = started && threads[index] != nullptr;
    }
    check(started, "wait all: start three");
    if (started) {
        event_set(blocked[0].release);
        event_set(blocked[2].release);
        check(!threads_wait_all(threads, 3, 20), "wait all: one thread still blocked");
        event_set(blocked[1].release);
        check(threads_wait_all(threads, 3, kLimit), "wait all: all three returned");
        check(!threads_wait_all(threads, 0, 0), "wait all: none is a failure");
    }
    for (u32 index = 0; index < 3; ++index) {
        if (threads[index] != nullptr) {
            thread_close(threads[index]);
        }
        waitable_close(blocked[index].release);
    }
}

// More starts than the Linux table has records: every record is reused.
void test_many_threads() noexcept {
    u32 total = 0;
    bool all = true;
    for (u32 index = 0; index < 200; ++index) {
        Thread* thread = thread_start(return_argument, reinterpret_cast<void*>(usize{index}), 64 * 1024, nullptr);
        u32 code = 0;
        all = all && thread != nullptr && thread_wait(thread, kLimit) && thread_exit_code(thread, &code) && code == index;
        total += code;
        if (thread != nullptr) {
            thread_close(thread);
        }
    }
    check(all && total == 199 * 200 / 2, "200 threads one after another");
}

u32 boost_and_return(void*) noexcept {
    thread_boost_realtime();
    return 1;
}

void test_helpers() noexcept {
    check(processor_count() >= 1, "processor count");
    thread_sleep(1);
    thread_sleep(0);
    Thread* thread = thread_start(boost_and_return, nullptr, 64 * 1024, nullptr);
    u32 code = 0;
    check(thread != nullptr && thread_wait(thread, kLimit) && thread_exit_code(thread, &code) && code == 1,
          "boost is best effort and returns");
    if (thread != nullptr) {
        thread_close(thread);
    }
    check(current_thread_id() != 0, "thread id");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_os_thread_entry) {
    using namespace maicord;
    test_start_and_wait();
    test_wait_times_out_while_running();
    test_close_running_thread();
    test_wait_all();
    test_many_threads();
    test_helpers();
    test_exit(g_failures, "os_thread");
}
