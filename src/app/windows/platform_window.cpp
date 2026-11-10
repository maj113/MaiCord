// The Windows half of the main window: how it is created (the frame glue and the hooks of platform/window/win32.hpp
// the application keeps for itself) and what only Windows has in the window procedure: UI Automation, the shell
// integration and the tray, a second launch. The portable event chain is shell/window_dispatch.cpp, the bar the
// frame glue asks about is shell/caption.cpp.
#include "../notifications/taskbar_attention.hpp"
#include "../accessibility/accessibility.hpp"
#include "../notifications/desktop.hpp"
#include "../shell/state.hpp"
#include "../shell/caption.hpp"
#include "../shell/window.hpp"
#include "../shell/window_runtime.hpp"
#include "maicord/platform/window/win32.hpp"
#include "maicord/sys/shell.hpp"

#if defined(MAICORD_PERF_PROBE) || defined(MAICORD_STARTUP_PROBE)
#include "../../../tools/perf/probe_window.hpp"
#endif

namespace maicord::app_detail {
namespace {

inline bool input_blocked(void*) noexcept {
    return application_input_blocked();
}

// UI Automation (WM_GETOBJECT) and its deferred actions (it observes WM_DESTROY); tray icon callbacks, Explorer
// restarts and launches forwarded by a second instance; the taskbar button's registered "created" message; the window
// menu's appearance.
bool shell_hook(void*, HWND, UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept {
    // The window menu (Alt+Space, the caption's icon and a right click on it) takes the theme's appearance as it opens,
    // so a theme change or a system scheme change since the last one shows; the default procedure then shows it.
    if (message == WM_ENTERMENULOOP)
        win32_menu_appearance(application_theme().appearance != ThemeAppearance::light);
    if (application_accessibility_window_message(message, wparam, lparam, result))
        return true;
    if (application_desktop_window_message(message, wparam, lparam, result))
        return true;
    if (application_input_blocked())
        return false;
    // A press on the frame, or a touch or pen contact, hides keyboard focus as a click in the client does.
    if ((message == WM_NCLBUTTONDOWN || message == WM_POINTERDOWN) && g_app.keyboard_focus_visible) {
        g_app.keyboard_focus_visible = false;
        application_request_frame();
    }
    if (application_taskbar_attention_window_message(message)) {
        *result = 0;
        return true;
    }
    return false;
}

// The caption controller as the frame glue asks it.
inline u32 caption_height(void*) noexcept {
    return g_app.renderer.caption_height;
}
inline bool caption_hidden(void*) noexcept {
    return application_caption_hidden();
}
inline FrameHit caption_hit(void*, float x, float y) noexcept {
    return application_caption_hit(x, y);
}
inline void caption_maximized(void*, bool maximized) noexcept {
    application_caption_maximized(maximized);
}
inline void caption_active(void*, bool active) noexcept {
    application_caption_active(active);
}
inline void caption_hover(void*, FrameHit part) noexcept {
    application_caption_hover(part);
}
inline void caption_leave(void*) noexcept {
    application_caption_leave();
}
inline void caption_press(void*, FrameHit part) noexcept {
    application_caption_press(part);
}
inline bool caption_release(void*, FrameHit part) noexcept {
    return application_caption_release(part);
}
inline bool caption_bar_rects(void*, FrameBarRects* rectangles) noexcept {
    return application_caption_bar_rects(rectangles);
}

} // namespace

void application_platform_window_prepare() noexcept {}

bool application_platform_window_create(Window* out, const char** failure) noexcept {
#if defined(MAICORD_PERF_PROBE) || defined(MAICORD_STARTUP_PROBE)
    // The benchmark window never activates and has no taskbar button.
    constexpr u32 extended_style = probe_window::kExtendedStyle;
#else
    constexpr u32 extended_style = 0;
#endif
    // The window keeps the system frame's outer size; the caption moves into the client (WM_NCCALCSIZE). The tables
    // are the ones the window procedure folds (kWin32Hooks, kWin32CaptionOps); the options carry them for test code.
    return win32_window_create({utf8_literal("MaiCord"), {}, 1280, 800, false},
                               {extended_style, &kWin32Hooks, &kWin32CaptionOps, true}, application_window_event,
                               nullptr, out, failure);
}

void application_platform_window_show(ShowMode mode) noexcept {
#if defined(MAICORD_PERF_PROBE) || defined(MAICORD_STARTUP_PROBE)
    // Shown without activation at the bottom of the z-order: the probe never takes keyboard focus or covers another
    // window.
    (void)mode;
    probe_window::show(win32_hwnd(g_app.window));
#else
    window_show(g_app.window, mode);
#endif
}

} // namespace maicord::app_detail

namespace maicord {
const Win32Hooks kWin32Hooks{nullptr, app_detail::shell_hook, app_detail::input_blocked};
const Win32CaptionOps kWin32CaptionOps{
    nullptr,
    app_detail::caption_height,    app_detail::caption_hidden,  app_detail::caption_hit,
    app_detail::caption_maximized, app_detail::caption_active,  app_detail::caption_hover,
    app_detail::caption_leave,     app_detail::caption_press,   app_detail::caption_release,
    app_detail::caption_bar_rects};
} // namespace maicord
