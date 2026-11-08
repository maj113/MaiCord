#pragma once

// The state and the cross-file entry points of the Wayland window backend (src/platform/window/wayland, README.md). One
// connection and one top-level window. Everything runs on the UI thread except window_wake and the global key bits,
// which are atomic. Listeners only record state and enqueue WindowEvents in the ring; window_pump drains it and calls
// the sink, so no wl_*_listener ever runs app code.

#include "maicord/os/wait.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/platform/window/wayland/click.hpp"
#include "maicord/platform/window/wayland/post_queue.hpp"
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
    bool cursor_surface_valid;   // it holds the theme's image of cursor_surface_code, with this hotspot
    u8 cursor_surface_code;
    i32 cursor_hotspot_x, cursor_hotspot_y;
    CursorTheme* theme;
    u32 theme_scale;
    u32 theme_size;       // the nominal size the theme was loaded for
    char theme_name[64];  // empty: the library's default theme
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
    u32 modifier_masks[6];     // the keymap's xkb mask of each modifier of keyboard.cpp's kModifierNames, 0 when absent
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
    u64 repeat_armed_at;  // the monotonic millisecond the timer is armed for, 0 when unknown (pump.cpp)
    // Keys currently down: per evdev code the portable key it was pressed as (0 = up).
    u8 down_key[768];
    u8 down_specific[768];
    u8 held_count[256];  // by Key, generic and side-specific
};

// The frame the app draws (client-side decorations): what the last hit_test said and which presses were taken.
struct FrameState {
    FrameHit hover;
    u8 consumed_buttons;  // 1 << PointerButton: presses the window took, whose releases it takes too
};

// The Linux mime types the clipboard and drag-and-drop code recognises, as bits of an offer.
enum MimeBit : u32 {
    mime_text_utf8 = 1,   // text/plain;charset=utf-8
    mime_text_plain = 2,  // text/plain
    mime_utf8_string = 4, // UTF8_STRING
    mime_png = 8,         // image/png
    mime_html = 16,       // text/html
    mime_uri_list = 32,   // text/uri-list
    mime_gnome_files = 64, // x-special/gnome-copied-files
    mime_any_text = mime_text_utf8 | mime_text_plain | mime_utf8_string,
};

inline constexpr u32 kMaxOffers = 8;

struct OfferSlot {
    void* offer;  // wl_data_offer* or zwp_primary_selection_offer_v1*
    u32 mimes;    // MimeBit
    bool primary;
};

// What a selection we own offers; freed when the source is cancelled or replaced.
struct Payload {
    u8* text;
    u32 text_bytes;
    u8* png;
    u32 png_bytes;
    u8* html;
    u32 html_bytes;
};

struct DataState {
    wl_data_device* device;
    zwp_primary_selection_device_v1* primary_device;
    OfferSlot offers[kMaxOffers];
    OfferSlot* selection;          // the clipboard, as announced
    OfferSlot* primary_selection;  // the primary selection
    OfferSlot* drag;               // the offer being dragged over the window
    bool drag_accepts;
    bool drop_target;              // window_set_drop_target
    wl_data_source* source;                       // the clipboard we own
    Payload* payload;
    zwp_primary_selection_source_v1* primary_source;
    Payload* primary_payload;
};

inline constexpr u32 kImeUnits = 8192;
inline constexpr u32 kImeBytes = 4096;

// zwp_text_input_v3 (IME): what the app asked for and the transaction being collected until `done`.
struct TextState {
    zwp_text_input_v3* input;
    bool entered;  // the compositor gave the text input the window's focus
    bool wanted;   // the app has text input enabled (applied on enter if not entered yet)
    bool had_preedit;  // the last done left a preedit on screen: the next one must clear it
    bool enabled;  // enable was sent for this focus
    // The surrounding text last sent, to turn UTF-8 byte counts of delete_surrounding_text into UTF-16 units.
    char surrounding[4004];
    u32 surrounding_bytes;
    u32 cursor_byte;
    // The pending transaction.
    char preedit[kImeBytes];
    u32 preedit_bytes;
    bool preedit_present;
    i32 preedit_begin, preedit_end;  // bytes, -1 when there is no cursor
    char commit[kImeBytes];
    u32 commit_bytes;
    bool commit_present;
    u32 delete_before, delete_after;  // bytes
    // Text of ime events lives here until the pump hands it to the sink (ring of units).
    u16 units[kImeUnits];
    u32 unit_cursor;
};

struct ExtrasState {
    xdg_activation_token_v1* token;
    u32 tokens_done;
    zwp_idle_inhibitor_v1* inhibitor;
    // wl_output scales, for compositors without fractional scale.
    wl_output* outputs[8];
    u32 output_scale[8];
    bool output_entered[8];
    u32 preferred_integer;  // wl_surface.preferred_buffer_scale, 0 until sent
    // xdg-foreign: the window's export; its handle goes to the portals (dbus::portal_set_parent_window).
    zxdg_exported_v2* exported;
    // ext-idle-notify: the system's input idle time. `idle_now` is the compositor's last word (idled or resumed) and
    // `idle_mark_ms` when it said it (monotonic); the watch is for `idle_timeout_ms` without input.
    ext_idle_notification_v1* idle;
    u32 idle_timeout_ms;
    bool idle_now;
    u64 idle_mark_ms;
};

