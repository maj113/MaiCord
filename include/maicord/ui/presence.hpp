#pragma once
#include "maicord/protocol/client_status.hpp"
#include "maicord/ui/paint.hpp"
namespace maicord {
enum class UiPresence : u8 {
    unknown,
    online,
    idle,
    busy,
    offline
};

// The phone-shaped status: online on
// mobile only. Used in place of the online dot for other users.
constexpr bool ui_presence_mobile(UiPresence status, u8 client_status) noexcept {
    return status == UiPresence::online && client_status_mobile_online(client_status);
}

// Geometry of the phone for a status dot of extent s (its square box): the
// box keeps the dot's width and bottom-right corner and is 1.5s tall. The
// body's corners are .1875s; the screen is .75s square at (.125s, .25s); the
// home button is a circle of radius .125s centred at (.5s, 1.25s). The avatar
// cutout grows the body by the ring width k with 0.2*(1.5s + 2k) corners.
struct UiMobilePresenceGeometry {
    UiRect cutout;
    float cutout_radius;
    UiRect body;
    float body_radius;
    UiRect screen;
    UiRect button;
};
inline UiMobilePresenceGeometry ui_presence_mobile_geometry(UiRect dot, float ring) noexcept {
    const float s = minimum(ui_rect_width(dot), ui_rect_height(dot));
    UiMobilePresenceGeometry g{};
    g.body = {dot.right - s, dot.bottom - 1.5f * s, dot.right, dot.bottom};
    g.body_radius = .1875f * s;
    g.cutout = {g.body.left - ring, g.body.top - ring, g.body.right + ring, g.body.bottom + ring};
    g.cutout_radius = .2f * (1.5f * s + 2.f * ring);
    g.screen = {g.body.left + .125f * s, g.body.top + .25f * s, g.body.left + .875f * s, g.body.top + s};
    const float cx = g.body.left + .5f * s, cy = g.body.top + 1.25f * s, r = .125f * s;
    g.button = {cx - r, cy - r, cx + r, cy + r};
    return g;
}

inline bool ui_presence_paint(PaintStream* paint, UiRect r, UiPresence status, Color background,
                              const Theme& theme, bool mobile = false) noexcept {
    const float extent = minimum(ui_rect_width(r), ui_rect_height(r)), radius = extent * .5f;
    const auto& tokens = theme.tokens.status;
    if (mobile && status == UiPresence::online) {
        const auto g = ui_presence_mobile_geometry(r, 2.f);
        const float button = ui_rect_width(g.button) * .5f;
        return paint_stream_push_rounded(paint, g.cutout.left, g.cutout.top, g.cutout.right, g.cutout.bottom,
                                         background, g.cutout_radius) &&
               paint_stream_push_rounded(paint, g.body.left, g.body.top, g.body.right, g.body.bottom, tokens.online,
                                         g.body_radius) &&
               paint_stream_push(paint, g.screen.left, g.screen.top, g.screen.right, g.screen.bottom, background) &&
               paint_stream_push_rounded(paint, g.button.left, g.button.top, g.button.right, g.button.bottom,
                                         background, button);
    }
    const Color color = status == UiPresence::online ? tokens.online
                        : status == UiPresence::idle ? tokens.idle
                        : status == UiPresence::busy ? tokens.dnd
                                                     : tokens.offline;
    if (!paint_stream_push_rounded_border(paint, r.left - 2, r.top - 2, r.right + 2, r.bottom + 2, color,
                                          background, radius + 2, 2))
        return false;
    if (status == UiPresence::busy)
        return paint_stream_push_line(paint, {r.left + extent * .25f, r.top + radius},
                                      {r.right - extent * .25f, r.top + radius},
                                      maximum(1.f, extent * .17f), background);
    if (status == UiPresence::idle)
        return paint_stream_push_rounded(paint, r.left - extent * .05f, r.top - extent * .05f,
                                         r.left + extent * .65f, r.top + extent * .65f, background,
                                         extent * .35f);
    if (status == UiPresence::offline || status == UiPresence::unknown)
        return paint_stream_push_rounded(paint, r.left + extent * .25f, r.top + extent * .25f,
                                         r.right - extent * .25f, r.bottom - extent * .25f, background,
                                         extent * .25f);
    return true;
}
} // namespace maicord
