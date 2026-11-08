// The window service on Windows (include/maicord/platform/window/window.hpp): the main window's pointer capture,
// focus, cursor, state and presentation calls over the HWND a Window stands for.
#include "maicord/platform/window/window.hpp"
#include "maicord/platform/window/win32.hpp"
#include "internal.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/desktop/window_frame.hpp"
#include "maicord/ui/core/cursor.hpp"

namespace maicord {
namespace {

constexpr WindowCaps kCaps{
    true,  // csd: the caption bar is drawn into the client area (WM_NCCALCSIZE)
    true,  // native_caption_buttons: snap layouts need the frame hit-test answers
    false, // fractional_scale
    true,  // window_menu
    true,  // maximize
    true,  // minimize
    true,  // fullscreen
};

// Stock cursors are shared system objects: load each once, and again after a system settings change (a new
// cursor scheme). UiCursor::hidden is the last enumerator.
constexpr u32 kStockCursorCount = static_cast<u32>(UiCursor::hidden) + 1;
HCURSOR g_stock_cursors[kStockCursorCount]{};

const wchar_t* cursor_resource(UiCursor cursor) noexcept {
    switch (cursor) {
    case UiCursor::hand:
        return IDC_HAND;
    case UiCursor::ibeam:
        return IDC_IBEAM;
    case UiCursor::resize_we:
        return IDC_SIZEWE;
    case UiCursor::move:
        return IDC_SIZEALL;
    case UiCursor::hidden:
        return nullptr;
    case UiCursor::arrow:
    default:
        return IDC_ARROW;
    }
}

// WS_EX_ACCEPTFILES as this module last set it. The style is read once per window and written only on a change;
// nothing else touches it.
HWND g_drop_window;
bool g_drop_registered;

// The window before fullscreen, restored on the way out.
struct Fullscreen {
    WINDOWPLACEMENT placement;
    LONG_PTR style;
    bool on;
} g_fullscreen{};

} // namespace

void win32_cursor_handles_reset() noexcept {
    for (HCURSOR& cursor : g_stock_cursors)
        cursor = nullptr;
}

void win32_menu_appearance(bool dark) noexcept {
    // Ordinal 135 takes its argument in ECX on every build: PreferredAppMode from 1903 (2 ForceDark, 0 Default), a
    // BOOL on 1809 (AllowDarkModeForApp tests its low byte, so 2 allows dark). Neither return value is used.
    using SetPreferredAppMode = int(WINAPI*)(int mode) noexcept;
    using FlushMenuThemes = void(WINAPI*)() noexcept;
    static SetPreferredAppMode set_mode;
    static FlushMenuThemes flush_menu_themes;
    static bool resolved;
    if (!resolved) {
        resolved = true;
        if (const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            set_mode = reinterpret_cast<SetPreferredAppMode>(
                GetProcAddress(uxtheme, reinterpret_cast<LPCSTR>(static_cast<usize>(135))));
            flush_menu_themes = reinterpret_cast<FlushMenuThemes>(
                GetProcAddress(uxtheme, reinterpret_cast<LPCSTR>(static_cast<usize>(136))));
        }
    }
    if (!set_mode || !flush_menu_themes)
        return;
    set_mode(dark ? 2 : 0);
    flush_menu_themes();
}

const WindowCaps& window_caps() noexcept {
    return kCaps;
}

void window_destroy(Window window) noexcept {
    DestroyWindow(win32_hwnd(window));
}

NativeWindow window_surface_native(Window window) noexcept {
    return window;
}

void window_show(Window window, ShowMode mode) noexcept {
    int command = SW_SHOWDEFAULT;
    switch (mode) {
    case ShowMode::hide:
        command = SW_HIDE;
        break;
    case ShowMode::show:
        command = SW_SHOW;
        break;
    case ShowMode::show_default:
        command = SW_SHOWDEFAULT;
        break;
    case ShowMode::minimize_no_activate:
        command = SW_SHOWMINNOACTIVE;
        break;
    case ShowMode::restore:
        command = SW_RESTORE;
        break;
    }
    ShowWindow(win32_hwnd(window), command);
}

void window_set_title(Window window, Utf8View title) noexcept {
    u16 text[256];
    u32 written = 0, next = 0;
    if (!utf8_copy_utf16_range(title, 0, title.bytes, {text, 255}, &written, &next))
        written = 0;
    text[written] = 0;
    SetWindowTextW(win32_hwnd(window), reinterpret_cast<const wchar_t*>(text));
}

void window_minimize(Window window) noexcept {
    SendMessageW(win32_hwnd(window), WM_SYSCOMMAND, SC_MINIMIZE, 0);
}

void window_toggle_maximize(Window window) noexcept {
    const HWND hwnd = win32_hwnd(window);
    SendMessageW(hwnd, WM_SYSCOMMAND, IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
}

void window_request_close(Window window) noexcept {
    PostMessageW(win32_hwnd(window), WM_CLOSE, 0, 0);
}

bool window_set_fullscreen(Window window, bool fullscreen) noexcept {
    const HWND hwnd = win32_hwnd(window);
    if (!hwnd || fullscreen == g_fullscreen.on)
        return false;
    if (fullscreen) {
        reset_value(&g_fullscreen.placement);
        g_fullscreen.placement.length = sizeof(g_fullscreen.placement);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetWindowPlacement(hwnd, &g_fullscreen.placement) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
            return false;
        g_fullscreen.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_fullscreen.style & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME));
        const auto& r = monitor.rcMonitor;
        SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_fullscreen.style);
        SetWindowPlacement(hwnd, &g_fullscreen.placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen.on = fullscreen;
    return true;
}

void window_show_system_menu(Window window, float, float) noexcept {
    // As Alt+Space: the window menu at the caption.
    PostMessageW(win32_hwnd(window), WM_SYSCOMMAND, SC_KEYMENU, key::space);
}

void window_set_min_size(Window, u32, u32) noexcept {}

void window_request_attention(Window window, Attention attention) noexcept {
    const bool start = attention == Attention::flash;
    FLASHWINFO info{sizeof(info), win32_hwnd(window),
                    start ? static_cast<DWORD>(FLASHW_TRAY | FLASHW_TIMERNOFG) : static_cast<DWORD>(FLASHW_STOP),
                    start ? 3u : 0u, 0};
    FlashWindowEx(&info);
}

void window_set_drop_target(Window window, bool accept_files) noexcept {
    const HWND hwnd = win32_hwnd(window);
    if (!hwnd)
        return;
    if (g_drop_window != hwnd) {
        g_drop_window = hwnd;
        g_drop_registered = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) != 0;
    }
    if (g_drop_registered == accept_files)
        return;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      accept_files ? style | WS_EX_ACCEPTFILES : style & ~LONG_PTR{WS_EX_ACCEPTFILES});
    g_drop_registered = accept_files;
}

