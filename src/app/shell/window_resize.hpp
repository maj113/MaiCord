#pragma once

#include "frame_presentation.hpp"
#include "../settings/settings.hpp"
#include "state.hpp"

namespace maicord::app_detail {
inline void application_begin_window_resize() noexcept {
    animation_visual_runtime_suspend(&g_app.animations, application_now_us());
    renderer_set_live_resize(&g_app.renderer, true);
}
inline void application_end_window_resize() noexcept {
    if (g_app.renderer_ready) {
        // The renderer's height is the content below the window caption.
        const WindowMetrics client = window_metrics(g_app.window);
        const auto caption = static_cast<i32>(g_app.renderer.caption_height);
        if (!client.width_px || !client.height_px ||
            !renderer_finish_resize(&g_app.renderer, maximum<u32>(1, client.width_px),
                                    static_cast<u32>(maximum<i32>(1, static_cast<i32>(client.height_px) - caption)))) {
            renderer_set_live_resize(&g_app.renderer, false);
            application_record_presentation_failure("resize_finish_rejected");
        }
    }
    application_preferences_apply();
    g_app.frame_clock_watchdog_at_us = 0;
    g_app.frame_clock_timer_fallback = false;
    application_request_frame();
}
inline bool application_present_resize_projection(bool prepared) noexcept {
    if (!prepared)
        return false;
    // Normal native dispatch publishes geometry without waiting for a display
    // slot. The outer loop acquires that slot after input; Windows' sizing
    // modal loop and startup still need a synchronous resize image.
    return g_app.frame_loop_active && !g_app.renderer.live_resize ? application_request_frame()
                                                                  : application_present_frame();
}
} // namespace maicord::app_detail
