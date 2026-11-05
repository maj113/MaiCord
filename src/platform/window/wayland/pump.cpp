// The event ring and the loop calls of the window service on Wayland: window_wait waits for the app's waitables, the
// display descriptor, the loop wake and the key-repeat timer at once; window_pump drains the ring into the sink.
#include "maicord/core/time.hpp"
#include "maicord/platform/window/pump.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/platform/dbus/instance.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "backend.hpp"

namespace maicord::wayland {

u32 now_ms32() noexcept {
    return static_cast<u32>(clock_monotonic_milliseconds());
}

WindowEvent make_event(EventType type, u32 time_ms) noexcept {
    WindowEvent event{};
    event.type = type;
    event.time_ms = time_ms;
    return event;
}

bool ring_push(const WindowEvent& event) noexcept {
    EventRing& ring = g.ring;
    if (ring.tail - ring.head >= kRingCapacity) {
        ++ring.dropped;
        return false;
    }
    ring.events[ring.tail % kRingCapacity] = event;
    ++ring.tail;
    return true;
}

namespace {

bool ring_empty() noexcept {
    return g.ring.head == g.ring.tail;
}

// Whether the pump would find work without reading the socket.
bool work_pending() noexcept {
    return !ring_empty() || g.quit || g.failed || __atomic_load_n(&g.wake_pending, __ATOMIC_ACQUIRE) != 0;
}

bool is_key_event(EventType type) noexcept {
    return type == EventType::key_down || type == EventType::key_up || type == EventType::character;
}

// What DefWindowProc makes of an event the app left unhandled on Windows, which the app relies on: the context menu of a
// right click (on the release) and of Shift+F10 or the Menu key (on the press, nested inside that key's dispatch; not
// for auto-repeats). The keyboard one carries ev_keyboard and no position.
void default_context_menu(const WindowEvent& event) noexcept {
    const bool by_pointer = event.type == EventType::pointer_up && event.button == static_cast<u8>(PointerButton::right);
    const bool by_key =
        event.type == EventType::key_down && (event.flags & ev_repeat) == 0 &&
        (event.key.key == key::menu ||
         (event.key.key == key::f10 && (event.modifiers & (mod_ctrl | mod_alt | mod_shift)) == mod_shift));
    if (!by_pointer && !by_key)
        return;
    WindowEvent menu = make_event(EventType::context_menu, event.time_ms);
    menu.modifiers = event.modifiers;
    if (by_pointer) {
        menu.pointer.x = event.pointer.x;
        menu.pointer.y = event.pointer.y;
        menu.pointer.buttons = event.pointer.buttons;
    } else {
        menu.flags |= ev_keyboard;
    }
    WindowEventResult ignored{};
    sink_call(menu, &ignored);
}

// One event to the sink, with the bookkeeping that follows what the app has seen.
void dispatch(const WindowEvent& event) noexcept {
    if (frame_filter(event))
        return;  // a client-side decoration took it: a move, a resize, the window menu
    pointer_before_dispatch(event);
    WindowEventResult result{};
    sink_call(event, &result);
    // What the app answers to size_limits follows its layout: ask again once the events that move it are handled.
    if (event.type == EventType::resize || event.type == EventType::scale_changed ||
        event.type == EventType::state_changed)
        apply_size_limits();
    pointer_after_dispatch(event);
    if (event.type == EventType::close_requested && !result.handled) {
        g.quit = true;
        g.exit_code = 0;
    }
    if (!result.handled)
        default_context_menu(event);
    if (event.type == EventType::pointer_move)
        pointer_cursor_query(event);
}

} // namespace

void post_drain() noexcept {
    EventRing& ring = g.ring;
    PostEntry entry;
    while (ring.tail - ring.head < kRingCapacity && post_queue_pop(&g.post, &entry)) {
        WindowEvent event = make_event(EventType::wake, now_ms32());
        event.wake.cause = entry.cause;
        event.wake.a = entry.a;
        event.wake.b = entry.b;
        ring_push(event);
    }
}

void display_poll_nonblocking() noexcept {
    if (!g.display || g.failed || g.read_prepared)
        return;
    while (g_client.display_prepare_read(g.display) != 0)
        g_client.display_dispatch_pending(g.display);
    g_client.display_flush(g.display);
    PollFd poll{g_client.display_get_fd(g.display), kPollIn, 0};
    const TimeSpec none{0, 0};
    const long ready = sys_ppoll(&poll, 1, &none);
    if (ready > 0 && (poll.revents & kPollIn)) {
        if (g_client.display_read_events(g.display) < 0)
            g.failed = true;
    } else {
        g_client.display_cancel_read(g.display);
        if (ready > 0)
            g.failed = true;
    }
    g_client.display_dispatch_pending(g.display);
    connection_check();
    if (g.failed && !g.quit) {
        g.quit = true;
        g.exit_code = 1;
    }
}

} // namespace maicord::wayland

