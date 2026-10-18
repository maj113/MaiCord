#include "application_private_conversation.hpp"
#include "application_friends.hpp"
#include "application_profile.hpp"
#include "application_account_presentation.hpp"
#include "application_channel_session.hpp"
#include "application_composer.hpp"
#include "application_message_interaction.hpp"
#include "application_state.hpp"
#include "application_read_state.hpp"
#include "maicord/core/log.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
bool private_navigation_changed() noexcept {
    application_friends_hide();
    application_read_state_activity();
    return synchronize_navigation_view() && synchronize_selected_channel() && synchronize_shell_layout() &&
        refresh_account_frame(true);
}
bool queue_private_conversation(PrivateConversationOperation operation, u64 id) noexcept {
    if (!g_app.account_ui || !g_app.account.instance) return false;
    auto& ui = *g_app.account_ui;
    if (operation == PrivateConversationOperation::open) {
        const u64 cached = private_conversation_cached(ui.chat_state, id);
        if (cached) {
            const auto& pending = ui.private_conversation;
            if (pending.operation == PrivateConversationOperation::close && pending.target_id == cached &&
                pending.phase != PrivateConversationPhase::idle && pending.phase != PrivateConversationPhase::failed) return false;
            return chat_state_store_select_channel(&ui.chat_state, cached).ok() && private_navigation_changed();
        }
    }
    if (!private_conversation_begin(&ui.private_conversation, ui.chat_state, operation, id)) return false;
    if (g_app.window) PostMessageW(g_app.window, WM_NULL, 0, 0);
    return true;
}
void fail_private_conversation() noexcept {
    if (!g_app.account_ui) return;
    auto& request = g_app.account_ui->private_conversation;
    request.phase = PrivateConversationPhase::failed;
    request.error = PrivateConversationError::transport;
    request.feedback_shown = false;
    application_profile_conversation_ready();
}
bool accept_private_conversation(const RestClientResponse& response, Utf8View body) noexcept {
    if (!g_app.account_ui) return false;
    auto& ui = *g_app.account_ui;
    const bool activate_dm = ui.private_conversation.operation == PrivateConversationOperation::open &&
        ui.private_conversation.intent_revision == ui.chat_state.navigation_intent_revision;
    if (!private_conversation_accept(&ui.private_conversation, &ui.chat_state, &ui.user_store,
        response, body, {ui.message_history_object_structurals, kMessageHistoryObjectStructuralCount},
        {reinterpret_cast<char*>(ui.event_payload_scratch), kGatewayClientDispatchPayloadCapacity},
        {ui.guild_id_scratch, kChatStateGuildCapacity})) return false;
    if (ui.private_conversation.phase == PrivateConversationPhase::failed)
        log_u64(LogLevel::warning, "private", "request_http_status", ui.private_conversation.status_code);
    else if (activate_dm && ui.private_conversation.phase == PrivateConversationPhase::idle) {
        application_friends_hide();
        application_read_state_activity();
    }
    return synchronize_navigation_view() && synchronize_selected_channel() && application_profile_conversation_ready() && refresh_account_frame(true);
}
bool present_private_conversation_error() noexcept {
    if (!g_app.account_ui || g_app.popup_scene.active || (g_app.profile && g_app.profile->scene.active)) return true;
    auto& request = g_app.account_ui->private_conversation;
    if (request.phase != PrivateConversationPhase::failed || request.feedback_shown) return true;
    const PopupItem items[] = {
        {request.operation == PrivateConversationOperation::open ? utf8_literal("Could not open this conversation.") :
            utf8_literal("Could not close this conversation."), 0, 0, 0},
        {utf8_literal("Try again"), popup_action_private_notice_retry, popup_item_enabled, 0},
        {utf8_literal("Dismiss"), popup_action_private_notice_close, popup_item_enabled, 0}
    };
    const auto anchor = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::title_bar);
    const auto opened = popup_scene_open(&g_app.popup_scene, PopupKind::popover, request.target_id,
        {items, 3}, anchor, application_viewport_dips(), 300.0f);
    if (!opened.ok()) return false;
    set_popup_owner(PopupOwnerKind::private_notice);
    request.feedback_shown = true;
    return refresh_account_frame(true);
}
}
