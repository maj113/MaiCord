#include "maicord/ui/login_scene.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/paint.hpp"

namespace maicord {

void test_widget_input(void (*check)(bool, const char*) noexcept) noexcept {
    UiWidget widgets[4]{};
    UiLayoutNode nodes[4]{};
    UiWidgetManager manager{};
    check(ui_widget_manager_initialize(&manager, {widgets, 4}, {nodes, 4}), "input manager initialize");
    auto root_style = ui_layout_style();
    root_style.width = root_style.height = ui_layout_fill();
    root_style.flow = UiLayoutFlow::vertical;
    const auto root = ui_widget_manager_add(&manager, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Root"), root_style}});
    auto child_style = ui_layout_style();
    child_style.width = ui_layout_fixed(50);
    child_style.height = ui_layout_fixed(20);
    constexpr u16 interactive = ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable;
    const auto first = ui_widget_manager_add(&manager, root.handle,
        {2, UiWidgetKind::text_field, interactive, {utf8_literal("First"), child_style}});
    const auto last = ui_widget_manager_add(&manager, root.handle,
        {3, UiWidgetKind::button, interactive, {utf8_literal("Last"), child_style}});
    if (!root.ok() || !first.ok() || !last.ok() || !ui_widget_manager_layout(&manager, {100, 100}).ok()) {
        check(false, "input hierarchy layout");
        return;
    }
    const auto equal = [](UiWidgetHandle a, UiWidgetHandle b) noexcept {
        return a.slot == b.slot && a.generation == b.generation;
    };
    check(ui_widget_manager_input_enabled(manager, first.handle) &&
          equal(ui_widget_manager_hit_test(manager, 10, 10), first.handle),
          "noninteractive parent permits child input");
    UiRect anchor{};
    check(!ui_widget_manager_context_anchor(manager, &anchor) &&
          ui_widget_manager_focus(&manager, last.handle) &&
          ui_widget_manager_context_anchor(manager, &anchor) &&
          ui_rect_equal(anchor, {0, 20, 50, 40}),
          "keyboard context anchor follows focus instead of the pointer over another row");
    check(!ui_widget_manager_context_anchor(manager, nullptr), "context anchor rejects null output");
    manager.focused = {last.handle.slot, static_cast<u16>(last.handle.generation + 1)};
    check(!ui_widget_manager_context_anchor(manager, &anchor) && !ui_rect_has_area(anchor),
          "obsolete widget generation cannot provide a context target");
    manager.focused = {65534, manager.generation};
    check(equal(ui_widget_manager_move_focus(&manager, true), last.handle), "reverse Tab recovers out of range focus");
    manager.focused = {65534, manager.generation};
    check(equal(ui_widget_manager_move_focus(&manager, false), first.handle), "Tab recovers out of range focus");
    manager.focused = {first.handle.slot, static_cast<u16>(first.handle.generation + 1)};
    check(equal(ui_widget_manager_move_focus(&manager, false), first.handle), "Tab rejects obsolete focus generation");
    check(ui_widget_manager_hover(&manager, first.handle) && ui_widget_manager_capture(&manager, first.handle) &&
          equal(ui_widget_manager_pointer_target(&manager, -20, 150), first.handle),
          "capture routes outside viewport");
    check(ui_widget_manager_capture(&manager, ui_widget_invalid_handle()) &&
          equal(ui_widget_manager_pointer_target(&manager, 10, 30), last.handle),
          "release restores ordinary hit testing");
    ui_widget_manager_capture(&manager, first.handle);
    check(ui_widget_manager_block_input(&manager, root.handle, true) &&
          !manager.focused.generation && !manager.hovered.generation && !manager.captured.generation &&
          !ui_widget_manager_hit_test(manager, 10, 10).generation &&
          !ui_widget_manager_focus(&manager, first.handle) &&
          !ui_widget_manager_capture(&manager, first.handle) &&
          !ui_widget_manager_hover(&manager, first.handle), "blocked ancestor cancels all input ownership");
    manager.focused = first.handle;
    check(!ui_widget_manager_context_anchor(manager, &anchor), "blocked focus cannot open context menu");
    manager.focused = last.handle;
    check(!ui_widget_manager_move_focus(&manager, true).generation && !manager.focused.generation,
          "empty focus traversal clears old focus");
    ui_widget_manager_block_input(&manager, root.handle, false);
    ui_widget_manager_focus(&manager, first.handle);
    ui_widget_manager_hover(&manager, first.handle);
    ui_widget_manager_capture(&manager, first.handle);
    nodes[root.handle.slot].spec.style.flags &= static_cast<u16>(~ui_layout_node_visible);
    check(!ui_widget_manager_context_anchor(manager, &anchor), "hidden focus cannot open context menu");
    check(ui_widget_manager_reconcile_input(&manager) && !manager.focused.generation &&
          !manager.hovered.generation && !manager.captured.generation &&
          !ui_widget_manager_hit_test(manager, 10, 10).generation,
          "hidden ancestor rejects input before layout");
    nodes[root.handle.slot].spec.style.flags |= ui_layout_node_visible;
    ui_widget_manager_capture(&manager, first.handle);
    widgets[first.handle.slot].flags &= static_cast<u16>(~ui_widget_enabled);
    manager.focused = first.handle;
    check(!ui_widget_manager_context_anchor(manager, &anchor), "disabled focus cannot open context menu");
    check(!ui_widget_manager_pointer_target(&manager, -1, -1).generation && !manager.captured.generation,
          "disabled captured widget cancels on pointer dispatch");
    widgets[first.handle.slot].flags |= ui_widget_enabled;
    check(ui_widget_manager_layout(&manager, {100, 25}).ok() &&
          ui_widget_manager_focus(&manager, last.handle) &&
          !ui_widget_manager_hit_test(manager, 10, 30).generation,
          "clipping limits pointer hits without removing keyboard focus");
    check(ui_widget_manager_context_anchor(manager, &anchor) && ui_rect_equal(anchor, {0, 20, 50, 25}),
          "partially clipped context anchor stays within visible bounds");
    PaintStream focus_paint{};
    check(paint_stream_reset(&focus_paint, 100, 25) && paint_stream_push_focus(&focus_paint, manager, kDefaultTheme.accent) &&
          focus_paint.count == 1 && focus_paint.clip_depth == 1 && focus_paint.quads[0].top == 20 &&
          focus_paint.quads[0].bottom == 25 && focus_paint.quads[0].fill.alpha == 0 &&
          focus_paint.quads[0].border_width == 2,
          "shared focus outline is transparent clipped and balances its clip stack");
    check(ui_widget_manager_layout(&manager, {100, 20}).ok() &&
          !ui_widget_manager_context_anchor(manager, &anchor) && !ui_rect_has_area(anchor),
          "fully clipped focus cannot anchor a menu outside the viewport");
    check(paint_stream_push_focus(&focus_paint, manager, kDefaultTheme.accent) && focus_paint.count == 1,
          "fully clipped focus does not append outline geometry");
    paint_stream_release(&focus_paint);
    check(ui_widget_manager_layout_rectangle(&manager, {120, 80, 220, 105}).ok() &&
          ui_widget_manager_context_anchor(manager, &anchor) && ui_rect_equal(anchor, {120, 100, 170, 105}),
          "context anchoring uses layout coordinates at nonzero viewport origins");
    widgets[first.handle.slot].parent = first.handle;
    check(!ui_widget_manager_input_enabled(manager, first.handle), "cyclic input hierarchy rejected");
    widgets[first.handle.slot].parent = {root.handle.slot, static_cast<u16>(root.handle.generation + 1)};
    check(!ui_widget_manager_input_enabled(manager, first.handle), "stale parent generation rejected");
    widgets[first.handle.slot].parent = root.handle;
    ui_widget_manager_capture(&manager, first.handle);
    ui_widget_manager_reset(&manager);
    check(!manager.captured.generation && !ui_widget_manager_capture(&manager, first.handle),
          "scene reset invalidates capture handle");
    manager.focused = first.handle;
    check(!ui_widget_manager_move_focus(&manager, true).generation && !manager.focused.generation,
          "empty manager clears stale focus");

    LoginScene login{};
    check(login_scene_layout(&login, 1000, 800) && login_scene_focus(&login, LoginControl::login),
          "login input scene ready");
    ui_widget_manager_capture(&login.widgets, login.widgets.focused);
    const auto passes = login.layout_passes;
    login.action_disabled = true;
    check(login_scene_layout(&login, 1000, 800) && login.layout_passes == passes &&
          login.focus == LoginControl::none && !login.widgets.focused.generation &&
          !login.widgets.captured.generation && !login_scene_focus(&login, LoginControl::password),
          "cached login update cancels disabled credential focus and capture");
    bool credentials_skipped = true;
    for (u32 i = 0; i < 8; ++i) {
        const auto control = login_scene_move_focus(&login, (i & 1) != 0);
        credentials_skipped = control != LoginControl::login && control != LoginControl::password &&
                              control != LoginControl::submit && credentials_skipped;
    }
    check(credentials_skipped, "disabled credentials excluded from keyboard traversal");
    login.action_disabled = false;
    login.submit_disabled = true;
    check(login_scene_layout(&login, 1000, 800) && login_scene_focus(&login, LoginControl::login) &&
          !login_scene_focus(&login, LoginControl::submit), "submit-only block preserves credential editing");
    login.submit_disabled = false;
    check(login_scene_layout(&login, 1000, 800) && login_scene_focus(&login, LoginControl::submit),
          "login input recovers when enabled");
    login_scene_release(&login);

    MessageComposer model{};
    char text[64]{};
    MessageComposerScene composer{};
    UiWidget composer_widgets[kMessageComposerWidgetCapacity]{};
    UiLayoutNode composer_nodes[kMessageComposerWidgetCapacity]{};
    check(message_composer_initialize(&model, {text, 64}) &&
          message_composer_scene_initialize(&composer, {composer_widgets, kMessageComposerWidgetCapacity},
            {composer_nodes, kMessageComposerWidgetCapacity}), "composer input scene ready");
    message_composer_reset(&model, 42, false);
    check(message_composer_scene_update(&composer, &model, {0, 0, 600, 100}, true).ok() &&
          message_composer_scene_focus_edit(&composer) &&
          ui_widget_manager_capture(&composer.widgets, composer.edit), "composer edit acquires shared capture");
    check(message_composer_scene_update(&composer, &model, {0, 0, 600, 100}, false).ok() &&
          !composer.widgets.focused.generation && !composer.widgets.captured.generation,
          "composer permission change cancels focus and capture");
    check(message_composer_scene_update(&composer, &model, {0, 0, 600, 100}, true).ok() &&
          ui_widget_manager_capture(&composer.widgets, composer.edit) &&
          message_composer_scene_update(&composer, nullptr, {}, false).ok() &&
          !composer.widgets.captured.generation &&
          !ui_widget_manager_input_enabled(composer.widgets, composer.edit),
          "inactive composer subtree cancels capture");
    check(message_composer_scene_update(&composer, &model, {0, 0, 600, 100}, true).ok() &&
          message_composer_scene_focus_edit(&composer), "reactivated composer accepts input");
    const auto gif_tab = ui_widget_manager_next_focus(composer.widgets, composer.edit, false, false);
    const auto sticker_tab = ui_widget_manager_next_focus(composer.widgets, gif_tab, false, false);
    const auto emoji_tab = ui_widget_manager_next_focus(composer.widgets, sticker_tab, false, false);
    check(gif_tab.slot == composer.gif.slot && sticker_tab.slot == composer.sticker.slot &&
          emoji_tab.slot == composer.emoji.slot &&
          !ui_widget_manager_next_focus(composer.widgets, emoji_tab, false, false).generation &&
          message_composer_scene_focus_control(&composer, MessageComposerControl::emoji) &&
          message_composer_scene_focused_control(composer) == MessageComposerControl::emoji,
          "composer Tab reaches GIFs, stickers and emoji while skipping disabled Send and attachment");
    check(message_composer_insert_utf8(&model, utf8_literal("draft")) == MessageComposerResult::changed &&
          message_composer_scene_update(&composer, &model, {0, 0, 600, 100}, true, 7, utf8_literal("Reply")).ok() &&
          message_composer_scene_focus_control(&composer, MessageComposerControl::send) &&
          ui_widget_manager_next_focus(composer.widgets, composer.emoji, false, false).slot == composer.send.slot &&
          ui_widget_manager_next_focus(composer.widgets, composer.edit, true, false).slot == composer.reply_cancel.slot &&
          ui_widget_manager_next_focus(composer.widgets, composer.reply_cancel, true, false).slot == composer.reply_mention.slot,
          "composer Tab follows enabled Send and separate ping and cancel reply controls");
    check(message_composer_scene_focus_control(&composer, MessageComposerControl::cancel_reply) &&
          message_composer_scene_update(&composer, &model, {0, 0, 600, 100}, true).ok() &&
          message_composer_scene_focused_control(composer) == MessageComposerControl::none,
          "removing a reply banner cancels its focus");
    check(message_composer_scene_focus_control(&composer, MessageComposerControl::emoji) &&
          message_composer_scene_update(&composer, &model, {0, 0, 600, 100}, false).ok() &&
          !composer.widgets.focused.generation &&
          !ui_widget_manager_next_focus(composer.widgets, ui_widget_invalid_handle(), false, false).generation,
          "permission loss removes composer editing buttons from keyboard traversal");
    message_composer_scene_release(&composer);
    message_composer_release(&model);
}

} // namespace maicord
