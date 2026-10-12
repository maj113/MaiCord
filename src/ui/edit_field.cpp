#include "maicord/ui/edit_field.hpp"
#include "maicord/ui/shell_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
namespace maicord {
bool ui_edit_field_paint(PaintStream* paint, UiRect field, UiRect content, const UiEditView& view,
                         const UiEditGeometry* geometry, Color background, const Theme& theme, u32 dpi,
                         float radius) noexcept {
    if (!view.active)
        return true;
    bool ok = ui_background_paint_surface(
        paint, field, theme, background, view.focused ? theme.tokens.border.focus : theme.tokens.border.strong, radius, 1, 1,
        UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest,
        view.focused ? UiBackgroundBorderTone::semantic : UiBackgroundBorderTone::palette);
    if (!ok || !view.focused || !geometry)
        return ok;
    if (!paint_stream_push_clip(paint, content.left, content.top, content.right, content.bottom))
        return false;
    Color selection = theme.tokens.emphasis.outline;
    selection.alpha = .38f;
    for (u32 i = 0; ok && i < geometry->selection_count; ++i) {
        const auto r = ui_rect_intersection(geometry->selections[i], content);
        if (ui_rect_has_area(r))
            ok = paint_stream_push(paint, r.left, r.top, r.right, r.bottom, selection);
    }
    const auto caret = caret_pixel_rectangle(geometry->caret, content, dpi);
    if (ok && view.caret_visible && ui_rect_has_area(caret))
        ok = paint_stream_push(paint, caret.left, caret.top, caret.right, caret.bottom, theme.text_primary);
    return paint_stream_pop_clip(paint) && ok;
}
bool ui_edit_field_text_output(UiTextOutput* text, UiWidgetReference owner, UiRect content,
                               const UiEditView& view, const UiEditGeometry* geometry, Utf8View placeholder,
                               const Theme& theme) noexcept {
    if (!view.active)
        return true;
    const bool entered = view.text.bytes && geometry;
    auto color = theme.text_primary;
    if (!view.text.bytes)
        color.alpha *= .6f;
    return ui_text_output_push(text, {view.text.bytes ? view.text : placeholder, owner,
                                      entered ? geometry->text : content, content, color, 0,
                                      UiTextAlignment::start,
                                      entered ? UiTextAlignment::start : UiTextAlignment::center,
                                      UiTextWrapping::no_wrap, UiTextOverflow::clip});
}
} // namespace maicord

namespace maicord {
} // namespace maicord
