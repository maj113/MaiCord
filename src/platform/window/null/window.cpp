// The headless window backend: the window service over plain memory (include/maicord/platform/window/null.hpp).
#include "maicord/platform/window/null.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/platform/window/global_keys.hpp"
#include "maicord/platform/window/wayland.hpp"
#include "maicord/ui/core/cursor.hpp"

namespace maicord {
namespace {

struct WindowObjectStorage {
    u8 unused;
};

constexpr WindowCaps kCaps{
    true,   // csd
    true,   // native_caption_buttons
    false,  // fractional_scale
    true,   // window_menu
    true,   // maximize
    true,   // minimize
    true,   // fullscreen
};

struct Null {
    WindowSink sink;
    void* context;
    NullWindowProbe probe;
    u64 keys_down[4];  // bit k of word k / 64: Key k is down
    u64 toggles[4];
    WindowMetrics metrics;
    bool focus_denied;
} g{};
WindowObjectStorage g_object;

bool key_state(const u64 (&bits)[4], Key key) noexcept {
    return (bits[key / 64] >> (key % 64)) & 1;
}

void set_bit(u64 (&bits)[4], Key key, bool on) noexcept {
    const u64 bit = 1ull << (key % 64);
    if (on)
        bits[key / 64] |= bit;
    else
        bits[key / 64] &= ~bit;
}

// The sink's synchronous report of a capture change, from inside the call that caused it.
void report_capture_lost(NativeWindow now_owner) noexcept {
    if (!g.sink)
        return;
    WindowEvent event{};
    event.type = EventType::capture_lost;
    event.focus.other = now_owner;
    WindowEventResult result{};
    g.sink(g.context, event, &result);
}

} // namespace

const NullWindowProbe& null_window_probe() noexcept {
    return g.probe;
}

void null_window_reset() noexcept {
    g = {};
}

void null_window_set_text_input_available(bool available) noexcept {
    g.probe.text_input_available = available;
}

void null_window_set_key(Key key, bool down) noexcept {
    set_bit(g.keys_down, key, down);
}

void null_window_set_toggle(Key key, bool on) noexcept {
    set_bit(g.toggles, key, on);
}

void null_window_release_keys() noexcept {
    for (u64& word : g.keys_down)
        word = 0;
}

void null_window_set_metrics(const WindowMetrics& metrics) noexcept {
    g.metrics = metrics;
}

void null_window_set_focus_allowed(bool allowed) noexcept {
    g.focus_denied = !allowed;
}

void null_window_set_focus(bool focused) noexcept {
    g.probe.focused = focused;
}

const WindowCaps& window_caps() noexcept {
    return kCaps;
}

bool window_create(const WindowCreateInfo&, WindowSink sink, void* context, Window* out) noexcept {
    if (!out || g.probe.created)
        return false;
    g.sink = sink;
    g.context = context;
    g.probe.created = true;
    g.metrics = {1280, 800, 120, 96};
    *out = reinterpret_cast<Window>(&g_object);
    return true;
}

void window_destroy(Window) noexcept {
    g.probe.created = false;
    g.probe.captured = false;
    g.probe.focused = false;
}

NativeWindow window_surface_native(Window window) noexcept {
    return window;
}

// The text input protocol, as a request log: the tests of the text store read what it asked for.
bool window_wayland_text_input_available(NativeWindow window) noexcept {
    return window && g.probe.text_input_available;
}

bool window_wayland_text_input_enable(NativeWindow window, const TextInputRequest& request) noexcept {
    if (!window_wayland_text_input_available(window))
        return false;
    NullWindowProbe& probe = g.probe;
    probe.text_input_enabled = true;
    ++probe.text_input_enables;
    probe.text_surrounding_bytes = request.surrounding.bytes;
    const u32 kept = request.surrounding.bytes < sizeof(probe.text_surrounding) ? request.surrounding.bytes
                                                                               : sizeof(probe.text_surrounding);
    copy_bytes(probe.text_surrounding, request.surrounding.data, kept);
    probe.text_cursor_byte = request.cursor_byte;
    probe.text_anchor_byte = request.anchor_byte;
    probe.text_purpose = static_cast<u8>(request.purpose);
    probe.text_sensitive = request.sensitive;
    probe.text_x = request.x;
    probe.text_y = request.y;
    probe.text_height = request.height;
    return true;
}

void window_wayland_text_input_disable(NativeWindow window) noexcept {
    if (window_wayland_text_input_available(window))
        g.probe.text_input_enabled = false;
}

bool window_wayland_global_shortcut(Key key, u8, const char*) noexcept {
    g.probe.shortcut_key = static_cast<u8>(key);
    ++g.probe.shortcut_binds;
    return true;
}

bool window_wayland_global_shortcut_ready() noexcept {
    return g.probe.shortcut_key != 0;
}

void window_wayland_global_shortcut_clear() noexcept {
    g.probe.shortcut_key = 0;
    ++g.probe.shortcut_clears;
}

// The compositor's integration protocols: a headless window has none.
bool window_wayland_set_idle_inhibit(Window, bool) noexcept {
    return false;
}

bool window_wayland_system_bell() noexcept {
    return false;
}

bool window_wayland_activate(const char*) noexcept {
    return false;
}

void window_show(Window, ShowMode mode) noexcept {
    g.probe.shown = mode != ShowMode::hide && mode != ShowMode::minimize_no_activate;
}

void window_set_title(Window, Utf8View title) noexcept {
    const u32 bytes = title.bytes < sizeof(g.probe.title) - 1 ? title.bytes : static_cast<u32>(sizeof(g.probe.title) - 1);
    copy_bytes(g.probe.title, title.data, bytes);
    g.probe.title[bytes] = 0;
    g.probe.title_bytes = bytes;
}

void window_set_icon(Window, const IconImage*, u32) noexcept {}

void window_minimize(Window) noexcept {}

void window_toggle_maximize(Window) noexcept {}

void window_request_close(Window) noexcept {
    ++g.probe.close_requests;
}

bool window_set_fullscreen(Window, bool fullscreen) noexcept {
    if (g.probe.fullscreen == fullscreen)
        return false;
    g.probe.fullscreen = fullscreen;
    return true;
}

void window_show_system_menu(Window, float, float) noexcept {}

void window_set_min_size(Window, u32 width_px, u32 height_px) noexcept {
    g.probe.min_width = width_px;
    g.probe.min_height = height_px;
    ++g.probe.min_size_calls;
}

void window_request_attention(Window, Attention) noexcept {}

void window_set_drop_target(Window, bool accept_files) noexcept {
    g.probe.drop_target = accept_files;
}

bool window_set_capture_excluded(Window, bool excluded) noexcept {
    g.probe.capture_excluded = excluded;
    return true;
}

void window_invalidate(Window) noexcept {
    ++g.probe.invalidations;
}

void window_set_cursor(Window, UiCursor cursor) noexcept {
    g.probe.cursor = static_cast<u8>(cursor);
    ++g.probe.cursor_changes;
}

void window_request_cursor_refresh(Window) noexcept {
    ++g.probe.cursor_refreshes;
}

bool window_pointer_position(Window, float*, float*) noexcept {
    return false;
}

// As the system does: capturing again for the window that holds it reports the capture lost to that window, naming
// itself as the new owner.
void window_capture_pointer(Window window) noexcept {
    ++g.probe.captures;
    if (g.probe.captured)
        report_capture_lost(window);
    g.probe.captured = true;
}

void window_release_capture(Window) noexcept {
    if (!g.probe.captured)
        return;
    g.probe.captured = false;
    ++g.probe.releases;
    report_capture_lost(nullptr);
}

bool window_pointer_captured(Window) noexcept {
    return g.probe.captured;
}

bool window_focus(Window) noexcept {
    ++g.probe.focus_requests;
    if (!g.focus_denied)
        g.probe.focused = true;
    return g.probe.focused;
}

u8 window_modifiers() noexcept {
    u32 modifiers = 0;
    if (key_state(g.keys_down, key::ctrl))
        modifiers |= mod_ctrl;
    if (key_state(g.keys_down, key::alt))
        modifiers |= mod_alt;
    if (key_state(g.keys_down, key::shift))
        modifiers |= mod_shift;
    if (key_state(g.keys_down, key::left_super) || key_state(g.keys_down, key::right_super))
        modifiers |= mod_super;
    if (key_state(g.keys_down, key::right_alt) && key_state(g.keys_down, key::left_ctrl))
        modifiers |= mod_altgr;
    if (key_state(g.toggles, key::caps_lock))
        modifiers |= mod_caps;
    if (key_state(g.toggles, key::num_lock))
        modifiers |= mod_num;
    return static_cast<u8>(modifiers);
}

bool window_key_down(Key key_code) noexcept {
    return key_state(g.keys_down, key_code);
}

bool window_toggle_state(Key key_code) noexcept {
    return key_state(g.toggles, key_code);
}

bool window_layout_types_text(Key, u8) noexcept {
    return false;
}

bool global_key_down(Key key_code) noexcept {
    return key_state(g.keys_down, key_code);
}

WindowMetrics window_metrics(Window) noexcept {
    return g.metrics;
}

u32 window_display_count() noexcept {
    return 1;
}

u32 window_state(Window) noexcept {
    return (g.probe.shown ? static_cast<u32>(state_visible) : 0u) | (g.probe.fullscreen ? static_cast<u32>(state_fullscreen) : 0u);
}

// The Windows frame's border at 96 DPI, scaled.
u32 window_resize_border(Window, u32 dpi) noexcept {
    return (8 * dpi + 48) / 96;
}

bool window_frame_inset(Window, u32* pixels) noexcept {
    *pixels = 0;
    return true;
}

void window_frame_callback_request(Window) noexcept {}

void window_set_frame_listener(Window, FrameListener, void*) noexcept {}

void window_wake(Window) noexcept {
    ++g.probe.wakes;
}

bool window_inject(Window, const WindowEvent& event, WindowEventResult* result) noexcept {
    if (result)
        *result = {};
    if (!g.sink)
        return false;
    WindowEvent synthetic = event;
    synthetic.flags |= ev_synthetic;
    WindowEventResult local{};
    g.sink(g.context, synthetic, result ? result : &local);
    return true;
}

// Runs inline: the headless backend has no second thread to cross.
bool window_post(const Waker& waker, u64 a, u64 b) noexcept {
    ++g.probe.posts;
    if (!g.sink)
        return false;
    WindowEvent event{};
    event.type = EventType::wake;
    event.wake.cause = waker.cause;
    event.wake.a = a;
    event.wake.b = b;
    WindowEventResult result{};
    g.sink(g.context, event, &result);
    return true;
}

#if defined(_WIN32)
ConstSlice<u8> window_app_icon_png(u32, u32, u32*) noexcept {
    return {};
}
#endif

// Fatal messages have nowhere to go without a desktop.
void window_fatal_message(Utf8View, Utf8View) noexcept {}

// What a desktop with the common defaults reports: Windows' double-click and caret values, three lines a notch.
PlatformSettings platform_settings_read() noexcept {
    PlatformSettings settings{};
    settings.double_click_ms = 500;
    settings.double_click_dx_px = 4;
    settings.double_click_dy_px = 4;
    settings.caret_blink_ms = 530;
    settings.wheel_lines = 3;
    settings.client_area_animation = true;
    settings.system_idle_ms = kSystemIdleUnknown;
    return settings;
}

} // namespace maicord
