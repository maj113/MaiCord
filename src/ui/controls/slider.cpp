#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"

namespace maicord {
namespace {
constexpr float kTrackInset = 5;
// A hovered, dragged or focused slider grows its grabber and bar about their
// centres; hit-testing keeps the resting geometry.
UiSliderGeometry active(const UiSettingsTokens& tokens, UiSliderGeometry g, UiControlState state) noexcept {
    if (!state.enabled || !(state.hovered || state.pressed || state.focused))
        return g;
    const float grow = tokens.metric.slider_grabber_grow * .5f, thicken = tokens.metric.slider_bar_grow * .5f;
    g.grabber = {g.grabber.left - grow, g.grabber.top - grow, g.grabber.right + grow, g.grabber.bottom + grow};
    g.bar.top -= thicken, g.bar.bottom += thicken;
    g.fill.top = g.bar.top, g.fill.bottom = g.bar.bottom;
    return g;
}
} // namespace

float ui_slider_height(const UiSettingsTokens& tokens, bool marks) noexcept {
    return 24 + (marks ? tokens.metric.slider_mark_height : 0);
}

float ui_slider_position(const UiSliderGeometry& geometry, u32 count, u32 index) noexcept {
    if (count < 2)
        return geometry.track_left;
    const float fraction = static_cast<float>(minimum(index, count - 1)) / static_cast<float>(count - 1);
    return geometry.track_left + (geometry.track_right - geometry.track_left) * fraction;
}

UiSliderGeometry ui_slider_geometry(const UiSettingsTokens& tokens, UiRect bounds, u32 count, u32 index,
                                    bool marks) noexcept {
    UiSliderGeometry g{};
    g.bounds = bounds;
    g.marks = marks;
    const float grabber = tokens.metric.slider_grabber, bar = tokens.metric.slider_bar;
    g.track_left = bounds.left + grabber * .5f;
    g.track_right = bounds.right - grabber * .5f;
    const float middle = bounds.top + (marks ? tokens.metric.slider_mark_height : 0) + 12;
    g.bar = {bounds.left + kTrackInset, middle - bar * .5f, bounds.right - kTrackInset, middle + bar * .5f};
    const float x = ui_slider_position(g, count, index);
    g.fill = {g.bar.left, g.bar.top, x, g.bar.bottom};
    g.grabber = {x - grabber * .5f, middle - grabber * .5f, x + grabber * .5f, middle + grabber * .5f};
    return g;
}

u32 ui_slider_index_at(const UiSliderGeometry& geometry, u32 count, float x) noexcept {
    if (count < 2 || !(geometry.track_right > geometry.track_left))
        return 0;
    float fraction = (x - geometry.track_left) / (geometry.track_right - geometry.track_left);
    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    return static_cast<u32>(fraction * static_cast<float>(count - 1) + .5f);
}

u32 ui_slider_page_step(u32 count) noexcept {
    const u32 range = count ? count - 1 : 0;
    return maximum(1u, (range + 5) / 10);
}

float ui_slider_grab_offset(UiRect grabber, float x, float y) noexcept {
    return ui_rect_contains(grabber, x, y) ? x - (grabber.left + grabber.right) * .5f : 0.f;
}

UiRect ui_slider_mark_label(const UiSettingsTokens& tokens, const UiSliderGeometry& geometry, u32 count,
                            u32 index) noexcept {
    const float x = ui_slider_position(geometry, count, index);
    const float half = 28;
    return {x - half, geometry.bounds.top, x + half, geometry.bounds.top + tokens.metric.slider_mark_height - 6};
}

bool ui_slider_paint(PaintStream* paint, const UiSettingsTokens& tokens, const UiSliderGeometry& resting, u32 count,
                     u32 default_index, UiControlState state) noexcept {
    const auto g = active(tokens, resting, state);
    const auto& c = tokens.color;
    const float alpha = state.enabled ? 1.f : tokens.disabled_alpha;
    const float bar_radius = ui_rect_height(g.bar) * .5f;
    bool ok = true;
    if (g.marks && count >= 2 && count <= 32) {
        const float top = g.bounds.top + tokens.metric.slider_mark_height - 2;
        const float bottom = g.bar.bottom + 6;
        for (u32 i = 0; ok && i < count; ++i) {
            const float x = ui_slider_position(g, count, i);
            const Color tick = i == default_index ? c.positive : c.slider_track;
            ok = paint_stream_push_rounded(paint, x - 1, top, x + 1, bottom, ui_settings_color_alpha(tick, alpha),
                                           1);
        }
    }
    const Color fill = c.brand;
    const float grab_radius = ui_rect_width(g.grabber) * .5f;
    ok = ok &&
         paint_stream_push_rounded(paint, g.bar.left, g.bar.top, g.bar.right, g.bar.bottom,
                                   ui_settings_color_alpha(c.slider_track, alpha), bar_radius) &&
         (ui_rect_width(g.fill) <= 0 ||
          paint_stream_push_rounded(paint, g.fill.left, g.fill.top, g.fill.right, g.fill.bottom,
                                    ui_settings_color_alpha(fill, alpha), bar_radius)) &&
         ui_settings_shadow_paint(paint, g.grabber, grab_radius, 2, .14f * alpha) &&
         paint_stream_push_rounded_border(paint, g.grabber.left, g.grabber.top, g.grabber.right, g.grabber.bottom,
                                          ui_settings_color_alpha(c.thumb, alpha),
                                          ui_settings_color_alpha(c.border_normal, alpha), grab_radius, 1);
    if (ok && state.focused)
        ok = ui_settings_focus_paint(paint, tokens, g.grabber, grab_radius);
    return ok;
}

bool ui_slider_meter_paint(PaintStream* paint, const UiSettingsTokens& tokens, const UiSliderGeometry& resting,
                           u32 count, u32 level, UiControlState state) noexcept {
    const auto g = active(tokens, resting, state);
    const auto& c = tokens.color;
    const float alpha = state.enabled ? 1.f : tokens.disabled_alpha;
    const float radius = ui_rect_height(g.bar) * .5f;
    const float thumb = (g.grabber.left + g.grabber.right) * .5f;
    const float lit = maximum(g.bar.left, minimum(g.bar.right, ui_slider_position(g, count, level)));
    const float quiet = minimum(lit, thumb);
    Color faint = c.positive;
    faint.alpha *= .35f * alpha;
    const float grab_radius = ui_rect_width(g.grabber) * .5f;
    bool ok = paint_stream_push_rounded(paint, g.bar.left, g.bar.top, g.bar.right, g.bar.bottom,
                                        ui_settings_color_alpha(c.slider_track, alpha), radius) &&
              (quiet <= g.bar.left ||
               paint_stream_push_rounded(paint, g.bar.left, g.bar.top, quiet, g.bar.bottom, faint, radius)) &&
              (lit <= thumb || paint_stream_push_rounded(paint, thumb, g.bar.top, lit, g.bar.bottom,
                                                         ui_settings_color_alpha(c.positive, alpha), radius)) &&
              ui_settings_shadow_paint(paint, g.grabber, grab_radius, 2, .14f * alpha) &&
              paint_stream_push_rounded_border(paint, g.grabber.left, g.grabber.top, g.grabber.right,
                                               g.grabber.bottom, ui_settings_color_alpha(c.thumb, alpha),
                                               ui_settings_color_alpha(c.border_normal, alpha), grab_radius, 1);
    if (ok && state.focused)
        ok = ui_settings_focus_paint(paint, tokens, g.grabber, grab_radius);
    return ok;
}

} // namespace maicord
