#include "../../guild/community.hpp"
#include "../../account/account_presentation.hpp"
#include "../../shell/overlay_input.hpp"
#include "attachments.hpp"
#include "commands.hpp"
#include "../../media/picker.hpp"

#include "composer.hpp"
#include "edit_menu.hpp"
#include "text_input.hpp"
#include "../../shell/click_count.hpp"
#include "../messages/history.hpp"
#include "../messages/send.hpp"
#include "../../shell/state.hpp"
#include "reply_draft.hpp"
#include "../../plugins/surface_host.hpp"
#include "../../voice/application_voice_message.hpp"

#include "maicord/app/chat/composer/message_composer_controller.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/ui/plugins/slots.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/ui/media/picker.hpp"

namespace maicord::app_detail {
namespace {

// Same contract as the message accessory: buttons run the plugin's slot action.
bool activate_plugin_slot(u32 index) noexcept {
    UiPluginSlotNode nodes[kMessageComposerPluginSlots];
    const u32 count = ui_plugin_slot_toolbar(kUiPluginSlotComposerToolbar, {nodes, kMessageComposerPluginSlots});
    if (index < count && nodes[index].action_id)
        application_plugin_surfaces_activate(nodes[index].owner_id, {kUiPluginSlotComposerToolbar, 0, 0, 0, 0},
                                             nodes[index].action_id);
    return true;
}

// Client DIPs of a mouse message position.
UiPoint dip_point(const WindowEvent& event) noexcept {
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    return {static_cast<float>(static_cast<short>(event.pointer.x)) * scale,
            static_cast<float>(static_cast<short>(event.pointer.y)) * scale};
}

bool activate_composer_control(MessageComposerControl control) noexcept {
    if (application_voice_message_activate(control))
        return true;
    if (control == MessageComposerControl::plugin_slot)
        return activate_plugin_slot(message_composer_scene_focused_plugin_slot(g_app.message_composer_scene));
    if (control == MessageComposerControl::add_attachment)
        return application_attachment_add_menu();
    if (control == MessageComposerControl::attachments)
        return application_manage_attachments();
    if (control == MessageComposerControl::send)
        return queue_message_send() && pump_message_history();
    if (control == MessageComposerControl::gif)
        return application_media_picker_open(UiMediaPickerKind::gif);
    if (control == MessageComposerControl::sticker)
        return application_media_picker_open(UiMediaPickerKind::sticker);
    if (control == MessageComposerControl::emoji)
        return application_media_picker_open(UiMediaPickerKind::emoji);
    if (control == MessageComposerControl::reply_mention || control == MessageComposerControl::cancel_reply)
        return application_reply_draft_activate(control);
    if (control == MessageComposerControl::command_close)
        return application_commands_close();
    return false;
}

// The edit holds the pointer while a selection drags; without the capture the selection ends at once.
void begin_composer_pointer_capture() noexcept {
    application_capture_pointer();
    g_app.message_composer_pointer_selection = application_pointer_captured();
    if (!g_app.message_composer_pointer_selection)
        ui_widget_manager_capture(&g_app.message_composer_scene.widgets, ui_widget_invalid_handle());
}

} // namespace

void end_message_composer_pointer_selection() noexcept {
    ui_widget_manager_capture(&g_app.message_composer_scene.widgets, ui_widget_invalid_handle());
    if (!g_app.message_composer_pointer_selection) {
        return;
    }
    g_app.message_composer_pointer_selection = false;
    if (g_app.account_ui && !g_app.message_edit.editor_active) {
        const MessageComposerControllerResult committed =
            message_composer_controller_commit_draft(&g_app.account_ui->message_composer_controller);
        if (!committed.ok()) {
            log_u64(LogLevel::error, "message", "composer_pointer_draft_commit_error",
                    static_cast<u64>(committed.error));
        }
    }
    application_release_capture();
}

void release_message_composer_focus() noexcept {
    stop_local_typing(g_app.message_composer.channel_id);
    g_app.message_composer.focused = false;
    ++g_app.message_composer.revision;
    reset_message_composer_caret_timing(false);
}

bool handle_composer_click(const WindowEvent& event) noexcept {
    if (!g_app.account_ui || g_app.login.visible || !g_app.message_composer_scene.active) {
        return false;
    }
    const i32 client_x = static_cast<short>(event.pointer.x);
    const i32 client_y = static_cast<short>(event.pointer.y);
    const auto [x, y] = dip_point(event);
    const MessageComposerControl hit = message_composer_scene_hit_test(g_app.message_composer_scene, x, y);
    if (hit == MessageComposerControl::none) {
        if (g_app.message_composer.focused) {
            release_message_composer_focus();
            refresh_message_composer_ui();
        }
        return false;
    }
    // The recording bar replaces the editor: a click plays or pauses the preview.
    if (hit == MessageComposerControl::edit && application_voice_message_bar_click())
        return true;
    // A server waiting for its rules opens them instead of the editor.
    if (hit == MessageComposerControl::edit && application_guild_community_composer_press())
        return true;
    if (hit == MessageComposerControl::edit) {
        u32 position = g_app.message_composer.caret;
        // A layout rebuild and a pointer message can cross during resize/DPI
        // changes. Retain the current scalar boundary in that rare frame so
        // the editor still acquires focus; the next rebuilt edit layout
        // restores exact point-to-text positioning.
        renderer_message_composer_edit_hit_test(g_app.renderer, client_x, client_y, &position);
        const bool shift = application_shift_down();
        // A third press within the double-click time/distance selects the
        // line; an IME composition keeps its selection (TSF owns it).
        const u8 clicks = application_click_count(event, &g_app.message_composer_scene, x, y, false);
        const bool composing = application_composer_text_input_composing();
        MessageComposerResult selection = MessageComposerResult::unchanged;
        u32 anchor = 0, caret = 0;
        if (ui_text_pointer_press(message_composer_text(g_app.message_composer), position, clicks, shift,
                                  g_app.message_composer.anchor, composing, &g_app.composer_pointer_unit, &anchor,
                                  &caret))
            selection = clicks == 1 ? message_composer_set_selection(&g_app.message_composer, anchor, caret)
                                    : message_composer_select_range(&g_app.message_composer, anchor, caret);
        if (selection == MessageComposerResult::invalid_input) {
            return false;
        }
        const bool focus_changed = !g_app.message_composer.focused;
        g_app.message_composer.focused = true;
        message_composer_scene_focus_edit(&g_app.message_composer_scene);
        g_app.context_focus = &g_app.message_composer_scene.widgets;
        if (focus_changed) {
            ++g_app.message_composer.revision;
        }
        if (selection == MessageComposerResult::changed && !g_app.message_edit.editor_active) {
            const MessageComposerControllerResult committed =
                message_composer_controller_commit_draft(&g_app.account_ui->message_composer_controller);
            if (!committed.ok()) {
                return false;
            }
        }
        reset_message_composer_caret_timing(true);
        if (!refresh_message_composer_ui()) {
            return false;
        }
        if (composing ||
            !ui_widget_manager_capture(&g_app.message_composer_scene.widgets, g_app.message_composer_scene.edit))
            return true;
        begin_composer_pointer_capture();
        return true;
    }
    if (hit == MessageComposerControl::attachments &&
        message_composer_scene_attachment_row(g_app.message_composer_scene) == message_composer_attachment_row_strip) {
        const auto tile = message_composer_scene_attachment_hit(g_app.message_composer_scene, x, y);
        if (tile.part != UiComposerAttachmentPart::none && !application_attachment_strip_activate(tile))
            log_write(LogLevel::warning, "ui", "attachment_strip_action_failed");
        return true;
    }
    if (hit == MessageComposerControl::plugin_slot)
        return activate_plugin_slot(message_composer_scene_plugin_slot(g_app.message_composer_scene, x, y));
    if (focus_message_composer_control(hit) && !activate_composer_control(hit)) {
        log_write(LogLevel::warning, "ui", "composer_control_action_failed");
        refresh_message_composer_ui();
    }
    return true;
}

bool handle_composer_double_click(const WindowEvent& event) noexcept {
    if (!g_app.account_ui || g_app.login.visible || !g_app.message_composer_scene.active) {
        return false;
    }
    const i32 client_x = static_cast<short>(event.pointer.x);
    const i32 client_y = static_cast<short>(event.pointer.y);
    const auto [x, y] = dip_point(event);
    if (message_composer_scene_hit_test(g_app.message_composer_scene, x, y) != MessageComposerControl::edit) {
        return false;
    }
    u32 position = g_app.message_composer.caret;
    if (!renderer_message_composer_edit_hit_test(g_app.renderer, client_x, client_y, &position)) {
        return true;
    }
    g_app.message_composer.focused = true;
    message_composer_scene_focus_edit(&g_app.message_composer_scene);
    g_app.context_focus = &g_app.message_composer_scene.widgets;
    const u8 clicks = application_click_count(event, &g_app.message_composer_scene, x, y, true);
    const bool composing = application_composer_text_input_composing();
    u32 anchor = 0, caret = 0;
    if (!ui_text_pointer_press(message_composer_text(g_app.message_composer), position, clicks, false,
                               g_app.message_composer.anchor, composing, &g_app.composer_pointer_unit, &anchor,
                               &caret))
        return true;
    if (!composer_apply_result(message_composer_select_range(&g_app.message_composer, anchor, caret))) {
        return false;
    }
    // The drag that follows extends by whole words from this one.
    if (!ui_widget_manager_capture(&g_app.message_composer_scene.widgets,
                                   g_app.message_composer_scene.edit))
        return true;
    begin_composer_pointer_capture();
    return true;
}

bool handle_composer_drag(const WindowEvent& event) noexcept {
    if (!g_app.message_composer_pointer_selection) {
        return false;
    }
    if (!input_held(event)) {
        end_message_composer_pointer_selection();
        return true;
    }
    const auto [x, y] = dip_point(event);
    const auto target = ui_widget_manager_pointer_target(&g_app.message_composer_scene.widgets, x, y);
    const auto edit = g_app.message_composer_scene.edit;
    if (!g_app.message_composer_scene.widgets.captured.generation || !ui_widget_handle_equal(target, edit)) {
        end_message_composer_pointer_selection();
        return true;
    }
    u32 position = g_app.message_composer.caret;
    if (!renderer_message_composer_edit_hit_test(g_app.renderer, static_cast<short>(event.pointer.x),
                                                 static_cast<short>(event.pointer.y), &position)) {
        return true;
    }
    u32 anchor = 0, caret = 0;
    if (!ui_text_pointer_drag(message_composer_text(g_app.message_composer), g_app.composer_pointer_unit, position,
                              application_composer_text_input_composing(), &anchor, &caret))
        return true;
    const MessageComposerResult selected =
        g_app.composer_pointer_unit.unit == UiTextSelectionUnit::character
            ? message_composer_set_selection(&g_app.message_composer, anchor, caret)
            : message_composer_select_range(&g_app.message_composer, anchor, caret);
    if (selected == MessageComposerResult::invalid_input) {
        end_message_composer_pointer_selection();
        return true;
    }
    if (selected == MessageComposerResult::changed) {
        reset_message_composer_caret_timing(true);
        if (!refresh_message_composer_ui()) {
            end_message_composer_pointer_selection();
        }
    }
    return true;
}

bool handle_composer_hover(const WindowEvent& event) noexcept {
    if (!g_app.account_ui || g_app.login.visible || !g_app.message_composer_scene.active) {
        return false;
    }
    const auto [x, y] = dip_point(event);
    const MessageComposerControl hit = message_composer_scene_hit_test(g_app.message_composer_scene, x, y);
    // Tile and action-bar hover highlights; leaving the strip clears them.
    if (message_composer_scene_attachment_hover(
            &g_app.message_composer_scene,
            hit == MessageComposerControl::attachments
                ? message_composer_scene_attachment_hit(g_app.message_composer_scene, x, y)
                : UiComposerAttachmentHit{kUiComposerAttachmentNoTile, UiComposerAttachmentPart::none}))
        refresh_account_frame(true);
    return hit != MessageComposerControl::none;
}

bool handle_composer_key(u32 key) noexcept {
    MessageComposer& composer = g_app.message_composer;
    if (!g_app.account_ui || !composer.focused || g_app.login.visible) {
        return false;
    }
    // AltGr is reported by Win32 as Ctrl+Alt. Treat Ctrl as a command
    // modifier only when Alt is not held so keyboard-layout characters such
    // as @ never trigger paste/select-all before WM_CHAR arrives.
    const bool alt = application_alt_down();
    const bool control = application_command_down();
    const bool shift = application_shift_down();
    if (control && (key == 'Z' || key == 'Y')) {
        return application_composer_edit_command(key == 'Y' || shift ? TextEditCommand::redo
                                                                     : TextEditCommand::undo);
    }
    if (control && key == 'A') {
        return application_composer_edit_command(TextEditCommand::select_all);
    }
    if ((control && key == 'V') || (shift && !control && !alt && key == key::insert)) {
        return application_composer_edit_command(TextEditCommand::paste);
    }
    if (control && (key == 'C' || key == key::insert))
        return application_composer_edit_command(TextEditCommand::copy);
    if ((control && key == 'X') || (shift && !control && !alt && key == key::del))
        return application_composer_edit_command(TextEditCommand::cut);
    switch (key) {
    case key::backspace:
        application_commands_prepare_delete(false);
        return composer_apply_result(
            control ? message_composer_delete_word(&composer, false, clock_monotonic_milliseconds())
                    : message_composer_backspace(&composer, clock_monotonic_milliseconds()),
            true);
    case key::del:
        application_commands_prepare_delete(true);
        return composer_apply_result(
            control ? message_composer_delete_word(&composer, true, clock_monotonic_milliseconds())
                    : message_composer_delete_forward(&composer, clock_monotonic_milliseconds()),
            true);
    case key::left:
        return composer_apply_result(message_composer_move(
            &composer, control ? MessageComposerMove::previous_word : MessageComposerMove::previous,
            shift));
    case key::right:
        return composer_apply_result(message_composer_move(
            &composer, control ? MessageComposerMove::next_word : MessageComposerMove::next, shift));
    case key::home:
        return composer_apply_result(message_composer_move(
            &composer, control ? MessageComposerMove::document_home : MessageComposerMove::line_home,
            shift));
    case key::end:
        return composer_apply_result(message_composer_move(
            &composer, control ? MessageComposerMove::document_end : MessageComposerMove::line_end, shift));
    case key::enter:
        if (shift) {
            return composer_apply_result(message_composer_insert_line_break(&composer), true);
        }
        return queue_message_send() && pump_message_history();
    case key::escape:
        if (g_app.message_edit.editor_active || application_reply_draft().message_id != 0)
            return application_reply_draft_activate(MessageComposerControl::cancel_reply);
        if (application_clear_channel_attachments())
            return refresh_message_composer_ui() && pump_message_history();
        release_message_composer_focus();
        return refresh_message_composer_ui();
    case key::up: // In an empty composer the registry's edit_last_message has first refusal.
    case key::down: {
        u32 position = composer.caret;
        if (!renderer_message_composer_edit_move_vertical(g_app.renderer, key == key::down, &position)) {
            return true;
        }
        return composer_apply_result(
            message_composer_set_selection(&composer, shift ? composer.anchor : position, position));
    }
    default:
        return false;
    }
}

bool handle_composer_character(u32 value) noexcept {
    if (!g_app.account_ui || g_app.login.visible || value < 0x20u || value == 0x7Fu || value > 0xFFFFu) {
        return false;
    }
    const auto focused = message_composer_scene_focused_control(g_app.message_composer_scene);
    // Strip shortcuts arrive as characters after their key-down was handled.
    if (focused == MessageComposerControl::attachments &&
        g_app.context_focus == &g_app.message_composer_scene.widgets &&
        application_attachment_strip_character(static_cast<u32>(value)))
        return true;
    if (value == ' ' &&
        (g_app.button_keyboard.pressed.manager ||
         (g_app.context_focus == &g_app.message_composer_scene.widgets &&
          focused != MessageComposerControl::none && focused != MessageComposerControl::edit)))
        return true;
    MessageComposer& composer = g_app.message_composer;
    if ((!composer.focused || focused != MessageComposerControl::edit) &&
        !focus_message_composer_control(MessageComposerControl::edit))
        return true;
    return composer_apply_result(
        message_composer_insert_utf16(&composer, static_cast<u16>(value), clock_monotonic_milliseconds()),
        true);
}

bool handle_composer_button_key(u32 key, bool down, bool repeated) noexcept {
    if (key != key::space && key != key::enter && key != key::escape)
        return false;
    if (!g_app.account_ui || g_app.login.visible ||
        g_app.context_focus != &g_app.message_composer_scene.widgets ||
        application_control_down() || application_alt_down()) {
        ui_button_keyboard_cancel(&g_app.button_keyboard);
        return false;
    }
    const auto logical = key == key::space    ? UiButtonKey::space
                         : key == key::enter ? UiButtonKey::enter
                                            : UiButtonKey::escape;
    const auto result = ui_button_keyboard_key(
        &g_app.button_keyboard, &g_app.message_composer_scene.widgets, logical, down, repeated);
    if (result.activate &&
        !activate_composer_control(message_composer_scene_focused_control(g_app.message_composer_scene)))
        log_write(LogLevel::warning, "ui", "composer_control_action_failed");
    return result.handled;
}

} // namespace maicord::app_detail
