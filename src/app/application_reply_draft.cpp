#include "application_reply_draft.hpp"
#include "application_composer.hpp"
#include "application_composer_input.hpp"
#include "application_message_edit.hpp"
#include "application_thread_actions.hpp"

namespace maicord::app_detail {

bool focus_message_composer_control(MessageComposerControl control) noexcept {
    if (!g_app.account_ui || g_app.login.visible ||
        !message_composer_scene_focus_control(&g_app.message_composer_scene, control)) return false;
    message_composer_break_history_run(&g_app.message_composer);
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    end_message_composer_pointer_selection();
    const bool editing = control == MessageComposerControl::edit;
    if (!editing && g_app.message_composer.focused) stop_local_typing(g_app.message_composer.channel_id);
    if (g_app.message_composer.focused != editing) {
        g_app.message_composer.focused = editing;
        ++g_app.message_composer.revision;
    }
    g_app.context_focus = &g_app.message_composer_scene.widgets;
    reset_message_composer_caret_timing(editing);
    return true;
}

bool application_composer_fill_command(Utf8View name) noexcept {
    char text[1 + 3 * 32 * 4 + 2 + 1];
    if (!g_app.account_ui || !g_app.message_composer_scene.active || !name.data || !name.bytes ||
        name.bytes + 2 > sizeof(text))
        return true;
    MessageComposer& composer = g_app.message_composer;
    const u64 channel_id = g_app.account_ui->active_channel_id;
    if (!channel_id || composer.channel_id != channel_id ||
        !application_thread_actions_channel_sendable(channel_id))
        return true;
    if (g_app.message_edit.editor_active && !cancel_message_edit_editor())
        return false;
    text[0] = '/';
    copy_bytes(text + 1, name.data, name.bytes);
    text[name.bytes + 1] = ' ';
    composer.focused = true;
    ++composer.revision;
    message_composer_scene_focus_edit(&g_app.message_composer_scene);
    g_app.context_focus = &g_app.message_composer_scene.widgets;
    return composer_apply_result(
        message_composer_replace_range(&composer, 0, composer.bytes, {text, name.bytes + 2}), true);
}

bool begin_message_reply(u64 message_id) noexcept {
    if (!g_app.account_ui || message_id == 0) {
        return false;
    }
    const MessageStoreResult found = message_store_find(g_app.message_view.store, message_id);
    const MessageRecord* record =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!record || record->delivery != MessageDeliveryState::authoritative ||
        record->content_availability != MessageContentAvailability::available ||
        (record->type != 0 && record->type != 19 && record->type != 20 && record->type != 23) ||
        record->channel_id != g_app.message_composer.channel_id ||
        record->channel_id != g_app.account_ui->active_channel_id ||
        !application_channel_readable(g_app, record->channel_id) ||
        !application_thread_actions_channel_sendable(record->channel_id)) {
        return false;
    }
    if (g_app.message_edit.editor_active && !cancel_message_edit_editor()) {
        return false;
    }
    if (!application_reply_draft_set(message_id)) return false;
    g_app.message_composer.focused = true;
    ++g_app.message_composer.revision;
    message_composer_scene_focus_edit(&g_app.message_composer_scene);
    g_app.context_focus = &g_app.message_composer_scene.widgets;
    return true;
}

bool application_reply_draft_activate(MessageComposerControl control) noexcept {
    if (control == MessageComposerControl::reply_mention) {
        const auto reply = application_reply_draft();
        if (!reply.message_id || !application_reply_draft_set(reply.message_id, !reply.mention_reply_author)) return false;
        return refresh_message_composer_ui();
    }
    if (control == MessageComposerControl::cancel_reply) {
        if (g_app.message_edit.editor_active) {
            if (!cancel_message_edit_editor()) return false;
        } else if (!application_reply_draft_cancel()) return false;
        focus_message_composer_control(MessageComposerControl::edit);
        return refresh_message_composer_ui();
    }
    return false;
}

} // namespace maicord::app_detail
