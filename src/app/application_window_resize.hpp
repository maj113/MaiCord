#pragma once

#include "application_frame_presentation.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "maicord/core/time.hpp"

#include <Windows.h>

namespace maicord::app_detail {
inline void application_begin_window_resize() noexcept {
    animation_visual_runtime_suspend(&g_app.animations,
                                     clock_microseconds(g_app.clock, g_app.process_start, clock_now()));
    renderer_set_live_resize(&g_app.renderer, true);
}
inline void application_end_window_resize(HWND window) noexcept {
    if (g_app.renderer_ready) {
        RECT client{};
        if (!GetClientRect(window, &client) ||
            !renderer_finish_resize(&g_app.renderer,
                                    static_cast<u32>(maximum<LONG>(1, client.right - client.left)),
                                    static_cast<u32>(maximum<LONG>(1, client.bottom - client.top)))) {
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
