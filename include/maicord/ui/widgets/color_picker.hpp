#pragma once

#include "maicord/ui/core/paint.hpp"

namespace maicord {

// The colour picker every colour field shares (the text form's colour mode hosts it, see
// app/chat/composer/text_form.hpp application_color_picker_open): a saturation/value square, a hue bar and
// suggested swatches, as in the official client's custom colour popout (220 DIP wide, 16 DIP padding, 16 DIP gaps,
// 32 DIP suggested swatches 12 apart). Hue, saturation and value are fractions in [0, 1].
struct ColorPickerHsv {
    float hue, saturation, value;
};
inline constexpr u32 kColorPickerSuggestedColors = 8;
inline constexpr float kColorPickerFieldHeight = 150;
inline constexpr float kColorPickerHueHeight = 12;
inline constexpr float kColorPickerSwatch = 32;
inline constexpr float kColorPickerSwatchGap = 12;

u32 color_picker_rgb(ColorPickerHsv hsv) noexcept;
// 0xRRGGBB as HSV. A grey keeps `previous`'s hue, so dragging through greys never makes the hue bar jump.
ColorPickerHsv color_picker_hsv(u32 rgb, ColorPickerHsv previous) noexcept;
// The colour at a point of the saturation/value square or the hue bar (clamped to them).
ColorPickerHsv color_picker_field_at(ColorPickerHsv hsv, const UiRect& field, float x, float y) noexcept;
ColorPickerHsv color_picker_hue_at(ColorPickerHsv hsv, const UiRect& bar, float x) noexcept;
// Keys (platform/window/key.hpp codes): in the square Left/Right change saturation and Up/Down value, on the bar
// Left/Right (and Up/Down) the hue; 1 % or 1 degree a press, 10 with Shift; Home/End go to the ends, Page Up/Down
// step 10. False for any other key.
bool color_picker_field_key(ColorPickerHsv* hsv, u32 key, bool shift) noexcept;
bool color_picker_hue_key(ColorPickerHsv* hsv, u32 key, bool shift) noexcept;

// Suggested swatches: rows of kColorPickerSwatch squares centred in `row`'s width.
float color_picker_swatches_height(float width, u32 count) noexcept;
UiRect color_picker_swatch_box(const UiRect& row, u32 count, u32 index) noexcept;
// The swatch under a point; ~0u for none.
u32 color_picker_swatch_at(const UiRect& row, u32 count, float x, float y) noexcept;
// Arrow keys move `focused` through the swatches (Up/Down by a row); false for other keys or no move.
bool color_picker_swatch_key(const UiRect& row, u32 count, u32* focused, u32 key) noexcept;

// The square (pure hue at top right) with a ring at the colour, the hue bar with its thumb, and the swatches with
// the one matching `rgb` outlined. `focus_ring` draws the keyboard focus outline (and, for swatches, at `focused`).
bool color_picker_paint_field(PaintStream* paint, const UiRect& field, ColorPickerHsv hsv, const Theme& theme,
                              bool focus_ring) noexcept;
bool color_picker_paint_hue(PaintStream* paint, const UiRect& bar, ColorPickerHsv hsv, const Theme& theme,
                            bool focus_ring) noexcept;
bool color_picker_paint_swatches(PaintStream* paint, const UiRect& row, ConstSlice<u32> colors, u32 rgb,
                                 u32 focused, const Theme& theme, bool focus_ring) noexcept;

} // namespace maicord
