#include "../chat/inbox.hpp"
#include "../media/downloads.hpp"
#include "../voice/go_live.hpp"
#include "../account/profile.hpp"

#include "state.hpp"
#include "overlay_input.hpp"
#include "../chat/composer/input.hpp"
#include "../chat/composer/attachments.hpp"
#include "content_input.hpp"
#include "../account/login/auth.hpp"
#include "../account/application_account_switch.hpp"
#include "../account/login/input.hpp"
#include "keyboard_focus.hpp"
#include "../chat/messages/interaction.hpp"
#include "../chat/messages/navigation.hpp"
#include "scrolling.hpp"
#include "../chat/composer/text_form.hpp"
#include "../chat/threads/forum_form.hpp"
#include "../settings/settings.hpp"
#include "../settings/theme_editor.hpp"
#include "../account/friends.hpp"
#include "../chat/search/search.hpp"
#include "../chat/messages/pinned_messages.hpp"
#include "../chat/threads/browser.hpp"
#include "../media/picker.hpp"
#include "quick_switcher.hpp"
#include "../chat/messages/forward.hpp"
#include "../account/account_menu.hpp"
#include "../chat/scoped_read_menu.hpp"
#include "../media/video.hpp"
#include "../chat/unread_banner.hpp"
#include "../chat/polls/poll.hpp"
#include "shortcuts.hpp"
#include "../voice/application_voice_message.hpp"
#include "maicord/app/shell/input_layers.hpp"
#include "maicord/app/shell/navigation_shortcuts.hpp"
#include "maicord/core/log.hpp"
#include "../account/account_ui_runtime.hpp"

#include "maicord/platform/window/key.hpp"

