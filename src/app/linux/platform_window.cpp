// The Linux half of the main window: it is created through the window service (the Wayland backend draws the frame, the
// caption controller answers its hit tests as events), shown the way the shell asks. The portable event chain is
// shell/window_dispatch.cpp.
#include "../shell/state.hpp"
#include "../shell/window.hpp"
#include "../shell/window_runtime.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/platform/window/wayland.hpp"

namespace maicord::app_detail {

void application_platform_window_prepare() noexcept {
    // The GPU device is made on the compositor's connection (EGL gets its display from it); a missing compositor
    // is reported when the window is created.
    if (void* display = window_wayland_connect())
        gpu_device_set_native_display(display);
}

bool application_platform_window_create(Window* out, const char** failure) noexcept {
    // The application id is the name of the desktop entry, which is how the compositor finds the icon.
    if (!window_create({utf8_literal("MaiCord"), utf8_literal("io.github.maj113.MaiCord"), 1280, 800, false},
                       application_window_event, nullptr, out)) {
        *failure = "create_window_failed";
        return false;
    }
    // The renderer presents through its own EGL surface on the window's wl_surface; the window must not paint its
    // wl_shm clear over it at the next configure.
    window_wayland_use_external_buffers(*out, true);
    return true;
}

void application_platform_window_show(ShowMode mode) noexcept {
    window_show(g_app.window, mode);
}

} // namespace maicord::app_detail
