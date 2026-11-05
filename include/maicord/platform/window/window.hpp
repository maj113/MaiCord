#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/platform/window/key.hpp"

namespace maicord {

enum class UiCursor : u8;

// The window service: free functions over an opaque handle. One top-level window per backend; window_id in
// WindowEvent is reserved for a second.
struct WindowObject;
using Window = WindowObject*;

// What the backend can do; portable code branches on these, never on the operating system.
struct WindowCaps {
    bool csd;                     // the app draws the frame
    bool native_caption_buttons;  // caption buttons need non-client hit-test answers (snap layouts)
    bool absolute_position;       // windows can be placed at screen coordinates
    bool sync_clipboard_read;     // clipboard text can be read synchronously
    bool key_prefilter;           // a text service sees keys before the app (TSF)
    bool global_key_state;        // key state is readable while the window is in the background
    bool fractional_scale;
    bool window_menu;             // window_show_system_menu works
    bool maximize, minimize, fullscreen;
};
const WindowCaps& window_caps() noexcept;

enum class ShowMode : u8 { hide, show, show_default, minimize_no_activate, restore };
enum class Attention : u8 { stop, flash };

// Straight-alpha BGRA, top-down, extent x extent.
struct IconImage {
    const u32* pixels;
    u32 extent;
};

struct WindowCreateInfo {
    Utf8View title;
    Utf8View app_id;
    u32 client_width_dips, client_height_dips;
    bool visible;
};

bool window_create(const WindowCreateInfo& info, WindowSink sink, void* context, Window* out) noexcept;
void window_destroy(Window window) noexcept;
NativeWindow window_surface_native(Window window) noexcept;
void window_show(Window window, ShowMode mode) noexcept;
void window_set_title(Window window, Utf8View title) noexcept;
// The window's icon at several sizes (taskbar and title bar; the smallest and the largest image are the small and
// the large icon where the system has both). `count` of at least one.
void window_set_icon(Window window, const IconImage* images, u32 count) noexcept;
void window_minimize(Window window) noexcept;
void window_toggle_maximize(Window window) noexcept;
void window_request_close(Window window) noexcept;
// Asynchronous where the compositor decides: the outcome arrives as state_changed.
bool window_set_fullscreen(Window window, bool fullscreen) noexcept;
void window_show_system_menu(Window window, float x, float y) noexcept;
// No-ops where the non-client frame does this (Windows).
void window_begin_move(Window window) noexcept;
void window_begin_resize(Window window, FrameHit edge) noexcept;
void window_set_min_size(Window window, u32 width_px, u32 height_px) noexcept;
void window_request_attention(Window window, Attention attention) noexcept;
void window_set_drop_target(Window window, bool accept_files) noexcept;
void window_invalidate(Window window) noexcept;
// The StateFlag bits of the window now: minimized, visible, maximized, fullscreen, ...
u32 window_state(Window window) noexcept;
// The thickness in pixels, at `dpi`, of the border a window the app draws the frame of can be resized from: the
// system's on Windows (outside the client at the sides and bottom, a band inside its top), a fixed band elsewhere.
u32 window_resize_border(Window window, u32 dpi) noexcept;
// The visible frame pixels beside the client beyond those above it: the shadow border of a restored Windows window,
// zero where the app draws the whole frame. False while it cannot be measured (hidden or minimized window).
bool window_frame_inset(Window window, u32* pixels) noexcept;

// Input.
void window_set_cursor(Window window, UiCursor cursor) noexcept;
void window_request_cursor_refresh(Window window) noexcept;
bool window_pointer_position(Window window, float* x_px, float* y_px) noexcept;
void window_capture_pointer(Window window) noexcept;
void window_release_capture(Window window) noexcept;
bool window_pointer_captured(Window window) noexcept;
bool window_focus(Window window) noexcept;
// Key state belongs to the input device, not to a window: these four take a null Window (the main window) so code
// without a window handle can read it. The state is as of the event being handled.
u8 window_modifiers(Window window) noexcept;
bool window_key_down(Window window, Key key) noexcept;
bool window_toggle_state(Window window, Key key) noexcept;
// Whether the active layout turns this key with these modifiers into text.
bool window_layout_types_text(Window window, Key key, u8 modifiers) noexcept;

// Geometry the app reads between events.
struct WindowMetrics {
    u32 width_px, height_px, scale_120, dpi;
};
WindowMetrics window_metrics(Window window) noexcept;

// Presentation. Wayland: queues one frame callback on the surface (answered through the frame listener when the compositor
// wants the next frame); it rides the next commit, which the GPU backend's buffer swap makes. Nothing on Windows.
void window_frame_callback_request(Window window) noexcept;
using FrameListener = void (*)(void* context, u32 time_ms) noexcept;
void window_set_frame_listener(Window window, FrameListener listener, void* context) noexcept;
// Any thread.
void window_wake(Window window) noexcept;
// Hands an event to the window's sink as the backend would, synchronously and marked ev_synthetic: accessibility
// actions and tests. UI thread only; false when the window has no sink. `result` may be null.
bool window_inject(Window window, const WindowEvent& event, WindowEventResult* result) noexcept;

// Fatal startup errors, before a window exists.
void window_fatal_message(Utf8View title, Utf8View text) noexcept;
// The application's own icon as stored in the program: the smallest PNG image (8-bit palette) whose side is at least
// `extent` and at most `largest`. Empty when there is none.
ConstSlice<u8> window_app_icon_png(u32 extent, u32 largest, u32* side) noexcept;

// Cross-thread wake: the cause reaches the window as a wake event with a and b.
struct Waker {
    NativeWindow window;
    u32 cause;  // WakeCause
};
bool window_post(const Waker& waker, u64 a, u64 b) noexcept;

// System values the app reads again on settings_changed.
inline constexpr u32 kSystemIdleUnknown = 0xFFFFFFFFu;
struct PlatformSettings {
    u32 double_click_ms;
    u32 double_click_dx_px, double_click_dy_px;
    u32 caret_blink_ms;
    u32 wheel_lines;
    bool wheel_page;  // one notch scrolls a page
    bool client_area_animation;
    bool dark_scheme;
    u32 system_idle_ms;  // kSystemIdleUnknown where the system cannot tell
};
PlatformSettings platform_settings_read() noexcept;

} // namespace maicord
