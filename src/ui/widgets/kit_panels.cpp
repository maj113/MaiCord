#include "maicord/ui/widgets/kit.hpp"

namespace maicord {
namespace {
UiRect centered(const UiRect& r, float width, float height) noexcept {
    const float cx = (r.left + r.right) * .5f, cy = (r.top + r.bottom) * .5f;
    return {cx - width * .5f, cy - height * .5f, cx + width * .5f, cy + height * .5f};
}
UiRect rect_at(float left, float top, float width, float height) noexcept {
    return {left, top, left + width, top + height};
}
constexpr float kTile = 28, kRowMinimum = 48, kRowPad = 10,
                kHostGap = 8, kDotSize = 6, kReset = 24, kKnobPad = 12, kKnobBesideWidth = 640, kSliderHeight = 24,
                kTick = 12, kTickWidth = 2;
} // namespace

// ---------------------------------------------------------------------------
Color ui_risk_color(const UiSettingsTokens& t, UiRisk risk) noexcept {
    return risk == UiRisk::low ? t.color.risk_low : risk == UiRisk::medium ? t.color.risk_medium : t.color.risk_high;
}
UiPermissionGeometry ui_permission_geometry(const UiSettingsLayoutTokens& t, const UiPermissionRow& row, float left,
                                            float top, float width, u32 host_chip_rows,
                                            const UiKitMeasure* measure) noexcept {
    const auto& m = t.metric;
    UiPermissionGeometry g{};
    const float inset = m.card_inset, text_left = left + inset + kTile + m.lead_gap, right = left + width - inset;
    const float name_width = row.name.bytes ? ui_kit_text_width(measure, row.name, kUiKitTextMono) : 0;
    const float sentence_right = right - (name_width > 0 ? name_width + m.lead_gap : 0);
    const float sentence_height = ui_kit_text_height(measure, row.sentence, kUiKitTextBody, maximum(1.f, sentence_right - text_left));
    const float content = maximum(kTile, sentence_height);
    const float rows = static_cast<float>(host_chip_rows);
    const float hosts = host_chip_rows ? rows * kUiKitChipHeight + (rows - 1) * kHostGap : 0;
    g.height = maximum(kRowMinimum, kRowPad + content + (host_chip_rows ? kHostGap + hosts + m.notice_padding : kRowPad));
    g.row = rect_at(left, top, width, g.height);
    const float middle = top + kRowPad + content * .5f;
    g.tile = rect_at(left + inset, middle - kTile * .5f, kTile, kTile);
    g.sentence = {text_left, middle - sentence_height * .5f, maximum(text_left + 1, sentence_right), middle + sentence_height * .5f};
    if (name_width > 0)
        g.name = {right - name_width, middle - ui_kit_line_height(kUiKitTextMono) * .5f, right,
                  middle + ui_kit_line_height(kUiKitTextMono) * .5f};
    if (host_chip_rows)
        g.hosts = {text_left, top + kRowPad + content + kHostGap, right, top + kRowPad + content + kHostGap + hosts};
    return g;
}
bool ui_permission_paint(PaintStream* paint, const UiSettingsTokens& t, const UiPermissionGeometry& g, UiIcon glyph,
                         bool last_in_group) noexcept {
    return ui_glyph_tile_paint(paint, t, g.tile, glyph, UiTileTone::neutral) &&
           (last_in_group || ui_card_hairline_paint(paint, t, g.row));
}

// ---------------------------------------------------------------------------
UiKnobGeometry ui_knob_geometry(const UiSettingsLayoutTokens& t, const UiKnob& knob, float left, float top, float width,
                                float segments_width, const UiKitMeasure* measure) noexcept {
    const auto& m = t.metric;
    UiKnobGeometry g{};
    const float inset = m.card_inset, line = ui_kit_line_height(kUiKitTextBody);
    const float inner_left = left + inset, inner_right = left + width - inset;
    const bool trailing = knob.kind == UiKnobKind::toggle || knob.kind == UiKnobKind::color || knob.kind == UiKnobKind::dropdown;
    const float trailing_width = knob.kind == UiKnobKind::toggle ? m.switch_width
                                 : knob.kind == UiKnobKind::color ? kReset
                                                                  : m.select_width;
    const float trailing_height = knob.kind == UiKnobKind::toggle ? m.switch_height
                                  : knob.kind == UiKnobKind::color ? kReset
                                                                   : m.control_height;
    float y = top + kKnobPad;
    float end = inner_right;
    if (trailing) {
        g.control = {end - trailing_width, y, end, y + trailing_height};
        end = g.control.left - m.field_gap;
    }
    const float row = trailing ? trailing_height : line;
    if (knob.overridden) {
        g.reset = rect_at(end - kReset, y + (row - kReset) * .5f, kReset, kReset);
        end = g.reset.left - m.field_gap;
    }
    const float value_width = knob.value_text.bytes ? ui_kit_text_width(measure, knob.value_text, kUiKitTextSmallBold) : 0;
    if (value_width > 0) {
        g.value = {end - value_width, y + (row - line) * .5f, end, y + (row + line) * .5f};
        end = g.value.left - m.field_gap;
    }
    g.label = {inner_left, y + (row - line) * .5f, maximum(inner_left + 1, end), y + (row + line) * .5f};
    if (knob.overridden) {
        const float label_width = minimum(ui_kit_text_width(measure, knob.label, kUiKitTextBody), ui_rect_width(g.label));
        g.dot = rect_at(g.label.left + label_width + 8, (g.label.top + g.label.bottom) * .5f - kDotSize * .5f, kDotSize, kDotSize);
    }
    y += row;
    if (!trailing) {
        y += m.field_gap;
        g.control = knob.kind == UiKnobKind::segments ? rect_at(inner_left, y, minimum(segments_width, inner_right - inner_left), m.button_small)
                                                      : rect_at(inner_left, y, inner_right - inner_left, kSliderHeight);
        y = g.control.bottom;
    }
    if (knob.hint.bytes) {
        const float hint = ui_kit_text_height(measure, knob.hint, kUiKitTextSmall, inner_right - inner_left);
        g.hint = {inner_left, y + m.label_gap * 2, inner_right, y + m.label_gap * 2 + hint};
        y = g.hint.bottom;
    }
    g.height = y + kKnobPad - top;
    g.row = rect_at(left, top, width, g.height);
    return g;
}
bool ui_knob_paint(PaintStream* paint, const UiSettingsTokens& t, const UiKnob& knob, const UiKnobGeometry& g,
                   UiControlState control, UiControlState reset, u32 hovered_option) noexcept {
    const auto& c = t.color;
    control.enabled = control.enabled && knob.enabled;
    bool ok = true;
    if (knob.overridden) {
        ok = paint_stream_push_rounded(paint, g.dot, c.brand, kDotSize * .5f) &&
             paint_stream_push_rounded(paint, g.reset, reset.pressed ? c.mod_strong : reset.hovered ? c.mod_normal : c.mod_subtle,
                                       t.metric.control_radius * .75f) &&
             ui_icon_paint(paint, UiIcon::reset, centered(g.reset, 16, 16), reset.hovered ? c.text_strong : c.text_subtle);
        if (ok && reset.focused)
            ok = ui_settings_focus_paint(paint, t, g.reset, t.metric.control_radius * .75f);
    }
    if (!ok)
        return false;
    switch (knob.kind) {
    case UiKnobKind::slider: {
        const auto slider = ui_slider_geometry(t, g.control, knob.count, knob.index, false);
        if (knob.default_index < knob.count) {
            // A 2 x 12 tick marks the default under the grabber.
            const float x = ui_slider_position(slider, knob.count, knob.default_index), middle = (slider.bar.top + slider.bar.bottom) * .5f;
            ok = paint_stream_push_rounded(paint, x - kTickWidth * .5f, middle - kTick * .5f, x + kTickWidth * .5f, middle + kTick * .5f,
                                           ui_settings_color_alpha(c.text_subtle, .6f), kTickWidth * .5f);
        }
        return ok && ui_slider_paint(paint, t, slider, knob.count, ~0u, control);
    }
    case UiKnobKind::toggle:
        return ui_switch_paint(paint, t, g.control, knob.checked, 1, true, control);
    case UiKnobKind::color:
        return paint_stream_push_rounded_border(paint, g.control, knob.swatch, c.border_normal, t.metric.control_radius * .75f, 1) &&
               (!control.focused || ui_settings_focus_paint(paint, t, g.control, t.metric.control_radius * .75f));
    case UiKnobKind::segments:
        return ui_segmented_paint(paint, t, g.control, knob.count, knob.index, hovered_option, control);
    case UiKnobKind::dropdown:
        return ui_select_paint(paint, t, g.control, false, control);
    }
    return ok;
}
bool ui_knob_beside(float width) noexcept {
    return width >= kKnobBesideWidth;
}
float ui_knob_column_width(const UiSettingsLayoutTokens& t, float width) noexcept {
    return ui_knob_beside(width) ? (width - t.metric.card_gap) * .5f : width;
}

} // namespace maicord
