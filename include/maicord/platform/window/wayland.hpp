#pragma once

#include "maicord/platform/window/window.hpp"

// Services of the Wayland window backend (Linux) that have no Windows counterpart: what the GPU backend needs to
// create an EGL window surface on the window's wl_surface, and bring-up presentation without a GPU backend.
// The portable window service is window.hpp; window_surface_native() returns the wl_surface*.

namespace maicord {

struct WaylandSurfaceInfo {
    void* display;  // wl_display*: eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display)
    void* surface;  // wl_surface*, the same as window_surface_native
    u32 width_px, height_px;  // the buffer size the window wants now: logical size at the scale
    u32 scale_120;            // 120 * dpi / 96, as WindowMetrics
    u32 logical_width, logical_height;  // the surface size in compositor units; the viewport destination
};
bool window_wayland_info(Window window, WaylandSurfaceInfo* info) noexcept;

// The GPU backend takes over the surface's buffers (true): from now on it attaches and commits (eglSwapBuffers) and
// the window stops presenting its wl_shm clear. The window keeps the wp_viewport destination at the logical size
// and sends frame callbacks through window_frame_callback_request. False hands the surface back; the window
// presents its clear again.
void window_wayland_use_external_buffers(Window window, bool external) noexcept;

// The colour (0xRRGGBB) of the wl_shm clear the window presents while it owns the buffers. Dark grey by default.
void window_wayland_set_clear_color(Window window, u32 xrgb) noexcept;

// The globals the compositor advertised, in registry order: false past the last. The name stays valid until the
// window is destroyed. What the protocol support matrix and diagnostics are built from.
bool window_wayland_global(u32 index, const char** name, u32* version) noexcept;

} // namespace maicord
