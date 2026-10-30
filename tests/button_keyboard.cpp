#include "maicord/ui/button_keyboard.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
void test_button_keyboard(void (*check)(bool, const char*) noexcept) noexcept {
    UiWidget widgets[4]{};
    UiLayoutNode nodes[4]{};
    UiWidgetManager manager{};
    UiButtonKeyboard keyboard{};
    ui_widget_manager_initialize(&manager, {widgets, 4}, {nodes, 4});
    auto root_style = ui_layout_style();
    root_style.width = root_style.height = ui_layout_fill();
    root_style.flow = UiLayoutFlow::vertical;
    auto button_style = ui_layout_style();
    button_style.width = ui_layout_fixed(80);
    button_style.height = ui_layout_fixed(20);
    constexpr u16 interactive = ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable;
    const UiWidgetDesc root_desc{
        1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Root"), root_style}};
    const UiWidgetDesc button_desc{
        2, UiWidgetKind::button, interactive, {utf8_literal("Button"), button_style}};
    auto root = ui_widget_manager_add(&manager, ui_widget_invalid_handle(), root_desc);
    const auto first = ui_widget_manager_add(&manager, root.handle, button_desc);
    const auto second = ui_widget_manager_add(
        &manager, root.handle,
        {3, UiWidgetKind::button, interactive, {utf8_literal("Second"), button_style}});
    const auto edit = ui_widget_manager_add(
        &manager, root.handle,
        {4, UiWidgetKind::text_field, interactive, {utf8_literal("Edit"), button_style}});
    if (!edit.ok() || !ui_widget_manager_layout_rectangle(&manager, {0, 0, 100, 100}).ok() ||
        !ui_widget_manager_focus(&manager, first.handle)) {
        check(false, "button keyboard fixture initializes");
        return;
    }
    check(ui_widget_manager_next_focus(manager, first.handle, false, false).slot == second.handle.slot &&
              ui_widget_manager_next_focus(manager, second.handle, false, false).slot == edit.handle.slot &&
              !ui_widget_manager_next_focus(manager, edit.handle, false, false).generation &&
              !ui_widget_manager_next_focus(manager, first.handle, true, false).generation &&
              ui_widget_manager_next_focus(manager, edit.handle, false, true).slot == first.handle.slot &&
              manager.focused.slot == first.handle.slot,
          "nonwrapping focus search detects scope boundaries without mutating current focus");
    const auto down = ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, true);
    check(down.handled && !down.activate && keyboard.pressed.manager == &manager &&
              !ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, true, true).activate &&
              ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, false).activate &&
              !keyboard.pressed.manager &&
              !ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, false).activate,
          "Space arms once and activates only once on release");
    check(ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::enter, true).activate &&
              !ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::enter, true, true).activate &&
              !ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::enter, false).activate,
          "Enter auto-repeat and key release cannot duplicate activation");
    ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, true);
    ui_widget_manager_focus(&manager, second.handle);
    ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, true, true);
    check(!ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, false).activate,
          "held Space cannot transfer activation to another focused button");
    ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, true);
    check(!ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::other, true).handled &&
              !ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, false).activate,
          "other navigation keys cancel a press without swallowing navigation");
    ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, true);
    check(ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::escape, true).handled &&
              !keyboard.pressed.manager,
          "Escape cancels pending button activation");
    ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, true);
    widgets[second.handle.slot].flags &= static_cast<u16>(~ui_widget_enabled);
    check(!ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, false).activate &&
              !keyboard.pressed.manager,
          "button disabled while held cannot activate");
    widgets[second.handle.slot].flags |= ui_widget_enabled;
    ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, true);
    nodes[root.handle.slot].spec.style.flags &= static_cast<u16>(~ui_layout_node_visible);
    check(!ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, false).activate,
          "ancestor hidden before layout cancels activation");
    nodes[root.handle.slot].spec.style.flags |= ui_layout_node_visible;
    ui_widget_manager_layout_rectangle(&manager, {0, 0, 100, 20});
    check(!ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::enter, true).activate &&
              !ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, true).handled,
          "fully clipped buttons cannot acquire keyboard presses");
    ui_widget_manager_layout_rectangle(&manager, {0, 0, 100, 100});
    ui_widget_manager_focus(&manager, edit.handle);
    check(!ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::enter, true).handled,
          "text fields retain their own Enter behavior");
    ui_widget_manager_focus(&manager, first.handle);
    ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, true);
    ui_widget_manager_reset(&manager);
    root = ui_widget_manager_add(&manager, ui_widget_invalid_handle(), root_desc);
    const auto recreated = ui_widget_manager_add(&manager, root.handle, button_desc);
    ui_widget_manager_layout_rectangle(&manager, {0, 0, 100, 100});
    ui_widget_manager_focus(&manager, recreated.handle);
    check(recreated.handle.generation != first.handle.generation &&
              !ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, false).activate,
          "a recreated button slot cannot inherit an old keyboard press");
    ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, true);
    ui_button_keyboard_cancel(&keyboard);
    check(!ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, false).activate,
          "platform focus loss cancellation prevents late key-up activation");
    PaintStream paint{}, original{};
    Theme gradient = kDefaultTheme;
    gradient.background = background_theme_defaults();
    const bool ready = paint_stream_initialize(&paint) && paint_stream_initialize(&original) &&
                       paint_stream_reset(&paint, 100, 100) && paint_stream_reset(&original, 100, 100);
    const bool themed = ready && ui_button_icon_paint(&paint, manager, recreated.handle, UiIcon::send,
                                                      gradient, true, true, UiButtonSurface::palette);
    check(themed && paint.count > 1 && paint.quads[0].border_width == -7 &&
              paint.quads[0].reserved_x == 1.5f &&
              bytes_equal(&paint.quads[0].border, &gradient.accent, sizeof(Color)) &&
              bytes_equal(&paint.quads[1].fill, &gradient.text_primary, sizeof(Color)) &&
              paint.clip_depth == 1 && paint.rejected == 0,
          "button palette: selected send uses readable primary icon and retains semantic focus border");
    bool flat = ready && paint_stream_reset(&paint, 100, 100) &&
                ui_button_icon_paint(&paint, manager, recreated.handle, UiIcon::send, kDefaultTheme, true,
                                     true, UiButtonSurface::palette) &&
                ui_button_icon_paint(&original, manager, recreated.handle, UiIcon::send, kDefaultTheme,
                                     true, true, UiButtonSurface::opaque) &&
                paint.count == original.count;
    for (u32 i = 0; flat && i < paint.count; ++i)
        flat = bytes_equal(&paint.quads[i], &original.quads[i], sizeof(PaintQuad));
    check(flat, "button palette: flat send preserves every original fill, focus, icon and clip descriptor");
    paint_stream_release(&paint);
    paint_stream_release(&original);
}
} // namespace maicord
