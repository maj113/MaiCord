// The window class and window procedure of the main window: every message becomes a WindowEvent for the sink
// (win32_translate), around the hooks the application keeps for what stays Windows-only (win32.hpp), with the
// system's default procedure as the answer to anything the sink leaves unhandled.
#include "maicord/platform/window/win32.hpp"

#include "internal.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {
namespace {

struct Backend {
    WindowSink sink;
    void* context;
#if defined(MAICORD_TEST_SEAMS)
    // The window options' tables (kWin32Hooks and kWin32CaptionOps are the shipped procedure's); the frame glue runs
    // only with a caption controller.
    Win32Hooks hooks;
    Win32CaptionOps caption;
    bool has_caption;
#endif
    // A TME_LEAVE request is outstanding; WM_MOUSELEAVE spends it.
    bool leave_armed;
    // The cursor of the WM_SETCURSOR being dispatched.
    bool cursor_valid;
    HWND cursor_window;
    float cursor_x, cursor_y;
} g{};

// Messages of the input classes a modal blocks that the sink does not model (IME, clipboard commands, commands,
// the odd key and mouse messages): swallowed while input_blocked.
bool blocked_class(UINT message) noexcept {
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST) || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
           (message >= WM_CUT && message <= WM_UNDO) ||
           (message >= WM_IME_STARTCOMPOSITION && message <= WM_IME_COMPOSITION) ||
           (message >= WM_IME_SETCONTEXT && message <= WM_IME_KEYUP) || message == WM_CONTEXTMENU ||
           message == WM_COMMAND || message == WM_COPYDATA;
}

// Hover affordances end when the pointer leaves the client, as a DOM mouseleave would: the request is shared by
// every layer and re-armed by the first move after each WM_MOUSELEAVE.
void arm_pointer_leave(HWND window) noexcept {
    if (g.leave_armed)
        return;
    TRACKMOUSEEVENT tracking{static_cast<DWORD>(sizeof(TRACKMOUSEEVENT)), TME_LEAVE, window, 0};
    g.leave_armed = TrackMouseEvent(&tracking) != FALSE;
}

// The system frame follows the dark scheme (DWMWA_USE_IMMERSIVE_DARK_MODE, with its pre-20H1 number as the fallback).
void enable_dark_frame(HWND window) noexcept {
    using SetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, const void*, DWORD);
    const HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm)
        return;
    const auto set_attribute = reinterpret_cast<SetWindowAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (!set_attribute)
        return;
    constexpr DWORD kImmersiveDarkModeCurrent = 20;
    constexpr DWORD kImmersiveDarkModeBefore20H1 = 19;
    const BOOL enabled = TRUE;
    // The module stays mapped: window_frame_inset measures the frame through it too.
    if (FAILED(set_attribute(window, kImmersiveDarkModeCurrent, &enabled, sizeof(enabled))))
        set_attribute(window, kImmersiveDarkModeBefore20H1, &enabled, sizeof(enabled));
}

} // namespace

