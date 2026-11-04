#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_types.hpp"

// Events, waitable timers and waits (D17): one `Waitable` type for everything a thread can wait for, with the
// Win32 semantics the shared code already relies on.
//
//   * An auto-reset waitable is consumed by the wait that returns it; a manual-reset one stays set until reset.
//   * A wait never returns the same auto-reset signal twice, and wait_any returns the lowest ready index.
//   * Timeouts are milliseconds, INFINITE (0xFFFFFFFF) waits forever; results keep the Win32 numbers
//     (WAIT_OBJECT_0 + index, WAIT_TIMEOUT, WAIT_FAILED), so comparisons such as `waited == WAIT_OBJECT_0 + 1`
//     stay valid.
//   * One thread waits for a given auto-reset waitable at a time, as in the code today.
//
// On Windows a Waitable is the HANDLE itself and every function is an inline forwarder to the Win32 call
// (maicord/os/windows/wait.inl), so call sites compile to the same imports. On Linux it is a tagged
// pointer-sized token over an eventfd or timerfd (nullptr = none, (HANDLE)-1 = invalid); the bodies are in
// src/os/linux/wait.cpp.

namespace maicord {

using Waitable = HANDLE;

// The most waitables one wait takes (Windows MAXIMUM_WAIT_OBJECTS).
inline constexpr u32 kWaitMaximumCount = 64;

struct WaitManyResult {
    // Bit i: waitable i was signaled (and, if auto-reset, consumed).
    u64 ready_mask;
    bool timed_out;
    bool failed;
};

} // namespace maicord

#if defined(_WIN32)
#include "maicord/os/windows/wait.inl"
#else
namespace maicord {

// An event: nullptr when the system refuses.
Waitable event_create(bool manual_reset, bool signaled) noexcept;
bool event_set(Waitable event) noexcept;
bool event_reset(Waitable event) noexcept;

// A one-shot waitable timer. `high_resolution` asks for finer than scheduler-tick expiry where the system has a
// choice (Windows); `manual_reset` keeps it signaled after expiry until the next timer_arm.
Waitable timer_create_waitable(bool manual_reset, bool high_resolution) noexcept;
// Fires once, `due_ms` from now (0 = at once), replacing any earlier arming and clearing an expiry nobody consumed.
// `tolerance_ms` is a coalescing hint (Windows applies it to a normal timer; arm a high-resolution one with 0; Linux
// timers have no slack, a wait that tolerates delay says so in wait_many).
bool timer_arm(Waitable timer, u64 due_ms, u32 tolerance_ms) noexcept;
// Disarms the timer. An expiry that already happened may stay signaled or not: arm the timer again before waiting.
bool timer_cancel(Waitable timer) noexcept;

// Closes an event or timer made here; a waitable of waitable_borrow_fd only forgets its token.
void waitable_close(Waitable waitable) noexcept;
// A file descriptor someone else owns (the window system's, pulse's, an inotify's) as a manual-reset,
// level-triggered waitable: it is signaled while the descriptor is readable and never consumed by a wait.
Waitable waitable_borrow_fd(int fd) noexcept;

u32 wait_one(Waitable waitable, u32 timeout_ms) noexcept;
// 1..kWaitMaximumCount waitables: WAIT_OBJECT_0 + the lowest signaled index, WAIT_TIMEOUT or WAIT_FAILED.
u32 wait_any(const Waitable* waitables, u32 count, u32 timeout_ms) noexcept;
// 1..kWaitMaximumCount waitables: every waitable that is signaled when the wait returns, not only the lowest.
// `tolerance_ms` is how much later than `timeout_ms` the wait may return, so the kernel can batch it with other
// wakeups (the thread's timer slack on Linux; Windows ignores it here, the window loop applies it with its wake timer).
WaitManyResult wait_many(const Waitable* waitables, u32 count, u32 timeout_ms, u32 tolerance_ms) noexcept;

// The process-wide auto-reset event that wakes the UI loop from any thread, created on first use.
Waitable loop_wake_waitable() noexcept;
void loop_wake() noexcept;

} // namespace maicord
#endif
