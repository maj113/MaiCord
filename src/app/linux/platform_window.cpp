// The Linux half of the main window: it is created through the window service (the Wayland backend draws the frame, the
// caption controller answers its hit tests as events), shown the way the shell asks. The portable event chain is
// shell/window_dispatch.cpp.
#include "../shell/state.hpp"
#include "../shell/window.hpp"
#include "../shell/window_runtime.hpp"
#include "maicord/app/account/presence_idle.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/gpu/gles/backend.hpp"
#include "maicord/platform/window/wayland.hpp"

namespace maicord::app_detail {
namespace {

// The compositor's frame callback is the frame clock: the surface asks for one before every swap and the window
// reports it here. The renderer (and so its surface) is made and remade after the window, hence the checks.
void frame_done(void*, u32) noexcept {
    if (g_app.renderer_ready && gpu_surface_valid(&g_app.renderer.graphics.surface))
        gpu_surface_frame_callback(&g_app.renderer.graphics.surface);
}

void frame_request(void*) noexcept {
    window_frame_callback_request(g_app.window);
}

} // namespace

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
    // The renderer presents through its own EGL surface on the window's wl_surface; the window only keeps its geometry.
    window_set_frame_listener(*out, frame_done, nullptr);
    gpu_surface_set_frame_request(frame_request, nullptr);
    // The compositor's input idle time is what "away" means for the presence (the same ten minutes Windows' last input
    // gives). Without ext-idle-notify (GNOME's Mutter has none) the system idle time stays unknown and the status never idles.
    window_wayland_watch_idle(*out, static_cast<u32>(kPresenceIdleTimeoutMs));
    return true;
}

void application_platform_window_show(ShowMode mode) noexcept {
    window_show(g_app.window, mode);
}

} // namespace maicord::app_detail
