#pragma once

#include "maicord/os/wait.hpp"
#include "maicord/platform/window/event.hpp"

namespace maicord {

struct WindowWaitResult {
    u64 ready_mask;  // bit i = set[i] is ready; on Windows only the lowest ready bit
    bool window_input;
    bool timed_out;
    bool failed;
};
// Waits for any waitable in `set`, window input or the timeout; `tolerance_ms` lets the system coalesce the wake.
WindowWaitResult window_wait(const Waitable* set, u32 count, u32 timeout_ms, u32 tolerance_ms) noexcept;

struct WindowPumpHooks {
    bool (*before_event)(void* context) noexcept;  // false stops the pump
    // Runs before an event is dispatched: held-key tails and text services. True consumes the event.
    bool (*key_prefilter)(void* context, const WindowEvent& event) noexcept;
    void (*after_event)(void* context) noexcept;
    void* context;
};
struct WindowPumpResult {
    u32 dispatched;
    bool quit;
    int exit_code;
    bool before_failed;
};
// Dispatches up to `max_events` events within `soft_budget_us`.
WindowPumpResult window_pump(u32 max_events, u32 soft_budget_us, const WindowPumpHooks& hooks) noexcept;

// Deadline of backend-internal timers (key repeat), folded into the loop timeout; 0xFFFFFFFF when none.
u32 window_next_deadline_ms(u64 now_ms) noexcept;

// The pieces of window_wait where the display is a descriptor (Linux). Windows has no display waitable:
// window_event_waitable returns nullptr and the wait covers the message queue itself.
Waitable window_event_waitable() noexcept;
void window_wait_begin() noexcept;
void window_wait_end(bool display_ready) noexcept;

} // namespace maicord
