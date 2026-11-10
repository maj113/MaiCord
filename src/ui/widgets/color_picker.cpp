#include "maicord/ui/widgets/color_picker.hpp"
#include "maicord/platform/window/key.hpp"

namespace maicord {
namespace {

float unit(float v) noexcept {
    return v != v ? 0 : maximum(0.f, minimum(v, 1.f));
}
// Fraction steps of the keys: 1 % (1 degree on the hue bar), Shift or Page Up/Down 10.
bool step(float* at, u32 code, bool shift, float small, bool reverse_vertical) noexcept {
    const float delta = shift ? small * 10 : small;
    switch (code) {
    case key::left: *at -= delta; break;
    case key::right: *at += delta; break;
    case key::up: *at += reverse_vertical ? -delta : delta; break;
    case key::down: *at += reverse_vertical ? delta : -delta; break;
    case key::page_up: *at += small * 10; break;
    case key::page_down: *at -= small * 10; break;
    case key::home: *at = 0; break;
    case key::end: *at = 1; break;
    default: return false;
    }
    *at = unit(*at);
    return true;
}
u32 per_row(float width) noexcept {
    const u32 n = static_cast<u32>((width + kColorPickerSwatchGap) / (kColorPickerSwatch + kColorPickerSwatchGap));
    return n ? n : 1;
}
// The thumb: a white ring with a dark outline, readable on any colour.
bool thumb(PaintStream* paint, const UiRect& r, const Theme& theme) noexcept {
    const float radius = ui_rect_width(r) * .5f;
    return paint_stream_push_rounded_border(paint, r, Color{}, theme.tokens.overlay.scrim, radius, 3) &&
           paint_stream_push_rounded_border(paint, {r.left + 1, r.top + 1, r.right - 1, r.bottom - 1}, Color{},
                                            color_from_rgb24(0xFFFFFF), radius - 1, 2);
}
bool focus(PaintStream* paint, const UiRect& r, const Theme& theme, float radius) noexcept {
    const float ring = theme.scale.space_1;
    return paint_stream_push_rounded_border(paint, {r.left - ring, r.top - ring, r.right + ring, r.bottom + ring},
                                            Color{}, theme.tokens.border.focus, radius + ring, 2);
}

} // namespace

u32 color_picker_rgb(ColorPickerHsv hsv) noexcept {
    const float h = unit(hsv.hue), s = unit(hsv.saturation), v = unit(hsv.value);
    const float sector = (h == 1 ? 0 : h) * 6;
    const u32 i = minimum(static_cast<u32>(sector), 5u);
    const float f = sector - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    // Each sector's red, green and blue among these four.
    const float values[4]{v, t, p, q};
    static constexpr u8 kOrder[6][3]{{0, 1, 2}, {3, 0, 2}, {2, 0, 1}, {2, 3, 0}, {1, 2, 0}, {0, 2, 3}};
    u32 rgb = 0;
    for (const u8 channel : kOrder[i])
        rgb = rgb << 8 | static_cast<u32>(values[channel] * 255 + .5f);
    return rgb;
}

ColorPickerHsv color_picker_hsv(u32 rgb, ColorPickerHsv previous) noexcept {
    const float r = (rgb >> 16 & 0xFF) / 255.f, g = (rgb >> 8 & 0xFF) / 255.f, b = (rgb & 0xFF) / 255.f;
    const float high = maximum(r, maximum(g, b)), low = minimum(r, minimum(g, b)), d = high - low;
    ColorPickerHsv out{unit(previous.hue), high ? d / high : 0, high};
    if (d) {
        float h = (high == r ? (g - b) / d : high == g ? (b - r) / d + 2 : (r - g) / d + 4) / 6;
        out.hue = h < 0 ? h + 1 : h;
    }
    return out;
}

ColorPickerHsv color_picker_field_at(ColorPickerHsv hsv, const UiRect& field, float x, float y) noexcept {
    if (ui_rect_has_area(field)) {
        hsv.saturation = unit((x - field.left) / ui_rect_width(field));
        hsv.value = 1 - unit((y - field.top) / ui_rect_height(field));
    }
    return hsv;
}

ColorPickerHsv color_picker_hue_at(ColorPickerHsv hsv, const UiRect& bar, float x) noexcept {
    if (ui_rect_has_area(bar))
        hsv.hue = unit((x - bar.left) / ui_rect_width(bar));
    return hsv;
}

bool color_picker_field_key(ColorPickerHsv* hsv, u32 code, bool shift) noexcept {
    if (code == key::up || code == key::down)
        return step(&hsv->value, code, shift, .01f, false);
    return step(&hsv->saturation, code, shift, .01f, false);
}

bool color_picker_hue_key(ColorPickerHsv* hsv, u32 code, bool shift) noexcept {
    return step(&hsv->hue, code, shift, 1 / 360.f, false);
}

float color_picker_swatches_height(float width, u32 count) noexcept {
    if (!count)
        return 0;
    const u32 rows = (count + per_row(width) - 1) / per_row(width);
    return rows * kColorPickerSwatch + (rows - 1) * kColorPickerSwatchGap;
}

UiRect color_picker_swatch_box(const UiRect& row, u32 count, u32 index) noexcept {
    if (index >= count)
        return {};
    const u32 columns = per_row(ui_rect_width(row));
    const u32 line = index / columns, first = line * columns;
    const u32 in_line = minimum(columns, count - first);
    const float width = in_line * kColorPickerSwatch + (in_line - 1) * kColorPickerSwatchGap;
    const float left = row.left + (ui_rect_width(row) - width) * .5f +
                       (index - first) * (kColorPickerSwatch + kColorPickerSwatchGap);
    const float top = row.top + line * (kColorPickerSwatch + kColorPickerSwatchGap);
    return {left, top, left + kColorPickerSwatch, top + kColorPickerSwatch};
}

u32 color_picker_swatch_at(const UiRect& row, u32 count, float x, float y) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (ui_rect_contains(color_picker_swatch_box(row, count, i), x, y))
            return i;
    return ~0u;
}

