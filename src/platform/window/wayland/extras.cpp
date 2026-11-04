// The smaller services of the window: the integer scale from wl_output and wl_surface for compositors without
// fractional-scale, xdg-activation (attention, focus requests), idle inhibit and the wl_egl_window the GPU backend renders to.
#include "maicord/platform/window/wayland.hpp"
#include "maicord/platform/window/wayland/abi.hpp"
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

bool is_window(Window window) noexcept {
    return g.window_exists && window == reinterpret_cast<Window>(&g.window);
}

} // namespace

void extras_attach_surface() noexcept {
    wl_surface_add_listener(g.window.surface, &kSurfaceEvents, nullptr);
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
    if (extras.egl)
        egl_window_api()->destroy(extras.egl);
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

void egl_sync() noexcept {
    if (g.extras.egl) {
        u32 width = 0;
        u32 height = 0;
        window_pixel_size(&width, &height);
        egl_window_api()->resize(g.extras.egl, static_cast<int>(width), static_cast<int>(height), 0, 0);
    }
    apply_surface_geometry();
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
    xdg_activation_token_v1_set_app_id(extras.token, "maicord");
    xdg_activation_token_v1_set_surface(extras.token, g.window.surface);
    xdg_activation_token_v1_commit(extras.token);
    g_client.display_flush(g.display);
}

bool window_wayland_activate_with_token(Window window, const char* token) noexcept {
    if (!is_window(window) || !g.activation_manager || !token || !*token)
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

void* window_wayland_egl_window(Window window) noexcept {
    ExtrasState& extras = g.extras;
    if (!is_window(window))
        return nullptr;
    if (!extras.egl) {
        const EglWindowApi* api = egl_window_api();
        if (!api)
            return nullptr;
        u32 width = 0;
        u32 height = 0;
        window_pixel_size(&width, &height);
        extras.egl = api->create(g.window.surface, static_cast<int>(width), static_cast<int>(height));
        if (!extras.egl)
            return nullptr;
        window_wayland_use_external_buffers(window, true);
        apply_surface_geometry();
    }
    return extras.egl;
}

void window_wayland_egl_release(Window window) noexcept {
    ExtrasState& extras = g.extras;
    if (!is_window(window) || !extras.egl)
        return;
    egl_window_api()->destroy(extras.egl);
    extras.egl = nullptr;
}

} // namespace maicord
