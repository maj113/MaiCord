#pragma once

// The Windows side of the window service: what code that must name an HWND uses. A Window is the HWND itself, so
// the conversions are free. Portable code never includes this header.
#include "maicord/core/win32_types.hpp"
#include "maicord/platform/window/window.hpp"

namespace maicord {

inline Window win32_window(HWND window) noexcept {
    return reinterpret_cast<Window>(window);
}
inline HWND win32_hwnd(Window window) noexcept {
    return reinterpret_cast<HWND>(window);
}
// The window win32_window_create made; the clipboard is owned by it. Null before it exists.
HWND win32_main_window() noexcept;

// Stock cursor objects are loaded once and shared; a system setting change (a new cursor scheme) makes them load
// again. The window procedure does it on WM_SETTINGCHANGE.
void win32_cursor_handles_reset() noexcept;
// The system values platform_settings_read keeps (the wheel setting, the app colour mode, the animation switch) are read
// again after a settings change. The window procedure does it on WM_SETTINGCHANGE.
void win32_settings_reset() noexcept;

// A top-down straight-alpha BGRA image (side `extent`, at most 64) as an icon the system copies from it; the caller
// destroys the icon. Null for an invalid image.
HICON win32_create_icon(ConstSlice<u32> pixels, u32 extent) noexcept;

// What the Windows build of the application keeps for itself in the window procedure, beside the sink every
// backend has: the frame it draws, UI Automation, the shell integration. Each hook that returns true has handled
// the raw message and `*result` is its LRESULT. The hooks run in this order, ahead of the sink:
//   observe       every modeled message, first: state the window procedure keeps for the application.
//   frame         the caption bar and the non-client messages (WM_NC*, the bar's own pointer items).
//   shell         UI Automation (WM_GETOBJECT and its deferred actions), tray icon callbacks, a second launch.
// `input_blocked` tells whether a modal owns all input: a message of the input classes the sink does not model
// (IME, clipboard commands) is then swallowed instead of reaching the default procedure.
struct Win32Hooks {
    void* context;
    void (*observe)(void* context, const WindowEvent& event) noexcept;
    bool (*frame)(void* context, HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept;
    bool (*shell)(void* context, HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept;
    bool (*input_blocked)(void* context) noexcept;
};

struct Win32WindowOptions {
    u32 extra_extended_style;  // WS_EX_* bits added to the application's own (probe windows)
    const Win32Hooks* hooks;   // copied; may be null
};

// Registers the window class and creates the main window with the window procedure of this backend, which
// translates every message into a WindowEvent for `sink` (win32_translate) and runs the hooks around it. Messages
// sent while the window is being created reach the sink too, before `*out` is set. False when the class or the
// window cannot be created; `reason`, when given, then names the step that failed ("register_class_failed",
// "adjust_initial_bounds_failed", "create_window_failed") for the caller's log.
bool win32_window_create(const WindowCreateInfo& info, const Win32WindowOptions& options, WindowSink sink,
                         void* context, Window* out, const char** reason) noexcept;

// Translates one message into the event it stands for and returns true when the message is modeled
// (event->type != none). Reads GetKeyState and the cursor and window position only.
bool win32_translate(HWND window, UINT message, WPARAM wparam, LPARAM lparam, WindowEvent* event) noexcept;

} // namespace maicord
