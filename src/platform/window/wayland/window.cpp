// The Wayland window: the connection, the registry, xdg-shell, the frame callback, a wl_shm clear for bring-up and
// the window service of maicord/platform/window/window.hpp. Input is in pointer.cpp and keyboard.cpp, the loop in
// pump.cpp; the shared state is in backend.hpp.
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/window/wayland.hpp"
#include "maicord/platform/window/wayland/abi.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "backend.hpp"
#include "fd.hpp"

namespace maicord::wayland {

Backend g;

namespace {

// Versions bound at most: the newest each feature of the backend uses.
constexpr u32 kCompositorVersion = 6;
constexpr u32 kSeatVersion = 8;
constexpr u32 kWmBaseVersion = 6;
constexpr u32 kCreateTimeoutMs = 10000;
constexpr u32 kDefaultDpi = 96;
constexpr u32 kDefaultClear = 0xFF1E1F22u;

bool same(const char* left, const char* right) noexcept {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

// Registry ------------------------------------------------------------------------------------------------------------

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

void on_global(void*, wl_registry* registry, u32 name, const char* interface, u32 version) noexcept {
    if (g.global_count < kMaxGlobals) {
        GlobalName& entry = g.globals[g.global_count++];
        u32 length = 0;
        for (; interface[length] && length + 1 < sizeof(entry.name); ++length)
            entry.name[length] = interface[length];
        entry.name[length] = 0;
        entry.version = version;
    }
    if (same(interface, "wl_compositor")) {
        if (version >= 4 && !g.compositor) {
            g.compositor_version = minimum(version, kCompositorVersion);
            g.compositor = static_cast<wl_compositor*>(
                wl_registry_bind(registry, name, &wl_compositor_interface, g.compositor_version));
        }
    } else if (same(interface, "wl_shm")) {
        if (!g.shm)
            g.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (same(interface, "xdg_wm_base")) {
        if (!g.wm_base) {
            g.wm_base_version = minimum(version, kWmBaseVersion);
            g.wm_base = static_cast<xdg_wm_base*>(
                wl_registry_bind(registry, name, &xdg_wm_base_interface, g.wm_base_version));
            xdg_wm_base_add_listener(g.wm_base, &kWmBaseListener, nullptr);
        }
    } else if (same(interface, "wl_seat")) {
        if (!g.seat) {
            g.seat_version = minimum(version, kSeatVersion);
            g.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, g.seat_version));
            wl_seat_add_listener(g.seat, &kSeatListener, nullptr);
        }
    } else if (same(interface, "wp_viewporter")) {
        if (!g.viewporter)
            g.viewporter = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    } else if (same(interface, "wp_fractional_scale_manager_v1")) {
        if (!g.fractional_manager)
            g.fractional_manager = static_cast<wp_fractional_scale_manager_v1*>(
                wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
    } else if (same(interface, "wp_cursor_shape_manager_v1")) {
        if (!g.cursor_shape_manager)
            g.cursor_shape_manager = static_cast<wp_cursor_shape_manager_v1*>(
                wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1));
    } else if (same(interface, "zxdg_decoration_manager_v1")) {
        if (!g.decoration_manager)
            g.decoration_manager = static_cast<zxdg_decoration_manager_v1*>(
                wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1));
    }
}

void on_global_remove(void*, wl_registry*, u32) noexcept {}

const wl_registry_listener kRegistryListener = {on_global, on_global_remove};

// Window state --------------------------------------------------------------------------------------------------------

u32 scale_to_dpi(u32 scale_120) noexcept {
    return (kDefaultDpi * scale_120 + 60) / 120;
}

u32 to_pixels(u32 logical) noexcept {
    return (logical * g.window.dpi + kDefaultDpi / 2) / kDefaultDpi;
}

u32 state_bits_of(u32 states) noexcept {
    constexpr u32 tiled = (1u << xdg_toplevel_state_tiled_left) | (1u << xdg_toplevel_state_tiled_right) |
                          (1u << xdg_toplevel_state_tiled_top) | (1u << xdg_toplevel_state_tiled_bottom);
    u32 bits = 0;
    if (states & (1u << xdg_toplevel_state_maximized))
        bits |= state_maximized;
    if (states & (1u << xdg_toplevel_state_fullscreen))
        bits |= state_fullscreen;
    if (states & (1u << xdg_toplevel_state_suspended))
        bits |= state_suspended;
    if (states & tiled)
        bits |= state_tiled;
    if (!g.caps.csd)
        bits |= state_decorated;
    if (g.window.visible && !(states & (1u << xdg_toplevel_state_suspended)))
        bits |= state_visible;
    return bits;
}

void emit_state_changed() noexcept {
    const u32 bits = state_bits_of(g.window.states);
    const u32 changed = bits ^ g.window.state_bits;
    g.window.state_bits = bits;
    if (!changed)
        return;
    WindowEvent event = make_event(EventType::state_changed, now_ms32());
    event.window_state.changed = changed;
    event.window_state.state = bits;
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
    constexpr u32 sized_states = (1u << xdg_toplevel_state_maximized) | (1u << xdg_toplevel_state_fullscreen) |
                                 (1u << xdg_toplevel_state_tiled_left) | (1u << xdg_toplevel_state_tiled_right) |
                                 (1u << xdg_toplevel_state_tiled_top) | (1u << xdg_toplevel_state_tiled_bottom);
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
    emit_state_changed();
    if (was_resizing && !now_resizing)
        emit_resize(EventType::resize_end);
    present_clear();
}

const xdg_surface_listener kSurfaceListener = {on_surface_configure};

void on_decoration_configure(void*, zxdg_toplevel_decoration_v1*, u32 mode) noexcept {
    g.caps.csd = mode != zxdg_toplevel_decoration_v1_mode_server_side;
    if (g.window.configured)
        emit_state_changed();
}

const zxdg_toplevel_decoration_v1_listener kDecorationListener = {on_decoration_configure};

void on_preferred_scale(void*, wp_fractional_scale_v1*, u32 scale) noexcept {
    if (scale == 0 || scale == g.window.scale_120)
        return;
    g.window.scale_120 = scale;
    g.window.dpi = scale_to_dpi(scale);
    if (!g.window.configured)
        return;
    emit_resize(EventType::scale_changed);
    pointer_scale_changed();
    present_clear();
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

// A wl_shm buffer of one colour, `width` by `height` client pixels.
wl_buffer* make_clear_buffer(u32 width, u32 height, u32 color) noexcept {
    if (!g.shm || width == 0 || height == 0)
        return nullptr;
    const usize stride = static_cast<usize>(width) * 4;
    const usize bytes = stride * height;
    const long fd = fd_memfd_create("maicord-clear");
    if (syscall_failed(fd))
        return nullptr;
    wl_buffer* buffer = nullptr;
    if (!syscall_failed(fd_truncate(static_cast<int>(fd), static_cast<long>(bytes)))) {
        const long address = fd_map(static_cast<int>(fd), bytes, kProtectRead | kProtectWrite, kMapShared);
        if (!syscall_failed(address)) {
            u32* pixels = reinterpret_cast<u32*>(address);
            for (usize i = 0; i < bytes / 4; ++i)
                pixels[i] = color;
            wl_shm_pool* pool = wl_shm_create_pool(g.shm, static_cast<i32>(fd), static_cast<i32>(bytes));
            buffer = wl_shm_pool_create_buffer(pool, 0, static_cast<i32>(width), static_cast<i32>(height),
                                               static_cast<i32>(stride), wl_shm_format_xrgb8888);
            wl_shm_pool_destroy(pool);
            sys_munmap(reinterpret_cast<void*>(address), bytes);
        }
    }
    sys_close(static_cast<int>(fd));
    return buffer;
}

void destroy_window_objects() noexcept {
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
    if (window.shm_buffer)
        wl_buffer_destroy(window.shm_buffer);
    if (window.surface)
        wl_surface_destroy(window.surface);
}

bool configured_and_scaled() noexcept {
    return g.window.configured;
}

void disconnect() noexcept {
    keyboard_shutdown();
    pointer_detach();
    if (g.seat) {
        if (g.seat_version >= wl_seat_release_since)
            wl_seat_release(g.seat);
        else
            g_client.proxy_destroy(reinterpret_cast<Proxy*>(g.seat));
    }
    if (g.decoration_manager)
        zxdg_decoration_manager_v1_destroy(g.decoration_manager);
    if (g.cursor_shape_manager)
        wp_cursor_shape_manager_v1_destroy(g.cursor_shape_manager);
    if (g.fractional_manager)
        wp_fractional_scale_manager_v1_destroy(g.fractional_manager);
    if (g.viewporter)
        wp_viewporter_destroy(g.viewporter);
    if (g.wm_base)
        xdg_wm_base_destroy(g.wm_base);
    if (g.shm)
        g_client.proxy_destroy(reinterpret_cast<Proxy*>(g.shm));  // wl_shm.release exists from version 2; this binds 1
    if (g.compositor)
        g_client.proxy_destroy(reinterpret_cast<Proxy*>(g.compositor));
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

// Shared with the other files -----------------------------------------------------------------------------------------

u32 dpi_of_scale(u32 scale_120) noexcept {
    return scale_to_dpi(scale_120);
}

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
    if (code != 0 && interface)
        log_write(LogLevel::error, "wayland", interface->name);
    log_write(LogLevel::error, "wayland", "the compositor connection failed");
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

void present_clear() noexcept {
    WindowState& window = g.window;
    if (!window.configured || !window.visible || window.external_buffers || !window.surface)
        return;
    u32 width = 0;
    u32 height = 0;
    window_pixel_size(&width, &height);
    wl_buffer* buffer = make_clear_buffer(width, height, window.clear_color);
    if (!buffer)
        return;
    if (window.viewport)
        wp_viewport_set_destination(window.viewport, static_cast<i32>(window.logical_width),
                                    static_cast<i32>(window.logical_height));
    wl_surface_attach(window.surface, buffer, 0, 0);
    wl_surface_damage_buffer(window.surface, 0, 0, static_cast<i32>(width), static_cast<i32>(height));
    wl_surface_commit(window.surface);
    if (window.shm_buffer)
        wl_buffer_destroy(window.shm_buffer);
    window.shm_buffer = buffer;
}

// The window service ---------------------------------------------------------------------------------------------------

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

bool window_create(const WindowCreateInfo& info, WindowSink sink, void* context, Window* out) noexcept {
    if (g.window_exists || !out || info.client_width_dips == 0 || info.client_height_dips == 0)
        return false;
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
    g.display_waitable = waitable_borrow_fd(g_client.display_get_fd(g.display));

    WindowState& window = g.window;
    window.sink = sink;
    window.sink_context = context;
    window.visible = info.visible;
    window.requested_width = info.client_width_dips;
    window.requested_height = info.client_height_dips;
    window.floating_width = info.client_width_dips;
    window.floating_height = info.client_height_dips;
    window.scale_120 = 120;
    window.dpi = kDefaultDpi;
    window.clear_color = kDefaultClear;

    g.caps = WindowCaps{};
    g.caps.csd = true;
    g.caps.sync_clipboard_read = true;
    g.caps.window_menu = true;
    g.caps.maximize = true;
    g.caps.minimize = true;
    g.caps.fullscreen = true;

    char title[256];
    char app_id[128];
    const auto copy = [](char* destination, u32 capacity, Utf8View text) {
        const u32 length = minimum<u32>(static_cast<u32>(text.bytes), capacity - 1);
        copy_bytes(destination, text.data, length);
        destination[length] = 0;
    };
    copy(title, sizeof(title), info.title);
    copy(app_id, sizeof(app_id), info.app_id);

    window.surface = wl_compositor_create_surface(g.compositor);
    window.shell_surface = xdg_wm_base_get_xdg_surface(g.wm_base, window.surface);
    xdg_surface_add_listener(window.shell_surface, &kSurfaceListener, nullptr);
    window.toplevel = xdg_surface_get_toplevel(window.shell_surface);
    xdg_toplevel_add_listener(window.toplevel, &kToplevelListener, nullptr);
    xdg_toplevel_set_title(window.toplevel, title);
    xdg_toplevel_set_app_id(window.toplevel, app_id);
    if (g.decoration_manager) {
        window.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(g.decoration_manager, window.toplevel);
        zxdg_toplevel_decoration_v1_add_listener(window.decoration, &kDecorationListener, nullptr);
        zxdg_toplevel_decoration_v1_set_mode(window.decoration, zxdg_toplevel_decoration_v1_mode_server_side);
    }
    if (g.viewporter) {
        window.viewport = wp_viewporter_get_viewport(g.viewporter, window.surface);
        if (g.fractional_manager) {
            window.fractional = wp_fractional_scale_manager_v1_get_fractional_scale(g.fractional_manager, window.surface);
            wp_fractional_scale_v1_add_listener(window.fractional, &kFractionalListener, nullptr);
            g.caps.fractional_scale = true;
        }
    }
    g.window_exists = true;
    // The initial commit asks for the first configure; the window is ready once it and the first scale arrived.
    wl_surface_commit(window.surface);
    if (!dispatch_until(configured_and_scaled, kCreateTimeoutMs) ||
        (g.caps.fractional_scale && g_client.display_roundtrip(g.display) < 0)) {
        log_write(LogLevel::error, "wayland", "the compositor never configured the window");
        destroy_window_objects();
        disconnect();
        return false;
    }
    *out = reinterpret_cast<Window>(&g.window);
    return true;
}

void window_destroy(Window window) noexcept {
    if (!is_window(window))
        return;
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
        if (!g.window.external_buffers) {
            wl_surface_attach(g.window.surface, nullptr, 0, 0);
            wl_surface_commit(g.window.surface);
        }
        break;
    case ShowMode::show:
    case ShowMode::show_default:
        g.window.visible = true;
        present_clear();
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

void window_set_icon(Window, const IconImage*) noexcept {}

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

void window_begin_move(Window window) noexcept {
    const u32 serial = latest_input_serial();
    if (is_window(window) && g.seat && serial != 0)
        xdg_toplevel_move(g.window.toplevel, g.seat, serial);
}

void window_begin_resize(Window window, FrameHit edge) noexcept {
    const u32 serial = latest_input_serial();
    if (is_window(window) && g.seat && serial != 0)
        xdg_toplevel_resize(g.window.toplevel, g.seat, serial, resize_edge(edge));
}

void window_set_min_size(Window window, u32 width_px, u32 height_px) noexcept {
    if (!is_window(window))
        return;
    const u32 dpi = g.window.dpi;
    xdg_toplevel_set_min_size(g.window.toplevel, static_cast<i32>(width_px * 96 / dpi),
                              static_cast<i32>(height_px * 96 / dpi));
}

void window_request_attention(Window, Attention) noexcept {}

void window_set_drop_target(Window, bool) noexcept {}

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

void window_frame_callback_request(Window window) noexcept {
    if (!is_window(window) || g.window.frame_callback)
        return;
    g.window.frame_callback = wl_surface_frame(g.window.surface);
    wl_callback_add_listener(g.window.frame_callback, &kFrameListener, nullptr);
    wl_surface_commit(g.window.surface);
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
    PlatformSettings settings{};
    settings.double_click_ms = 500;
    settings.double_click_dx_px = 4;
    settings.double_click_dy_px = 4;
    settings.caret_blink_ms = 530;
    settings.wheel_lines = 3;
    settings.wheel_page = false;
    settings.client_area_animation = true;
    settings.dark_scheme = false;
    settings.system_idle_ms = kSystemIdleUnknown;
    return settings;
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

void window_wayland_use_external_buffers(Window window, bool external) noexcept {
    if (!is_window(window))
        return;
    g.window.external_buffers = external;
    if (external && g.window.shm_buffer) {
        wl_buffer_destroy(g.window.shm_buffer);
        g.window.shm_buffer = nullptr;
    } else if (!external) {
        present_clear();
    }
}

void window_wayland_set_clear_color(Window window, u32 xrgb) noexcept {
    if (!is_window(window))
        return;
    g.window.clear_color = xrgb | 0xFF000000u;
    present_clear();
}

} // namespace maicord
