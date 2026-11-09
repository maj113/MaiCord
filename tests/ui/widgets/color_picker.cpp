#include "maicord/ui/widgets/color_picker.hpp"
#include "maicord/ui/text/form.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/core/memory.hpp"
#include "../../support/fake_text_measure.hpp"

namespace maicord {
namespace {
bool near(float a, float b) noexcept {
    return a - b < .002f && b - a < .002f;
}
} // namespace

void test_color_picker(void (*check)(bool, const char*) noexcept) noexcept {
    // HSV <-> RGB: primaries, the hue seam, and greys keep the hue they had.
    const auto blurple = color_picker_hsv(0x5865F2, {});
    check(color_picker_rgb(blurple) == 0x5865F2 && color_picker_rgb({0, 1, 1}) == 0xFF0000 &&
              color_picker_rgb({1, 1, 1}) == 0xFF0000 && color_picker_rgb({2.f / 3, 1, 1}) == 0x0000FF,
          "color picker: HSV and RGB round-trip and the hue seam wraps");
    const auto grey = color_picker_hsv(0x808080, {.4f, .9f, .1f});
    check(near(grey.hue, .4f) && grey.saturation == 0 && near(grey.value, 128 / 255.f) &&
              color_picker_hsv(0, {.25f, .5f, .5f}).hue == .25f,
          "color picker: a grey or black keeps the previous hue");
    check(color_picker_rgb({-1, 2, .5f}) == color_picker_rgb({0, 1, .5f}),
          "color picker: out-of-range values clamp");

    // Pointer mapping: the square's top right is pure hue, its bottom black; the bar maps left to right.
    const UiRect field{10, 20, 210, 170}, bar{10, 180, 210, 192};
    auto at = color_picker_field_at({.5f, 0, 0}, field, 210, 20);
    check(at.saturation == 1 && at.value == 1 && color_picker_rgb(at) == 0x00FFFF &&
              color_picker_field_at(at, field, -100, 400).saturation == 0 &&
              color_picker_field_at(at, field, -100, 400).value == 0,
          "color picker: the square maps saturation across and value down, clamped");
    check(near(color_picker_hue_at({}, bar, 110).hue, .5f) && color_picker_hue_at({}, bar, 999).hue == 1,
          "color picker: the hue bar maps its width to the hue");

    // Keys: 1 % (1 degree) a press, 10 with Shift; Home/End; other keys are not consumed.
    ColorPickerHsv hsv{.5f, .5f, .5f};
    check(color_picker_field_key(&hsv, key::right, false) && near(hsv.saturation, .51f) &&
              color_picker_field_key(&hsv, key::down, true) && near(hsv.value, .4f) &&
              color_picker_field_key(&hsv, key::end, false) && hsv.saturation == 1 &&
              !color_picker_field_key(&hsv, 'A', false),
          "color picker: arrow keys step saturation and value");
    check(color_picker_hue_key(&hsv, key::left, true) && near(hsv.hue, .5f - 10 / 360.f) &&
              color_picker_hue_key(&hsv, key::home, false) && hsv.hue == 0,
          "color picker: arrow keys step the hue");

    // Swatches: 32 DIP squares 12 apart, centred in rows; hit tests and arrow keys follow the rows.
    const UiRect row{0, 0, 188, 0};
    check(color_picker_swatches_height(188, 8) == 2 * kColorPickerSwatch + kColorPickerSwatchGap &&
              color_picker_swatches_height(188, 0) == 0,
          "color picker: eight suggestions wrap into two rows of four");
    const auto first = color_picker_swatch_box(row, 5, 0), last = color_picker_swatch_box(row, 5, 4);
    check(near(first.left, (188 - 4 * 32 - 3 * 12) * .5f) && near(last.left, (188 - 32) * .5f) &&
              last.top == 44 && color_picker_swatch_at(row, 5, last.left + 4, last.top + 4) == 4 &&
              color_picker_swatch_at(row, 5, first.right + 6, 10) == ~0u,
          "color picker: swatch boxes centre each row and hit test");
    u32 focused = 0;
    check(color_picker_swatch_key(row, 5, &focused, key::down) && focused == 4 &&
              !color_picker_swatch_key(row, 5, &focused, key::right) &&
              color_picker_swatch_key(row, 5, &focused, key::home) && focused == 0,
          "color picker: arrow keys move through the swatches");

    // Paint: square and hue bar use their shader modes; the thumb rings and a focus ring add instances.
    PaintStream paint{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000), "color picker: paint stream");
    const u32 before = paint.count;
    const u32 suggested[]{0xFF0000, 0x00FF00, 0x0000FF};
    check(color_picker_paint_field(&paint, field, blurple, kDefaultTheme, false) &&
              paint.quads[before].border_width == -4 &&
              color_picker_paint_hue(&paint, bar, blurple, kDefaultTheme, true) &&
              color_picker_paint_swatches(&paint, {10, 200, 210, 232}, {suggested, 3}, 0x00FF00, 1, kDefaultTheme,
                                          true) &&
              paint.count == before + 3 + 4 + 4 && !paint.rejected,
          "color picker: square, hue bar and swatches paint with rings");
    paint_stream_release(&paint);

