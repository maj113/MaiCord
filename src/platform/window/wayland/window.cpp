// The Wayland window: the connection, the registry, xdg-shell, the frame callback and the window service of
// maicord/platform/window/window.hpp. Input is in pointer.cpp and keyboard.cpp, the loop in pump.cpp, the integration
// protocols in extras.cpp; the shared state is in backend.hpp. The window has no buffers: the GPU backend's EGL surface commits.
#include "maicord/core/log_event.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/test_hook.hpp"
#include "maicord/platform/window/wayland.hpp"
#include "maicord/platform/window/wayland/abi.hpp"
#include "maicord/platform/window/wayland/settings_map.hpp"
#include "maicord/platform/dbus/desktop_settings.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "backend.hpp"

namespace maicord::wayland {

Backend g;

namespace {

// Versions bound at most: the newest each feature of the backend uses.
constexpr u32 kCompositorVersion = 6;
constexpr u32 kSeatVersion = 8;
constexpr u32 kWmBaseVersion = 6;
constexpr u32 kCreateTimeoutMs = 10000;
constexpr u32 kDefaultDpi = 96;
constexpr u32 kTiledStates = (1u << xdg_toplevel_state_tiled_left) | (1u << xdg_toplevel_state_tiled_right) |
                             (1u << xdg_toplevel_state_tiled_top) | (1u << xdg_toplevel_state_tiled_bottom);

void on_seat_capabilities(void*, wl_seat* seat, u32 capabilities) noexcept {
    if (capabilities & wl_seat_capability_pointer)
        pointer_attach(seat);
    else
        pointer_detach();
    if (capabilities & wl_seat_capability_keyboard)
        keyboard_attach(seat);
    else
        keyboard_detach();
}

void on_seat_name(void*, wl_seat*, const char*) noexcept {}

const wl_seat_listener kSeatListener = {on_seat_capabilities, on_seat_name};

void on_wm_base_ping(void*, xdg_wm_base* base, u32 serial) noexcept {
    xdg_wm_base_pong(base, serial);
}

const xdg_wm_base_listener kWmBaseListener = {on_wm_base_ping};

// The globals bound as the registry announces them, once each, in the order disconnect destroys them: the newest version
// bound, the oldest taken, the opcode of the request that destroys the proxy (kPlainDestroy: it has none, the proxy is just
// freed) and the member of Backend the proxy goes in. The fractional-scale pair is left out by
// MAICORD_WAYLAND_NO_FRACTIONAL=1 (the integer scale path, wl_surface buffer scale, for testing it where the compositor
// offers both).
constexpr u8 kPlainDestroy = 0xFF;

struct Binding {
    const Interface* interface;
    u16 slot;
    u8 version;
    u8 oldest;
    u8 destroy;
    bool fractional;
};

#define MAICORD_BINDING(protocol, member, version, oldest, destroy, fractional) \
    Binding{&protocol##_interface, static_cast<u16>(__builtin_offsetof(Backend, member)), version, oldest, destroy, fractional}

constexpr Binding kBindings[] = {
    MAICORD_BINDING(wl_data_device_manager, data_manager, 3, 1, kPlainDestroy, false),
    MAICORD_BINDING(zwp_text_input_manager_v3, text_input_manager, 1, 1, 0, false),
    MAICORD_BINDING(xdg_activation_v1, activation_manager, 1, 1, 0, false),
    MAICORD_BINDING(zwp_idle_inhibit_manager_v1, idle_manager, 1, 1, 0, false),
    MAICORD_BINDING(zxdg_exporter_v2, exporter_manager, 1, 1, 0, false),
    MAICORD_BINDING(ext_idle_notifier_v1, idle_notifier, 2, 1, 0, false),
    MAICORD_BINDING(xdg_system_bell_v1, bell, 1, 1, 0, false),
    MAICORD_BINDING(xdg_toplevel_icon_manager_v1, icon_manager, 1, 1, 0, false),
    MAICORD_BINDING(wl_seat, seat, kSeatVersion, 1, 3, false),  // wl_seat.release, from version 5
    MAICORD_BINDING(zxdg_decoration_manager_v1, decoration_manager, 1, 1, 0, false),
    MAICORD_BINDING(wp_cursor_shape_manager_v1, cursor_shape_manager, 1, 1, 0, false),
    MAICORD_BINDING(wp_fractional_scale_manager_v1, fractional_manager, 1, 1, 0, true),
    MAICORD_BINDING(wp_viewporter, viewporter, 1, 1, 0, true),
    MAICORD_BINDING(xdg_wm_base, wm_base, kWmBaseVersion, 1, 0, false),
    MAICORD_BINDING(wl_shm, shm, 1, 1, kPlainDestroy, false),  // wl_shm.release exists from version 2; this binds 1
    MAICORD_BINDING(wl_compositor, compositor, kCompositorVersion, 4, kPlainDestroy, false),
};
#undef MAICORD_BINDING

void on_global(void*, wl_registry* registry, u32 name, const char* interface, u32 version) noexcept {
    if (g.global_count < kMaxGlobals) {
        GlobalName& entry = g.globals[g.global_count++];
        u32 length = 0;
        for (; interface[length] && length + 1 < sizeof(entry.name); ++length)
            entry.name[length] = interface[length];
        entry.name[length] = 0;
        entry.version = version;
    }
    for (const Binding& binding : kBindings) {
        if (!c_string_equal(interface, binding.interface->name))
            continue;
        // The slot is a member of another pointer type: read and written as bytes.
        u8* const slot = reinterpret_cast<u8*>(&g) + binding.slot;
        void* proxy;
        copy_bytes(&proxy, slot, sizeof(proxy));
        if (proxy || version < binding.oldest || (binding.fractional && test_hook("MAICORD_WAYLAND_NO_FRACTIONAL")))
            return;
        const u32 bound = minimum(version, static_cast<u32>(binding.version));
        proxy = wl_registry_bind(registry, name, binding.interface, bound);
        copy_bytes(slot, &proxy, sizeof(proxy));
        if (binding.interface == &xdg_wm_base_interface) {
            xdg_wm_base_add_listener(g.wm_base, &kWmBaseListener, nullptr);
        } else if (binding.interface == &wl_seat_interface) {
            g.seat_version = bound;
            wl_seat_add_listener(g.seat, &kSeatListener, nullptr);
        } else if (binding.interface == &ext_idle_notifier_v1_interface) {
            g.idle_notifier_version = bound;
        }
        return;
    }
    if (c_string_equal(interface, "wl_output")) {
        // Version 2 reports the integer scale; the output is only tracked when the window needs it.
        if (version >= 2)
            extras_output_added(static_cast<wl_output*>(
                wl_registry_bind(registry, name, &wl_output_interface, minimum(version, 3u))));
    }
}

void on_global_remove(void*, wl_registry*, u32) noexcept {}

const wl_registry_listener kRegistryListener = {on_global, on_global_remove};

u32 scale_to_dpi(u32 scale_120) noexcept {
    return (kDefaultDpi * scale_120 + 60) / 120;
}

u32 to_pixels(u32 logical) noexcept {
    return (logical * g.window.dpi + kDefaultDpi / 2) / kDefaultDpi;
}

u32 state_bits_of(u32 states) noexcept {
    u32 bits = 0;
    if (states & (1u << xdg_toplevel_state_maximized))
        bits |= state_maximized;
    if (states & (1u << xdg_toplevel_state_fullscreen))
        bits |= state_fullscreen;
    if (states & (1u << xdg_toplevel_state_suspended))
        bits |= state_suspended;
    if (states & kTiledStates)
        bits |= state_tiled;
    if (!g.caps.csd)
        bits |= state_decorated;
    if (g.window.visible && !(states & (1u << xdg_toplevel_state_suspended)))
        bits |= state_visible;
    return bits;
}

// `geometry` is state_geometry when the client's pixel size changed with this configure or scale: the app sizes its
// surface again on it (as for WM_WINDOWPOSCHANGED), so the pixel size must be settled before the event is pushed.
void emit_state_changed(u32 geometry = 0) noexcept {
    const u32 bits = state_bits_of(g.window.states);
    const u32 changed = (bits ^ g.window.state_bits) | geometry;
    g.window.state_bits = bits;
    if (!changed)
        return;
    WindowEvent event = make_event(EventType::state_changed, now_ms32());
    event.window_state.changed = changed;
    event.window_state.state = bits | geometry;
    ring_push(event);
}

void emit_resize(EventType type) noexcept {
    u32 width = 0;
    u32 height = 0;
    window_pixel_size(&width, &height);
    WindowEvent event = make_event(type, now_ms32());
    event.resize.width_px = width;
    event.resize.height_px = height;
    event.resize.scale_120 = g.window.dpi * 120 / kDefaultDpi;
    ring_push(event);
}

void on_toplevel_configure(void*, xdg_toplevel*, i32 width, i32 height, Array* states) noexcept {
    WindowState& window = g.window;
    window.pending_width = width > 0 ? static_cast<u32>(width) : 0;
    window.pending_height = height > 0 ? static_cast<u32>(height) : 0;
    u32 bits = 0;
    if (states && states->data) {
        const u32* state = static_cast<const u32*>(states->data);
        const usize count = states->size / sizeof(u32);
        for (usize i = 0; i < count; ++i)
            if (state[i] < 32)
                bits |= 1u << state[i];
    }
    window.pending_states = bits;
}

void on_toplevel_close(void*, xdg_toplevel*) noexcept {
    ring_push(make_event(EventType::close_requested, now_ms32()));
}

void on_toplevel_configure_bounds(void*, xdg_toplevel*, i32, i32) noexcept {}

void on_toplevel_wm_capabilities(void*, xdg_toplevel*, Array* capabilities) noexcept {
    bool menu = false;
    bool maximize = false;
    bool fullscreen = false;
    bool minimize = false;
    if (capabilities && capabilities->data) {
        const u32* value = static_cast<const u32*>(capabilities->data);
        for (usize i = 0; i < capabilities->size / sizeof(u32); ++i) {
            menu |= value[i] == xdg_toplevel_wm_capabilities_window_menu;
            maximize |= value[i] == xdg_toplevel_wm_capabilities_maximize;
            fullscreen |= value[i] == xdg_toplevel_wm_capabilities_fullscreen;
            minimize |= value[i] == xdg_toplevel_wm_capabilities_minimize;
        }
    }
    g.caps.window_menu = menu;
    g.caps.maximize = maximize;
    g.caps.fullscreen = fullscreen;
    g.caps.minimize = minimize;
}

const xdg_toplevel_listener kToplevelListener = {on_toplevel_configure, on_toplevel_close,
                                                 on_toplevel_configure_bounds, on_toplevel_wm_capabilities};

// The configure sequence: the toplevel events describe the new state, the xdg_surface event completes it. Apply
// everything, acknowledge, then report what changed.
void on_surface_configure(void*, xdg_surface* surface, u32 serial) noexcept {
    WindowState& window = g.window;
    constexpr u32 sized_states =
        (1u << xdg_toplevel_state_maximized) | (1u << xdg_toplevel_state_fullscreen) | kTiledStates;
    const u32 states = window.pending_states;
    const bool floating = (states & sized_states) == 0;
    u32 width = window.pending_width;
    u32 height = window.pending_height;
    if (width && height) {
        if (floating) {
            window.floating_width = width;
            window.floating_height = height;
        }
    } else if (floating) {
        width = window.floating_width;
        height = window.floating_height;
    } else {
        width = window.logical_width ? window.logical_width : window.floating_width;
        height = window.logical_height ? window.logical_height : window.floating_height;
    }
    xdg_surface_ack_configure(surface, serial);

    const bool first = !window.configured;
    const bool size_changed = width != window.logical_width || height != window.logical_height;
    const u32 previous_states = window.states;
    window.configured = true;
    window.logical_width = width;
    window.logical_height = height;
    window.states = states;

    const bool was_resizing = window.resizing;
    const bool now_resizing = (states & (1u << xdg_toplevel_state_resizing)) != 0;
    window.resizing = now_resizing;
    if (now_resizing && !was_resizing)
        emit_resize(EventType::resize_begin);
    if (first || size_changed)
        emit_resize(EventType::resize);
    const bool was_active = (previous_states & (1u << xdg_toplevel_state_activated)) != 0;
    const bool now_active = (states & (1u << xdg_toplevel_state_activated)) != 0;
    if (now_active != was_active || (first && now_active)) {
        WindowEvent event = make_event(EventType::activation_changed, now_ms32());
        event.focus.gained = now_active;
        ring_push(event);
    }
    emit_state_changed((first || size_changed) ? state_geometry : 0);
    if (was_resizing && !now_resizing)
        emit_resize(EventType::resize_end);
    surface_sync();
}

const xdg_surface_listener kSurfaceListener = {on_surface_configure};

void on_decoration_configure(void*, zxdg_toplevel_decoration_v1*, u32 mode) noexcept {
    g.caps.csd = mode != zxdg_toplevel_decoration_v1_mode_server_side;
    if (g.window.configured)
        emit_state_changed();
}

const zxdg_toplevel_decoration_v1_listener kDecorationListener = {on_decoration_configure};

void on_preferred_scale(void*, wp_fractional_scale_v1*, u32 scale) noexcept {
    window_scale_changed(scale);
}

const wp_fractional_scale_v1_listener kFractionalListener = {on_preferred_scale};

void on_frame_done(void*, wl_callback* callback, u32 time) noexcept {
    wl_callback_destroy(callback);
    g.window.frame_callback = nullptr;
    ring_push(make_event(EventType::frame_done, time));
    if (g.window.frame_listener)
        g.window.frame_listener(g.window.frame_listener_context, time);
}

const wl_callback_listener kFrameListener = {on_frame_done};

void destroy_window_objects() noexcept {
    extras_shutdown();  // the idle inhibitor, the export, the idle watch and an activation token belong to the surface
    WindowState& window = g.window;
    if (window.frame_callback)
        wl_callback_destroy(window.frame_callback);
    if (window.fractional)
        wp_fractional_scale_v1_destroy(window.fractional);
    if (window.viewport)
        wp_viewport_destroy(window.viewport);
    if (window.decoration)
        zxdg_toplevel_decoration_v1_destroy(window.decoration);
    if (window.toplevel)
        xdg_toplevel_destroy(window.toplevel);
    if (window.shell_surface)
        xdg_surface_destroy(window.shell_surface);
    if (window.surface)
        wl_surface_destroy(window.surface);
}

bool window_configured() noexcept {
    return g.window.configured;
}

void disconnect() noexcept {
    data_shutdown();
    text_shutdown();
    keyboard_shutdown();
    pointer_detach();
    for (const Binding& binding : kBindings) {
        Proxy* proxy;
        copy_bytes(&proxy, reinterpret_cast<u8*>(&g) + binding.slot, sizeof(proxy));
        if (!proxy)
            continue;
        if (binding.destroy == kPlainDestroy || (binding.interface == &wl_seat_interface && g.seat_version < wl_seat_release_since))
            g_client.proxy_destroy(proxy);
        else
            g_client.proxy_marshal_array_flags(proxy, binding.destroy, nullptr, 0, kMarshalFlagDestroy, nullptr);
    }
    if (g.registry)
        wl_registry_destroy(g.registry);
    if (g.display) {
        if (g.read_prepared)
            g_client.display_cancel_read(g.display);
        g_client.display_disconnect(g.display);
    }
    waitable_close(g.display_waitable);
    clear_bytes(&g, static_cast<u32>(sizeof(g)));
}

xdg_toplevel_resize_edge resize_edge(FrameHit edge) noexcept {
    switch (edge) {
    case FrameHit::left:
        return xdg_toplevel_resize_edge_left;
    case FrameHit::right:
        return xdg_toplevel_resize_edge_right;
    case FrameHit::top:
        return xdg_toplevel_resize_edge_top;
    case FrameHit::bottom:
        return xdg_toplevel_resize_edge_bottom;
    case FrameHit::top_left:
        return xdg_toplevel_resize_edge_top_left;
    case FrameHit::top_right:
        return xdg_toplevel_resize_edge_top_right;
    case FrameHit::bottom_left:
        return xdg_toplevel_resize_edge_bottom_left;
    case FrameHit::bottom_right:
        return xdg_toplevel_resize_edge_bottom_right;
    default:
        return xdg_toplevel_resize_edge_none;
    }
}

u32 latest_input_serial() noexcept {
    return g.pointer.press_serial ? g.pointer.press_serial : g.keyboard.serial;
}

} // namespace

float pixel_scale() noexcept {
    return static_cast<float>(g.window.dpi) / static_cast<float>(kDefaultDpi);
}

void window_pixel_size(u32* width, u32* height) noexcept {
    *width = to_pixels(g.window.logical_width);
    *height = to_pixels(g.window.logical_height);
}

void sink_call(const WindowEvent& event, WindowEventResult* result) noexcept {
    if (g.window.sink)
        g.window.sink(g.window.sink_context, event, result);
}

void set_min_size_px(u32 width_px, u32 height_px) noexcept {
    WindowState& window = g.window;
    if (!window.toplevel)
        return;
    // Logical units, rounded up: a window the compositor sizes to the minimum must still hold the layout.
    const u32 dpi = window.dpi ? window.dpi : kDefaultDpi;
    const u32 width = (width_px * kDefaultDpi + dpi - 1) / dpi;
    const u32 height = (height_px * kDefaultDpi + dpi - 1) / dpi;
    if (width == window.min_logical_width && height == window.min_logical_height)
        return;
    window.min_logical_width = width;
    window.min_logical_height = height;
    xdg_toplevel_set_min_size(window.toplevel, static_cast<i32>(width), static_cast<i32>(height));
}

void apply_size_limits() noexcept {
    if (!g.window.sink || !g.window.configured)
        return;
    WindowEvent query = make_event(EventType::size_limits, now_ms32());
    WindowEventResult answer{};
    sink_call(query, &answer);
    if (answer.handled)
        set_min_size_px(answer.width_px, answer.height_px);
}

void connection_check() noexcept {
    if (g.failed || !g.display)
        return;
    const int error = g_client.display_get_error(g.display);
    if (error == 0)
        return;
    g.failed = true;
    g.quit = true;
    g.exit_code = 1;
    const Interface* interface = nullptr;
    u32 id = 0;
    const u32 code = g_client.display_get_protocol_error(g.display, &interface, &id);
    LogEvent line("connection_failed");
    if (code != 0 && interface)
        line.field("interface", interface->name).field("protocol_error", code).field("object", id);
    line.write(LogLevel::error, "wayland");
}

bool dispatch_until(bool (*done)() noexcept, u32 timeout_ms) noexcept {
    const u64 start = clock_monotonic_milliseconds();
    while (!done()) {
        if (g.failed)
            return false;
        while (g_client.display_prepare_read(g.display) != 0)
            g_client.display_dispatch_pending(g.display);
        g_client.display_flush(g.display);
        const u64 elapsed = clock_monotonic_milliseconds() - start;
        if (elapsed >= timeout_ms) {
            g_client.display_cancel_read(g.display);
            return false;
        }
        const u64 remaining = timeout_ms - elapsed;
        PollFd poll{g_client.display_get_fd(g.display), kPollIn, 0};
        const TimeSpec timeout{static_cast<i64>(remaining / 1000), static_cast<i64>(remaining % 1000) * 1'000'000};
        const long ready = sys_ppoll(&poll, 1, &timeout);
        if (ready > 0 && (poll.revents & kPollIn)) {
            if (g_client.display_read_events(g.display) < 0) {
                connection_check();
                g.failed = true;
                return false;
            }
        } else {
            g_client.display_cancel_read(g.display);
            if (ready > 0) {
                g.failed = true;
                return false;
            }
        }
        g_client.display_dispatch_pending(g.display);
        connection_check();
    }
    return true;
}

// The opaque region: the whole surface. The compositor then draws the window without blending and stops drawing what
// lies behind it. The window has no rounded or transparent corners (the caption bar is square and drawn inside the
// surface) and nothing in it is transparent (the GPU backend's EGL config has no alpha). The compositor keeps the region
// until another one comes, so it is sent only when the surface's size changes; the next commit applies it.
static void apply_opaque_region() noexcept {
    WindowState& window = g.window;
    if (!window.surface || !g.compositor)
        return;
    const i32 width = static_cast<i32>(window.logical_width);
    const i32 height = static_cast<i32>(window.logical_height);
    if (window.opaque_sent && width == window.opaque_width && height == window.opaque_height)
        return;
    window.opaque_width = width;
    window.opaque_height = height;
    window.opaque_sent = true;
    if (width == 0 || height == 0) {
        wl_surface_set_opaque_region(window.surface, nullptr);
        return;
    }
    wl_region* region = wl_compositor_create_region(g.compositor);
    if (!region)
        return;
    wl_region_add(region, 0, 0, width, height);
    wl_surface_set_opaque_region(window.surface, region);
    wl_region_destroy(region);
}

// What the surface needs besides a buffer: the destination size of the viewport (fractional scale) or the integer buffer
// scale. Both are applied by the next commit, whoever makes it, and kept until another value comes: a configure that
// leaves the size alone (sway repeats each one after the commit) or a scale change (the same logical size) sends nothing.
void apply_surface_geometry() noexcept {
    WindowState& window = g.window;
    if (window.viewport) {
        const i32 width = static_cast<i32>(window.logical_width);
        const i32 height = static_cast<i32>(window.logical_height);
        if (width != window.viewport_width || height != window.viewport_height) {
            window.viewport_width = width;
            window.viewport_height = height;
            wp_viewport_set_destination(window.viewport, width, height);
        }
    } else {  // wl_compositor 4 is bound at least, and wl_surface.set_buffer_scale is version 3
        const u32 scale = maximum<u32>(window.dpi / kDefaultDpi, 1);
        if (scale != window.buffer_scale) {
            window.buffer_scale = scale;
            wl_surface_set_buffer_scale(window.surface, static_cast<i32>(scale));
        }
    }
    apply_opaque_region();
}

void window_scale_changed(u32 scale_120) noexcept {
    if (scale_120 == 0 || scale_120 == g.window.scale_120)
        return;
    g.window.scale_120 = scale_120;
    g.window.dpi = scale_to_dpi(scale_120);
    if (!g.window.configured)
        return;
    emit_resize(EventType::scale_changed);
    // The same logical size is another pixel size: the app sizes its surface again.
    emit_state_changed(state_geometry);
    pointer_scale_changed();
    surface_sync();
}

void surface_sync() noexcept {
    if (g.window.configured && g.window.surface)
        apply_surface_geometry();
}

void begin_move() noexcept {
    const u32 serial = latest_input_serial();
    if (g.seat && serial != 0)
        xdg_toplevel_move(g.window.toplevel, g.seat, serial);
}

void begin_resize(FrameHit edge) noexcept {
    const u32 serial = latest_input_serial();
    if (g.seat && serial != 0)
        xdg_toplevel_resize(g.window.toplevel, g.seat, serial, resize_edge(edge));
}

} // namespace maicord::wayland

namespace maicord {

using namespace wayland;

namespace {

bool is_window(Window window) noexcept {
    return g.window_exists && window == reinterpret_cast<Window>(&g.window);
}

} // namespace

const WindowCaps& window_caps() noexcept {
    return g.caps;
}

// The portal's settings changed (or were first read): the app reads PlatformSettings again for these areas.
static void on_desktop_settings(void*, u32 areas) noexcept {
    if (!g.window_exists)
        return;
    WindowEvent event = make_event(EventType::settings_changed, now_ms32());
    event.settings.areas = areas;
    ring_push(event);
    if (areas & dbus::desktop_cursor) {
        // The cursor theme or size changed: the next cursor is loaded afresh.
        g.pointer.applied_valid = false;
        window_request_cursor_refresh(nullptr);
    }
}

// Connects to the compositor and reads its globals; a second call finds the connection made. Separate from the window
// so the GPU backend can open EGL on the display before the window exists (the device start-up overlaps window creation).
static bool connect_display() noexcept {
    if (g.display)
        return true;
    if (!client_load())
        return false;
    g.display = g_client.display_connect(nullptr);
    if (!g.display) {
        log_write(LogLevel::error, "wayland", "cannot connect to the Wayland display (WAYLAND_DISPLAY, XDG_RUNTIME_DIR)");
        return false;
    }
    g.registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(g.registry, &kRegistryListener, nullptr);
    // Globals, then what the bound globals announce (seat capabilities, the keymap).
    if (g_client.display_roundtrip(g.display) < 0 || g_client.display_roundtrip(g.display) < 0) {
        log_write(LogLevel::error, "wayland", "the compositor did not answer");
        disconnect();
        return false;
    }
    if (!g.compositor || !g.shm || !g.wm_base) {
        log_write(LogLevel::error, "wayland", "the compositor lacks wl_compositor 4, wl_shm or xdg_wm_base");
        disconnect();
        return false;
    }
    return true;
}

void* window_wayland_connect() noexcept {
    return connect_display() ? g.display : nullptr;
}

bool window_create(const WindowCreateInfo& info, WindowSink sink, void* context, Window* out) noexcept {
    if (g.window_exists || !out || info.client_width_dips == 0 || info.client_height_dips == 0)
        return false;
    if (!connect_display())
        return false;
    g.display_waitable = waitable_borrow_fd(g_client.display_get_fd(g.display));
    post_queue_init(&g.post);
    __atomic_store_n(&g.post_ready, 1u, __ATOMIC_RELEASE);
    // The desktop's settings come over the session bus while the compositor handshake runs.
    dbus::desktop_settings_set_handler(on_desktop_settings, nullptr);
    dbus::desktop_settings_start();
    data_attach();
    text_attach();

    WindowState& window = g.window;
    window.sink = sink;
    window.sink_context = context;
    window.visible = info.visible;
    window.floating_width = info.client_width_dips;
    window.floating_height = info.client_height_dips;
    window.scale_120 = 120;
    window.dpi = kDefaultDpi;

    g.caps = WindowCaps{};
    g.caps.csd = true;
    g.caps.window_menu = true;
    g.caps.maximize = true;
    g.caps.minimize = true;
    g.caps.fullscreen = true;

    char title[256];
    const auto copy = [](char* destination, u32 capacity, Utf8View text) {
        const u32 length = minimum<u32>(static_cast<u32>(text.bytes), capacity - 1);
        copy_bytes(destination, text.data, length);
        destination[length] = 0;
    };
    copy(title, sizeof(title), info.title);
    copy(window.app_id, sizeof(window.app_id), info.app_id);

    window.surface = wl_compositor_create_surface(g.compositor);
    window.shell_surface = xdg_wm_base_get_xdg_surface(g.wm_base, window.surface);
    xdg_surface_add_listener(window.shell_surface, &kSurfaceListener, nullptr);
    window.toplevel = xdg_surface_get_toplevel(window.shell_surface);
    xdg_toplevel_add_listener(window.toplevel, &kToplevelListener, nullptr);
    xdg_toplevel_set_title(window.toplevel, title);
    xdg_toplevel_set_app_id(window.toplevel, window.app_id);
    if (g.decoration_manager) {
        window.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(g.decoration_manager, window.toplevel);
        zxdg_toplevel_decoration_v1_add_listener(window.decoration, &kDecorationListener, nullptr);
        zxdg_toplevel_decoration_v1_set_mode(window.decoration, zxdg_toplevel_decoration_v1_mode_client_side);
    }
    if (g.viewporter) {
        window.viewport = wp_viewporter_get_viewport(g.viewporter, window.surface);
        if (g.fractional_manager) {
            window.fractional = wp_fractional_scale_manager_v1_get_fractional_scale(g.fractional_manager, window.surface);
            wp_fractional_scale_v1_add_listener(window.fractional, &kFractionalListener, nullptr);
            g.caps.fractional_scale = true;
        }
    }
    extras_attach_surface();
    g.window_exists = true;
    // The initial commit asks for the first configure. The window is ready once that has arrived and what the bound
    // globals announce (the preferred scale, the selection) has been answered.
    wl_surface_commit(window.surface);
    if (!dispatch_until(window_configured, kCreateTimeoutMs) || g_client.display_roundtrip(g.display) < 0) {
        log_write(LogLevel::error, "wayland", "the compositor never configured the window");
        destroy_window_objects();
        disconnect();
        return false;
    }
    // Settings that did not arrive during the handshake are waited for briefly: a first frame in the wrong colour
    // scheme would flash; a desktop without the portal answers at once with an error.
    dbus::desktop_settings_wait_loaded(250);
    *out = reinterpret_cast<Window>(&g.window);
    return true;
}

void window_destroy(Window window) noexcept {
    if (!is_window(window))
        return;
    dbus::desktop_settings_set_handler(nullptr, nullptr);
    destroy_window_objects();
    disconnect();
}

NativeWindow window_surface_native(Window window) noexcept {
    return is_window(window) ? g.window.surface : nullptr;
}

void window_show(Window window, ShowMode mode) noexcept {
    if (!is_window(window))
        return;
    switch (mode) {
    case ShowMode::hide:
        g.window.visible = false;
        break;
    case ShowMode::show:
    case ShowMode::show_default:
        g.window.visible = true;
        surface_sync();
        break;
    case ShowMode::minimize_no_activate:
        window_minimize(window);
        break;
    case ShowMode::restore:
        if (g.window.states & (1u << xdg_toplevel_state_maximized))
            xdg_toplevel_unset_maximized(g.window.toplevel);
        break;
    }
    emit_state_changed();
}

void window_set_title(Window window, Utf8View title) noexcept {
    if (!is_window(window))
        return;
    char text[512];
    const u32 length = minimum<u32>(static_cast<u32>(title.bytes), sizeof(text) - 1);
    copy_bytes(text, title.data, length);
    text[length] = 0;
    xdg_toplevel_set_title(g.window.toplevel, text);
}

void window_minimize(Window window) noexcept {
    if (is_window(window))
        xdg_toplevel_set_minimized(g.window.toplevel);
}

void window_toggle_maximize(Window window) noexcept {
    if (!is_window(window))
        return;
    if (g.window.states & (1u << xdg_toplevel_state_maximized))
        xdg_toplevel_unset_maximized(g.window.toplevel);
    else
        xdg_toplevel_set_maximized(g.window.toplevel);
}

void window_request_close(Window window) noexcept {
    if (!is_window(window))
        return;
    ring_push(make_event(EventType::close_requested, now_ms32()));
    loop_wake();
}

// No Wayland protocol lets a client keep its surface out of captures: the compositor and the portal decide.
bool window_set_capture_excluded(Window, bool) noexcept {
    return false;
}

bool window_set_fullscreen(Window window, bool fullscreen) noexcept {
    if (!is_window(window) || !g.caps.fullscreen)
        return false;
    if (fullscreen)
        xdg_toplevel_set_fullscreen(g.window.toplevel, nullptr);
    else
        xdg_toplevel_unset_fullscreen(g.window.toplevel);
    return true;
}

void window_show_system_menu(Window window, float x, float y) noexcept {
    const u32 serial = latest_input_serial();
    if (!is_window(window) || !g.caps.window_menu || !g.seat || serial == 0)
        return;
    const float scale = pixel_scale();
    xdg_toplevel_show_window_menu(g.window.toplevel, g.seat, serial, static_cast<i32>(x / scale),
                                  static_cast<i32>(y / scale));
}

void window_set_min_size(Window window, u32 width_px, u32 height_px) noexcept {
    if (is_window(window))
        set_min_size_px(width_px, height_px);
}


void window_invalidate(Window) noexcept {}

bool window_focus(Window window) noexcept {
    return is_window(window) && g.keyboard.focused;
}

WindowMetrics window_metrics(Window window) noexcept {
    WindowMetrics metrics{};
    if (!is_window(window))
        return metrics;
    window_pixel_size(&metrics.width_px, &metrics.height_px);
    metrics.dpi = g.window.dpi;
    metrics.scale_120 = g.window.dpi * 120 / 96;
    return metrics;
}

u32 window_display_count() noexcept {
    u32 count = 0;
    for (const wl_output* output : g.extras.outputs)
        count += output != nullptr;
    return count;
}

u32 window_state(Window window) noexcept {
    return is_window(window) ? g.window.state_bits : 0;
}

// The band of the resize border the app's frame draws: 8 logical pixels, as the Windows frame has at 96 DPI.
u32 window_resize_border(Window, u32 dpi) noexcept {
    return (8 * dpi + 48) / 96;
}

// The app draws the whole frame: no shadow border beside the client.
bool window_frame_inset(Window window, u32* pixels) noexcept {
    *pixels = 0;
    return is_window(window);
}

void window_frame_callback_request(Window window) noexcept {
    if (!is_window(window) || g.window.frame_callback)
        return;
    g.window.frame_callback = wl_surface_frame(g.window.surface);
    wl_callback_add_listener(g.window.frame_callback, &kFrameListener, nullptr);
}

void window_set_frame_listener(Window window, FrameListener listener, void* context) noexcept {
    if (!is_window(window))
        return;
    g.window.frame_listener = listener;
    g.window.frame_listener_context = context;
}

void window_fatal_message(Utf8View title, Utf8View text) noexcept {
    sys_write(2, title.data, title.bytes);
    sys_write(2, ": ", 2);
    sys_write(2, text.data, text.bytes);
    sys_write(2, "\n", 1);
}

PlatformSettings platform_settings_read() noexcept {
    dbus::DesktopSettings desktop;
    dbus::desktop_settings_get(&desktop);
    PlatformSettings settings = wayland::platform_settings_from_desktop(desktop);
    settings.system_idle_ms = extras_system_idle_ms();
    return settings;
}

bool window_inject(Window window, const WindowEvent& event, WindowEventResult* result) noexcept {
    if (!is_window(window) || !g.window.sink)
        return false;
    // As a real event goes: the pointer and capture bookkeeping around the sink (not the decoration filter: an
    // accessibility action is not a click on the frame).
    WindowEvent synthetic = event;
    synthetic.flags |= ev_synthetic;
    WindowEventResult local{};
    pointer_before_dispatch(synthetic);
    sink_call(synthetic, &local);
    pointer_after_dispatch(synthetic);
    if (event.type == EventType::close_requested && !local.handled) {
        g.quit = true;
        g.exit_code = 0;
    }
    if (result)
        *result = local;
    return true;
}

bool window_wayland_global(u32 index, const char** name, u32* version) noexcept {
    if (index >= g.global_count)
        return false;
    if (name)
        *name = g.globals[index].name;
    if (version)
        *version = g.globals[index].version;
    return true;
}

bool window_wayland_info(Window window, WaylandSurfaceInfo* info) noexcept {
    if (!is_window(window) || !info)
        return false;
    info->display = g.display;
    info->surface = g.window.surface;
    window_pixel_size(&info->width_px, &info->height_px);
    info->scale_120 = g.window.dpi * 120 / 96;
    info->logical_width = g.window.logical_width;
    info->logical_height = g.window.logical_height;
    return true;
}

bool window_wayland_opaque_region(Window window, u32* width, u32* height) noexcept {
    if (!is_window(window) || !g.window.opaque_sent || g.window.opaque_width == 0 || g.window.opaque_height == 0)
        return false;
    if (width)
        *width = static_cast<u32>(g.window.opaque_width);
    if (height)
        *height = static_cast<u32>(g.window.opaque_height);
    return true;
}

} // namespace maicord
