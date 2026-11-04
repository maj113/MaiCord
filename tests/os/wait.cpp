#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "../support/test_output.hpp"
#if !defined(_WIN32)
#include "maicord/sys/linux/calls.hpp"
#endif

// Events, waitable timers and waits (maicord/os/wait.hpp): the semantics shared code relies on, the same on
// Windows (the Win32 calls) and Linux (eventfd, timerfd, ppoll). No test measures time: waits that must expire use
// a zero timeout, waits that must succeed use a generous limit.

namespace maicord {
namespace {

constexpr u32 kLimit = 5000;
u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

void test_auto_reset_event() noexcept {
    const Waitable event = event_create(false, false);
    check(event != nullptr, "event_create");
    check(wait_one(event, 0) == WAIT_TIMEOUT, "auto: unsignaled times out");
    check(event_set(event) && event_set(event), "auto: set (twice)");
    check(wait_one(event, kLimit) == WAIT_OBJECT_0, "auto: set event is returned");
    check(wait_one(event, 0) == WAIT_TIMEOUT, "auto: the wait consumed it, even set twice");
    check(event_set(event) && event_reset(event) && wait_one(event, 0) == WAIT_TIMEOUT, "auto: reset clears");
    waitable_close(event);

    const Waitable signaled = event_create(false, true);
    check(wait_one(signaled, 0) == WAIT_OBJECT_0 && wait_one(signaled, 0) == WAIT_TIMEOUT, "auto: initially signaled");
    waitable_close(signaled);
}

void test_manual_reset_event() noexcept {
    const Waitable event = event_create(true, false);
    check(wait_one(event, 0) == WAIT_TIMEOUT, "manual: unsignaled times out");
    check(event_set(event), "manual: set");
    check(wait_one(event, 0) == WAIT_OBJECT_0 && wait_one(event, 0) == WAIT_OBJECT_0, "manual: stays signaled");
    check(event_reset(event) && wait_one(event, 0) == WAIT_TIMEOUT, "manual: reset");
    waitable_close(event);
    const Waitable signaled = event_create(true, true);
    check(wait_one(signaled, 0) == WAIT_OBJECT_0 && wait_one(signaled, 0) == WAIT_OBJECT_0, "manual: initially signaled");
    waitable_close(signaled);
}

void test_wait_any() noexcept {
    Waitable events[3];
    for (Waitable& event : events) {
        event = event_create(false, false);
    }
    check(wait_any(events, 3, 0) == WAIT_TIMEOUT, "wait_any: nothing signaled");
    event_set(events[2]);
    event_set(events[1]);
    check(wait_any(events, 3, kLimit) == WAIT_OBJECT_0 + 1, "wait_any: the lowest ready index");
    check(wait_any(events, 3, kLimit) == WAIT_OBJECT_0 + 2, "wait_any: the one before was consumed");
    check(wait_any(events, 3, 0) == WAIT_TIMEOUT, "wait_any: both consumed");

    // A manual-reset waitable in front keeps winning; the auto-reset one behind it waits its turn.
    const Waitable manual = event_create(true, true);
    const Waitable mixed[2]{manual, events[0]};
    event_set(events[0]);
    check(wait_any(mixed, 2, 0) == WAIT_OBJECT_0 && wait_any(mixed, 2, 0) == WAIT_OBJECT_0, "wait_any: manual wins");
    event_reset(manual);
    check(wait_any(mixed, 2, 0) == WAIT_OBJECT_0 + 1 && wait_any(mixed, 2, 0) == WAIT_TIMEOUT,
          "wait_any: auto-reset behind it is returned once");

    check(wait_any(events, 0, 0) == WAIT_FAILED, "wait_any: no waitables fails");
    check(wait_one(nullptr, 0) == WAIT_FAILED, "wait_one: null fails");
    waitable_close(manual);
    for (const Waitable event : events) {
        waitable_close(event);
    }
}

void test_wait_limit() noexcept {
    Waitable events[kWaitMaximumCount + 1];
    for (Waitable& event : events) {
        event = event_create(false, false);
    }
    check(wait_any(events, kWaitMaximumCount, 0) == WAIT_TIMEOUT, "limit: 64 waitables");
    event_set(events[kWaitMaximumCount - 1]);
    check(wait_any(events, kWaitMaximumCount, 0) == WAIT_OBJECT_0 + kWaitMaximumCount - 1, "limit: the last of 64");
    check(wait_any(events, kWaitMaximumCount + 1, 0) == WAIT_FAILED, "limit: 65 waitables fail");
    const WaitManyResult too_many = wait_many(events, kWaitMaximumCount + 1, 0, 0);
    check(too_many.failed && too_many.ready_mask == 0, "limit: wait_many of 65 fails");
    for (const Waitable event : events) {
        waitable_close(event);
    }
}

void test_wait_many() noexcept {
    Waitable events[4]{event_create(false, false), event_create(true, false), event_create(false, false),
                       event_create(false, false)};
    WaitManyResult none = wait_many(events, 4, 0, 0);
    check(none.timed_out && !none.failed && none.ready_mask == 0, "wait_many: timeout");
    event_set(events[0]);
    event_set(events[1]);
    event_set(events[3]);
    const WaitManyResult all = wait_many(events, 4, kLimit, 0);
    check(!all.timed_out && !all.failed && all.ready_mask == 0b1011, "wait_many: every signaled waitable");
    const WaitManyResult again = wait_many(events, 4, 0, 0);
    check(!again.timed_out && !again.failed && again.ready_mask == 0b0010,
          "wait_many: auto-reset consumed, manual-reset kept");
    event_reset(events[1]);
    event_set(events[2]);
    const WaitManyResult tolerant = wait_many(events, 4, kLimit, 10);
    check(!tolerant.failed && tolerant.ready_mask == 0b0100, "wait_many: with a tolerance");
    check(wait_many(events, 0, 0, 0).failed, "wait_many: no waitables fails");
    for (const Waitable event : events) {
        waitable_close(event);
    }
}

void test_timer() noexcept {
    const Waitable timer = timer_create_waitable(false, false);
    check(timer != nullptr, "timer_create_waitable");
    check(wait_one(timer, 0) == WAIT_TIMEOUT, "timer: not armed");
    check(timer_arm(timer, 1, 0), "timer: arm");
    check(wait_one(timer, kLimit) == WAIT_OBJECT_0, "timer: fires");
    check(wait_one(timer, 0) == WAIT_TIMEOUT, "timer: auto-reset consumed by the wait");
    check(timer_arm(timer, 1, 0) && timer_arm(timer, 3'600'000, 0), "timer: re-arm replaces the earlier one");
    check(wait_one(timer, 20) == WAIT_TIMEOUT, "timer: re-armed far away");
    check(timer_cancel(timer) && timer_arm(timer, 0, 0) && wait_one(timer, kLimit) == WAIT_OBJECT_0,
          "timer: cancel, then arm for now");
    check(timer_cancel(timer), "timer: cancel an armed timer");
    waitable_close(timer);

    const Waitable manual = timer_create_waitable(true, false);
    check(manual != nullptr && timer_arm(manual, 1, 5), "manual timer: arm with a tolerance");
    check(wait_one(manual, kLimit) == WAIT_OBJECT_0 && wait_one(manual, 0) == WAIT_OBJECT_0,
          "manual timer: stays signaled");
    check(timer_cancel(manual), "manual timer: cancel");
    check(timer_arm(manual, 3'600'000, 0) && wait_one(manual, 0) == WAIT_TIMEOUT, "manual timer: arming clears it");
    check(timer_cancel(manual), "manual timer: cancel an armed timer");
    waitable_close(manual);

    // A high-resolution timer is armed without a tolerance.
    const Waitable fine = timer_create_waitable(false, true);
    check(fine != nullptr && timer_arm(fine, 1, 0) && wait_one(fine, kLimit) == WAIT_OBJECT_0,
          "high-resolution timer: fires");
    waitable_close(fine);
}

void test_loop_wake() noexcept {
    const Waitable wake = loop_wake_waitable();
    check(wake != nullptr && wake == loop_wake_waitable(), "loop wake: one process-wide waitable");
    check(wait_one(wake, 0) == WAIT_TIMEOUT, "loop wake: quiet");
    loop_wake();
    loop_wake();
    check(wait_one(wake, kLimit) == WAIT_OBJECT_0 && wait_one(wake, 0) == WAIT_TIMEOUT, "loop wake: auto-reset");
}

struct Signal {
    Waitable event;
    Waitable done;
};

u32 signal_after_wait(void* argument) noexcept {
    auto* signal = static_cast<Signal*>(argument);
    wait_one(signal->event, kLimit);
    event_set(signal->done);
    return 0;
}

void test_wake_from_thread() noexcept {
    Signal signal{event_create(false, false), event_create(false, false)};
    u32 error = 0;
    Thread* worker = thread_start(signal_after_wait, &signal, 64 * 1024, &error);
    check(worker != nullptr, "wake: thread starts");
    if (worker != nullptr) {
        check(wait_one(signal.done, 20) == WAIT_TIMEOUT, "wake: the thread waits for the event");
        event_set(signal.event);
        check(wait_one(signal.done, kLimit) == WAIT_OBJECT_0, "wake: another thread's set wakes a blocked wait");
        thread_wait(worker, kLimit);
        thread_close(worker);
    }
    waitable_close(signal.event);
    waitable_close(signal.done);
}

#if !defined(_WIN32)
// A descriptor the caller owns is a manual-reset, level-triggered waitable that waits never consume.
void test_borrowed_descriptor() noexcept {
    const long fd = sys_eventfd(0, kEventfdNonBlock | kEventfdCloseOnExec);
    check(!syscall_failed(fd), "borrowed: eventfd");
    const Waitable borrowed = waitable_borrow_fd(static_cast<int>(fd));
    check(borrowed != nullptr && waitable_borrow_fd(-1) == nullptr, "borrowed: token");
    check(wait_one(borrowed, 0) == WAIT_TIMEOUT, "borrowed: not readable");
    const u64 one = 1;
    sys_write(static_cast<int>(fd), &one, sizeof(one));
    check(wait_one(borrowed, 0) == WAIT_OBJECT_0 && wait_one(borrowed, 0) == WAIT_OBJECT_0,
          "borrowed: level-triggered, never consumed");
    const WaitManyResult many = wait_many(&borrowed, 1, 0, 0);
    check(many.ready_mask == 1, "borrowed: wait_many reports it");
    u64 counter = 0;
    sys_read(static_cast<int>(fd), &counter, sizeof(counter));
    check(wait_one(borrowed, 0) == WAIT_TIMEOUT, "borrowed: drained by its owner");
    waitable_close(borrowed); // forgets the token only
    check(!syscall_failed(sys_write(static_cast<int>(fd), &one, sizeof(one))), "borrowed: close left the descriptor open");
    sys_close(static_cast<int>(fd));
}
#endif

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_os_wait_entry) {
    using namespace maicord;
    test_auto_reset_event();
    test_manual_reset_event();
    test_wait_any();
    test_wait_limit();
    test_wait_many();
    test_timer();
    test_loop_wake();
    test_wake_from_thread();
#if !defined(_WIN32)
    test_borrowed_descriptor();
#endif
    test_exit(g_failures, "os_wait");
}
