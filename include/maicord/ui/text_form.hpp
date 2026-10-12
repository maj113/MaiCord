#pragma once

#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_composition.hpp"

namespace maicord {

enum class TextFormElement : u8 {
    root,
    panel,
    title,
    context,
    label,
    edit,
    status,
    option, // optional checkbox row
    actions,
    cancel,
    accept,
    count
};
inline constexpr u32 kTextFormElements = static_cast<u32>(TextFormElement::count);

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
void text_form_close(TextFormScene* scene) noexcept;
void text_form_release(TextFormScene* scene) noexcept;
const UiLayoutNode* text_form_node(const TextFormScene& scene, TextFormElement element) noexcept;
// The option row's checkbox square (empty when the form has no option).
UiRect text_form_option_box(const TextFormScene& scene) noexcept;
TextFormElement text_form_hit_test(const TextFormScene& scene, float x, float y) noexcept;
TextFormElement text_form_focused(const TextFormScene& scene) noexcept;
bool text_form_focus(TextFormScene* scene, TextFormElement element) noexcept;
bool text_form_move_focus(TextFormScene* scene, bool reverse) noexcept;
bool text_form_scroll(TextFormScene* scene, float delta) noexcept;
bool text_form_paint(const TextFormScene& scene, const TextFormEditGeometry& edit, const Theme& theme,
                     PaintStream* paint) noexcept;
bool text_form_text(const TextFormScene& scene, const TextFormEditGeometry& edit, const Theme& theme,
                    UiTextCommandStream* text) noexcept;

} // namespace maicord

namespace maicord {
bool text_form_text_output(const TextFormScene& scene, const TextFormEditGeometry& edit, const Theme& theme,
                           UiTextOutput* stream) noexcept;
} // namespace maicord