namespace maicord {

using namespace wayland;

namespace {

void window_wait_begin() noexcept {
    if (!g.display || g.failed || g.read_prepared)
        return;
    // Events other readers (Mesa on its own queue never reaches this) left in the default queue are dispatched
    // first: prepare_read only succeeds when that queue is empty.
    while (g_client.display_prepare_read(g.display) != 0)
        g_client.display_dispatch_pending(g.display);
    g.read_prepared = true;
    if (g_client.display_flush(g.display) < 0 && g_client.display_get_error(g.display) != 0)
        connection_check();
}

void window_wait_end(bool display_ready) noexcept {
    if (!g.display)
        return;
    if (g.read_prepared) {
        g.read_prepared = false;
        if (display_ready) {
            if (g_client.display_read_events(g.display) < 0)
                g.failed = true;
        } else {
            g_client.display_cancel_read(g.display);
        }
    }
    g_client.display_dispatch_pending(g.display);
    connection_check();
    if (g.failed && !g.quit) {
        g.quit = true;
        g.exit_code = 1;
    }
}

} // namespace

WindowWaitResult window_wait(const Waitable* set, u32 count, u32 timeout_ms, u32 tolerance_ms) noexcept {
    WindowWaitResult result{};
    constexpr u32 kExtra = 5 + dbus::kSessionBusExtraMaximum;  // the display, the loop wake, the key-repeat timer, the session bus, the instance socket, the bus extras
    if (count > kWaitMaximumCount - kExtra) {
        result.failed = true;
        return result;
    }
    if (!g.display) {
        const Waitable wake = loop_wake_waitable();
        Waitable all[kWaitMaximumCount];
        for (u32 i = 0; i < count; ++i)
            all[i] = set[i];
        all[count] = wake;
        const WaitManyResult waited = wait_many(all, count + 1, timeout_ms, tolerance_ms);
        result.ready_mask = waited.ready_mask & ((1ull << count) - 1);
        result.window_input = (waited.ready_mask >> count) & 1;
        result.timed_out = waited.timed_out;
        result.failed = waited.failed;
        return result;
    }

    window_wait_begin();
    const u64 now = clock_monotonic_milliseconds();
    const u32 repeat_ms = keyboard_repeat_wait_ms(now);
    if (work_pending() || repeat_ms == 0) {
        // Something already queued, or a repeat due: do not sleep. Whoever else read our events (or a listener that
        // just ran) left work in the ring.
        window_wait_end(false);
        result.window_input = true;
        return result;
    }
    Waitable all[kWaitMaximumCount];
    for (u32 i = 0; i < count; ++i)
        all[i] = set[i];
    u32 total = count;
    const u32 display_bit = total;
    all[total++] = g.display_waitable;
    const u32 wake_bit = total;
    all[total++] = loop_wake_waitable();
    u32 repeat_bit = ~0u;
    if (repeat_ms != kRepeatNever) {
        if (timer_arm(keyboard_repeat_waitable(), repeat_ms, 0)) {
            repeat_bit = total;
            all[total++] = keyboard_repeat_waitable();
        } else if (repeat_ms < timeout_ms) {
            timeout_ms = repeat_ms;
        }
    }
    // A flush that could not send everything leaves requests buffered: look again soon instead of sleeping on.
    if (g_client.display_flush(g.display) < 0 && timeout_ms > 4)
        timeout_ms = 4;
    // The session bus: its traffic wakes the loop (window_input), and a call timeout or a reconnect bounds the sleep.
    u32 bus_bit = ~0u;
    if (const Waitable bus = dbus::session_bus_waitable()) {
        bus_bit = total;
        all[total++] = bus;
    }
    // The single-instance socket (no session bus): a second launch's request wakes the loop the same way.
    u32 instance_bit = ~0u;
    if (const Waitable instance = dbus::instance_waitable()) {
        instance_bit = total;
        all[total++] = instance;
    }
    // The session bus's extra connections (the accessibility bus) and their due times.
    Waitable extras[dbus::kSessionBusExtraMaximum];
    const u32 extra_count = dbus::session_bus_extra_waitables(extras, dbus::kSessionBusExtraMaximum);
    const u32 extra_first = total;
    for (u32 i = 0; i < extra_count; ++i)
        all[total++] = extras[i];
    const u32 bus_ms = dbus::session_bus_wait_ms(now);
    if (bus_ms < timeout_ms)
        timeout_ms = bus_ms;

    const WaitManyResult waited = wait_many(all, total, timeout_ms, tolerance_ms);
    const bool display_ready = (waited.ready_mask >> display_bit) & 1;
    window_wait_end(display_ready);

    result.ready_mask = waited.ready_mask & ((1ull << count) - 1);
    const bool woken = (waited.ready_mask >> wake_bit) & 1;
    const bool repeat_fired = repeat_bit != ~0u && ((waited.ready_mask >> repeat_bit) & 1);
    const u64 extra_mask = extra_count ? ((1ull << extra_count) - 1) << extra_first : 0;
    const bool bus_ready = (bus_bit != ~0u && ((waited.ready_mask >> bus_bit) & 1)) ||
                           (instance_bit != ~0u && ((waited.ready_mask >> instance_bit) & 1)) ||
                           (waited.ready_mask & extra_mask) != 0;
    result.window_input = woken || repeat_fired || bus_ready || work_pending();
    result.timed_out = waited.timed_out && !result.window_input;
    result.failed = waited.failed;
    return result;
}

WindowPumpResult window_pump(u32 max_events, u32 soft_budget_us, const WindowPumpHooks& hooks) noexcept {
    WindowPumpResult result{};
    if (!g.window_exists) {
        result.quit = g.quit;
        result.exit_code = g.exit_code;
        return result;
    }
    const i64 started = clock_now();
    const i64 budget = static_cast<i64>(soft_budget_us) * 1000;
    display_poll_nonblocking();
    dbus::session_bus_process(clock_monotonic_milliseconds());
    dbus::instance_process();
    keyboard_poll_repeat(clock_monotonic_milliseconds());
    post_drain();
    if (__atomic_exchange_n(&g.wake_pending, 0u, __ATOMIC_ACQ_REL)) {
        WindowEvent event = make_event(EventType::wake, now_ms32());
        event.wake.cause = wake_poke;
        ring_push(event);
    }
    EventRing& ring = g.ring;
    while (result.dispatched < max_events && ring.head != ring.tail) {
        if (hooks.before_event && !hooks.before_event(hooks.context)) {
            result.before_failed = true;
            break;
        }
        const WindowEvent event = ring.events[ring.head % kRingCapacity];
        ++ring.head;
        const bool consumed = hooks.key_prefilter && is_key_event(event.type) && hooks.key_prefilter(hooks.context, event);
        if (!consumed)
            dispatch(event);
        ++result.dispatched;
        if (hooks.after_event)
            hooks.after_event(hooks.context);
        if (clock_now() - started > budget)
            break;
    }
    result.quit = g.quit;
    result.exit_code = g.exit_code;
    return result;
}

void window_wake(Window) noexcept {
    __atomic_store_n(&g.wake_pending, 1u, __ATOMIC_RELEASE);
    loop_wake();
}

bool window_post(const Waker& waker, u64 a, u64 b) noexcept {
    if (!__atomic_load_n(&g.post_ready, __ATOMIC_ACQUIRE))
        return false;
    const wayland::PostEntry entry{waker.cause, a, b};
    if (!wayland::post_queue_push(&g.post, entry))
        return false;
    loop_wake();
    return true;
}

} // namespace maicord