bool color_picker_swatch_key(const UiRect& row, u32 count, u32* focused, u32 code) noexcept {
    if (!count)
        return false;
    const i32 columns = static_cast<i32>(per_row(ui_rect_width(row)));
    const i32 at = *focused < count ? static_cast<i32>(*focused) : 0;
    const i32 next = code == key::left    ? at - 1
                     : code == key::right ? at + 1
                     : code == key::up    ? at - columns
                     : code == key::down  ? at + columns
                     : code == key::home  ? 0
                     : code == key::end   ? static_cast<i32>(count) - 1
                                          : -2;
    if (next == -2 || next < 0 || next >= static_cast<i32>(count) || next == static_cast<i32>(*focused))
        return false;
    *focused = static_cast<u32>(next);
    return true;
}

bool color_picker_paint_field(PaintStream* paint, const UiRect& field, ColorPickerHsv hsv, const Theme& theme,
                              bool focus_ring) noexcept {
    const float radius = theme.scale.radius_sm, side = kColorPickerHueHeight + 2;
    const float x = field.left + unit(hsv.saturation) * ui_rect_width(field);
    const float y = field.top + (1 - unit(hsv.value)) * ui_rect_height(field);
    return paint_stream_push_sv_square(paint, field, color_from_rgb24(color_picker_rgb({hsv.hue, 1, 1})), radius) &&
           thumb(paint, {x - side * .5f, y - side * .5f, x + side * .5f, y + side * .5f}, theme) &&
           (!focus_ring || focus(paint, field, theme, radius));
}

bool color_picker_paint_hue(PaintStream* paint, const UiRect& bar, ColorPickerHsv hsv, const Theme& theme,
                            bool focus_ring) noexcept {
    const float radius = ui_rect_height(bar) * .5f, side = ui_rect_height(bar) + 4;
    const float x =
        maximum(bar.left + radius, minimum(bar.left + unit(hsv.hue) * ui_rect_width(bar), bar.right - radius));
    const float y = (bar.top + bar.bottom) * .5f;
    return paint_stream_push_hue_strip(paint, bar, radius) &&
           thumb(paint, {x - side * .5f, y - side * .5f, x + side * .5f, y + side * .5f}, theme) &&
           (!focus_ring || focus(paint, bar, theme, radius));
}

bool color_picker_paint_swatches(PaintStream* paint, const UiRect& row, ConstSlice<u32> colors, u32 rgb,
                                 u32 focused, const Theme& theme, bool focus_ring) noexcept {
    const float radius = theme.scale.radius_sm;
    bool valid = true;
    for (u32 i = 0; valid && i < colors.count; ++i) {
        const auto box = color_picker_swatch_box(row, colors.count, i);
        const bool chosen = colors[i] == rgb;
        valid = paint_stream_push_rounded_border(paint, box, color_from_rgb24(colors[i]),
                                                 chosen ? theme.text_primary : theme.tokens.border.strong, radius,
                                                 chosen ? 2.f : 1.f) &&
                (!focus_ring || i != focused || focus(paint, box, theme, radius));
    }
    return valid;
}

} // namespace maicord
