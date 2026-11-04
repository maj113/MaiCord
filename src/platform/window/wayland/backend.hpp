#pragma once

// The state and the cross-file entry points of the Wayland window backend (src/platform/window/wayland). One
// connection and one top-level window, as the window service has for now. Everything runs on the UI thread except
// window_wake and the global key bits, which are atomic. Listeners only record state and enqueue WindowEvents in the
// ring; window_pump drains it and calls the sink, so no wl_*_listener ever runs app code (design-window-input.md §5).

#include "maicord/os/wait.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/platform/window/wayland/click.hpp"
#include "maicord/platform/window/wayland/repeat.hpp"
#include "maicord/platform/window/wayland/xkb.hpp"
#include "maicord/platform/window/window.hpp"
#include "wayland_protocols.hpp"

namespace maicord::wayland {

inline constexpr u32 kRingCapacity = 1024;
inline constexpr u32 kMaxGlobals = 128;

struct GlobalName {
    char name[56];
    u32 version;
};

struct EventRing {
    WindowEvent events[kRingCapacity];
    u32 head;  // next to read
    u32 tail;  // next to write
    u32 dropped;
};

// Bits of PointerEvent::buttons: 1 << PointerButton.
inline constexpr u8 button_bit(PointerButton button) noexcept {
    return static_cast<u8>(1u << static_cast<u8>(button));
}

struct PointerState {
    wl_pointer* pointer;
    wp_cursor_shape_device_v1* shape_device;
    bool inside;
    u32 enter_serial;
    float x, y;  // client pixels, the last position the compositor reported
    u8 buttons;  // 1 << PointerButton, as the compositor sent them
    u32 press_serial;

    bool motion_pending;
    u32 motion_time;
    bool axis_pending;
    u32 axis_time;
    float axis_px_x, axis_px_y;  // wl_pointer.axis in surface units, positive = right and down
    i32 axis_value120_x, axis_value120_y;
    i32 axis_discrete_x, axis_discrete_y;
    u8 axis_source;
    float remainder_x, remainder_y;  // pixel scroll not yet turned into 120ths
    ClickTracker click;

    // What the app has seen: updated as events are dispatched, not as they arrive.
    u8 app_buttons;
    bool captured;

    // The cursor: what the app asked for, and what the compositor shows.
    u8 desired_cursor;  // UiCursor
    u8 applied_cursor;
    bool applied_valid;
    bool in_cursor_query;
    wl_surface* cursor_surface;  // the wl_cursor fallback
    CursorTheme* theme;
    u32 theme_scale;
};

struct KeyboardState {
    wl_keyboard* keyboard;
    XkbContext* context;
    XkbKeymap* keymap;
    XkbState* state;
    XkbComposeTable* compose_table;
    XkbComposeState* compose_state;
    bool compose_tried;
    u32 level_three_modifier;  // the xkb index of the AltGr modifier, kXkbModifierInvalid when the keymap has none
    u8 altgr_count;            // AltGr (ISO_Level3_Shift) keys down
    u8 down_altgr[768];
    bool focused;
    u32 serial;
    u8 modifiers;  // Modifier bits of the current xkb state
    u32 group;     // the locked layout group of the last wl_keyboard.modifiers
    i32 repeat_rate;   // per second, 0 = off
    i32 repeat_delay;  // milliseconds
    KeyRepeat repeat;
    Waitable repeat_timer;
    // The text a repeating key produced when it went down.
    u16 repeat_text[4];
    u32 repeat_text_units;
    u8 repeat_key;
    u8 repeat_flags;
    // Keys currently down: per evdev code the portable key it was pressed as (0 = up).
    u8 down_key[768];
    u8 down_specific[768];
    u8 held_count[256];  // by Key, generic and side-specific
};

struct WindowState {
    WindowSink sink;
    void* sink_context;

    wl_surface* surface;
    xdg_surface* shell_surface;
    xdg_toplevel* toplevel;
    zxdg_toplevel_decoration_v1* decoration;
    wp_viewport* viewport;
    wp_fractional_scale_v1* fractional;
    wl_callback* frame_callback;
    FrameListener frame_listener;
    void* frame_listener_context;