namespace maicord::app_detail {
namespace {

u32 navigation_held_key;
// A letter shortcut (R, E, F on a focused message) must not also type its
// character into the composer it may have focused.
u32 shortcut_held_key;

bool consume_shortcut_tail(UiInputKind kind, u32 code) noexcept {
    if (!shortcut_held_key)
        return false;
    if (kind == UiInputKind::key_up && code == shortcut_held_key) {
        shortcut_held_key = 0;
        return true;
    }
    if (kind == UiInputKind::character)
        return true;
    if (kind == UiInputKind::key_down && code != shortcut_held_key)
        shortcut_held_key = 0;
    return false;
}

// Escape-style cancellations apply only when no edit or reply is in progress.
bool no_edit_or_reply() noexcept {
    return !g_app.message_edit.editor_active &&
           (!g_app.account_ui || !g_app.account_ui->message_composer_controller.reply_message_id);
}

UiInputDisposition disposition(bool handled) noexcept {
    return handled ? UiInputDisposition::handled : UiInputDisposition::ignored;
}

UiInputDisposition login_input(void*, const UiInputEvent& event) noexcept {
    if (!g_app.renderer_ready)
        return UiInputDisposition::ignored;
    // Esc leaves Add an account, or returns to a saved account.
    if (event.kind == UiInputKind::key_down && event.code == key::escape && !event.repeated &&
        application_account_switch_login_escape())
        return UiInputDisposition::handled;
    if (event.kind == UiInputKind::key_down)
        return disposition(application_login_handle_key(event.code, event.repeated));
    if (event.kind == UiInputKind::character)
        return disposition(application_login_handle_character(event.code));
    return UiInputDisposition::ignored;
}

UiInputDisposition viewer_input(void*, const UiInputEvent& event) noexcept {
    if (g_app.renderer_ready &&
        (event.kind == UiInputKind::key_down || event.kind == UiInputKind::system_key_down))
        return disposition(handle_reaction_viewer_key(event.code));
    return UiInputDisposition::ignored;
}

UiInputDisposition popup_input(void*, const UiInputEvent& event) noexcept {
    if (!g_app.renderer_ready)
        return UiInputDisposition::ignored;
    // A retained popup can stay open after activation (for example a spoiler
    // toggle). Holding Enter/Space must not repeat that action.
    if ((event.kind == UiInputKind::key_down || event.kind == UiInputKind::system_key_down) &&
        event.repeated &&
        (event.code == key::enter ||
         (event.code == key::space && g_app.popup_scene.kind != PopupKind::autocomplete)))
        return UiInputDisposition::handled;
    if (g_app.popup_scene.kind == PopupKind::autocomplete &&
        (event.kind == UiInputKind::key_down || event.kind == UiInputKind::system_key_down) &&
        (event.code == key::up || event.code == key::down) &&
        navigation_shortcut(event.code, application_control_down(), application_alt_down(),
                            application_shift_down())
                .mode != NavigationMode::none)
        return UiInputDisposition::ignored;
    if (event.kind == UiInputKind::key_down || event.kind == UiInputKind::system_key_down)
        return disposition(handle_popup_key(event.code));
    return UiInputDisposition::ignored;
}

UiInputDisposition content_input(void*, const UiInputEvent& event) noexcept {
    if (!g_app.renderer_ready)
        return UiInputDisposition::ignored;
    if (application_poll_input(event.kind, event.code, event.repeated))
        return UiInputDisposition::handled;
    if (application_thread_browser_input(event.kind, event.code, event.repeated))
        return UiInputDisposition::handled;
    if (application_media_picker_input(event.kind, event.code, event.repeated))
        return UiInputDisposition::handled;
    if (application_inbox_input(event.kind, event.code, event.repeated))
        return UiInputDisposition::handled;
    if (application_pinned_messages_input(event.kind, event.code, event.repeated))
        return UiInputDisposition::handled;
    if (application_search_input(event.kind, event.code, event.repeated)) {
        if (application_search_edit_focused())
            application_video_blur();
        return UiInputDisposition::handled;
    }
    if (event.kind == UiInputKind::key_down && !event.repeated && application_account_menu_key(event.code))
        return UiInputDisposition::handled;
    // A voice message recording owns Escape (delete), Enter (send) and typing.
    if (application_voice_message_input(event.kind, event.code, event.repeated))
        return UiInputDisposition::handled;
    if (application_friends_requests_active())
        return disposition(application_friends_input(event.kind, event.code, event.repeated));
    const ShortcutAction shortcut = event.kind == UiInputKind::key_down && event.code == key::escape
                                        ? application_shortcut_action(event.code)
                                        : ShortcutAction::none;
    if (event.kind == UiInputKind::key_down && !event.repeated && shortcut == ShortcutAction::mark_server_read &&
        no_edit_or_reply() && application_scoped_read_shortcut()) {
        navigation_held_key = event.code;
        return UiInputDisposition::handled;
    }
    // Modal layers have first refusal above. Keep an editor's Escape semantics,
    // then let an unobscured pending link retire before ordinary pane focus.
    if (event.kind == UiInputKind::key_down && event.code == key::escape && !event.repeated &&
        no_edit_or_reply() && application_message_navigation_cancel()) {
        navigation_held_key = event.code;
        return UiInputDisposition::handled;
    }
    // The official client's plain Escape: mark read and return to the live edge once
    // edits, replies and pending attachments have had first refusal.
    if (event.kind == UiInputKind::key_down && !event.repeated && shortcut == ShortcutAction::mark_channel_read &&
        no_edit_or_reply() &&
        (!g_app.context_focus || g_app.context_focus == &g_app.message_scene.widgets ||
         g_app.context_focus == &g_app.message_composer_scene.widgets) &&
        application_unread_banner_escape()) {
        navigation_held_key = event.code;
        return UiInputDisposition::handled;
    }
    // The registry: custom keybinds, then built-ins by focus precedence.
    if ((event.kind == UiInputKind::key_down || event.kind == UiInputKind::system_key_down) &&
        handle_shortcut_key(event.code, event.repeated)) {
        if (event.code >= 'A' && event.code <= 'Z')
            shortcut_held_key = event.code;
        return UiInputDisposition::handled;
    }
    if (application_friends_input(event.kind, event.code, event.repeated))
        return UiInputDisposition::handled;
    if ((event.kind == UiInputKind::key_down || event.kind == UiInputKind::key_up) &&
        handle_thread_header_key(event.code, event.kind == UiInputKind::key_down, event.repeated))
        return UiInputDisposition::handled;
    if (event.kind == UiInputKind::key_down && event.repeated &&
        (event.code == key::enter || event.code == key::space))
        return UiInputDisposition::handled;
    if (event.kind == UiInputKind::key_down && (event.code == key::enter || event.code == key::space) &&
        !application_control_down() && !application_alt_down() &&
        application_received_attachment_activate_focused())
        return UiInputDisposition::handled;
    if (event.kind == UiInputKind::key_down && handle_content_focus_key(event.code))
        return UiInputDisposition::handled;
    if (application_friends_active())
        return UiInputDisposition::blocked;
    // A focused upload strip owns its selection and tile shortcuts.
    if (event.kind == UiInputKind::key_down && g_app.context_focus == &g_app.message_composer_scene.widgets &&
        message_composer_scene_focused_control(g_app.message_composer_scene) == MessageComposerControl::attachments &&
        message_composer_scene_attachment_row(g_app.message_composer_scene) == message_composer_attachment_row_strip &&
        application_attachment_strip_key(event.code))
        return UiInputDisposition::handled;
    if (event.kind == UiInputKind::key_up)
        return disposition(handle_composer_button_key(event.code, false, event.repeated));
    if (event.kind == UiInputKind::key_down && handle_composer_button_key(event.code, true, event.repeated))
        return UiInputDisposition::handled;
    if (event.kind == UiInputKind::character) {
        const bool handled = handle_composer_character(event.code);
        if (handled)
            g_app.context_focus = &g_app.message_composer_scene.widgets;
        return disposition(handled);
    }
    if (event.kind == UiInputKind::key_down) {
        if (event.code == 'C' &&
            (g_app.context_focus == &g_app.message_scene.widgets ||
             (!g_app.context_focus && !g_app.message_composer.focused)) &&
            application_command_down() &&
            copy_message_text_selection())
            return UiInputDisposition::handled;
        if ((!g_app.context_focus || g_app.context_focus == &g_app.message_composer_scene.widgets) &&
            handle_composer_key(event.code)) {
            g_app.context_focus = &g_app.message_composer_scene.widgets;
            return UiInputDisposition::handled;
        }
        return disposition((!g_app.context_focus || g_app.context_focus == &g_app.message_scene.widgets) &&
                           handle_message_key(event.code));
    }
    return UiInputDisposition::ignored;
}

UiInputDisposition profile_input(void*, const UiInputEvent& event) noexcept {
    // The message field's emoji popout sits over the profile and takes its keys first.
    if (application_media_picker_input(event.kind, event.code, event.repeated))
        return UiInputDisposition::handled;
    if (event.kind == UiInputKind::key_down || event.kind == UiInputKind::system_key_down ||
        event.kind == UiInputKind::key_up)
        application_profile_key(event.code, event.kind != UiInputKind::key_up, event.repeated);
    if (event.kind == UiInputKind::character)
        application_profile_character(event.code);
    return UiInputDisposition::handled;
}

} // namespace

bool route_application_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    // Observation only: Shift expands or collapses an open hover bar.
    if (code == key::shift && !repeated && (kind == UiInputKind::key_down || kind == UiInputKind::key_up))
        application_message_action_bar_modifiers_changed();
    // Cancellation may remove the navigation owner before key release. Retain
    // its tail so a held Escape cannot dismiss the next foreground surface.
    if (ui_input_consume_key_tail(&navigation_held_key, {kind, code, repeated}))
        return true;
    if (consume_shortcut_tail(kind, code))
        return true;
    if (application_custom_keybind_input(kind, code, repeated))
        return true;
    if (g_app.text_form && application_forum_form_active() &&
        application_text_form_input(kind, code, repeated))
        return true;
    if (application_forum_form_input(kind, code, repeated))
        return true;
    // The retained tail survives close/account navigation and must be consumed
    // before another modal or the underlying draft can receive it.
    if (application_poll_consume_key_tail(kind, code, repeated))
        return true;
    if (application_thread_browser_consume_key_tail(kind, code, repeated))
        return true;
    if (application_downloads_input(kind, code, repeated))
        return true;
    if (application_go_live_modal_input(kind, code, repeated))
        return true;
    if (application_shortcuts_overlay_input(kind, code, repeated))
        return true;
    if (application_message_forward_input(kind, code, repeated))
        return true;
    if (application_quick_switcher_input(kind, code, repeated))
        return true;
    if (application_text_form_input(kind, code, repeated))
        return true;
    if (application_theme_editor_input(kind, code, repeated))
        return true;
    if (application_settings_input(kind, code, repeated))
        return true;
    if (ui_input_consume_key_tail(&g_app.composer_menu_held_key, {kind, code, repeated}))
        return true;
    if ((kind == UiInputKind::key_down || kind == UiInputKind::system_key_down) && code != key::space)
        ui_button_keyboard_cancel(&g_app.button_keyboard);
    const AppPopupInput popup = !g_app.popup_scene.active ? AppPopupInput::none
                                : g_app.popup_scene.kind == PopupKind::autocomplete
                                    ? AppPopupInput::autocomplete
                                    : AppPopupInput::modal;
    const auto frame =
        app_input_layers({popup, g_app.reaction_viewer.active, g_app.login.visible,
                          g_app.profile && g_app.profile->scene.active},
                         {nullptr, login_input, viewer_input, popup_input, content_input, profile_input});
    const auto result = ui_input_route({frame.layers, frame.count}, {kind, code, repeated});
    if (result.disposition == UiInputDisposition::failed)
        log_u64(LogLevel::error, "ui", "input_route_failed_layer", result.layer_id);
    return result.consumed();
}

} // namespace maicord::app_detail
