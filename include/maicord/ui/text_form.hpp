#pragma once

#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_composition.hpp"

namespace maicord {

struct UiTextMeasureCache;

enum class TextFormElement : u8 {
    root,
    panel,
    title,
    context,
    // An optional radio group above the editor.
    choices_label,
    choice_0,
    choice_1,
    choice_2,
    choice_3,
    label,
    edit,
    // An optional dropdown below the editor; its options list while open.
    select_label,
    select,
    select_0,
    select_1,
    select_2,
    select_3,
    select_4,
    select_5,
    select_6,
    select_7,
    status,
    option, // optional checkbox row
    actions,
    cancel,
    accept,
    count
};
inline constexpr u32 kTextFormElements = static_cast<u32>(TextFormElement::count);
inline constexpr u32 kTextFormChoices = 4;
inline constexpr u32 kTextFormSelectOptions = 8;
// The radio group and dropdown elements, absent unless a form uses them.
inline constexpr u32 kTextFormChoiceElements = 2 + kTextFormChoices + 1 + kTextFormSelectOptions;

// Borrowed immutable projection. All strings outlive presentation; the host
// owns editing, validation, translation, modal routing and semantic actions.
struct TextFormContent {
    Utf8View title, context, label, status, cancel, accept, text;
    u32 anchor_utf16, caret_utf16;
    bool can_accept, caret_visible;
    ConstSlice<TextCompositionSpan> composition;
    bool focus_visible;
    // Native confirmation uses context/status/buttons without an editor.
    // Editing fields are ignored; the host dispatches the selected action.
    bool confirmation_only = false;
    bool editable = true;
    bool single_line = false;
    bool can_cancel = true;
    // Accept is destructive: it uses the danger fill instead of the accent.
    bool danger = false;
    // A checkbox row above the actions when the label is not empty.
    Utf8View option;
    bool option_checked = false;
    // A radio group above the editor (up to kTextFormChoices) under its
    // label; ~0u selects none. When choice_editor names an option the editor
    // collapses unless that option is chosen. An editor with choices needs no
    // label.
    Utf8View choices_label;
    ConstSlice<Utf8View> choices;
    u32 choice_selected = ~0u;
    u32 choice_editor = ~0u;
    // A dropdown below the editor (up to kTextFormSelectOptions options)
    // showing the selected option; while expanded its options list beneath.
    Utf8View select_label;
    ConstSlice<Utf8View> select_options;
    u32 select_selected = 0;
    bool select_expanded = false;
    // Editors: a hint shown while the field is empty; an error outlines the
    // field and shows the status in the danger colour; a swatch previews the
    // colour the text names (0xRRGGBBAA) at the field's end.
    Utf8View placeholder;
    bool error = false;
    bool swatch = false;
    u32 swatch_rgba = 0;
};

struct UiResolvedThemeStyles;
struct TextFormScene {
    const UiResolvedThemeStyles* theme_styles{};
    u32 theme_geometry_revision{};
    u16 measure_revision{1};
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    UiWidgetHandle elements[kTextFormElements];
    UiTextMeasureCache* measure;
    TextFormContent content;
    UiRect viewport;
    u64 layout_signature;
    u64 text_signature;
    u64 composition_signature;
    // The elements the widgets were built with, one bit each.
    u32 built;
    u32 revision;
    bool active;
};

using TextFormCompositionMark = TextCompositionMark;
struct TextFormEditGeometry {
    UiRect text_rectangle;
    ConstSlice<UiRect> selections;
    UiRect caret;
    ConstSlice<TextFormCompositionMark> composition;
};

bool text_form_bind_measure_revision(TextFormScene*, u16) noexcept;
bool text_form_bind_theme_styles(TextFormScene*, const UiResolvedThemeStyles*) noexcept;
bool text_form_update(TextFormScene* scene, const TextFormContent& content, UiRect viewport,
                      UiTextMeasureCache* measure) noexcept;
void text_form_release(TextFormScene* scene) noexcept;
const UiLayoutNode* text_form_node(const TextFormScene& scene, TextFormElement element) noexcept;
// The option row's checkbox square (empty when the form has no option).
UiRect text_form_option_box(const TextFormScene& scene) noexcept;
// The colour preview square inside the editor (empty without a swatch).
UiRect text_form_swatch_box(const TextFormScene& scene) noexcept;
// A radio row's circle (a choice or an open dropdown's option); empty for
// other or absent elements.
UiRect text_form_choice_marker(const TextFormScene& scene, TextFormElement element) noexcept;
// Choice and dropdown option elements as indexes; false for other elements.
bool text_form_choice_index(TextFormElement element, u32* index) noexcept;
bool text_form_select_index(TextFormElement element, u32* index) noexcept;
TextFormElement text_form_hit_test(const TextFormScene& scene, float x, float y) noexcept;
TextFormElement text_form_focused(const TextFormScene& scene) noexcept;
bool text_form_focus(TextFormScene* scene, TextFormElement element) noexcept;
bool text_form_move_focus(TextFormScene* scene, bool reverse) noexcept;
bool text_form_scroll(TextFormScene* scene, float delta) noexcept;
bool text_form_paint(const TextFormScene& scene, const TextFormEditGeometry& edit, const Theme& theme,
                     PaintStream* paint) noexcept;
bool text_form_text_output(const TextFormScene& scene, const TextFormEditGeometry& edit, const Theme& theme,
                           UiTextOutput* stream) noexcept;
} // namespace maicord