namespace win32_detail {

void dispatch_cursor_begin(HWND window, float x, float y) noexcept {
    g.cursor_valid = true;
    g.cursor_window = window;
    g.cursor_x = x;
    g.cursor_y = y;
}

void dispatch_cursor_end() noexcept {
    g.cursor_valid = false;
}

bool dispatch_cursor(HWND window, float* x, float* y) noexcept {
    if (!g.cursor_valid || g.cursor_window != window)
        return false;
    *x = g.cursor_x;
    *y = g.cursor_y;
    return true;
}

#if defined(MAICORD_TEST_SEAMS)
const Win32Hooks& hooks() noexcept {
    return g.hooks;
}

const Win32CaptionOps* caption_ops() noexcept {
    return g.has_caption ? &g.caption : nullptr;
}
#else
const Win32Hooks& hooks() noexcept {
    return kWin32Hooks;
}

const Win32CaptionOps* caption_ops() noexcept {
    return &kWin32CaptionOps;
}
#endif

bool deliver(const WindowEvent& event, WindowEventResult* result) noexcept {
    if (!g.sink)
        return false;
    WindowEventResult local{};
    g.sink(g.context, event, result ? result : &local);
    return true;
}

void deliver_provisional_resize(HWND window, const RECT& client) noexcept {
    if (client.right <= client.left || client.bottom <= client.top)
        return;
    WindowEvent provisional{};
    provisional.type = EventType::resize;
    provisional.flags = ev_provisional;
    provisional.resize.width_px = static_cast<u32>(client.right - client.left);
    provisional.resize.height_px = static_cast<u32>(client.bottom - client.top);
    provisional.window = window;
    deliver(provisional, nullptr);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    WindowEvent event;
    const bool modeled = win32_translate(window, message, wparam, lparam, &event);
    struct CursorScope {
        bool active;
        ~CursorScope() {
            if (active)
                dispatch_cursor_end();
        }
    } cursor_scope{modeled && event.type == EventType::cursor_query};
    if (cursor_scope.active)
        dispatch_cursor_begin(window, event.pointer.x, event.pointer.y);
    if (message == WM_MOUSEMOVE)
        arm_pointer_leave(window);
    else if (message == WM_MOUSELEAVE)
        g.leave_armed = false;
    else if (message == WM_SETTINGCHANGE) {
        win32_cursor_handles_reset();
        win32_settings_reset();
    }
    LRESULT result = 0;
    // The frame the application draws owns the rows above the content and the non-client messages.
    if (const Win32CaptionOps* caption = caption_ops();
        caption && caption_frame_message(*caption, window, message, wparam, lparam, &result))
        return result;
    // UI Automation, tray icon callbacks, Explorer restarts and launches forwarded by a second instance.
    if (hooks().shell && hooks().shell(hooks().context, window, message, wparam, lparam, &result))
        return result;
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCCALCSIZE: {
        // Prepare the composition surface before Windows commits the HWND's new origin. Painting in
        // WM_WINDOWPOSCHANGED lets DWM display the old width at the new origin for one frame when dragging the
        // left edge.
        result = DefWindowProcW(window, message, wparam, lparam);
        if (wparam && lparam)
            deliver_provisional_resize(window, reinterpret_cast<const NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]);
        return result;
    }
    case WM_PAINT:
        // The composition surface and caret are GPU-rendered. Acknowledge the update region without acquiring a
        // GDI DC that no paint path uses.
        ValidateRect(window, nullptr);
        break;
    default:
        break;
    }
    if (!modeled) {
        if (blocked_class(message) && hooks().input_blocked && hooks().input_blocked(hooks().context))
            return 0;
        return DefWindowProcW(window, message, wparam, lparam);
    }
    WindowEventResult answer{};
    g.sink(g.context, event, &answer);
    switch (event.type) {
    case EventType::size_limits:
        if (answer.handled) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            if (limits) {
                // The sink answers the smallest client; the track size is the window around it.
                u32 width = 0, height = 0;
                frame_outer_size(caption_ops(), window, answer.width_px, answer.height_px, &width, &height);
                limits->ptMinTrackSize.x = maximum(limits->ptMinTrackSize.x, static_cast<LONG>(width));
                limits->ptMinTrackSize.y = maximum(limits->ptMinTrackSize.y, static_cast<LONG>(height));
            }
        }
        return 0;
    case EventType::scale_changed:
        if (const auto* suggested = reinterpret_cast<const RECT*>(lparam))
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    case EventType::destroyed:
        PostQuitMessage(0);
        return 0;
    case EventType::paint:
    case EventType::state_changed:
    case EventType::resize_begin:
    case EventType::resize_end:
        return 0;
    default:
        break;
    }
    if (!answer.handled)
        return DefWindowProcW(window, message, wparam, lparam);
    // A handled cursor query answers TRUE, which keeps the system from setting its own cursor; the rest answer 0.
    return event.type == EventType::cursor_query ? TRUE : 0;
}

} // namespace win32_detail

bool win32_window_create(const WindowCreateInfo& info, const Win32WindowOptions& options, WindowSink sink,
                         void* context, Window* out, const char** reason) noexcept {
    static constexpr wchar_t kClassName[] = L"MaiCordMainWindow";
    const char* reported = "";
    const char** failure = reason ? reason : &reported;
    if (!out || !sink || info.client_width_dips == 0 || info.client_height_dips == 0) {
        *failure = "window_request_invalid";
        return false;
    }
    g = {};
    g.sink = sink;
    g.context = context;
#if defined(MAICORD_TEST_SEAMS)
    if (options.hooks)
        g.hooks = *options.hooks;
    if (options.caption) {
        g.caption = *options.caption;
        g.has_caption = true;
    }
#endif
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    // A flip-model swap chain owns all client pixels. Class-wide redraws generate duplicate WM_PAINT traffic during
    // the modal sizing loop, and CS_OWNDC only retains a GDI DC that this renderer never uses.
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = win32_detail::window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kClassName;
    if (!RegisterClassExW(&window_class)) {
        *failure = "register_class_failed";
        return false;
    }
    const DWORD extended_style = WS_EX_NOREDIRECTIONBITMAP | options.extra_extended_style;
    RECT bounds{0, 0, static_cast<LONG>(info.client_width_dips), static_cast<LONG>(info.client_height_dips)};
    if (!AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, extended_style, 96)) {
        *failure = "adjust_initial_bounds_failed";
        return false;
    }
    u16 title[128];
    u32 written = 0, next = 0;
    if (!utf8_copy_utf16_range(info.title, 0, info.title.bytes, {title, 127}, &written, &next))
        written = 0;
    title[written] = 0;
    // The window keeps the system frame's outer size; the caption moves into the client (WM_NCCALCSIZE).
    const HWND window = CreateWindowExW(extended_style, kClassName, reinterpret_cast<const wchar_t*>(title),
                                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
                                        bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
    if (!window) {
        *failure = "create_window_failed";
        return false;
    }
    win32_detail::set_main_window(window);
    if (options.dark_frame)
        enable_dark_frame(window);
    *out = win32_window(window);
    return true;
}

bool window_create(const WindowCreateInfo& info, WindowSink sink, void* context, Window* out) noexcept {
    return win32_window_create(info, {}, sink, context, out, nullptr);
}

} // namespace maicord
