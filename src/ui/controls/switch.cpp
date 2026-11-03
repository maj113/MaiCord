#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"

namespace maicord {
namespace {
// Thumb keyframes inside the 24-unit thumb box while turning on; turning off
// mirrors x. Stops are normalized animation time.
constexpr float kStops[]{0, .1f, .2f, .5f, .7f, 1};
constexpr float kX[]{4, 4, 2.5f, 1, 2.5f, 4};
constexpr float kY[]{4, 7, 2.5f, 7, 2.5f, 4};
constexpr float kW[]{16, 16, 19, 22, 19, 16};
constexpr float kH[]{16, 10, 19, 10, 19, 16};
constexpr float kR[]{8, 5, 9.5f, 5, 9.5f, 8};

float keyframe(const float (&values)[6], float t) noexcept {
    for (u32 i = 1; i < 6; ++i)
        if (t <= kStops[i]) {
            const float local = (t - kStops[i - 1]) / (kStops[i] - kStops[i - 1]);
            return values[i - 1] + (values[i] - values[i - 1]) * local;
        }
    return values[5];
}
// How far a switch has turned on: its position moves between 40% and 60% of the morph; reduced motion slides.
float on_fraction(float t, bool reduced_motion, bool checked) noexcept {
    const float slide = reduced_motion ? t : clamp_unit((t - .4f) / .2f);
    return checked ? slide : 1 - slide;
}
struct Thumb {
    UiRect box;
    float radius;
};
Thumb thumb_geometry(const UiSettingsLayoutTokens& tokens, const UiRect& track, bool checked, float t, float position,
                     bool reduced_motion, bool pressed) noexcept {
    const float unit = ui_rect_height(track) / 24;
    const float travel = ui_rect_width(track) - ui_rect_height(track);
    float x = 4, y = 4, w = 16, h = 16, r = 8;
    if (!reduced_motion && t < 1) {
        x = keyframe(kX, t), y = keyframe(kY, t), w = keyframe(kW, t), h = keyframe(kH, t),
        r = keyframe(kR, t);
        if (!checked)
            x = 24 - x - w;
    } else if (pressed && !reduced_motion) {
        y = kY[1], h = kH[1], r = kR[1];
    }
    const float scale = tokens.metric.switch_thumb / 16;
    const float cx = track.left + position * travel + 12 * unit, cy = track.top + 12 * unit;
    const float left = cx + (x - 12) * unit * scale, top = cy + (y - 12) * unit * scale;
    return {{left, top, left + w * unit * scale, top + h * unit * scale}, r * unit * scale};
}
} // namespace

bool ui_switch_paint(PaintStream* paint, const UiSettingsTokens& tokens, const UiRect& track, bool checked,
                     float motion, bool reduced_motion, UiControlState state) noexcept {
    const auto& c = tokens.color;
    const float t = clamp_unit(motion), on = on_fraction(t, reduced_motion, checked);
    const Color on_fill = state.pressed ? c.brand_active : state.hovered ? c.brand_hover : c.brand;
    Color fill = ui_settings_color_mix(c.switch_track, on_fill, on);
    Color border = ui_settings_color_mix(state.hovered ? c.switch_border_hover : c.switch_border,
                                         c.switch_border_on, on);
    Color thumb = c.thumb;
    if (!state.enabled) {
        fill = ui_settings_color_alpha(fill, tokens.disabled_alpha);
        border = ui_settings_color_alpha(border, tokens.disabled_alpha);
        thumb = ui_settings_color_alpha(thumb, tokens.disabled_alpha);
    }
    const float radius = ui_rect_height(track) * .5f;
    const auto knob = thumb_geometry(tokens, track, checked, t, on, reduced_motion, state.pressed && state.enabled);
    bool ok = paint_stream_push_rounded_border(paint, track, fill, border, radius, 1) &&
              paint_stream_push_rounded(paint, knob.box, thumb, knob.radius);
    if (ok && state.focused)
        ok = ui_settings_focus_paint(paint, tokens, track, radius);
    return ok;
}

} // namespace maicord
