// The event loop of the window service on Windows (include/maicord/platform/window/pump.hpp): the wait over the
// application's handles and the message queue, and the drain of that queue into the window procedure.
#include "maicord/platform/window/pump.hpp"

#include "maicord/core/time.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/win32.hpp"

namespace maicord {
namespace {

// Synchronization timer for tolerant waits: SetWaitableTimerEx lets the system slip an expiry to batch it with other
// wakeups, which a timeout of MsgWaitForMultipleObjectsEx cannot.
Waitable g_wake_timer;
bool g_wake_timer_created;

bool key_message(UINT message) noexcept {
    return message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
}

} // namespace

// The single lowest-ready bit of ready_mask, as the system returns it. A tolerant wait replaces the exact timeout
// with the timer, whose expiry reads as the timeout.
WindowWaitResult window_wait(const Waitable* set, u32 count, u32 timeout_ms, u32 tolerance_ms) noexcept {
    Waitable joined[kWaitMaximumCount];
    const Waitable* handles = set;
    u32 total = count;
    u32 timer_index = 0xFFFFFFFFu;
    u32 wait_timeout = timeout_ms;
    if (tolerance_ms && count < kWaitMaximumCount - 1) {
        if (!g_wake_timer_created) {
            g_wake_timer_created = true;
            g_wake_timer = timer_create_waitable(false, false);
        }
        if (g_wake_timer && timer_arm(g_wake_timer, timeout_ms, tolerance_ms)) {
            for (u32 i = 0; i < count; ++i)
                joined[i] = set[i];
            joined[count] = g_wake_timer;
            handles = joined;
            timer_index = count;
            total = count + 1;
            wait_timeout = INFINITE;
        }
    }
    DWORD waited = MsgWaitForMultipleObjectsEx(total, total != 0 ? handles : nullptr, wait_timeout, QS_ALLINPUT,
                                               MWMO_INPUTAVAILABLE);
    if (timer_index != 0xFFFFFFFFu) {
        if (waited == WAIT_OBJECT_0 + timer_index)
            waited = WAIT_TIMEOUT;
        else
            timer_cancel(g_wake_timer);
    }
    WindowWaitResult result{};
    result.failed = waited == WAIT_FAILED;
    result.timed_out = waited == WAIT_TIMEOUT;
    result.window_input = waited == WAIT_OBJECT_0 + total;
    if (waited >= WAIT_OBJECT_0 && waited < WAIT_OBJECT_0 + total)
        result.ready_mask = u64{1} << (waited - WAIT_OBJECT_0);
    return result;
}

WindowPumpResult window_pump(u32 max_events, u32 soft_budget_us, const WindowPumpHooks& hooks) noexcept {
    static const Clock clock = clock_initialize();
    WindowPumpResult result{};
    MSG message{};
    const i64 start = clock_now();
    for (u32 handled = 0; handled < max_events && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++handled) {
        if (hooks.before_event && !hooks.before_event(hooks.context)) {
            result.before_failed = true;
            break;
        }
        ++result.dispatched;
        if (message.message == WM_QUIT) {
            result.exit_code = static_cast<int>(message.wParam);
            result.quit = true;
            break;
        }
        // The text services and overlay key filters run ahead of TranslateMessage, on the presses and releases of
        // plain keys bound for the main window. The event carries the system message for the text services.
        bool consumed = false;
        if (hooks.key_prefilter && (message.message == WM_KEYDOWN || message.message == WM_KEYUP) &&
            message.hwnd == win32_main_window()) {
            WindowEvent event{};
            if (win32_translate(message.hwnd, message.message, message.wParam, message.lParam, &event)) {
                event.native = &message;
                consumed = hooks.key_prefilter(hooks.context, event);
            }
        }
        if (!consumed) {
            if (key_message(message.message))
                TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (hooks.after_event)
            hooks.after_event(hooks.context);
        // Include IME-consumed input in the soft fairness budget. A slow handler cannot be preempted; pending
        // messages remain queued.
        if ((handled & 7u) == 7u && clock_microseconds(clock, start, clock_now()) >= soft_budget_us)
            break;
    }
    return result;
}

} // namespace maicord
