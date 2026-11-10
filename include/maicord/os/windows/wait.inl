// The Windows bodies of maicord/os/wait.hpp: inline forwarders to the Win32 calls, so a call site compiles to the Win32
// import. Included by wait.hpp only.

namespace maicord {

__forceinline Waitable event_create(bool manual_reset, bool signaled) noexcept {
    return CreateEventW(nullptr, manual_reset ? TRUE : FALSE, signaled ? TRUE : FALSE, nullptr);
}

__forceinline bool event_set(Waitable event) noexcept {
    return SetEvent(event) != 0;
}

__forceinline bool event_reset(Waitable event) noexcept {
    return ResetEvent(event) != 0;
}

__forceinline Waitable timer_create_waitable(bool manual_reset, bool high_resolution) noexcept {
    return CreateWaitableTimerExW(nullptr, nullptr,
                                  (manual_reset ? CREATE_WAITABLE_TIMER_MANUAL_RESET : 0) |
                                      (high_resolution ? CREATE_WAITABLE_TIMER_HIGH_RESOLUTION : 0),
                                  TIMER_ALL_ACCESS);
}

// One shot, `due_ms` from now (a negative due time is relative, in 100 ns units); `tolerance_ms` lets Windows
// coalesce the expiry with other timers.
__forceinline bool timer_arm(Waitable timer, u64 due_ms, u32 tolerance_ms) noexcept {
    Win32LargeInteger due{};
    due.QuadPart = -static_cast<LONGLONG>(due_ms) * 10'000;
    return SetWaitableTimerEx(timer, due.native(), 0, nullptr, nullptr, nullptr, tolerance_ms) != 0;
}

__forceinline bool timer_cancel(Waitable timer) noexcept {
    return CancelWaitableTimer(timer) != 0;
}

__forceinline void waitable_close(Waitable waitable) noexcept {
    CloseHandle(waitable);
}

__forceinline u32 wait_one(Waitable waitable, u32 timeout_ms) noexcept {
    return WaitForSingleObject(waitable, timeout_ms);
}

__forceinline u32 wait_any(const Waitable* waitables, u32 count, u32 timeout_ms) noexcept {
    return WaitForMultipleObjects(count, waitables, FALSE, timeout_ms);
}

// WaitForMultipleObjects returns the lowest signaled handle; the ones after it are tested without waiting.
// `tolerance_ms` is not used: a handle wait has no tolerance on Windows, the window loop applies it with a
// waitable timer (SetWaitableTimerEx).
inline WaitManyResult wait_many(const Waitable* waitables, u32 count, u32 timeout_ms, u32 tolerance_ms) noexcept {
    (void)tolerance_ms;
    WaitManyResult result{};
    if (count == 0 || count > kWaitMaximumCount) {
        result.failed = true;
        return result;
    }
    const u32 first = WaitForMultipleObjects(count, waitables, FALSE, timeout_ms);
    if (first == WAIT_TIMEOUT) {
        result.timed_out = true;
        return result;
    }
    if (first >= WAIT_OBJECT_0 + count) {
        result.failed = true;
        return result;
    }
    result.ready_mask = u64{1} << (first - WAIT_OBJECT_0);
    for (u32 index = first - WAIT_OBJECT_0 + 1; index < count; ++index) {
        if (WaitForSingleObject(waitables[index], 0) == WAIT_OBJECT_0) {
            result.ready_mask |= u64{1} << index;
        }
    }
    return result;
}

// The process-wide wake event of the UI loop, created on first use.
inline Waitable loop_wake_waitable() noexcept {
    static HANDLE volatile wake;
    HANDLE existing = wake;
    if (existing == nullptr) {
        HANDLE created = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        existing = InterlockedCompareExchangePointer(&wake, created, nullptr);
        if (existing == nullptr) {
            return created;
        }
        if (created != nullptr) {
            CloseHandle(created);
        }
    }
    return existing;
}

inline void loop_wake() noexcept {
    const Waitable wake = loop_wake_waitable();
    if (wake != nullptr) {
        SetEvent(wake);
    }
}

} // namespace maicord
