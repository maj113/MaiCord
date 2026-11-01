#include "image_zoom.hpp"
#include "image_viewer.hpp"
#include "../settings/settings.hpp"
#include "../shell/input.hpp"
#include "../shell/overlay_input.hpp"
#include "../shell/state.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
UiZoomLens& lens() noexcept {
    return g_app.image_zoom.lens;
}
// Moves, shows or hides the lens; any change repaints.
void place(bool active, UiPoint point) noexcept {
    auto& l = lens();
    if (l.active == active && (!active || (l.pointer.x == point.x && l.pointer.y == point.y)))
        return;
    l.active = active;
    if (active)
        l.pointer = point;
    ++l.revision;
    application_request_frame();
}
bool ctrl_only() noexcept {
    return application_control_down() && !application_alt_down();
}
// A hovered chat preview that would open in the viewer; spoilers and
// obscured media stay unrevealed, videos never qualify.
void refresh_chat(UiPoint point) noexcept {
    place(g_app.preferences.image_zoom && ctrl_only() && application_image_viewer_open_at(point.x, point.y, true),
          point);
}
} // namespace

void application_image_zoom_configure() noexcept {
    const auto& preferences = g_app.preferences;
    if (!preferences.image_zoom)
        application_image_zoom_end();
    ui_zoom_lens_configure(&lens(), preferences.image_zoom_factor, preferences.image_zoom_size,
                           preferences.image_zoom_square);
}

bool application_image_zoom_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (!g_app.preferences.image_zoom)
        return false;
    auto& zoom = g_app.image_zoom;
    const bool key = message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN ||
                     message == WM_SYSKEYUP;
    // Ctrl is read at key and focus events, so a mouse move costs a load.
    if ((key && (wparam == VK_CONTROL || wparam == VK_MENU)) || message == WM_SETFOCUS ||
        (message == WM_ACTIVATE && LOWORD(wparam) != WA_INACTIVE))
        zoom.ctrl = application_control_down();
    // The viewer keeps its own lens.
    if (g_app.image_viewer.scene.active)
        return false;
    switch (message) {
    case WM_MOUSEMOVE:
        if (zoom.lens.active || zoom.ctrl)
            refresh_chat({client_dip_x(lparam), client_dip_y(lparam)});
        return false;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (wparam == VK_CONTROL || wparam == VK_MENU) {
            UiPoint point{};
            if (application_pointer_dip(&point))
                refresh_chat(point);
        }
        return false;
    case WM_MOUSEWHEEL:
        return application_image_zoom_wheel(wparam);
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MOUSELEAVE:
    case WM_CANCELMODE:
        application_image_zoom_end();
        return false;
    case WM_KILLFOCUS:
        zoom.ctrl = false;
        application_image_zoom_end();
        return false;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            zoom.ctrl = false;
            application_image_zoom_end();
        }
        return false;
    default:
        return false;
    }
}

void application_image_zoom_viewer_pointer(UiPoint point, bool over_image, bool hold) noexcept {
    auto& zoom = g_app.image_zoom;
    zoom.held = hold && over_image;
    const bool show = g_app.preferences.image_zoom && over_image && (hold || ctrl_only());
    place(show, point);
    if (!show)
        application_image_zoom_end();
}

bool application_image_zoom_wheel(WPARAM wparam) noexcept {
    auto& zoom = g_app.image_zoom;
    if (!zoom.lens.active || !g_app.renderer.zoom_lens_source.ready)
        return false;
    if (ui_zoom_lens_wheel(&zoom.lens, static_cast<short>(HIWORD(wparam)), (LOWORD(wparam) & MK_SHIFT) != 0)) {
        zoom.changed = true;
        application_request_frame();
    }
    return true;
}

void application_image_zoom_end() noexcept {
    auto& zoom = g_app.image_zoom;
    zoom.held = false;
    if (zoom.lens.active) {
        zoom.lens.active = false;
        ++zoom.lens.revision;
        application_request_frame();
    }
    if (!zoom.changed)
        return;
    zoom.changed = false;
    const u32 halves = static_cast<u32>(zoom.lens.zoom * 2 + .5f), tens = static_cast<u32>(zoom.lens.size / 10 + .5f);
    application_preferences_image_zoom(static_cast<u8>(maximum(2u, minimum(100u, halves))),
                                       static_cast<u8>(maximum(5u, minimum(100u, tens))));
}
} // namespace maicord::app_detail
