#pragma once

#include "maicord/core/base.hpp"
#include "maicord/platform/window/key.hpp"

namespace maicord {

struct DroppedFiles;

// The platform's own window handle: an HWND, or the wl_surface* of the Wayland backend. What GPU surface creation
// and text services take; everything else goes through Window (window.hpp).
using NativeWindow = void*;

enum class EventType : u8 {
    none,
    // Pointer. Coordinates are client pixels, origin top-left of the client area (caption rows included), and
    // may lie outside the window while a button is held.
    pointer_move,
    pointer_down,
    pointer_up,
    pointer_leave,
    wheel,
    capture_lost,  // WM_CAPTURECHANGED; also synthesised when a Wayland grab ends
    cancel_input,  // WM_CANCELMODE
    cursor_query,  // WM_SETCURSOR over the client area; result.handled = a layer answered
    context_menu,  // pointer or keyboard (ev_keyboard), WM_CONTEXTMENU
    // Keyboard and text.
    key_down,
    key_up,
    character,  // one UTF-16 code unit
    ime_preedit,
    ime_commit,
    ime_delete_surrounding,  // Linux text-input-v3 only
    // Focus and state.
    focus_changed,       // keyboard focus (WM_SETFOCUS/KILLFOCUS, wl_keyboard enter/leave)
    activation_changed,  // foreground (WM_ACTIVATE, xdg_toplevel activated)
    state_changed,       // StateFlag bits
    resize,
    resize_begin,
    resize_end,
    scale_changed,
    close_requested,  // result.handled = veto
    destroyed,
    // Presentation.
    paint,       // Windows WM_PAINT damage; never on Wayland
    frame_done,  // wl_callback.done; never on Windows
    // Data.
    drop_files,
    clipboard_changed,
    // System and loop.
    settings_changed,
    wake,
    timer,                 // reserved for Windows UIA only
    clipboard_data_ready,  // reserved: asynchronous clipboard read; not emitted yet
    // Synchronous queries; the answer goes in WindowEventResult.
    hit_test,
    size_limits,
};

enum EventFlag : u8 {
    ev_repeat = 1,        // auto-repeat (WM_KEYDOWN bit 30; Linux: backend timer)
    ev_double_click = 2,  // a pointer_down that Windows delivers as WM_xBUTTONDBLCLK
    ev_system = 4,        // WM_SYSKEY*/WM_SYSCHAR: Alt-modified (or F10) key
    ev_synthetic = 8,     // injected: accessibility actions, cursor refresh, test fixtures
    ev_keyboard = 16,     // context_menu raised by the keyboard (lparam == -1 today)
    ev_provisional = 32,  // resize computed during WM_NCCALCSIZE, before the window moves
    ev_minimized = 64,    // activation_changed while the window is minimized (WM_ACTIVATE's HIWORD)
    ev_owned_focus = 128, // focus_changed (lost): the window gaining focus is one this window owns (a dialog or menu)
};

// state_changed bits.
enum StateFlag : u32 {
    state_minimized = 1,
    state_visible = 2,
    state_maximized = 4,
    state_fullscreen = 8,
    state_suspended = 16,
    state_tiled = 32,
    state_decorated = 64,
    state_geometry = 128,  // the size or the frame changed (WM_WINDOWPOSCHANGED without SWP_NOSIZE, or SWP_FRAMECHANGED)
};

// settings_changed areas: what to read again from PlatformSettings.
enum SettingsArea : u32 {
    settings_pointer = 1,  // double-click time and rectangle, wheel lines
    settings_caret = 2,    // caret blink time
    settings_motion = 4,   // client-area animation
    settings_scheme = 8,   // dark or light colour scheme
};

enum WakeCause : u32 {
    wake_poke,  // WM_NULL
    wake_begin_webauthn,
    wake_phone_verify,
    wake_notifications,
    wake_accessibility,
    wake_tray,  // Windows
};

// Where a point of the window lies for the frame the app draws; the answer to hit_test.
enum class FrameHit : u8 {
    client,
    caption,
    system_menu,
    minimize,
    maximize,
    close,
    left,
    right,
    top,
    bottom,
    top_left,
    top_right,
    bottom_left,
    bottom_right,
    nowhere,
};


struct WindowEvent {
    EventType type;
    u8 flags;      // EventFlag
    u8 modifiers;  // Modifier; filled for key, pointer-button and wheel events only
    u8 button;     // PointerButton for pointer_down and pointer_up
    u32 time_ms;   // message time (wraps)
    u32 window_id; // the main window until a second window exists
    union {
        struct {
            float x, y;
            u8 buttons;
        } pointer;  // pointer_*, cursor_query, context_menu, hit_test
        struct {
            float x, y;
            i32 dx120, dy120;  // 120 = one notch
            float dx_px, dy_px;
            u8 source;
        } wheel;
        struct {
            u32 key;  // a Key
            u32 scancode;
        } key;
        struct {
            u32 unit;
        } character;
        struct {
            const u16* text;
            u32 count;
            i32 cursor_begin, cursor_end;
            u32 delete_before, delete_after;
        } ime;
        struct {
            bool gained;
            NativeWindow other;  // the window gaining or losing focus or capture
        } focus;
        struct {
            u32 changed, state;  // StateFlag
        } window_state;
        struct {
            u32 width_px, height_px, scale_120;  // scale_120 = 120 * dpi / 96
        } resize;
        struct {
            DroppedFiles* files;
        } drop;
        struct {
            u32 cause;  // WakeCause
            u64 a, b;
        } wake;
        struct {
            u32 areas;  // SettingsArea
        } settings;
    };
    NativeWindow window;  // the platform window the event is for (the HWND on Windows; null where it is not needed)
    const void* native;  // Windows: the MSG being dispatched, for the text-services key filter; null elsewhere
};

// An unhandled event means the backend default: DefWindowProc on Windows, the documented default per event on
// Wayland.
struct WindowEventResult {
    bool handled;
    i32 value;  // FrameHit, consumed-cursor flag, ...
    u32 width_px, height_px;
};

using WindowSink = void (*)(void* context, const WindowEvent& event, WindowEventResult* result) noexcept;

} // namespace maicord
