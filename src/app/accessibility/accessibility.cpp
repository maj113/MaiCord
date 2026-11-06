#include "accessibility_internal.hpp"
#include "accessibility.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/win32.hpp"
#include "maicord/core/time.hpp"
#include "../shell/window_state.hpp"

namespace maicord::app_detail {
namespace {

void publish_events() noexcept {
    const UiAccessTree* tree = access_tree(nullptr);
    if (!tree)
        return;
    UiAccessEventPlan plan{};
    if (!ui_access_plan_events(&g_access.events, *tree, &plan))
        return;
    if (plan.structure_root)
        uia_host_raise_structure(&g_access.host, 0);
    else
        for (u32 index = 0; index < plan.structure_count; ++index)
            uia_host_raise_structure(&g_access.host, access_event_key(*tree, plan.structures[index]));
    for (u32 index = 0; index < plan.property_count; ++index) {
        const auto& change = plan.properties[index];
        const u64 key = access_event_key(*tree, change.node);
        uia_host_raise_property(&g_access.host, key, change);
        if (change.property == UiAccessProperty::name && (tree->nodes[change.node].state & ui_access_live))
            uia_host_raise_live_region(&g_access.host, key);
    }
    if (plan.focus_changed && application_window_keyboard_focus())
        uia_host_raise_focus(&g_access.host, access_event_key(*tree, plan.focus_node));
    announce_status(*tree, clock_monotonic_milliseconds());
}

bool sample_listening(u64 now_ms) noexcept {
    if (!g_access.listen_sampled_ms || now_ms - g_access.listen_sampled_ms >= kListenSampleMs) {
        g_access.listening = uia_host_listening(g_access.host);
        g_access.listen_sampled_ms = now_ms ? now_ms : 1;
    }
    return g_access.listening;
}

void arm_rebuild_timer(u64 now_ms) noexcept {
    const u64 delay = ui_access_throttle_delay(g_access.throttle, now_ms);
    if (win32_hwnd(g_app.window) && SetTimer(win32_hwnd(g_app.window), kRebuildTimer, static_cast<UINT>(delay ? delay : 1), nullptr))
        g_access.timer = true;
}

// Input that can move focus or change structure makes the next frame urgent.
bool urgent_message(UINT message) noexcept {
    switch (message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_UNICHAR:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ACTIVATE:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
        return true;
    default:
        return false;
    }
}

} // namespace

bool application_accessibility_window_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept {
    if (message == kApplicationAccessibilityMessage) {
        g_access.posted = false;
        run_pending();
        *result = 0;
        return true;
    }
    if (g_access.initialized && urgent_message(message))
        ui_access_throttle_mark_urgent(&g_access.throttle);
    if (message == WM_TIMER && wparam == kRebuildTimer) {
        KillTimer(win32_hwnd(g_app.window), kRebuildTimer);
        g_access.timer = false;
        if (g_access.initialized && !g_access.shut_down && g_access.throttle.deferred) {
            const u64 now = clock_monotonic_milliseconds();
            const auto decision = ui_access_throttle_frame(&g_access.throttle, now, sample_listening(now));
            if (decision == UiAccessThrottleDecision::build)
                publish_events();
            else if (decision == UiAccessThrottleDecision::defer)
                arm_rebuild_timer(now);
        }
        *result = 0;
        return true;
    }
    if (message == WM_DESTROY) {
        application_accessibility_shutdown();
        return false;
    }
    // Provider calls marshaled from UI Automation threads onto this thread.
    if (g_access.initialized && g_access.host.call_message && message == g_access.host.call_message)
        return uia_host_window_message(&g_access.host, message, wparam, lparam, result);
    if (message != WM_GETOBJECT || static_cast<long>(lparam) != -25 /* UiaRootObjectId */ || g_access.shut_down ||
        !g_app.window)
        return false;
    if (!g_access.initialized) {
        const UiaHostCallbacks callbacks{nullptr, access_tree, access_act, access_scale, access_text_bounds};
        if (!uia_host_initialize(&g_access.host, win32_hwnd(g_app.window), callbacks))
            return false;
        g_access.initialized = true;
    }
    if (!ensure_storage())
        return false;
    g_access.dirty = true;
    return uia_host_window_message(&g_access.host, message, wparam, lparam, result);
}

void application_accessibility_frame_presented() noexcept {
    // A store and a flag test until a UI Automation client first asks.
    g_access.dirty = true;
    if (!g_access.initialized || g_access.shut_down)
        return;
    const u64 now = clock_monotonic_milliseconds();
    switch (ui_access_throttle_frame(&g_access.throttle, now, sample_listening(now))) {
    case UiAccessThrottleDecision::build:
        publish_events();
        return;
    case UiAccessThrottleDecision::defer:
        // Animation frames coalesce; the timer publishes the trailing state.
        if (!g_access.timer)
            arm_rebuild_timer(now);
        return;
    default:
        // Nobody listens: status observations restart silently later.
        forget_status();
        return;
    }
}

void application_accessibility_shutdown() noexcept {
    if (g_access.shut_down)
        return;
    g_access.shut_down = true;
    if (g_access.timer && g_app.window)
        KillTimer(win32_hwnd(g_app.window), kRebuildTimer);
    g_access.timer = false;
    if (g_access.initialized)
        uia_host_shutdown(&g_access.host);
}

} // namespace maicord::app_detail
