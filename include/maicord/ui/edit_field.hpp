#pragma once
#include "maicord/ui/paint.hpp"
#include "maicord/ui/surface_geometry.hpp"
namespace maicord {
struct UiTextOutput;
struct UiEditView {
    Utf8View text;
    u32 anchor, caret;
    bool active, focused, caret_visible;
};
struct UiEditGeometry {
    UiRect text, caret;
    UiRect selections[48];
    u32 selection_count;
};
bool ui_edit_field_paint(PaintStream* paint, UiRect field, UiRect content, const UiEditView& view,
                         const UiEditGeometry* geometry, Color background, const Theme& theme,
                         u32 dpi, float radius = kUiEditCornerRadius) noexcept;
} // namespace maicord

namespace maicord {
bool ui_edit_field_text_output(UiTextOutput* text, UiWidgetReference owner, UiRect content,
                        const UiEditView& view, const UiEditGeometry* geometry, Utf8View placeholder,
                        const Theme& theme) noexcept;
} // namespace maicord
