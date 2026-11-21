#pragma once
// What the Wayland window tests read of the backend's state (src/platform/window/wayland/backend.hpp; the client reads
// none of it): the surface and its sizes, the opaque region last sent, the resize margin, the cursor and the activation
// token that waits for the compositor. The test needs
// "${MAICORD_WAYLAND_GENERATED_DIRECTORY}" among its INCLUDES.
#include "../../src/platform/window/wayland/backend.hpp"

namespace maicord {

struct WaylandSurfaceInfo {
    void* display;  // wl_display*
    void* surface;  // wl_surface*, the same as window_surface_native
    u32 width_px, height_px;  // the buffer size the window wants now: logical size at the scale
    u32 scale_120;            // 120 * dpi / 96, as WindowMetrics
    u32 logical_width, logical_height;  // the surface size in compositor units; the viewport destination
};

inline bool window_wayland_info(Window window, WaylandSurfaceInfo* info) noexcept {
    if (!wayland::is_window(window) || !info)
        return false;
    info->display = wayland::g.display;
    info->surface = wayland::g.window.surface;
    wayland::window_pixel_size(&info->width_px, &info->height_px);
    info->scale_120 = wayland::g.window.dpi * 120 / 96;
    info->logical_width = wayland::g.window.logical_width;
    info->logical_height = wayland::g.window.logical_height;
    return true;
}

// The opaque region last sent (wl_surface.set_opaque_region), in surface units; false before the first one.
inline bool window_wayland_opaque_region(Window window, u32* width, u32* height) noexcept {
    const wayland::WindowState& state = wayland::g.window;
    if (!wayland::is_window(window) || !state.opaque_sent || state.opaque_width == 0 || state.opaque_height == 0)
        return false;
    *width = static_cast<u32>(state.opaque_width);
    *height = static_cast<u32>(state.opaque_height);
    return true;
}

// The resize margin last committed, in surface units: false when the window has none (the compositor lacks
// wl_subcompositor or wp_viewporter). Width and height are 0 while the margin is unmapped.
inline bool window_wayland_resize_margin(Window window, u32* width, u32* height) noexcept {
    const wayland::WindowState& state = wayland::g.window;
    if (!wayland::is_window(window) || !state.margin_surface)
        return false;
    *width = static_cast<u32>(state.margin_width);
    *height = static_cast<u32>(state.margin_height);
    return true;
}

// The cursor the window last gave the compositor: a UiCursor, or kFrameCursorBase + the FrameHit of a frame edge. 0xFF
// when it gave none since the pointer entered.
inline u8 window_wayland_applied_cursor() noexcept {
    return wayland::g.pointer.applied_valid ? wayland::g.pointer.applied_cursor : 0xFF;
}

// Whether the compositor gave the pointer to the resize margin: it is over the margin now.
inline bool window_wayland_pointer_on_margin() noexcept {
    return wayland::g.pointer.margin;
}

// Whether an activation token waits for the compositor's answer (window_request_attention asked for it).
inline bool window_wayland_activation_pending() noexcept {
    return wayland::g.extras.token != nullptr;
}

} // namespace maicord
