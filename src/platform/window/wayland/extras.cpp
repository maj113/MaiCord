// The smaller services of the window: the integer scale from wl_output and wl_surface for compositors without
// fractional-scale, xdg-activation (attention, focus requests), idle inhibit, and the integration protocols:
// xdg-foreign (the window as the parent of portal dialogs), ext-idle-notify (the system's input idle time),
// xdg-system-bell and xdg-toplevel-icon.
#include "maicord/core/time.hpp"
#include "maicord/platform/dbus/portal.hpp"
#include "maicord/platform/window/wayland.hpp"
#include "maicord/platform/window/wayland/abi.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "backend.hpp"

namespace maicord::wayland {
namespace {

constexpr u32 kMaxOutputs = 8;

u32 output_slot(wl_output* output) noexcept {
    for (u32 i = 0; i < kMaxOutputs; ++i)
        if (g.extras.outputs[i] == output)
            return i;
    return kMaxOutputs;
}

// The scale the window gets when the compositor has no fractional scale: what the compositor prefers for the surface
// (wl_surface version 6), else the largest scale of the outputs the surface is on.
void update_integer_scale() noexcept {
    if (g.fractional_manager)
        return;
    ExtrasState& extras = g.extras;
    u32 scale = extras.preferred_integer;
    if (scale == 0) {
        for (u32 i = 0; i < kMaxOutputs; ++i)
            if (extras.output_entered[i] && extras.output_scale[i] > scale)
                scale = extras.output_scale[i];
    }
    if (scale != 0)
        window_scale_changed(scale * 120);
}

void on_output_geometry(void*, wl_output*, i32, i32, i32, i32, i32, const char*, const char*, i32) noexcept {}
void on_output_mode(void*, wl_output*, u32, i32, i32, i32) noexcept {}
void on_output_done(void*, wl_output*) noexcept {}
void on_output_name(void*, wl_output*, const char*) noexcept {}
void on_output_description(void*, wl_output*, const char*) noexcept {}

void on_output_scale(void*, wl_output* output, i32 factor) noexcept {
    const u32 slot = output_slot(output);
    if (slot == kMaxOutputs || factor < 1)
        return;
    g.extras.output_scale[slot] = static_cast<u32>(factor);
    update_integer_scale();
}

const wl_output_listener kOutputListener = {on_output_geometry, on_output_mode, on_output_done, on_output_scale,
                                            on_output_name,     on_output_description};

void on_surface_enter(void*, wl_surface*, wl_output* output) noexcept {
    const u32 slot = output_slot(output);
    if (slot == kMaxOutputs)
        return;
    g.extras.output_entered[slot] = true;
    update_integer_scale();
}

void on_surface_leave(void*, wl_surface*, wl_output* output) noexcept {
    const u32 slot = output_slot(output);
    if (slot == kMaxOutputs)
        return;
    g.extras.output_entered[slot] = false;
    update_integer_scale();
}

void on_preferred_buffer_scale(void*, wl_surface*, i32 factor) noexcept {
    g.extras.preferred_integer = factor > 0 ? static_cast<u32>(factor) : 0;
    update_integer_scale();
}

void on_preferred_buffer_transform(void*, wl_surface*, u32) noexcept {}

const wl_surface_listener kSurfaceEvents = {on_surface_enter, on_surface_leave, on_preferred_buffer_scale,
                                            on_preferred_buffer_transform};

void on_token_done(void*, xdg_activation_token_v1* token, const char* string) noexcept {
    ++g.extras.tokens_done;
    if (g.activation_manager && g.window.surface)
        xdg_activation_v1_activate(g.activation_manager, string, g.window.surface);
    xdg_activation_token_v1_destroy(token);
    g.extras.token = nullptr;
}

const xdg_activation_token_v1_listener kTokenListener = {on_token_done};

// The export's handle arrives once; the portals take it as "wayland:<handle>" (dbus::portal_parent_window).
void on_exported_handle(void*, zxdg_exported_v2*, const char* handle) noexcept {
    char text[96];
    u32 length = 0;
    for (const char* scheme = "wayland:"; *scheme; ++scheme)
        text[length++] = *scheme;
    for (; *handle && length + 1 < sizeof(text); ++handle)
        text[length++] = *handle;
    text[*handle ? 0 : length] = 0;  // a handle that does not fit is no handle
    dbus::portal_set_parent_window(text);
}

const zxdg_exported_v2_listener kExportedListener = {on_exported_handle};

void on_idled(void*, ext_idle_notification_v1*) noexcept {
    g.extras.idle_now = true;
    g.extras.idle_mark_ms = clock_monotonic_milliseconds();
}

void on_resumed(void*, ext_idle_notification_v1*) noexcept {
    g.extras.idle_now = false;
    g.extras.idle_mark_ms = clock_monotonic_milliseconds();
}

const ext_idle_notification_v1_listener kIdleListener = {on_idled, on_resumed};

bool is_window(Window window) noexcept {
    return g.window_exists && window == reinterpret_cast<Window>(&g.window);
}

} // namespace

void extras_attach_surface() noexcept {
    wl_surface_add_listener(g.window.surface, &kSurfaceEvents, nullptr);
    if (g.exporter_manager) {
        g.extras.exported = zxdg_exporter_v2_export_toplevel(g.exporter_manager, g.window.surface);
        zxdg_exported_v2_add_listener(g.extras.exported, &kExportedListener, nullptr);
    }
}

void extras_output_added(wl_output* output) noexcept {
    ExtrasState& extras = g.extras;
    const u32 slot = output_slot(nullptr);
    if (slot == kMaxOutputs) {
        g_client.proxy_destroy(reinterpret_cast<Proxy*>(output));
        return;
    }
    extras.outputs[slot] = output;
    extras.output_scale[slot] = 1;
    wl_output_add_listener(output, &kOutputListener, nullptr);
}

void extras_shutdown() noexcept {
    ExtrasState& extras = g.extras;
    if (extras.token)
        xdg_activation_token_v1_destroy(extras.token);
    if (extras.inhibitor)
        zwp_idle_inhibitor_v1_destroy(extras.inhibitor);
    if (extras.exported) {
        zxdg_exported_v2_destroy(extras.exported);
        dbus::portal_set_parent_window(nullptr);
    }
    if (extras.idle)
        ext_idle_notification_v1_destroy(extras.idle);
    for (u32 i = 0; i < kMaxOutputs; ++i) {
        wl_output* output = extras.outputs[i];
        if (!output)
            continue;
        if (g_client.proxy_get_version(reinterpret_cast<Proxy*>(output)) >= wl_output_release_since)
            wl_output_release(output);
        else
            g_client.proxy_destroy(reinterpret_cast<Proxy*>(output));
    }
    clear_bytes(&extras, static_cast<u32>(sizeof(extras)));
}

// What the system's input idle time is, for PlatformSettings: kSystemIdleUnknown without a watch; once the compositor has
// said "idle" the timeout and what has passed since; while the user is active 0 (the protocol says no more than "less
// than the timeout").
u32 extras_system_idle_ms() noexcept {
    const ExtrasState& extras = g.extras;
    if (!extras.idle)
        return kSystemIdleUnknown;
    if (!extras.idle_now)
        return 0;
    const u64 total = extras.idle_timeout_ms + (clock_monotonic_milliseconds() - extras.idle_mark_ms);
    return total >= kSystemIdleUnknown ? kSystemIdleUnknown - 1 : static_cast<u32>(total);
}

} // namespace maicord::wayland

