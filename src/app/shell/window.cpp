
#include "caption.hpp"
#include "window_state.hpp"
#include "state.hpp"
#include "window.hpp"

#include "maicord/core/log.hpp"
#include "maicord/ui/core/scene.hpp"
#include "maicord/ui/voice/call_scene.hpp"
#if !defined(_WIN32)
#include "maicord/platform/window/wayland.hpp"
#endif


namespace maicord::app_detail {
namespace {

float minimum_client_width_dips(const Application& application) noexcept {
    if (!application.account_ui || application.login.visible) {
        return kMinimumLoginClientWidthDips;
    }
    const float navigation = application.channel_pane_width > 0.0f
                                 ? application.channel_pane_width
                                 : kGuildChannelShellLayoutMetrics.navigation_width;
    const float members =
        application.member_pane_visible
            ? (application.member_pane_width > 0.0f
                   ? application.member_pane_width
                   : kGuildChannelShellLayoutMetrics.member_sidebar_width)
            : 0.0f;
    return kGuildChannelShellLayoutMetrics.guild_rail_width + navigation + members +
           kMinimumAccountMessageWidthDips;
}

} // namespace

void application_minimum_client_size(u32* width, u32* height) noexcept {
    if (!g_app.window) {
        return;
    }
    const u32 queried_dpi = application_window_dpi(g_app.window);
    const u32 dpi = queried_dpi != 0 ? queried_dpi : maximum(g_app.renderer.dpi, 96u);
    *width = static_cast<u32>(minimum_client_width_dips(g_app) * static_cast<float>(dpi) / 96.0f + 0.999f);
    // The rows of the caption bar lie above the content.
    *height = static_cast<u32>(kMinimumClientHeightDips * static_cast<float>(dpi) / 96.0f + 0.999f) +
              application_caption_pixels(dpi);
}

#if !defined(_WIN32)
void application_window_refresh_minimum_size() noexcept {
    if (!g_app.window)
        return;
    u32 width = 0, height = 0;
    application_minimum_client_size(&width, &height);
    window_set_min_size(g_app.window, width, height);
}

void application_window_refresh_idle_inhibit() noexcept {
    static bool applied;
    const bool wanted = g_app.window && g_app.renderer_ready && g_app.renderer.call_scene && g_app.renderer.call_scene->active;
    if (wanted == applied)
        return;
    applied = wanted;
    window_wayland_set_idle_inhibit(g_app.window, wanted);
}
#endif

bool application_create_main_window() noexcept {
    application_caption_enable();
    Window created{};
    const char* failure = "create_window_failed";
    if (!application_platform_window_create(&created, &failure)) {
        log_write(LogLevel::error, "window", failure);
        return false;
    }
    g_app.window = created;
    application_platform_desktop_fonts();
    // Creation-time messages saw only a partly built window: take the state it ended with.
    application_window_observe_position(created);
    g_window_state.dpi = window_metrics(created).dpi;
    // The renderer splits its first swap chain between caption and content.
    application_caption_synchronize();
    return true;
}

namespace {
bool g_fullscreen_on;
} // namespace

bool application_window_fullscreen() noexcept {
    return g_fullscreen_on;
}

bool application_window_set_fullscreen(bool on) noexcept {
    if (!g_app.window || on == g_fullscreen_on)
        return false;
    // The caption's height is gone (or back) before the frame changes, so the new client is laid out without it.
    application_caption_set_hidden(on);
    if (!window_set_fullscreen(g_app.window, on)) {
        application_caption_set_hidden(!on);
        return false;
    }
    g_fullscreen_on = on;
    return true;
}

} // namespace maicord::app_detail
