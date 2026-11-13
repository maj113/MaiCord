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

// What a procedure did: it copies `input` to `output`.
struct Cell {
    volatile Atomic32 input;
    volatile Atomic32 output;
};

u32 copy_input(void* argument) noexcept {
    auto* cell = static_cast<Cell*>(argument);
    write_release(&cell->output, read_acquire(&cell->input));
    return 0;
}

// Waits for `release`, then raises `ran` and `finished` (when given).
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
    return 0;
}

void test_start_and_wait() noexcept {
    u32 error = 0xFFFFFFFFu;
    Cell cell{7, 0};
    Thread* thread = thread_start(copy_input, &cell, 64 * 1024, &error);
    check(thread != nullptr, "start");
    if (thread == nullptr) {
        return;
    }
    check(thread_wait(thread, kLimit), "wait returns once the thread returned");
    check(read_acquire(&cell.output) == 7, "the procedure ran with its argument");
    check(thread_wait(thread, 0), "a finished thread stays waited for");
    thread_close(thread);
    Cell plain_cell{1, 0};
    Thread* plain = thread_start(copy_input, &plain_cell, 0, nullptr);
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
        check(read_acquire(&blocked.ran) == 0, "blocked: it has not passed its wait");
        event_set(blocked.release);
        check(thread_wait(thread, kLimit) && read_acquire(&blocked.ran) == 1, "blocked: released thread returns");
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
        Cell cell{static_cast<Atomic32>(index), -1};
        Thread* thread = thread_start(copy_input, &cell, 64 * 1024, nullptr);
        all = all && thread != nullptr && thread_wait(thread, kLimit) && read_acquire(&cell.output) == static_cast<Atomic32>(index);
        total += static_cast<u32>(read_acquire(&cell.output));
        if (thread != nullptr) {
            thread_close(thread);
        }
    }
    check(all && total == 199 * 200 / 2, "200 threads one after another");
}

void test_helpers() noexcept {
    check(processor_count() >= 1, "processor count");
    check(current_thread_id() != 0, "thread id");
}

u32 enter_and_leave(void* argument) noexcept {
    const bool entered = thread_com_enter();
    if (entered)
        thread_com_leave();
    write_release(static_cast<volatile Atomic32*>(argument), entered ? 1 : 0);
    return 0;
}

// thread_waitable is signaled by the thread's end, so a wait set can hold a thread beside events; the COM and
// synchronous-I/O hooks are inert where they have nothing to do.
void test_thread_waitable() noexcept {
    Blocked blocked{event_create(true, false), nullptr, 0};
    Thread* thread = thread_start(block_until_released, &blocked, 64 * 1024, nullptr);
    check(thread != nullptr && thread_waitable(thread) != nullptr, "thread_waitable of a running thread");
    if (thread == nullptr) {
        waitable_close(blocked.release);
        return;
    }
    check(wait_one(thread_waitable(thread), 20) == WAIT_TIMEOUT, "thread_waitable is not signaled while it runs");
    thread_cancel_synchronous_io(thread);
    event_set(blocked.release);
    check(wait_one(thread_waitable(thread), kLimit) == WAIT_OBJECT_0, "thread_waitable is signaled when it ends");
    check(read_acquire(&blocked.ran) == 1, "thread_waitable is signaled after the procedure's work");
    thread_close(thread);
    waitable_close(blocked.release);
    volatile Atomic32 entered = 0;
    Thread* com = thread_start(enter_and_leave, const_cast<Atomic32*>(&entered), 64 * 1024, nullptr);
    check(com != nullptr && thread_wait(com, kLimit) && read_acquire(&entered) == 1,
          "thread_com_enter and thread_com_leave");
    if (com != nullptr)
        thread_close(com);
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
    test_thread_waitable();
    test_exit(g_failures, "os_thread");
}
