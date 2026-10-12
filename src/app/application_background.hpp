#pragma once
#include "application_state.hpp"
#include "application_settings.hpp"
#include "application_theme_editor.hpp"
#include "maicord/ui/background_animation.hpp"

#include <Windows.h>

namespace maicord::app_detail {
inline bool application_background_animate() noexcept {
    const auto& r = g_app.renderer;
    return g_app.renderer_ready && g_app.window &&
           background_animation_allowed(
               application_theme().background, d3d11_background_fx_plugin_active(r.background_fx),
               IsWindowVisible(g_app.window) != FALSE,
               IsIconic(g_app.window) != FALSE, r.live_resize,
               g_app.preferences.reduced_motion || r.reduced_motion,
               g_app.surface_motion.system_preference_cached && g_app.surface_motion.client_area_animation,
               g_app.login.visible ||
                   (application_settings_active() && !application_theme_editor_active()));
}
inline u64 application_background_next_wake(u64 now_ms) noexcept {
    if (!application_background_animate()) {
        // Visibility can suppress every presentation. Retire the clock when
        // the scheduler observes that state so resume cannot catch up a hidden
        // deadline, while keeping the exact paused shader phase.
        g_app.renderer.background_motion_running = false;
        g_app.renderer.background_next_frame_ms = 0;
        return 0;
    }
    // Initial enable/resume needs one coalesced frame to arm its stable deadline.
    return g_app.renderer.background_motion_running && g_app.renderer.background_next_frame_ms
               ? g_app.renderer.background_next_frame_ms
               : now_ms;
}
inline bool application_background_synchronize(u64 now_ms) noexcept {
    auto& r = g_app.renderer;
    const auto sample =
        background_animation_sample(r.background_next_frame_ms, r.background_phase_seconds,
                                    r.background_motion_running, now_ms, application_background_animate());
    if (!renderer_set_background_phase(&r, sample.phase_seconds))
        return false;
    r.background_next_frame_ms = sample.next_frame_ms;
    r.background_motion_running = sample.running;
    return true;
}
inline bool application_background_request_frame_if_due(u64 now_ms, bool frame_available) noexcept {
    const u64 wake = application_background_next_wake(now_ms);
    if (!frame_available || !wake || now_ms < wake)
        return false;
    application_request_frame();
    return true;
}
} // namespace maicord::app_detail
