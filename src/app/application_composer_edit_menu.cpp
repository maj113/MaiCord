#include "application_composer_edit_menu.hpp"
#include "application_attachments.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_composer_input.hpp"
#include "application_composer_text_input.hpp"
#include "application_message_interaction.hpp"
#include "application_state.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/platform/clipboard_attachment.hpp"
#include "maicord/core/log.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
TextEditMenuState state() noexcept {
    const auto& composer = g_app.message_composer;
    const auto history = message_composer_history_costs(composer);
    return {history.undo_steps != 0, history.redo_steps != 0, composer.anchor != composer.caret,
        composer.bytes != 0, IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE || clipboard_attachment_available(), true, true,
        application_composer_text_input_composing()};
}
bool ready() noexcept {
    return g_app.account_ui && !g_app.login.visible && !g_app.text_form && g_app.message_composer.text &&
        g_app.message_composer.channel_id && g_app.message_composer_scene.active;
}
bool apply_spelling(u32 action) noexcept {
    auto& composer = g_app.message_composer;
    if (g_app.message_composer_scene.model != &composer) return true;
    const auto activation = composer_spellcheck_menu_activate(&g_app.composer_spellcheck,
        message_composer_text(composer), composer.text_revision, action);
    if (activation.effect == ComposerSpellcheckEffect::learned) return refresh_account_frame(true);
    if (activation.effect != ComposerSpellcheckEffect::replace) return true;
    message_composer_break_history_run(&composer);
    return composer_apply_result(
        message_composer_replace_range(&composer, activation.first, activation.last, activation.replacement), true);
}
}
bool application_composer_edit_command(TextEditCommand command, const ComposerEditClipboard* clipboard) noexcept {
    if (!ready()) return false;
    if (application_composer_text_input_composing()) return true;
    auto& composer = g_app.message_composer;
    message_composer_break_history_run(&composer);
    auto capabilities = state();
    if (clipboard) capabilities.clipboard = clipboard->has_text;
    if (!text_edit_command_enabled(command, capabilities)) return command != TextEditCommand::none;
    MessageComposerResult result = MessageComposerResult::unchanged;
    switch (command) {
    case TextEditCommand::undo: result = message_composer_undo(&composer); break;
    case TextEditCommand::redo: result = message_composer_redo(&composer); break;
    case TextEditCommand::select_all: result = message_composer_select_all(&composer); break;
    case TextEditCommand::erase: result = message_composer_delete_forward(&composer); break;
    case TextEditCommand::copy: case TextEditCommand::cut: {
        const u32 first = minimum(composer.anchor, composer.caret), last = maximum(composer.anchor, composer.caret);
        const Utf8View selected{composer.text + first, last - first};
        const bool copied = clipboard ? clipboard->write && clipboard->write(clipboard->context, selected) :
            clipboard_write_utf8(g_app.window, selected);
        if (!copied) {
            log_write(LogLevel::warning, "ui", "composer_clipboard_write_failed");
            return true;
        }
        if (command == TextEditCommand::cut) result = message_composer_delete_forward(&composer);
        break;
    }
    case TextEditCommand::paste: {
        if (!clipboard && clipboard_attachment_available()) return application_paste_attachments();
        char text[kMessageComposerMaximumBytes]{};
        const auto copied = clipboard ? clipboard->read ? clipboard->read(clipboard->context, {text, sizeof(text)}) :
            ClipboardTextResult{0, ClipboardTextError::unavailable} : clipboard_read_utf8(g_app.window, {text, sizeof(text)});
        if (copied.ok() && copied.bytes) {
            u32 bytes = 0;
            for (u32 i = 0; i < copied.bytes; ++i) {
                if (text[i] == '\r' && i + 1 < copied.bytes && text[i + 1] == '\n') continue;
                text[bytes++] = text[i] == '\r' ? '\n' : text[i];
            }
            result = message_composer_insert_utf8(&composer, {text, bytes});
        } else if (!copied.ok()) log_write(LogLevel::warning, "ui", "composer_clipboard_read_failed");
        SecureZeroMemory(text, sizeof(text));
        break;
    }
    default: return false;
    }
    return composer_apply_result(result, command != TextEditCommand::select_all && command != TextEditCommand::copy);
}
bool application_composer_edit_menu_open(UiRect anchor, u32 spell_offset) noexcept {
    if (!ready()) return false;
    if (application_composer_text_input_composing()) return true;
    end_message_composer_pointer_selection();
    if (!focus_message_composer_control(MessageComposerControl::edit)) return false;
    message_composer_break_history_run(&g_app.message_composer);
    // Spelling suggestions for a flagged word lead the standard edit commands.
    PopupItem items[kComposerSpellcheckMenuItems + kTextEditMenuItems]{};
    auto& composer = g_app.message_composer;
    const u32 spelling = g_app.message_composer_scene.model != &composer ? 0 :
        composer_spellcheck_menu_build(&g_app.composer_spellcheck, message_composer_text(composer), composer.text_revision,
            spell_offset == kComposerEditMenuAtCaret ? composer.caret : spell_offset, {items, kComposerSpellcheckMenuItems});
    text_edit_menu_build(state(), {items + spelling, kTextEditMenuItems});
    if (spelling) items[spelling].flags |= popup_item_separator_before;
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, composer.document_identity,
            {items, spelling + kTextEditMenuItems}, anchor, application_viewport_dips(), 260.0f).ok()) return false;
    set_popup_owner(PopupOwnerKind::composer_edit);
    g_app.composer_menu_revision = g_app.message_composer.revision;
    return refresh_account_frame(true);
}
bool application_composer_edit_menu_action(u32 action) noexcept {
    if (!ready() || !g_app.popup_scene.active || g_app.popup_owner_kind != PopupOwnerKind::composer_edit) return false;
    const bool current = g_app.popup_scene.owner_id == g_app.message_composer.document_identity &&
        g_app.composer_menu_revision == g_app.message_composer.revision;
    const bool spelling = composer_spellcheck_menu_action(action);
    if (!current || action == 0 || (action > kTextEditMenuItems && !spelling)) return dismiss_popup();
    bool enabled = false;
    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
        if (g_app.popup_scene.items[i].action == action) enabled = (g_app.popup_scene.items[i].flags & popup_item_enabled) != 0;
    if (!enabled) return true;
    if (!dismiss_popup()) return false;
    if (spelling) return apply_spelling(action);
    return application_composer_edit_command(static_cast<TextEditCommand>(action));
}
bool application_composer_edit_menu_synchronize() noexcept {
    if (!g_app.popup_scene.active || g_app.popup_owner_kind != PopupOwnerKind::composer_edit) return true;
    if (!ready() || g_app.popup_scene.owner_id != g_app.message_composer.document_identity ||
        g_app.composer_menu_revision != g_app.message_composer.revision) return dismiss_popup();
    return true;
}
}
