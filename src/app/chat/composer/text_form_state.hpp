#pragma once
#include "text_form.hpp"
#include "maicord/render/text/form_surface.hpp"
#include "maicord/ui/text/secure_text_store.hpp"
#include "maicord/ui/widgets/button_keyboard.hpp"

namespace maicord::app_detail {
inline constexpr u32 kApplicationTextFormBytes = 4096;
inline constexpr u32 kApplicationTextFormDetailBytes = 2048;
// A secret field's longest text, and its bullets (U+2022, three bytes each) as the scene shows them.
inline constexpr u32 kApplicationTextFormSecretCodepoints = 128;
struct ApplicationTextForm {
    TextFormScene scene;
    TextFormSurface surface;
    UiTextMeasureCache* measure;
    SecureEdit edit;
    SecureTextStore store;
    SecureTextComposition composition;
    ApplicationTextFormRequest request;
    UiWidgetReference previous_focus;
    UiWidgetHandle pointer_button;
    UiButtonKeyboard keyboard;
    u64 blink_at_ms;
    u32 blink_ms;
    bool previous_composer_focus;
    bool dragging;
    bool refreshing;
    bool caret_visible;
    bool editable = true;
    bool can_accept = true;
    bool can_cancel = true;
    bool dismissible = true;
    bool option_checked;
    // The radio group, the dropdown and whether text was typed since the
    // last choice; their labels live in `labels`.
    u32 choice_selected, select_selected;
    bool select_expanded, typed;
    Utf8View choices[kTextFormChoices], options[kTextFormSelectOptions];
    u64 progress_revision;
    Utf8View progress_accept;
    Utf8View status;
    // The status is an error: the field and status show the danger colour
    // until the next edit.
    bool error;
    // The last text the request's check accepted and the colour it names.
    u32 swatch_rgba;
    u8 accepted_bytes;
    char accepted[64];
    char mask[3 * kApplicationTextFormSecretCodepoints];
    char labels[2048];
    char detail[kApplicationTextFormDetailBytes];
    // A colour picker's state: the colour as HSV, its suggested swatches, the part a pointer drag moves (the square
    // or the hue bar; count when none), and whether the picker itself wrote the field (so the check does not
    // recompute the hue from the text) or the eyedropper is picking.
    TextFormColorPicker color;
    u32 suggested[kColorPickerSuggestedColors];
    TextFormElement color_drag;
    bool color_writing, color_picking;
};
} // namespace maicord::app_detail