namespace maicord {

using namespace wayland;

void window_request_attention(Window window, Attention attention) noexcept {
    if (attention == Attention::flash)
        window_wayland_request_activation(window);
}

void window_wayland_request_activation(Window window) noexcept {
    ExtrasState& extras = g.extras;
    if (!is_window(window) || !g.activation_manager || extras.token)
        return;
    extras.token = xdg_activation_v1_get_activation_token(g.activation_manager);
    xdg_activation_token_v1_add_listener(extras.token, &kTokenListener, nullptr);
    if (g.seat && g.input_serial != 0)
        xdg_activation_token_v1_set_serial(extras.token, g.input_serial, g.seat);
    xdg_activation_token_v1_set_app_id(extras.token, g.window.app_id);
    xdg_activation_token_v1_set_surface(extras.token, g.window.surface);
    xdg_activation_token_v1_commit(extras.token);
    g_client.display_flush(g.display);
}

bool window_wayland_activate(const char* token) noexcept {
    if (!g.window_exists || !g.activation_manager || !token || !*token)
        return false;
    xdg_activation_v1_activate(g.activation_manager, token, g.window.surface);
    g_client.display_flush(g.display);
    return true;
}

u32 window_wayland_activation_count() noexcept {
    return g.extras.tokens_done;
}

bool window_wayland_set_idle_inhibit(Window window, bool inhibit) noexcept {
    ExtrasState& extras = g.extras;
    if (!is_window(window))
        return false;
    if (inhibit && !extras.inhibitor) {
        if (!g.idle_manager)
            return false;
        extras.inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(g.idle_manager, g.window.surface);
    } else if (!inhibit && extras.inhibitor) {
        zwp_idle_inhibitor_v1_destroy(extras.inhibitor);
        extras.inhibitor = nullptr;
    }
    g_client.display_flush(g.display);
    return true;
}

bool window_wayland_watch_idle(Window window, u32 timeout_ms) noexcept {
    ExtrasState& extras = g.extras;
    if (!is_window(window) || !g.idle_notifier || !g.seat || timeout_ms == 0)
        return false;
    if (extras.idle)
        ext_idle_notification_v1_destroy(extras.idle);
    // Version 2 has the notification that counts input alone: our own idle inhibitor (a call) must not hide the user's absence.
    extras.idle = g.idle_notifier_version >= ext_idle_notifier_v1_get_input_idle_notification_since
                      ? ext_idle_notifier_v1_get_input_idle_notification(g.idle_notifier, timeout_ms, g.seat)
                      : ext_idle_notifier_v1_get_idle_notification(g.idle_notifier, timeout_ms, g.seat);
    ext_idle_notification_v1_add_listener(extras.idle, &kIdleListener, nullptr);
    extras.idle_timeout_ms = timeout_ms;
    extras.idle_now = false;
    extras.idle_mark_ms = clock_monotonic_milliseconds();
    g_client.display_flush(g.display);
    return true;
}

bool window_wayland_system_bell() noexcept {
    if (!g.bell)
        return false;
    xdg_system_bell_v1_ring(g.bell, g.window_exists ? g.window.surface : nullptr);
    g_client.display_flush(g.display);
    return true;
}

// The window's icon as xdg-toplevel-icon pixel buffers. IconImage is straight alpha; wl_shm's ARGB8888 is premultiplied.
void window_set_icon(Window window, const IconImage* images, u32 count) noexcept {
    constexpr u32 kMaxIcons = 4;
    constexpr u32 kMaxExtent = 256;
    if (!is_window(window) || !g.icon_manager || !g.shm || !images)
        return;
    const IconImage* chosen[kMaxIcons];
    usize offsets[kMaxIcons];
    u32 used = 0;
    usize bytes = 0;
    for (u32 i = 0; i < count && used < kMaxIcons; ++i) {
        if (!images[i].pixels || images[i].extent == 0 || images[i].extent > kMaxExtent)
            continue;
        chosen[used] = &images[i];
        offsets[used++] = bytes;
        bytes += static_cast<usize>(images[i].extent) * images[i].extent * 4;
    }
    if (!used)
        return;
    const long fd = sys_memfd_create("maicord-icon", kMemfdCloseOnExec);
    if (syscall_failed(fd))
        return;
    const long address = syscall_failed(sys_ftruncate(static_cast<int>(fd), bytes))
                             ? -1
                             : sys_mmap_file(static_cast<int>(fd), bytes, kProtectRead | kProtectWrite, kMapShared);
    if (syscall_failed(address)) {
        sys_close(static_cast<int>(fd));
        return;
    }
    u32* memory = reinterpret_cast<u32*>(address);
    for (u32 i = 0; i < used; ++i) {
        u32* out = memory + offsets[i] / 4;
        const u32 pixels = chosen[i]->extent * chosen[i]->extent;
        for (u32 p = 0; p < pixels; ++p) {
            const u32 pixel = chosen[i]->pixels[p];
            const u32 alpha = pixel >> 24;
            const auto scale = [alpha](u32 channel) { return (channel * alpha + 127) / 255; };
            out[p] = alpha << 24 | scale(pixel >> 16 & 0xFF) << 16 | scale(pixel >> 8 & 0xFF) << 8 | scale(pixel & 0xFF);
        }
    }
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, static_cast<i32>(fd), static_cast<i32>(bytes));
    sys_munmap(memory, bytes);
    sys_close(static_cast<int>(fd));
    xdg_toplevel_icon_v1* icon = xdg_toplevel_icon_manager_v1_create_icon(g.icon_manager);
    wl_buffer* buffers[kMaxIcons];
    for (u32 i = 0; i < used; ++i) {
        const i32 extent = static_cast<i32>(chosen[i]->extent);
        buffers[i] = wl_shm_pool_create_buffer(pool, static_cast<i32>(offsets[i]), extent, extent, extent * 4,
                                               wl_shm_format_argb8888);
        xdg_toplevel_icon_v1_add_buffer(icon, buffers[i], 1);
    }
    xdg_toplevel_icon_manager_v1_set_icon(g.icon_manager, g.window.toplevel, icon);
    // The compositor keeps what it took; the buffers must outlive the icon object only.
    xdg_toplevel_icon_v1_destroy(icon);
    for (u32 i = 0; i < used; ++i)
        wl_buffer_destroy(buffers[i]);
    wl_shm_pool_destroy(pool);
    g_client.display_flush(g.display);
}

} // namespace maicord