    // The text form's picker mode: the square, hue bar, row, eyedropper and swatches are built in tab order,
    // the label is not shown, and screen-reader values follow the colour.
    UiTextMeasureRecord records[48]{};
    UiTextMeasureCache cache{};
    check(ui_text_measure_cache_initialize(&cache, {records, 48}, fake_text_measure<true>, nullptr),
          "color picker form: measure cache");
    TextFormContent content{};
    content.title = utf8_literal("Primary Color");
    content.label = utf8_literal("Primary Color");
    content.cancel = utf8_literal("Cancel");
    content.accept = utf8_literal("Apply");
    content.text = utf8_literal("#5865F2");
    content.single_line = content.can_accept = content.focus_visible = content.swatch = content.picker = true;
    content.swatch_rgba = 0x5865F2FF;
    content.color = {blurple, {suggested, 3}, 0, true, false};
    TextFormScene scene{};
    const UiRect viewport{0, 0, 800, 700};
    const bool built = text_form_update(&scene, content, viewport, &cache);
    const auto* square = text_form_node(scene, TextFormElement::color_field);
    const auto* hue = text_form_node(scene, TextFormElement::color_hue);
    const auto* edit = text_form_node(scene, TextFormElement::edit);
    const auto* dropper = text_form_node(scene, TextFormElement::eyedropper);
    const auto* swatches = text_form_node(scene, TextFormElement::color_swatches);
    check(built && square && hue && edit && dropper && swatches && !text_form_node(scene, TextFormElement::label) &&
              ui_rect_height(square->rectangle) == kColorPickerFieldHeight &&
              ui_rect_height(hue->rectangle) == kColorPickerHueHeight && square->rectangle.bottom <= hue->rectangle.top &&
              hue->rectangle.bottom <= edit->rectangle.top && edit->rectangle.right <= dropper->rectangle.left &&
              edit->rectangle.bottom <= swatches->rectangle.top &&
              ui_rect_height(swatches->rectangle) == kColorPickerSwatch,
          "color picker form: square, hue, field with eyedropper and swatches stack in order");
    check(text_form_focused(scene) == TextFormElement::edit && text_form_move_focus(&scene, false) &&
              text_form_focused(scene) == TextFormElement::eyedropper && text_form_move_focus(&scene, false) &&
              text_form_focused(scene) == TextFormElement::color_swatches && text_form_move_focus(&scene, true) &&
              text_form_move_focus(&scene, true) && text_form_move_focus(&scene, true) &&
              text_form_focused(scene) == TextFormElement::color_hue,
          "color picker form: tab order runs square, hue, field, eyedropper, swatches");
    check(utf8_equal({scene.color_values[1], static_cast<u32>(c_string_length(scene.color_values[1]))},
                     utf8_literal("Hue 235 degrees")) &&
              utf8_equal({scene.color_values[2], static_cast<u32>(c_string_length(scene.color_values[2]))},
                         utf8_literal("#FF0000")),
          "color picker form: screen-reader values name the hue and the keyboard's swatch");
    content.color.eyedropper = false;
    content.color.suggested = {};
    check(text_form_update(&scene, content, viewport, &cache) &&
              !text_form_node(scene, TextFormElement::eyedropper) &&
              !text_form_node(scene, TextFormElement::color_swatches) &&
              text_form_node(scene, TextFormElement::color_field),
          "color picker form: no eyedropper or swatches when the desktop or owner offers none");
    text_form_release(&scene);
}

} // namespace maicord
