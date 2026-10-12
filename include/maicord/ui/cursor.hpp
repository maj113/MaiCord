#pragma once

#include "maicord/ui/widget_manager.hpp"

namespace maicord {

// Semantic pointer shape. Scenes report what lies under the pointer; the
// platform maps this to a native cursor in exactly one place.
enum class UiCursor : u8 {
    arrow,
    hand,
    ibeam,
    resize_we,
    resize_ns,
    move,
    hidden
};

// Any enabled hit region that is not a text editor presents a hand.
constexpr UiCursor ui_action_cursor(bool actionable, bool text = false) noexcept {
    return text ? UiCursor::ibeam : actionable ? UiCursor::hand : UiCursor::arrow;
}

// Retained widgets declare their own interaction: enabled hit-testable
// buttons are clickable and text fields are editable. Containers, labels,
// images and scroll views are passive unless a scene says otherwise.
inline UiCursor ui_widget_cursor(const UiWidgetManager& manager, UiWidgetHandle handle) noexcept {
    const UiWidget* widget = ui_widget_manager_resolve(manager, handle);
    if (!widget || (widget->flags & ui_widget_enabled) == 0 || (widget->flags & ui_widget_hit_test) == 0 ||
        !ui_widget_manager_input_enabled(manager, handle))
        return UiCursor::arrow;
    return ui_action_cursor(widget->kind == UiWidgetKind::button, widget->kind == UiWidgetKind::text_field);
}

inline UiCursor ui_widget_manager_cursor(const UiWidgetManager& manager, float x, float y) noexcept {
    return ui_widget_cursor(manager, ui_widget_manager_hit_test(manager, x, y));
}

} // namespace maicord
