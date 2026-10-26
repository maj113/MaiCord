#include "maicord/ui/button.hpp"
#include "maicord/ui/surface_geometry.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
namespace maicord {
namespace {
bool button(const UiWidgetManager& owner, UiWidgetHandle handle) noexcept {
    const auto* widget = ui_widget_manager_resolve(owner, handle);
    return widget && widget->kind == UiWidgetKind::button && ui_widget_manager_input_enabled(owner, handle);
}
} // namespace
UiButtonPointerResult ui_button_pointer(UiWidgetManager* owner, UiButtonPointerEvent event,
                                        UiPoint point) noexcept {
    return ui_button_pointer_target(owner, event,
                                    owner && event != UiButtonPointerEvent::cancel
                                        ? ui_widget_manager_hit_test(*owner, point.x, point.y)
                                        : ui_widget_invalid_handle());
}
UiButtonPointerResult ui_button_pointer_target(UiWidgetManager* owner, UiButtonPointerEvent event,
                                               UiWidgetHandle target) noexcept {
    if (!owner)
        return {};
    const auto captured = owner->captured;
    const auto hovered = owner->hovered;
    const auto* captured_widget = ui_widget_manager_resolve(*owner, captured);
    const bool held = captured_widget && captured_widget->kind == UiWidgetKind::button;
    if (event == UiButtonPointerEvent::cancel || !button(*owner, target))
        target = ui_widget_invalid_handle();
    ui_widget_manager_hover(owner, target);
    UiButtonPointerResult result{target, target.generation != 0 || held, false, !ui_widget_handle_equal(hovered, target)};
    if (event == UiButtonPointerEvent::press && target.generation) {
        ui_widget_manager_focus(owner, target);
        ui_widget_manager_capture(owner, target);
        result.changed = true;
    } else if (event == UiButtonPointerEvent::release || event == UiButtonPointerEvent::cancel) {
        result.activate = held && button(*owner, captured) && event == UiButtonPointerEvent::release &&
                          ui_widget_handle_equal(captured, target);
        if (held || event == UiButtonPointerEvent::cancel) {
            ui_widget_manager_capture(owner, ui_widget_invalid_handle());
            result.changed = true;
        }
    }
    return result;
}
Color ui_button_background(const UiWidgetManager& owner, UiWidgetHandle handle, const Theme& theme,
                           Color idle, UiButtonSurface surface) noexcept {
    if (surface == UiButtonSurface::icon_only)
        return {};
    if (!button(owner, handle) || !ui_widget_handle_equal(owner.hovered, handle))
        return idle;
    if (ui_background_active(theme)) {
        const Color emphasis = theme.tokens.emphasis.fill;
        if (idle.alpha == 1 && idle.red == emphasis.red && idle.green == emphasis.green &&
            idle.blue == emphasis.blue) {
            const float shade = ui_widget_handle_equal(owner.captured, handle) ? .88f : .94f;
            return {idle.red * shade, idle.green * shade, idle.blue * shade, 1};
        }
        return ui_background_hover_color(
            theme, ui_widget_handle_equal(owner.captured, handle) ? theme.interactive : theme.surface_high,
            ui_widget_handle_equal(owner.captured, handle) ? .32f : .20f);
    }
    if (surface != UiButtonSurface::opaque)
        return ui_widget_handle_equal(owner.captured, handle) ? theme.tokens.state.pressed : theme.tokens.state.hover;
    return ui_widget_handle_equal(owner.captured, handle) ? theme.interactive : theme.surface_high;
}
bool ui_button_icon_paint(PaintStream* paint, const UiWidgetManager& owner, UiWidgetHandle handle,
                          UiIcon icon, const Theme& theme, bool selected, bool keyboard_focus,
                          UiButtonSurface surface) noexcept {
    const bool palette = surface == UiButtonSurface::palette && ui_background_active(theme);
    if (surface == UiButtonSurface::palette)
        surface = UiButtonSurface::opaque;
    const auto* node = ui_widget_manager_layout_node(owner, handle);
    if (!node)
        return false;
    const bool enabled = ui_widget_manager_input_enabled(owner, handle);
    const bool hover = enabled && ui_widget_handle_equal(owner.hovered, handle);
    const bool pressed = hover && ui_widget_handle_equal(owner.captured, handle);
    const bool focus = keyboard_focus && enabled && ui_widget_handle_equal(owner.focused, handle);
    const auto r = node->rectangle;
    if (!paint_stream_push_clip(paint, node->clip_rectangle))
        return false;
    Color background = ui_button_background(owner, handle, theme,
                                            palette || selected                       ? theme.surface
                                            : surface == UiButtonSurface::translucent ? theme.tokens.fill.translucent
                                                                                      : Color{},
                                            surface);
    if (palette)
        background.alpha = 1;
    bool ok = palette ? ui_background_paint_surface(
                            paint, r, theme, background, focus ? theme.tokens.border.focus : background,
                            kUiControlCornerRadius, focus ? 1.5f : .75f,
                            pressed ? .90f
                            : hover ? 1.f
                                    : .80f,
                            UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest,
                            focus ? UiBackgroundBorderTone::semantic : UiBackgroundBorderTone::palette)
                      : paint_stream_push_rounded_border(paint, r, background, focus ? theme.tokens.border.focus : background, kUiControlCornerRadius, focus ? 1.5f : 0);
    const float extent = ui_icon_button_extent(minimum(ui_rect_width(r), ui_rect_height(r)));
    const float x = (r.left + r.right - extent) * .5f,
                y = (r.top + r.bottom - extent) * .5f + (pressed ? 1 : 0);
    const Color normal = ui_icon_role_color(theme, UiIconRole::normal);
    Color idle = normal;
    idle.alpha *= .72f;
    Color disabled_ink = ui_icon_role_color(theme, UiIconRole::muted);
    disabled_ink.alpha *= .55f;
    ok = ok && ui_icon_paint(paint, icon, {x, y, x + extent, y + extent},
                             !enabled           ? (palette ? disabled_ink : theme.interactive)
                             : pressed || hover ? normal
                             : palette          ? normal
                             : selected         ? ui_icon_role_color(theme, UiIconRole::accent)
                                                : idle);
    return paint_stream_pop_clip(paint) && ok;
}
} // namespace maicord
