#include "maicord/core/memory.hpp"

#include "application_caption.hpp"
#include "application_state.hpp"

#include "maicord/core/log.hpp"
#include "maicord/ui/scene.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

#if defined(MAICORD_PERF_PROBE) || defined(MAICORD_STARTUP_PROBE)
#include "../../tools/perf/probe_window.hpp"
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

void application_apply_minimum_window_track(HWND window, MINMAXINFO* limits) noexcept {
    if (!window || !limits) {
        return;
    }
    const UINT queried_dpi = GetDpiForWindow(window);
    const UINT dpi = queried_dpi != 0 ? queried_dpi : maximum(g_app.renderer.dpi, 96u);
    const LONG client_width = static_cast<LONG>(
        minimum_client_width_dips(g_app) * static_cast<float>(dpi) / 96.0f + 0.999f);
    const LONG client_height =
        static_cast<LONG>(kMinimumClientHeightDips * static_cast<float>(dpi) / 96.0f + 0.999f);
    const SIZE frame = application_caption_window_size(window, dpi, client_width, client_height);
    limits->ptMinTrackSize.x = maximum(limits->ptMinTrackSize.x, frame.cx);
    limits->ptMinTrackSize.y = maximum(limits->ptMinTrackSize.y, frame.cy);
}

bool application_create_main_window(WNDPROC window_proc) noexcept {
    static constexpr wchar_t kClassName[] = L"MaiCordMainWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    // A flip-model swap chain owns all client pixels. Class-wide redraws
    // generate duplicate WM_PAINT traffic during the modal sizing loop, and
    // CS_OWNDC only retains a GDI DC that this renderer never uses.
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = g_app.instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kClassName;
    if (!RegisterClassExW(&window_class)) {
        log_write(LogLevel::error, "window", "register_class_failed");
        return false;
    }
#if defined(MAICORD_PERF_PROBE) || defined(MAICORD_STARTUP_PROBE)
    // The benchmark window never activates and has no taskbar button.
    constexpr DWORD extended_style = WS_EX_NOREDIRECTIONBITMAP | probe_window::kExtendedStyle;
#else
    constexpr DWORD extended_style = WS_EX_NOREDIRECTIONBITMAP;
#endif
    RECT bounds{0, 0, 1280, 800};
    if (!AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, extended_style, 96)) {
        log_write(LogLevel::error, "window", "adjust_initial_bounds_failed");
        return false;
    }
    // The window keeps the system frame's outer size; the caption moves into
    // the client (WM_NCCALCSIZE).
    application_caption_enable();
    g_app.window = CreateWindowExW(
        extended_style, kClassName, L"MaiCord", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr,
        g_app.instance, nullptr);
    if (!g_app.window) {
        log_write(LogLevel::error, "window", "create_window_failed");
        return false;
    }
    enable_dark_window_frame(g_app.window);
    // The renderer splits its first swap chain between caption and content.
    application_caption_synchronize();
    return true;
}

namespace {
struct WindowFullscreen {
    WINDOWPLACEMENT placement;
    LONG_PTR style;
    bool on;
} g_fullscreen{};
} // namespace

bool application_window_fullscreen() noexcept {
    return g_fullscreen.on;
}

bool application_window_set_fullscreen(bool on) noexcept {
    const HWND window = g_app.window;
    if (!window || on == g_fullscreen.on)
        return false;
    if (on) {
        reset_value(&g_fullscreen.placement);
        g_fullscreen.placement.length = sizeof(g_fullscreen.placement);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetWindowPlacement(window, &g_fullscreen.placement) ||
            !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
            return false;
        g_fullscreen.style = GetWindowLongPtrW(window, GWL_STYLE);
        application_caption_set_hidden(true);
        SetWindowLongPtrW(window, GWL_STYLE,
                          g_fullscreen.style & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME));
        const auto& r = monitor.rcMonitor;
        SetWindowPos(window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        application_caption_set_hidden(false);
        SetWindowLongPtrW(window, GWL_STYLE, g_fullscreen.style);
        SetWindowPlacement(window, &g_fullscreen.placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen.on = on;
    return true;
}

} // namespace maicord::app_detail
