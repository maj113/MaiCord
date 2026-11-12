// maicord/os/wait.hpp on Linux: events and timers are eventfds and timerfds, a wait is one ppoll over them.
//
// A Waitable is the token (fd + 1) | kManual | kOwned: nullptr (0) is "none" and (HANDLE)-1 "invalid", as for a
// Windows HANDLE. An auto-reset waitable is consumed by reading its counter, which zeroes an eventfd and returns the
// expirations of a timerfd; a manual-reset or borrowed one is never read, so it stays readable until it is reset
// or (for a borrowed descriptor) drained by its owner.
#include "maicord/os/wait.hpp"
#include "maicord/core/intrinsics.hpp"
#include "maicord/os/linux/error.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "monotonic.hpp"

namespace maicord {
namespace {

constexpr u64 kManual = u64{1} << 32;
constexpr u64 kOwned = u64{1} << 33;
constexpr u64 kFdMask = 0xffffffffull;
constexpr u32 kInfinite = 0xFFFFFFFFu;
constexpr i64 kNanosecondsPerMillisecond = 1'000'000;

Waitable make_waitable(int fd, bool manual, bool owned) noexcept {
    return reinterpret_cast<Waitable>(static_cast<u64>(fd + 1) | (manual ? kManual : 0) | (owned ? kOwned : 0));
}

bool valid(Waitable waitable) noexcept {
    return waitable != nullptr && waitable != INVALID_HANDLE_VALUE;
}

int descriptor(Waitable waitable) noexcept {
    return static_cast<int>((reinterpret_cast<u64>(waitable) & kFdMask) - 1);
}

bool manual_reset(Waitable waitable) noexcept {
    return (reinterpret_cast<u64>(waitable) & kManual) != 0;
}

bool owned(Waitable waitable) noexcept {
    return (reinterpret_cast<u64>(waitable) & kOwned) != 0;
}

// Reads the counter of an auto-reset waitable: true when it was signaled (and is now consumed).
bool consume(Waitable waitable) noexcept {
    u64 counter = 0;
    return sys_read(descriptor(waitable), &counter, sizeof(counter)) == static_cast<long>(sizeof(counter));
}

// The thread's timer slack, so ppoll may return up to `tolerance_ms` late and share a wakeup. Only changed when
// the wanted slack differs from the last one this thread set (0 restores the kernel's default).
void apply_tolerance(u32 tolerance_ms) noexcept {
    static __thread u32 applied;
    if (applied != tolerance_ms) {
        sys_prctl(kPrctlSetTimerSlack, static_cast<unsigned long>(tolerance_ms) * 1'000'000ul);
        applied = tolerance_ms;
    }
}

// Waits on the descriptors until one is ready; `ready` receives the poll flags of each. Returns the count of ready
// descriptors, 0 on timeout, -1 on failure. A signal interrupts only for the time it takes to resume.
long poll_ready(PollFd* descriptors, u32 count, u32 timeout_ms) noexcept {
    const i64 deadline = timeout_ms == kInfinite ? 0 : monotonic_nanoseconds() + timeout_ms * kNanosecondsPerMillisecond;
    for (;;) {
        TimeSpec timeout{};
        const TimeSpec* limit = nullptr;
        if (timeout_ms != kInfinite) {
            timeout = timespec_from_nanoseconds(deadline - monotonic_nanoseconds());
            limit = &timeout;
        }
        const long result = sys_ppoll(descriptors, count, limit);
        if (result == -kErrorInterrupted) {
            continue;
        }
        if (syscall_failed(result)) {
            set_thread_last_error(win32_error_from_result(result));
            return -1;
        }
        return result;
    }
}

} // namespace

// Creation runs once per waitable: compiled for size.
__attribute__((minsize)) Waitable event_create(bool manual, bool signaled) noexcept {
    const long fd = sys_eventfd(signaled ? 1u : 0u, kEventfdNonBlock | kEventfdCloseOnExec);
    if (syscall_failed(fd)) {
        set_thread_last_error(win32_error_from_result(fd));
        return nullptr;
    }
    return make_waitable(static_cast<int>(fd), manual, true);
}

bool event_set(Waitable event) noexcept {
    if (!valid(event)) {
        return false;
    }
    const u64 one = 1;
    const long result = sys_write(descriptor(event), &one, sizeof(one));
    // A full counter (EAGAIN) is a set event all the same.
    return result == static_cast<long>(sizeof(one)) || result == -kErrorAgain;
}

bool event_reset(Waitable event) noexcept {
    if (!valid(event)) {
        return false;
    }
    consume(event);
    return true;
}

__attribute__((minsize)) Waitable timer_create_waitable(bool manual, bool high_resolution) noexcept {
    (void)high_resolution; // timerfds have nanosecond resolution
    const long fd = sys_timerfd_create(kClockBoottime, kTimerfdNonBlock | kTimerfdCloseOnExec);
    if (syscall_failed(fd)) {
        set_thread_last_error(win32_error_from_result(fd));
        return nullptr;
    }
    return make_waitable(static_cast<int>(fd), manual, true);
}

bool timer_arm(Waitable timer, u64 due_ms, u32 tolerance_ms) noexcept {
    (void)tolerance_ms; // timerfd expiry has no slack
    if (!valid(timer)) {
        return false;
    }
    ITimerSpec setting{};
    // A zero value would disarm; "at once" is the nearest nanosecond.
    const u64 nanoseconds = due_ms == 0 ? 1 : due_ms * static_cast<u64>(kNanosecondsPerMillisecond);
    setting.value.seconds = static_cast<i64>(nanoseconds / 1'000'000'000);
    setting.value.nanoseconds = static_cast<i64>(nanoseconds % 1'000'000'000);
    const long result = sys_timerfd_settime(descriptor(timer), 0, &setting, nullptr);
    if (syscall_failed(result)) {
        set_thread_last_error(win32_error_from_result(result));
        return false;
    }
    return true;
}

bool timer_cancel(Waitable timer) noexcept {
    if (!valid(timer)) {
        return false;
    }
    // Disarming a timerfd also drops an expiry nobody has consumed; whether CancelWaitableTimer leaves one signaled
    // is up to Windows (a manual-reset timer was seen to clear), so the contract says only that the timer is disarmed.
    const ITimerSpec disarmed{};
    return !syscall_failed(sys_timerfd_settime(descriptor(timer), 0, &disarmed, nullptr));
}

void waitable_close(Waitable waitable) noexcept {
    if (valid(waitable) && owned(waitable)) {
        sys_close(descriptor(waitable));
    }
}

Waitable waitable_borrow_fd(int fd) noexcept {
    return fd < 0 ? nullptr : make_waitable(fd, true, false);
}

u32 wait_one(Waitable waitable, u32 timeout_ms) noexcept {
    return wait_any(&waitable, 1, timeout_ms);
}

namespace {

// wait_many, or with `first` wait_any: the lowest waitable that is signaled when the wait returns, and only that one
// consumed. One whose signal was consumed by someone else meanwhile does not count, and the wait goes on with what is
// left of the timeout.
WaitManyResult wait_set(const Waitable* waitables, u32 count, u32 timeout_ms, u32 tolerance_ms, bool first) noexcept {
    WaitManyResult result{};
    if (waitables == nullptr || count == 0 || count > kWaitMaximumCount) {
        set_thread_last_error(ERROR_INVALID_PARAMETER);
        result.failed = true;
        return result;
    }
    apply_tolerance(tolerance_ms);
    PollFd descriptors[kWaitMaximumCount];
    for (u32 index = 0; index < count; ++index) {
        if (!valid(waitables[index])) {
            set_thread_last_error(ERROR_INVALID_HANDLE);
            result.failed = true;
            return result;
        }
        descriptors[index] = PollFd{descriptor(waitables[index]), kPollIn, 0};
    }
    const i64 deadline = timeout_ms == kInfinite ? 0 : monotonic_nanoseconds() + timeout_ms * kNanosecondsPerMillisecond;
    u32 remaining = timeout_ms;
    for (;;) {
        const long ready = poll_ready(descriptors, count, remaining);
        if (ready < 0) {
            result.failed = true;
            return result;
        }
        if (ready == 0) {
            result.timed_out = true;
            return result;
        }
        for (u32 index = 0; index < count; ++index) {
            const i16 events = descriptors[index].revents;
            if (events == 0) {
                continue;
            }
            if ((events & kPollInvalid) != 0) {
                set_thread_last_error(ERROR_INVALID_HANDLE);
                result.ready_mask = 0;
                result.failed = true;
                return result;
            }
            if (manual_reset(waitables[index]) || consume(waitables[index])) {
                result.ready_mask |= u64{1} << index;
                if (first) {
                    return result;
                }
            }
        }
        if (result.ready_mask != 0) {
            return result;
        }
        if (timeout_ms != kInfinite) {
            const i64 left = deadline - monotonic_nanoseconds();
            if (left <= 0) {
                result.timed_out = true;
                return result;
            }
            remaining = static_cast<u32>((left + kNanosecondsPerMillisecond - 1) / kNanosecondsPerMillisecond);
        }
    }
}

} // namespace

u32 wait_any(const Waitable* waitables, u32 count, u32 timeout_ms) noexcept {
    const WaitManyResult result = wait_set(waitables, count, timeout_ms, 0, true);
    return result.failed      ? WAIT_FAILED
           : result.timed_out ? WAIT_TIMEOUT
                              : WAIT_OBJECT_0 + lowest_set_bit(result.ready_mask);
}

WaitManyResult wait_many(const Waitable* waitables, u32 count, u32 timeout_ms, u32 tolerance_ms) noexcept {
    return wait_set(waitables, count, timeout_ms, tolerance_ms, false);
}

Waitable loop_wake_waitable() noexcept {
    static Waitable wake;
    Waitable existing = __atomic_load_n(&wake, __ATOMIC_ACQUIRE);
    if (existing == nullptr) {
        Waitable created = event_create(false, false);
        if (created == nullptr) {
            return nullptr;
        }
        Waitable expected = nullptr;
        if (__atomic_compare_exchange_n(&wake, &expected, created, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return created;
        }
        waitable_close(created);
        existing = expected;
    }
    return existing;
}

void loop_wake() noexcept {
    const Waitable wake = loop_wake_waitable();
    if (wake != nullptr) {
        event_set(wake);
    }
}

} // namespace maicord
