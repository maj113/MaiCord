#pragma once
#include "maicord/ui/icon.hpp"
namespace maicord {
enum class UiButtonPointerEvent : u8 {
    move,
    press,
    release,
    cancel
};
struct UiButtonPointerResult {
    UiWidgetHandle target = ui_widget_invalid_handle();
    bool handled = false, activate = false, changed = false;
};
// State is owned by the widget manager's generation-checked hover/capture handles.
// Callers translate platform events, mirror capture to the OS and dispatch actions.
UiButtonPointerResult ui_button_pointer(UiWidgetManager* owner, UiButtonPointerEvent event,
                                        UiPoint point) noexcept;
// Reuse a hit from the same owner/dispatch. Generation and enabled state are
// checked again; callers must not reuse it after layout or geometry changes.
UiButtonPointerResult ui_button_pointer_target(UiWidgetManager* owner, UiButtonPointerEvent event,
                                               UiWidgetHandle target) noexcept;
enum class UiButtonSurface : u8 {
    opaque,
    palette
};
Color ui_button_background(const UiWidgetManager& owner, UiWidgetHandle button, const Theme& theme,
                           Color idle = {}, UiButtonSurface surface = UiButtonSurface::opaque) noexcept;
bool ui_button_icon_paint(PaintStream* paint, const UiWidgetManager& owner, UiWidgetHandle button,
                          UiIcon icon, const Theme& theme, bool selected = false,
                          bool keyboard_focus = false,
                          UiButtonSurface surface = UiButtonSurface::opaque) noexcept;
} // namespace maicord
