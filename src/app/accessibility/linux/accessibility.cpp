// The application's accessibility server on Linux: AT-SPI over D-Bus (platform/atspi), the counterpart of the UI Automation
// server of Windows (../accessibility.cpp). The snapshots (accessibility_describe.cpp) and the actions they route
// (accessibility_actions.cpp) are shared; what differs is how an assistive technology finds the application.
//
// Nothing is connected until one is on: the first presented frame registers the server, which watches org.a11y.Status on
// the session bus (platform/atspi/server.hpp). Until a client has called the application a frame costs a flag test and a
// store, as on Windows until the first WM_GETOBJECT.
#include "../accessibility_internal.hpp"
#include "../accessibility.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/atspi/server.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/platform/window/event.hpp"
#include "../../shell/window_state.hpp"

namespace maicord::app_detail {
namespace {

// The host of the AT-SPI server: the snapshot, the action queue (run from the loop by a wake_accessibility event, never
// inside a D-Bus call) and the window's state.

// The caption's height above the snapshot origin in logical pixels: the snapshot's units are DIPs, and the compositor's
// logical pixel is one DIP at any scale.
float host_content_top(void*) noexcept {
    const float scale = access_scale(nullptr);
    return static_cast<float>(g_app.renderer.caption_height) / scale;
}

bool host_window_active(void*) noexcept {
    return application_window_foreground() && application_window_shown();
}

void publish_events() noexcept {
    const UiAccessTree* tree = access_tree(nullptr);
    if (!tree)
        return;
    UiAccessEventPlan plan{};
    if (!ui_access_plan_events(&g_access.events, *tree, &plan))
        return;
    atspi::server_publish(*tree, plan);
    announce_status(*tree, clock_monotonic_milliseconds());
}

// Nobody listens: the event and status sources start from their then-current state, silently, when someone does.
void forget_observations() noexcept {
    g_access.events.primed = false;
    g_access.events.focus_key = 0;
    forget_status();
}

// The loop serves the accessibility bus with the session bus (dbus/session.hpp): its socket wakes the loop
// (atspi::server_waitable), and a deferred snapshot rebuild bounds the sleep as the rebuild timer does on Windows.
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
    const bool asked = atspi::server_queried();
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
    static const atspi::HostCallbacks host{nullptr,           access_tree,       access_act,        host_content_top,
                                           host_window_active, access_text_bounds};
    const dbus::SessionBusExtra loop{atspi::server_waitable, loop_wait_ms, loop_process};
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
    switch (ui_access_throttle_frame(&g_access.throttle, now, atspi::server_queried())) {
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