    bool configured;
    bool visible;
    bool external_buffers;  // the GPU backend commits buffers; no shm clear
    u32 requested_width, requested_height;  // logical size from WindowCreateInfo
    u32 logical_width, logical_height;
    u32 floating_width, floating_height;    // the last size outside maximized, fullscreen and tiled states
    u32 pending_width, pending_height;
    u32 pending_states;
    u32 states;
    u32 scale_120;  // the compositor's preferred scale, 120 = 1.0
    u32 dpi;
    u32 state_bits;  // StateFlag
    bool resizing;
    u32 clear_color;
    wl_buffer* shm_buffer;
};

struct Backend {
    wl_display* display;
    wl_registry* registry;
    Waitable display_waitable;
    bool read_prepared;
    bool failed;  // the connection broke or the compositor raised a protocol error

    wl_compositor* compositor;
    wl_shm* shm;
    xdg_wm_base* wm_base;
    wl_seat* seat;
    wp_viewporter* viewporter;
    wp_fractional_scale_manager_v1* fractional_manager;
    wp_cursor_shape_manager_v1* cursor_shape_manager;
    zxdg_decoration_manager_v1* decoration_manager;
    u32 compositor_version;
    u32 seat_version;
    u32 wm_base_version;

    WindowCaps caps;
    EventRing ring;
    PointerState pointer;
    KeyboardState keyboard;
    WindowState window;
    bool window_exists;
    GlobalName globals[kMaxGlobals];  // everything the registry advertised, for the support matrix
    u32 global_count;

    bool quit;
    int exit_code;
    u32 wake_pending;  // atomic: window_wake was called since the last pump
};

extern Backend g;

// Events -------------------------------------------------------------------------------------------------------------

// The low 32 bits of the monotonic millisecond clock: the time of events the backend raises itself.
u32 now_ms32() noexcept;
WindowEvent make_event(EventType type, u32 time_ms) noexcept;
// Appends to the ring; false (and counted) when it is full.
bool ring_push(const WindowEvent& event) noexcept;

// window.cpp ---------------------------------------------------------------------------------------------------------

// Sends one event to the sink now, for the synchronous kinds (cursor_query, capture_lost) and for the pump.
void sink_call(const WindowEvent& event, WindowEventResult* result) noexcept;
// The window's size in client pixels at its scale.
void window_pixel_size(u32* width, u32* height) noexcept;
u32 dpi_of_scale(u32 scale_120) noexcept;
// Client pixels per logical unit.
float pixel_scale() noexcept;
void present_clear() noexcept;
// Dispatches until `done()` is true, the connection fails or `timeout_ms` passes.
bool dispatch_until(bool (*done)() noexcept, u32 timeout_ms) noexcept;
void connection_check() noexcept;

// pump.cpp -----------------------------------------------------------------------------------------------------------

// Reads and dispatches what is readable without waiting (window_pump and window_create).
void display_poll_nonblocking() noexcept;

// pointer.cpp --------------------------------------------------------------------------------------------------------

void pointer_attach(wl_seat* seat) noexcept;
void pointer_detach() noexcept;
// Capture and button bookkeeping in step with what the app has seen: before the sink gets a pointer event (the
// buttons the app sees held) and after it (a capture ends with the last button or when the pointer leaves).
void pointer_before_dispatch(const WindowEvent& event) noexcept;
void pointer_after_dispatch(const WindowEvent& event) noexcept;
// The cursor_query that follows a pointer_move the sink has seen, and the cursor the app chose.
void pointer_cursor_query(const WindowEvent& move) noexcept;
// Applies the cursor the app chose after a cursor_query (or an explicit window_set_cursor).
void cursor_apply() noexcept;
void pointer_scale_changed() noexcept;

// keyboard.cpp -------------------------------------------------------------------------------------------------------

void keyboard_attach(wl_seat* seat) noexcept;
void keyboard_detach() noexcept;
void keyboard_shutdown() noexcept;
// Raises due key repeats into the ring; at most one per call.
void keyboard_poll_repeat(u64 now_ms) noexcept;
u32 keyboard_repeat_wait_ms(u64 now_ms) noexcept;
Waitable keyboard_repeat_waitable() noexcept;

// global_keys.cpp ----------------------------------------------------------------------------------------------------

void global_keys_set(Key key, bool down) noexcept;
void global_keys_clear() noexcept;

} // namespace maicord::wayland