void window_invalidate(Window window) noexcept {
    InvalidateRect(win32_hwnd(window), nullptr, FALSE);
}

void window_set_cursor(Window, UiCursor cursor) noexcept {
    const u32 index = static_cast<u32>(cursor) < kStockCursorCount ? static_cast<u32>(cursor) : 0;
    HCURSOR& handle = g_stock_cursors[index];
    if (!handle)
        if (const wchar_t* resource = cursor_resource(static_cast<UiCursor>(index)))
            handle = LoadCursorW(nullptr, resource);
    SetCursor(handle);
}

void window_request_cursor_refresh(Window window) noexcept {
    const HWND hwnd = win32_hwnd(window);
    POINT cursor{};
    // Captured drags own their cursor until release.
    if (!hwnd || GetCapture() || !GetCursorPos(&cursor) || WindowFromPoint(cursor) != hwnd)
        return;
    const LRESULT hit = SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(cursor.x, cursor.y));
    SendMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELPARAM(static_cast<WORD>(hit), WM_MOUSEMOVE));
}

bool window_pointer_position(Window window, float* x_px, float* y_px) noexcept {
    const HWND hwnd = win32_hwnd(window);
    float x = 0, y = 0;
    if (!win32_detail::dispatch_cursor(hwnd, &x, &y)) {
        POINT point{};
        if (!GetCursorPos(&point) || !ScreenToClient(hwnd, &point))
            return false;
        x = static_cast<float>(point.x);
        y = static_cast<float>(point.y);
    }
    if (x_px)
        *x_px = x;
    if (y_px)
        *y_px = y;
    return true;
}

void window_capture_pointer(Window window) noexcept {
    SetCapture(win32_hwnd(window));
}

// Pointer capture and keyboard focus belong to the main window; never release another owner's capture.
void window_release_capture(Window window) noexcept {
    if (GetCapture() == win32_hwnd(window))
        ReleaseCapture();
}

bool window_pointer_captured(Window window) noexcept {
    return GetCapture() == win32_hwnd(window);
}

// One copy: the callers ignore the result, so inlining would only repeat the GetFocus fallback at every click.
__declspec(noinline) bool window_focus(Window window) noexcept {
    const HWND hwnd = win32_hwnd(window);
    // SetFocus returns the window that had the focus, null when none did or on failure.
    return SetFocus(hwnd) != nullptr || GetFocus() == hwnd;
}

WindowMetrics window_metrics(Window window) noexcept {
    const HWND hwnd = win32_hwnd(window);
    RECT client{};
    GetClientRect(hwnd, &client);
    const u32 dpi = GetDpiForWindow(hwnd);
    return {static_cast<u32>(client.right), static_cast<u32>(client.bottom), dpi * 120 / 96, dpi};
}