struct WindowState {
    WindowSink sink;
    void* sink_context;
    char app_id[128];  // the desktop entry's name: what activation tokens and the compositor identify the window by

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
    u32 logical_width, logical_height;
    u32 floating_width, floating_height;    // the last size outside maximized, fullscreen and tiled states
    u32 pending_width, pending_height;
    u32 pending_states;
    u32 states;
    u32 scale_120;  // the compositor's preferred scale, 120 = 1.0
    u32 dpi;
    u32 state_bits;  // StateFlag
    bool resizing;
    u32 min_logical_width, min_logical_height;  // what xdg_toplevel.set_min_size last said
    // The opaque region last sent (from the origin, in surface units): the compositor keeps it until another one comes.
    i32 opaque_width, opaque_height;
    bool opaque_sent;
    // The viewport destination (surface units) and the integer buffer scale last sent, 0 until sent: the surface keeps
    // both until another value comes.
    i32 viewport_width, viewport_height;
    u32 buffer_scale;
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
    u32 seat_version;

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
    u32 input_serial;  // the serial of the latest key or button press: what selections and activation ask for

    wl_data_device_manager* data_manager;
    zwp_primary_selection_device_manager_v1* primary_manager;
    zwp_text_input_manager_v3* text_input_manager;
    xdg_activation_v1* activation_manager;
    zwp_idle_inhibit_manager_v1* idle_manager;
    zxdg_exporter_v2* exporter_manager;
    ext_idle_notifier_v1* idle_notifier;
    u32 idle_notifier_version;
    xdg_system_bell_v1* bell;
    xdg_toplevel_icon_manager_v1* icon_manager;
    FrameState frame;
    DataState data;
    TextState text;
    ExtrasState extras;
    PostQueue post;
    u32 post_ready;  // atomic: the queue is initialised (window_create ran)
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
// Asks the sink for the window's smallest client size (the synchronous size_limits query: the Win32 side answers
// WM_GETMINMAXINFO) and tells the compositor (xdg_toplevel.set_min_size); the pump calls it after a state_changed, the
// event that follows every resize and scale change and whose handling settles the layout the answer depends on.
void apply_size_limits() noexcept;
// The window's smallest client size in pixels at its scale, as xdg_toplevel.set_min_size in logical units (sent only
// when it changed; zero is no limit).
void set_min_size_px(u32 width_px, u32 height_px) noexcept;
// The window's size in client pixels at its scale.
void window_pixel_size(u32* width, u32* height) noexcept;
// Client pixels per logical unit.
float pixel_scale() noexcept;
// After a configure, a resize or a scale change: the surface geometry (viewport destination, opaque region) follows.
void surface_sync() noexcept;
// The viewport destination or integer buffer scale of the surface, each sent when it changed (applied by the next commit).
void apply_surface_geometry() noexcept;
// A new scale from an integer source (wl_output, preferred_buffer_scale) or fractional-scale: scale_120.
void window_scale_changed(u32 scale_120) noexcept;
// Dispatches until `done()` is true, the connection fails or `timeout_ms` passes.
bool dispatch_until(bool (*done)() noexcept, u32 timeout_ms) noexcept;
void connection_check() noexcept;
// The compositor's interactive move and resize, for the frame the app draws (frame.cpp).
void begin_move() noexcept;
void begin_resize(FrameHit edge) noexcept;

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

// frame.cpp ----------------------------------------------------------------------------------------------------------

// Cursor codes at or above this are frame hits (kFrameCursorBase + FrameHit); below are UiCursor values.
inline constexpr u8 kFrameCursorBase = 32;
// Client-side decorations: asks the app where a pointer event lies (hit_test) and takes the presses that start a move,
// a resize, a maximize or the window menu. True when the window took the event and the app must not see it. Also
// records the hit under the pointer for the cursor.
bool frame_filter(const WindowEvent& event) noexcept;

// data.cpp -----------------------------------------------------------------------------------------------------------

// Creates the data and primary-selection devices once the seat and the managers are bound.
void data_attach() noexcept;
void data_shutdown() noexcept;

// text_input.cpp -----------------------------------------------------------------------------------------------------

void text_attach() noexcept;
void text_shutdown() noexcept;

// extras.cpp ---------------------------------------------------------------------------------------------------------

// The wl_surface listener (enter/leave an output, preferred buffer scale), idle inhibitor and activation state.
void extras_attach_surface() noexcept;
void extras_output_added(wl_output* output) noexcept;
void extras_shutdown() noexcept;
// The system's input idle time in milliseconds for PlatformSettings, kSystemIdleUnknown without ext-idle-notify or a watch.
u32 extras_system_idle_ms() noexcept;

// pump.cpp: the queue behind window_post, drained into wake events.
void post_drain() noexcept;

} // namespace maicord::wayland
