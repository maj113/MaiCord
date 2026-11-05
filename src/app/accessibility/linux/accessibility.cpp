// The application's accessibility server on Linux: AT-SPI over D-Bus (platform/atspi), the counterpart of the UI Automation
// server of Windows (../accessibility.cpp). The snapshots (accessibility_describe.cpp) and the actions they route
// (accessibility_actions.cpp) are shared; what differs is how an assistive technology finds the application.
//
// Nothing is connected until one is on: the first presented frame registers the server, which watches org.a11y.Status on
// the session bus (platform/atspi/server.hpp). Until a client has called the application a frame costs a flag test and a
// store, as on Windows until the first WM_GETOBJECT.
#include "../accessibility_internal.hpp"
#include "../accessibility.hpp"
#include "accessibility_status.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/atspi/server.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/platform/window/event.hpp"
#include "../../chat/composer/text_input.hpp"
#include "../../shell/window_state.hpp"

namespace maicord::app_detail {
namespace {

// Deferred actions run from the loop (a wake_accessibility event), never inside a D-Bus call.
void run_pending() noexcept {
    PendingAction actions[kPendingCapacity];
    const u32 count = g_access.pending_count;
    for (u32 index = 0; index < count; ++index)
        actions[index] = g_access.pending[index];
    g_access.pending_count = 0;
    for (u32 index = 0; index < count; ++index) {
        g_access.dirty = true;
        run_action(actions[index]);
    }
    g_access.dirty = true;
    // The next frame reports the action's result at once.
    ui_access_throttle_mark_urgent(&g_access.throttle);
}

// The host of the AT-SPI server: the snapshot, the action queue and the window's state.
const UiAccessTree* host_tree(void*) noexcept {
    return access_tree(nullptr);
}

bool host_act(void* context, u64 key, UiAccessAction action, double number, Utf8View text) noexcept {
    return access_act(context, key, action, number, text);
}

// The caption's height above the snapshot origin in logical pixels: the snapshot's units are DIPs, and the compositor's
// logical pixel is one DIP at any scale.
float host_content_top(void*) noexcept {
    const float scale = access_scale(nullptr);
    return static_cast<float>(g_app.renderer.caption_height) / scale;
}

bool host_window_active(void*) noexcept {
    return application_window_foreground() && application_window_shown();
}

// Text geometry for the edit that owns native text input.
u32 host_text_bounds(void*, u64 key, u32 start, u32 end, UiRect* rectangles, u32 capacity) noexcept {
    const auto& tree = g_access.tree;
    const u32 index = tree.finished && capacity ? ui_access_find(tree, key) : kUiAccessNone;
    if (index == kUiAccessNone || !(tree.nodes[index].state & ui_access_focused))
        return 0;
    return application_composer_text_input_range_bounds(ui_access_value(tree, index), start, end, rectangles) ? 1 : 0;
}

bool listening() noexcept {
    return atspi::server_queried();
}

void publish_events() noexcept {
    const UiAccessTree* tree = access_tree(nullptr);
    if (!tree)
        return;
    UiAccessEventPlan plan{};
    if (!ui_access_plan_events(&g_access.events, *tree, &plan))
        return;
    atspi::server_publish(*tree, plan);
    application_accessibility_announce(*tree, clock_monotonic_milliseconds());
}

// Nobody listens: the event and status sources start from their then-current state, silently, when someone does.
void forget_observations() noexcept {
    g_access.events.primed = false;
    g_access.events.focus_key = 0;
    application_accessibility_announce_reset();
}

// The loop serves the accessibility bus with the session bus (dbus/session.hpp): its socket wakes the loop, and a
// deferred snapshot rebuild bounds the sleep as the rebuild timer does on Windows.
Waitable loop_waitable() noexcept {
    return atspi::server_waitable();
}

u32 loop_wait_ms(u64 now_ms) noexcept {
    u32 wait = atspi::server_wait_ms(now_ms);
    if (g_access.initialized && !g_access.shut_down && g_access.throttle.deferred) {
        const u64 delay = ui_access_throttle_delay(g_access.throttle, now_ms);
        wait = minimum<u32>(wait, static_cast<u32>(delay));
    }
    return wait;
}

void loop_process(u64 now_ms) noexcept {
    atspi::server_process(now_ms);
    if (!g_access.initialized || g_access.shut_down)
        return;
    // The first call of a client starts the event stream from the state then: the window's activation and its focus are
    // told at once, not with whatever frame comes next (an idle window draws none).
    const bool asked = listening();
    const bool starting = asked && !g_access.listening;
    g_access.listening = asked;
    if (!starting && !g_access.throttle.deferred)
        return;
    if (starting)
        ui_access_throttle_mark_urgent(&g_access.throttle);
    const auto decision = ui_access_throttle_frame(&g_access.throttle, now_ms, asked);
    if (decision == UiAccessThrottleDecision::build)
        publish_events();
    else if (decision == UiAccessThrottleDecision::idle)
        forget_observations();
}

// Input that can move focus or change structure makes the next frame urgent.
bool urgent_event(EventType type) noexcept {
    switch (type) {
    case EventType::key_down:
    case EventType::key_up:
    case EventType::character:
    case EventType::pointer_down:
    case EventType::pointer_up:
    case EventType::wheel:
    case EventType::focus_changed:
    case EventType::activation_changed:
    case EventType::ime_preedit:
    case EventType::ime_commit:
        return true;
    default:
        return false;
    }
}

void start_server() noexcept {
    static const atspi::HostCallbacks host{nullptr,           host_tree,          host_act,       host_content_top,
                                           host_window_active, host_text_bounds};
    const dbus::SessionBusExtra loop{loop_waitable, loop_wait_ms, loop_process};
    if (!dbus::session_bus_add_extra(loop) || !atspi::server_start(host))
        log_write(LogLevel::warning, "accessibility", "atspi_server_not_started");
}

} // namespace

bool application_accessibility_window_event(const WindowEvent& event) noexcept {
    switch (event.type) {
    case EventType::wake:
        if (event.wake.cause != wake_accessibility)
            return false;
        g_access.posted = false;
        run_pending();
        return true;
    case EventType::destroyed:
        application_accessibility_shutdown();
        return false;
    default:
        if (g_access.initialized && urgent_event(event.type))
            ui_access_throttle_mark_urgent(&g_access.throttle);
        return false;
    }
}

void application_accessibility_frame_presented() noexcept {
    // A store and a flag test until an AT-SPI client has called.
    g_access.dirty = true;
    if (g_access.shut_down)
        return;
    if (!g_access.initialized) {
        g_access.initialized = true;
        start_server();
        return;
    }
    const u64 now = clock_monotonic_milliseconds();
    switch (ui_access_throttle_frame(&g_access.throttle, now, listening())) {
    case UiAccessThrottleDecision::build:
        publish_events();
        return;
    case UiAccessThrottleDecision::defer:
        // Animation frames coalesce; the loop's due time publishes the trailing state.
        return;
    default:
        forget_observations();
        return;
    }
}

void application_accessibility_shutdown() noexcept {
    if (g_access.shut_down)
        return;
    g_access.shut_down = true;
    if (g_access.initialized)
        atspi::server_stop();
}

} // namespace maicord::app_detail