u32 window_display_count() noexcept {
    const int count = GetSystemMetrics(SM_CMONITORS);
    return count > 0 ? static_cast<u32>(count) : 0;
}

u32 window_state(Window window) noexcept {
    const HWND hwnd = win32_hwnd(window);
    u32 state = 0;
    if (IsIconic(hwnd))
        state |= state_minimized;
    if (IsWindowVisible(hwnd))
        state |= state_visible;
    if (IsZoomed(hwnd))
        state |= state_maximized;
    if (g_fullscreen.on)
        state |= state_fullscreen;
    return state;
}

// SM_CXFRAME + SM_CXPADDEDBORDER.
u32 window_resize_border(Window, u32 dpi) noexcept {
    return static_cast<u32>(window_frame_border(dpi));
}

// The DWM border of a restored window lies outside the client at the sides but over the client's top row; none while
// maximized.
bool window_frame_inset(Window window, u32* pixels) noexcept {
    using GetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, void*, DWORD);
    static GetWindowAttribute get_attribute;
    static bool resolved;
    if (!resolved) {
        resolved = true;
        if (const HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            get_attribute = reinterpret_cast<GetWindowAttribute>(GetProcAddress(dwm, "DwmGetWindowAttribute"));
    }
    constexpr DWORD kExtendedFrameBounds = 9;
    const HWND hwnd = win32_hwnd(window);
    RECT visible{}, client{};
    POINT origin{};
    if (!get_attribute || !IsWindowVisible(hwnd) || IsIconic(hwnd) || !GetClientRect(hwnd, &client) ||
        !ClientToScreen(hwnd, &origin) || FAILED(get_attribute(hwnd, kExtendedFrameBounds, &visible, sizeof(visible))))
        return false;
    const LONG side = minimum(origin.x - visible.left, visible.right - origin.x - client.right);
    const LONG frame = side - (origin.y - visible.top);
    *pixels = frame > 0 && frame < 8 ? static_cast<u32>(frame) : 0;
    return true;
}

void window_frame_callback_request(Window) noexcept {}

void window_set_frame_listener(Window, FrameListener, void*) noexcept {}

void window_wake(Window window) noexcept {
    PostMessageW(win32_hwnd(window), WM_NULL, 0, 0);
}

// Pointer events go through the window procedure as the messages Windows itself sends (the application's own
// hooks see them, and a nested SendMessage keeps its timing); anything else is handed to the sink directly.
bool window_inject(Window window, const WindowEvent& event, WindowEventResult* result) noexcept {
    const HWND hwnd = win32_hwnd(window);
    if (result)
        *result = {};
    WPARAM wparam = 0;
    UINT message = 0;
    const u32 buttons = event.pointer.buttons;
    if (event.type == EventType::pointer_move || event.type == EventType::pointer_down ||
        event.type == EventType::pointer_up) {
        wparam = ((buttons & (1u << static_cast<u8>(PointerButton::left))) ? MK_LBUTTON : 0) |
                 ((buttons & (1u << static_cast<u8>(PointerButton::right))) ? MK_RBUTTON : 0) |
                 ((buttons & (1u << static_cast<u8>(PointerButton::middle))) ? MK_MBUTTON : 0) |
                 ((event.modifiers & mod_shift) ? MK_SHIFT : 0) | ((event.modifiers & mod_ctrl) ? MK_CONTROL : 0);
        if (event.type == EventType::pointer_move) {
            message = WM_MOUSEMOVE;
        } else if (event.button == static_cast<u8>(PointerButton::left)) {
            message = event.type == EventType::pointer_down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
        } else if (event.button == static_cast<u8>(PointerButton::right)) {
            message = event.type == EventType::pointer_down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
        } else if (event.button == static_cast<u8>(PointerButton::middle)) {
            message = event.type == EventType::pointer_down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
        }
        if (message) {
            SendMessageW(hwnd, message, wparam,
                         MAKELPARAM(static_cast<i32>(event.pointer.x), static_cast<i32>(event.pointer.y)));
            if (result)
                result->handled = true;
            return true;
        }
    }
    if (event.type == EventType::wheel) {
        POINT screen{static_cast<LONG>(event.wheel.x), static_cast<LONG>(event.wheel.y)};
        if (!ClientToScreen(hwnd, &screen))
            return false;
        const i32 delta = event.wheel.dy120 ? event.wheel.dy120 : event.wheel.dx120;
        SendMessageW(hwnd, event.wheel.dy120 ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL, MAKEWPARAM(0, delta),
                     MAKELPARAM(screen.x, screen.y));
        if (result)
            result->handled = true;
        return true;
    }
    WindowEvent synthetic = event;
    synthetic.flags |= ev_synthetic;
    return win32_detail::deliver(synthetic, result);
}

} // namespace maicord
