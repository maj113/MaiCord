#pragma once
// What the Wayland window tests read of the backend's state (src/platform/window/wayland/backend.hpp; the client reads
// none of it): the surface and its sizes, the globals the compositor advertised, the opaque region last sent and the
// activation tokens the compositor answered. The test needs "${MAICORD_WAYLAND_GENERATED_DIRECTORY}" among its INCLUDES.
#include "../../src/platform/window/wayland/backend.hpp"

namespace maicord {

struct WaylandSurfaceInfo {
    void* display;  // wl_display*
    void* surface;  // wl_surface*, the same as window_surface_native
    u32 width_px, height_px;  // the buffer size the window wants now: logical size at the scale
    u32 scale_120;            // 120 * dpi / 96, as WindowMetrics
    u32 logical_width, logical_height;  // the surface size in compositor units; the viewport destination
};

inline bool wayland_test_is_window(Window window) noexcept {
    return wayland::g.window_exists && window == reinterpret_cast<Window>(&wayland::g.window);
}

inline bool window_wayland_info(Window window, WaylandSurfaceInfo* info) noexcept {
    if (!wayland_test_is_window(window) || !info)
        return false;
    info->display = wayland::g.display;
    info->surface = wayland::g.window.surface;
    wayland::window_pixel_size(&info->width_px, &info->height_px);
    info->scale_120 = wayland::g.window.dpi * 120 / 96;
    info->logical_width = wayland::g.window.logical_width;
    info->logical_height = wayland::g.window.logical_height;
    return true;
}

// The globals the compositor advertised, in registry order: false past the last.
inline bool window_wayland_global(u32 index, const char** name, u32* version) noexcept {
    if (index >= wayland::g.global_count)
        return false;
    *name = wayland::g.globals[index].name;
    *version = wayland::g.globals[index].version;
    return true;
}

// The opaque region last sent (wl_surface.set_opaque_region), in surface units; false before the first one.
inline bool window_wayland_opaque_region(Window window, u32* width, u32* height) noexcept {
    const wayland::WindowState& state = wayland::g.window;
    if (!wayland_test_is_window(window) || !state.opaque_sent || state.opaque_width == 0 || state.opaque_height == 0)
        return false;
    *width = static_cast<u32>(state.opaque_width);
    *height = static_cast<u32>(state.opaque_height);
    return true;
}

// The activation tokens the compositor has answered.
inline u32 window_wayland_activation_count() noexcept {
    return wayland::g.extras.tokens_done;
}

} // namespace maicord
