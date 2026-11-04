#include "maicord/core/memory.hpp"

#include "caption.hpp"
#include "window_state.hpp"
#include "state.hpp"
#include "window_runtime.hpp"

#include "maicord/core/log.hpp"
#include "maicord/ui/core/scene.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

#if defined(MAICORD_PERF_PROBE) || defined(MAICORD_STARTUP_PROBE)
#include "../../../tools/perf/probe_window.hpp"
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

void enable_dark_window_frame(HWND window) noexcept {
    using SetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, const void*, DWORD);
    const HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm) {
        return;
    }
    const auto set_attribute =
        reinterpret_cast<SetWindowAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (set_attribute) {
        constexpr DWORD kImmersiveDarkModeCurrent = 20;
        constexpr DWORD kImmersiveDarkModeBefore20H1 = 19;
        const BOOL enabled = TRUE;
        if (FAILED(set_attribute(window, kImmersiveDarkModeCurrent, &enabled, sizeof(enabled)))) {
            set_attribute(window, kImmersiveDarkModeBefore20H1, &enabled, sizeof(enabled));
        }
    }
    // Stays mapped: the caption measures the frame through the same module, so
    // releasing it here only made the loader map and unmap it twice.
}

} // namespace

void application_minimum_window_frame(HWND window, u32* width, u32* height) noexcept {
    if (!window) {
        return;
    }
    const UINT queried_dpi = application_window_dpi(window);
    const UINT dpi = queried_dpi != 0 ? queried_dpi : maximum(g_app.renderer.dpi, 96u);
    const LONG client_width = static_cast<LONG>(
        minimum_client_width_dips(g_app) * static_cast<float>(dpi) / 96.0f + 0.999f);
    const LONG client_height =
        static_cast<LONG>(kMinimumClientHeightDips * static_cast<float>(dpi) / 96.0f + 0.999f);
    const SIZE frame = application_caption_window_size(window, dpi, client_width, client_height);
    *width = static_cast<u32>(frame.cx);
    *height = static_cast<u32>(frame.cy);
}

bool application_create_main_window() noexcept {
#if defined(MAICORD_PERF_PROBE) || defined(MAICORD_STARTUP_PROBE)
    // The benchmark window never activates and has no taskbar button.
    constexpr u32 extended_style = probe_window::kExtendedStyle;
#else
    constexpr u32 extended_style = 0;
#endif
    // The window keeps the system frame's outer size; the caption moves into the client (WM_NCCALCSIZE).
    application_caption_enable();
    Window created{};
    const char* failure = "create_window_failed";
    if (!win32_window_create({utf8_literal("MaiCord"), {}, 1280, 800, false}, {extended_style, application_window_hooks()},
                             application_window_event, nullptr, &created, &failure)) {
        log_write(LogLevel::error, "window", failure);
        return false;
    }
    g_app.window = created;
    const HWND window = win32_hwnd(created);
    // Creation-time messages saw only a partly built window: take the state it ended with.
    application_window_observe_position(window);
    g_window_state.dpi = GetDpiForWindow(window);
    enable_dark_window_frame(window);
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
