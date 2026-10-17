#include "maicord/ui/controls/settings_controls.hpp"

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
float clamp01(float value) noexcept {
    return value < 0 ? 0 : value > 1 ? 1 : value;
}
} // namespace

UiSwitchThumb ui_switch_thumb(const UiSettingsTokens& tokens, UiRect track, bool checked, float motion,
                              bool reduced_motion, bool pressed) noexcept {
    const float unit = ui_rect_height(track) / 24;
    const float travel = ui_rect_width(track) - ui_rect_height(track);
    const float t = clamp01(motion);
    // Position moves between 40% and 60% of the morph; reduced motion slides.
    const float slide = reduced_motion ? t : clamp01((t - .4f) / .2f);
    const float position = checked ? slide : 1 - slide;
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

bool ui_switch_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect track, bool checked,
                     float motion, bool reduced_motion, UiControlState state) noexcept {
    const auto& c = tokens.color;
    const float t = clamp01(motion);
    const float slide = reduced_motion ? t : clamp01((t - .4f) / .2f);
    const float on = checked ? slide : 1 - slide;
    const Color on_fill = state.pressed ? c.brand_active : state.hovered ? c.brand_hover : c.brand;
    Color fill = ui_settings_color_mix(c.switch_track, on_fill, on);
    Color border = ui_settings_color_mix(state.hovered ? c.switch_border_hover : c.switch_border,
                                         c.switch_border_on, on);
    Color thumb = c.thumb;
    if (!state.enabled) {
        fill = ui_settings_color_alpha(fill, .5f);
        border = ui_settings_color_alpha(border, .5f);
        thumb = ui_settings_color_alpha(thumb, .5f);
    }
    const float radius = ui_rect_height(track) * .5f;
    const auto knob = ui_switch_thumb(tokens, track, checked, motion, reduced_motion,
                                      state.pressed && state.enabled);
    bool ok = paint_stream_push_rounded_border(paint, track.left, track.top, track.right, track.bottom, fill,
                                               border, radius, 1) &&
              paint_stream_push_rounded(paint, knob.box.left, knob.box.top, knob.box.right, knob.box.bottom,
                                        thumb, knob.radius);
    if (ok && state.focused)
        ok = paint_stream_push_rounded_border(paint, track.left - 3, track.top - 3, track.right + 3,
                                              track.bottom + 3, {}, c.focus_ring, radius + 3, 2);
    return ok;
}

} // namespace maicord
