#include "image_zoom.hpp"
#include "image_viewer.hpp"
#include "../settings/settings.hpp"
#include "../shell/input.hpp"
#include "../shell/overlay_input.hpp"
#include "../shell/panel_cover.hpp"
#include "../shell/state.hpp"
#include "../../render/d3d11/renderer_interaction_state.hpp"
#include "maicord/platform/window/key.hpp"

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
// The chat picture at `point` is the topmost thing there: no page, modal, menu, popout or viewer hides the
// timeline (the surfaces that hide its inline scenes), and no docked panel, banner, jump pill or hover toolbar
// sits over the point. The toolbar's published occluders are the banner's own.
bool timeline_front(UiPoint point) noexcept {
    const auto& renderer = g_app.renderer;
    if (!application_panel_uncovered() || !renderer.zoom_lens)
        return false;
    const u32 shown = d3d11_renderer_surfaces(renderer);
    if (!d3d11_renderer_zoom_lens_allowed(renderer, shown))
        return false;
    for (const UiRect& occluder : g_app.message_action_bar.occluders)
        if (ui_rect_contains(occluder, point.x, point.y))
            return false;
    if (message_action_bar_hit_test(g_app.message_action_bar, point.x, point.y).inside)
        return false;
    if ((shown & d3d11_surface_search) && ui_rect_contains(renderer.search_scene->panel, point.x, point.y))
        return false;
    if (shown & d3d11_surface_pinned_messages) {
        const auto& pins = *renderer.pinned_messages_scene;
        if (pins.overlay || ui_rect_contains(pins.panel, point.x, point.y))
            return false;
    }
    return !((shown & d3d11_surface_inbox) &&
             ui_rect_contains(renderer.inbox_scene->panel, point.x, point.y));
}
// A hovered chat preview that would open in the viewer; spoilers and
// obscured media stay unrevealed, videos never qualify.
void refresh_chat(UiPoint point) noexcept {
    place(g_app.preferences.image_zoom && ctrl_only() && timeline_front(point) &&
              application_image_viewer_open_at(point.x, point.y, true),
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

bool application_image_zoom_message(const WindowEvent& event) noexcept {
    if (!g_app.preferences.image_zoom)
        return false;
    auto& zoom = g_app.image_zoom;
    const bool keyboard = event.type == EventType::key_down || event.type == EventType::key_up;
    const u32 code = keyboard ? event.key.key : 0;
    // Ctrl is read at key and focus events, so a mouse move costs a load.
    if ((keyboard && (code == key::ctrl || code == key::alt)) ||
        (event.type == EventType::focus_changed && event.focus.gained) ||
        (event.type == EventType::activation_changed && event.focus.gained))
        zoom.ctrl = application_control_down();
    // The viewer keeps its own lens.
    if (g_app.image_viewer.scene.active)
        return false;
    switch (event.type) {
    case EventType::pointer_move:
        if (zoom.lens.active || zoom.ctrl)
            refresh_chat({client_dip_x(event), client_dip_y(event)});
        return false;
    case EventType::key_down:
    case EventType::key_up:
        if (code == key::ctrl || code == key::alt) {
            UiPoint point{};
            if (application_pointer_dip(&point))
                refresh_chat(point);
        }
        return false;
    case EventType::wheel:
        return input_vertical_wheel(event) && application_image_zoom_wheel(event.wheel.dy120, input_shift(event));
    case EventType::pointer_down:
        if (input_press(event, PointerButton::left) || input_press(event, PointerButton::right))
            application_image_zoom_end();
        return false;
    case EventType::pointer_leave:
    case EventType::cancel_input:
        application_image_zoom_end();
        return false;
    case EventType::focus_changed:
        if (!event.focus.gained) {
            zoom.ctrl = false;
            application_image_zoom_end();
        }
        return false;
    case EventType::activation_changed:
        if (!event.focus.gained) {
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

bool application_image_zoom_wheel(i32 delta120, bool shift) noexcept {
    auto& zoom = g_app.image_zoom;
    if (!zoom.lens.active || !g_app.renderer.zoom_lens_source.ready)
        return false;
    if (ui_zoom_lens_wheel(&zoom.lens, static_cast<short>(delta120), shift)) {
        zoom.changed = true;
        application_request_frame();
    }
    return true;
}

void application_image_zoom_synchronize() noexcept {
    // A modal, menu or panel that opened under a still pointer ends the lens; the viewer owns its own.
    const auto& shown = g_app.image_zoom.lens;
    if (shown.active && !g_app.image_viewer.scene.active && !timeline_front(shown.pointer))
        application_image_zoom_end();
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
