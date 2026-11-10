#pragma once

// The Windows side of the window service: what code that must name an HWND uses. A Window is the HWND itself, so
// the conversions are free. Portable code never includes this header.
#include "maicord/core/win32_types.hpp"
#include "maicord/platform/window/frame.hpp"
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
// The native menus (process-wide: the window menu, the tray's) are dark or light from their next opening. uxtheme's
// undocumented app mode, export ordinal 135: SetPreferredAppMode from Windows 10 1903 (dark forces dark, light is the
// default, light menus); AllowDarkModeForApp(BOOL) on 1809, where a dark menu also needs the system's dark app mode.
// Then FlushMenuThemes (ordinal 136) drops the menu themes uxtheme cached for the previous mode.
void win32_menu_appearance(bool dark) noexcept;

// A top-down straight-alpha BGRA image (side `extent`, at most 64) as an icon the system copies from it; the caller
// destroys the icon. Null for an invalid image.
HICON win32_create_icon(ConstSlice<u32> pixels, u32 extent) noexcept;

// The application's half of the window procedure, called by name (src/app/windows/platform_window.cpp defines them;
// a test that creates a window defines its own).
//
// The caption controller, which the frame glue (caption_frame.cpp) asks: the glue owns the system side of a client
// area that extends over the system caption: WM_NCCALCSIZE, WM_NCHITTEST, the non-client mouse messages of the
// maximize button, WM_NCACTIVATE, the window menu and the snap layouts flyout. Client pixels are relative to the
// client origin; the glue converts to and from screen coordinates.
// False: no caption controller, the system frame and client coordinates from the window's corner.
bool win32_caption_enabled() noexcept;
u32 win32_caption_height() noexcept;  // rows of the bar above the content; zero without one
bool win32_caption_hidden() noexcept; // fullscreen: the system frame again
FrameHit win32_caption_hit(float x, float y) noexcept; // the frame part under a client pixel
void win32_caption_maximized(bool maximized) noexcept;
void win32_caption_active(bool active) noexcept;       // the frame's activation (WM_NCACTIVATE)
// The non-client button (FrameHit::maximize) under the pointer: pointer over a part (FrameHit::client: none), gone
// from the frame, pressed, released on a part. release returns whether it ended a press of the controller's.
void win32_caption_hover(FrameHit part) noexcept;
void win32_caption_leave() noexcept;
void win32_caption_press(FrameHit part) noexcept;
bool win32_caption_release(FrameHit part) noexcept;
bool win32_caption_title_bar(FrameBarRects* rectangles) noexcept; // false without a bar

// What the Windows build of the application keeps for itself in the window procedure, beside the sink every backend
// has: UI Automation and the shell integration. win32_shell_message returns true when it handled the raw message and
// `*result` is its LRESULT; it runs ahead of the sink, after the caption frame glue (UI Automation: WM_GETOBJECT and
// its deferred actions, tray icon callbacks, a second launch).
// win32_input_blocked tells whether a modal owns all input: a message of the input classes the sink does not model
// (IME, clipboard commands) is then swallowed instead of reaching the default procedure.
bool win32_shell_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept;
bool win32_input_blocked() noexcept;

struct Win32WindowOptions {
    u32 extra_extended_style; // WS_EX_* bits added to the application's own (probe windows)
    bool dark_frame;          // the system frame follows the dark scheme
};

// Registers the window class and creates the main window with the window procedure of this backend, which
// translates every message into a WindowEvent for `sink` (win32_translate) and runs the hooks around it. Messages
// sent while the window is being created reach the sink too, before `*out` is set. False when the class or the
// window cannot be created; `reason`, when given, then names the step that failed ("register_class_failed",
// "adjust_initial_bounds_failed", "create_window_failed") for the caller's log.
bool win32_window_create(const WindowCreateInfo& info, const Win32WindowOptions& options, WindowSink sink,
                         void* context, Window* out, const char** reason) noexcept;

// Translates one message into the event it stands for and returns true when the message is modeled
// (event->type != none). Reads GetKeyState, a press's or release's message time and the cursor and window position
// only; a cursor query takes the point of the WM_NCHITTEST before it, and asks the system without one.
bool win32_translate(HWND window, UINT message, WPARAM wparam, LPARAM lparam, WindowEvent* event) noexcept;

} // namespace maicord
