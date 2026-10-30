#include "application_dm_profile.hpp"
#include "application_activity.hpp"
#include "application_invite_cards.hpp"
#include "application_message_components.hpp"
#include "application_action_captcha.hpp"
#include "application_emoji_preferences.hpp"
#include "application_guild_invite.hpp"
#include "application_commands.hpp"
#include "application_message_history.hpp"
#include "application_profile.hpp"
#include "application_attachments.hpp"
#include "application_friends.hpp"
#include "application_read_state.hpp"
#include "application_scoped_read.hpp"
#include "application_search.hpp"
#include "application_search_navigation.hpp"
#include "application_message_navigation.hpp"
#include "application_thread_actions.hpp"
#include "application_thread_browser.hpp"
#include "application_thread_creation.hpp"
#include "application_forum_post.hpp"
#include "application_pinned_messages.hpp"
#include "application_media_picker.hpp"
#include "application_expression_card.hpp"
#include "application_interaction_card.hpp"
#include "application_poll.hpp"
#include "application_member_moderation.hpp"

#include "application_account_presentation.hpp"
#include "application_message_feedback.hpp"
#include "application_composer.hpp"
#include "application_message_delete.hpp"
#include "application_message_edit.hpp"
#include "application_message_reaction.hpp"
#include "application_reaction_tooltip.hpp"
#include "application_message_send.hpp"
#include "application_message_forward.hpp"
#include "application_private_conversation.hpp"
#include "application_state.hpp"
#include "application_channel_authority.hpp"
#include "application_channel_snapshots.hpp"
#include "application_load_trace.hpp"
#include "application_self_status.hpp"
#include "application_user_settings.hpp"
#include "application_guild_folders.hpp"
#include "application_account_settings.hpp"
#include "application_private_channel.hpp"
#include "application_notification_controls.hpp"
#include "application_custom_status.hpp"
#include "application_inbox.hpp"
#include "application_channel_overview.hpp"
#include "application_stream.hpp"
#include "application_stage.hpp"
#include "application_soundboard.hpp"
#include "application_startup_history.hpp"

#include "application_log_fields.hpp"
#include "maicord/core/log.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/message_scene.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {

// The selected channel needs a latest-page catch-up and none is running.
static bool catchup_wanted() noexcept {
    const auto& catchup = g_app.message_catchup;
    const bool inactive =
        catchup.state == MessageHistoryCatchupState::idle || message_history_browsing_ready(catchup.state) ||
        (g_app.message_pump.resync_required && catchup.state == MessageHistoryCatchupState::failed);
    return g_app.account_ui->active_channel_id != 0 && g_app.pending_message_jump_message_id == 0 &&
           inactive &&
           (g_app.message_pump.resync_required ||
            ((g_app.message_view.timeline.follow_bottom ||
              message_history_newer_prefetch_needed(g_app.message_view.timeline)) &&
             (g_app.message_pump.catchup_required || g_app.message_view.timeline.newer_refetch_required)));
}

// A response over the shared buffer (a large list such as a server's bans,
// submitted with rest_client_submit_large_response) is taken into a bank of
// its exact size that lives until the drain returns.
struct LargeResponseBank {
    u8* data = nullptr;
    u32 bytes = 0;

    bool allocate(u32 size) noexcept {
        if (data || !size || size > kRestClientMaximumResponseBytes)
            return false;
        data = static_cast<u8*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        bytes = data ? size : 0;
        return data != nullptr;
    }
    ~LargeResponseBank() noexcept {
        if (data)
            VirtualFree(data, 0, MEM_RELEASE);
    }
};

struct MessageHistoryResponseErase {
    u8* data;
    u32 bytes;
    bool favorites = false;

    ~MessageHistoryResponseErase() noexcept {
        if (data && bytes != 0) {
            secure_clear_bytes(data, bytes);
        }
        if (favorites) {
            application_gif_favorites_response_drained();
            application_emoji_preferences_response_drained();
        }
    }
};

static u64 next_message_history_request_id() noexcept {
    u64& request_id = g_app.account_ui->message_history_request_id;
    ++request_id;
    if (request_id == 0) {
        ++request_id;
    }
    return request_id;
}
// A message-jump history request is over: the search navigation takes its outcome and the jump target clears.
static void end_message_jump(const RestClientResponse& response, const MessageHistoryRestResult& result) noexcept {
    application_search_navigation_history_response(response, result);
    g_app.pending_message_jump_channel_id = 0;
    g_app.pending_message_jump_message_id = 0;
}
static void fail_message_jump(const RestClientResponse& response, MessageHistoryRestError error) noexcept {
    MessageHistoryRestResult failed{};
    failed.error = error;
    end_message_jump(response, failed);
}

// The reply refresh gave up on its message: the lane moves on without retrying it.
static void give_up_reply_refresh() noexcept {
    g_app.reply_refresh.completed_through_id = g_app.reply_refresh.message_id;
    g_app.reply_refresh.state = AttachmentRefreshState::idle;
}

static bool begin_required_catchup() noexcept {
    if (application_thread_browser_read_blocked())
        return true;
    if (g_app.message_history_request_owner != MessageHistoryRequestOwner::none)
        return true;
    if (catchup_wanted()) {
        if (g_app.message_pump.resync_required) {
            if (!message_timeline_reset(&g_app.message_view.timeline) ||
                !message_timeline_set_scroll(&g_app.message_view.timeline, 0, true)) {
                return false;
            }
            g_app.message_pump.catchup_required = true;
            g_app.message_pump.resync_required = false;
        }
        if (!message_history_catchup_begin(&g_app.message_catchup, &g_app.message_pump,
                                           g_app.account_ui->active_channel_id,
                                           kMessageHistoryRequestLimit)) {
            return false;
        }
        if (g_app.message_older.state == MessageHistoryOlderState::request_required ||
            g_app.message_older.state == MessageHistoryOlderState::waiting_retry) {
            g_app.message_older.state = MessageHistoryOlderState::idle;
            g_app.message_older.retry_at_ms = 0;
        }
    }
    return true;
}

struct ReactionPrimaryTurn {
    u64 account_scope, profile_id, self_user_id;
    bool yield;
};
static_assert(sizeof(ReactionPrimaryTurn) == 32);
static ReactionPrimaryTurn reaction_primary_turn{};

// One DETAILS request is accepted per pump. Rotate the starting surface after success so
// repeated user work on any surface cannot starve another ready surface.
struct DetailsRequestTurn {
    u64 account_scope;
    u8 next;
};
static DetailsRequestTurn details_request_turn{};

// A request lane: `pending` reports work and `submit` sends it, tagged with `owner`.
struct RequestLane {
    bool (*pending)(u64 now) noexcept;
    bool (*submit)(u64 request_id, u64 now) noexcept;
    MessageHistoryRequestOwner owner;
};

// Short settings-style requests on the primary lane. Search yields to a pending message jump.
static constexpr RequestLane kPrimaryLanes[] = {
    {application_self_status_pending, application_self_status_submit, MessageHistoryRequestOwner::self_status},
    {application_custom_status_pending, application_custom_status_submit,
     MessageHistoryRequestOwner::custom_status},
    {application_activity_sharing_pending, application_activity_sharing_submit,
     MessageHistoryRequestOwner::activity_sharing},
    {application_user_settings_pending, application_user_settings_submit,
     MessageHistoryRequestOwner::user_settings},
    // Guild folder layout edits share the settings-proto lane and owner.
    {application_guild_folders_pending, application_guild_folders_submit,
     MessageHistoryRequestOwner::user_settings},
    {application_search_pending, application_search_submit, MessageHistoryRequestOwner::message_search},
    {application_media_picker_pending, application_media_picker_submit,
     MessageHistoryRequestOwner::media_catalog},
    // The emoji/sticker popout's sticker, pack list and retired pack.
    {application_expression_card_pending, application_expression_card_submit,
     MessageHistoryRequestOwner::expression_card},
    {application_gif_favorites_pending, application_gif_favorites_submit,
     MessageHistoryRequestOwner::gif_favorites},
    // The command chip popout's interaction data.
    {application_interaction_card_pending, application_interaction_card_submit,
     MessageHistoryRequestOwner::interaction_data},
    {application_emoji_preferences_pending, application_emoji_preferences_submit,
     MessageHistoryRequestOwner::emoji_preferences},
    {application_friends_pending, application_friends_submit, MessageHistoryRequestOwner::friends},
};
static constexpr u32 kPrimaryLaneSearch = 5, kPrimaryLaneMedia = 6, kPrimaryLaneFriends = 11,
                     kPrimaryLaneEnd = 12;
static_assert(sizeof(kPrimaryLanes) / sizeof(kPrimaryLanes[0]) == kPrimaryLaneEnd);

// The first pending lane in [first, last) takes the pump's turn, whether or not its submit was
// accepted (a refused one is retried by a later pump).
static bool admit_primary_lanes(u32 first, u32 last, u64 now) noexcept {
    for (; first < last; ++first) {
        const RequestLane& lane = kPrimaryLanes[first];
        if (!lane.pending(now))
            continue;
        if (lane.submit(next_message_history_request_id(), now))
            g_app.message_history_request_owner = lane.owner;
        return true;
    }
    return false;
}

// A lane without a `submit` (channel overview, profile, commands) tags its own request; a lane with
// one is admitted when its pending reports work.
static constexpr RequestLane kDetailsLanes[] = {
    {application_thread_actions_pending, application_thread_actions_submit,
     MessageHistoryRequestOwner::thread_actions},
    {application_search_navigation_pending, application_search_navigation_submit,
     MessageHistoryRequestOwner::search_navigation},
    {application_pinned_messages_pending, application_pinned_messages_submit,
     MessageHistoryRequestOwner::pinned_messages},
    {application_thread_browser_pending, application_thread_browser_submit,
     MessageHistoryRequestOwner::thread_browser},
    {application_channel_overview_submit},
    {application_profile_submit},
    {application_commands_submit},
    {application_thread_creation_pending, application_thread_creation_submit,
     MessageHistoryRequestOwner::thread_creation},
    {application_forum_post_pending, application_forum_post_submit, MessageHistoryRequestOwner::forum_post},
    {application_poll_pending, application_poll_submit, MessageHistoryRequestOwner::poll},
    {application_member_moderation_pending, application_member_moderation_submit,
     MessageHistoryRequestOwner::member_moderation},
    {application_guild_invite_pending, application_guild_invite_submit,
     MessageHistoryRequestOwner::guild_invite},
    {application_account_settings_pending, application_account_settings_submit,
     MessageHistoryRequestOwner::account_settings},
    {application_private_channel_pending, application_private_channel_submit,
     MessageHistoryRequestOwner::private_channel},
    {application_notification_controls_pending, application_notification_controls_submit,
     MessageHistoryRequestOwner::guild_notification_settings},
    {application_inbox_pending, application_inbox_submit, MessageHistoryRequestOwner::inbox_mentions},
    {application_stream_preview_pending, application_stream_preview_submit,
     MessageHistoryRequestOwner::stream_preview},
    {application_stage_pending, application_stage_submit, MessageHistoryRequestOwner::stage},
    {application_message_components_pending, application_message_components_submit,
     MessageHistoryRequestOwner::message_components},
    {application_invite_cards_pending, application_invite_cards_submit,
     MessageHistoryRequestOwner::invite_cards},
    {application_soundboard_pending, application_soundboard_submit, MessageHistoryRequestOwner::soundboard},
    {application_dm_profile_pending, application_dm_profile_submit, MessageHistoryRequestOwner::dm_profile},
};

static bool admit_details_lane(const RequestLane& lane, u64 now) noexcept {
    if (!lane.submit)
        return lane.pending(now);
    if (!lane.pending(now))
        return false;
    const u64 request_id = next_message_history_request_id();
    if (!lane.submit(request_id, now))
        return false;
    g_app.details_request_owner = lane.owner;
    g_app.details_request_id = request_id;
    return true;
}

static void admit_details_request(u64 now) noexcept {
    auto& turn = details_request_turn;
    if (turn.account_scope != g_app.plugin_account_scope_serial)
        turn = {g_app.plugin_account_scope_serial, 0};
    constexpr u8 lanes = sizeof(kDetailsLanes) / sizeof(kDetailsLanes[0]);
    for (u8 tried = 0; tried < lanes; ++tried) {
        const u8 index = static_cast<u8>((turn.next + tried) % lanes);
        const RequestLane& lane = kDetailsLanes[index];
        if (admit_details_lane(lane, now)) {
            turn.next = static_cast<u8>((index + 1) % lanes);
            return;
        }
    }
}

void application_message_history_reset_reaction_turn() noexcept {
    reset_value(&reaction_primary_turn);
}
static bool reaction_primary_yield() noexcept {
    auto& turn = reaction_primary_turn;
    const u64 self = g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0;
    if (turn.account_scope != g_app.plugin_account_scope_serial ||
        turn.profile_id != g_app.account.status.profile_id || turn.self_user_id != self) {
        turn = {g_app.plugin_account_scope_serial, g_app.account.status.profile_id, self, false};
    }
    return turn.yield;
}
static bool admit_message_reaction(u64 now_ms) noexcept {
    if (!application_message_reaction_pending(now_ms))
        return false;
    const u64 request_id = next_message_history_request_id();
    if (!application_message_reaction_submit(request_id, now_ms))
        return false;
    g_app.message_history_request_owner = MessageHistoryRequestOwner::message_reaction;
    g_app.primary_request_id = request_id;
    reaction_primary_turn = {g_app.plugin_account_scope_serial, g_app.account.status.profile_id,
                             g_app.account_ui->chat_state.self_user_id, true};
    return true;
}

// An edit or delete the client refuses to send (`log_key` null: access is gone, reported as a 403)
// or whose submit failed is reported to the user.
static void notice_mutation_refused(ApplicationMessageMutationKind kind, u64 channel_id, u64 message_id,
                                    const char* log_key, RestClientSubmitError submit_error) noexcept {
    RestClientResponse response{};
    if (!log_key) {
        response.status_code = 403;
    } else {
        log_u64(LogLevel::error, "rest", log_key, static_cast<u64>(submit_error));
        response.error = submit_error == RestClientSubmitError::account_restricted
                             ? RestClientError::account_restricted
                             : RestClientError::invalid_input;
    }
    application_message_mutation_notice_failed(kind, channel_id, message_id, response);
}

// The message edit request is over without an applied edit: an open editor keeps its text.
static void end_message_edit_request() noexcept {
    if (g_app.message_edit.editor_active)
        g_app.message_edit.phase = MessageEditRequestPhase::idle;
    else
        reset_message_edit();
}

static bool pump_message_history_lower(u64 now_ms) noexcept {
    AccountHost& host = g_app.account;
    MessageHistoryCatchup& catchup = g_app.message_catchup;
    MessageHistoryOlder& older = g_app.message_older;
    MessageEditRuntime& message_edit = g_app.message_edit;
    const bool edit_ready = message_edit.message_id != 0 &&
                            (message_edit.phase == MessageEditRequestPhase::queued ||
                             (message_edit.phase == MessageEditRequestPhase::retry_wait &&
                              message_edit.retry_at_ms != 0 && message_edit.retry_at_ms <= now_ms));
    if (edit_ready) {
        if (!application_message_edit_pending_available()) {
            notice_mutation_refused(ApplicationMessageMutationKind::edit, message_edit.channel_id,
                                    message_edit.message_id, nullptr, {});
            message_edit.retry_at_ms = 0;
            end_message_edit_request();
            return refresh_message_composer_ui();
        }
        const RestClientSubmitResult submitted =
            host.api.message_edit(host.instance, message_edit.channel_id, message_edit.message_id,
                                  Utf8View{message_edit.content, message_edit.content_bytes},
                                  next_message_history_request_id());
        if (submitted.error == RestClientSubmitError::busy) {
            return true;
        }
        if (!submitted.ok()) {
            notice_mutation_refused(ApplicationMessageMutationKind::edit, message_edit.channel_id,
                                    message_edit.message_id, "message_edit_submit_error", submitted.error);
            end_message_edit_request();
            return refresh_message_composer_ui();
        }
        message_edit.phase = MessageEditRequestPhase::submitting;
        message_edit.retry_at_ms = 0;
        g_app.message_history_request_owner = MessageHistoryRequestOwner::message_edit;
        return true;
    }

    if (g_app.message_delete.message_id != 0 &&
        (g_app.message_delete.retry_at_ms == 0 || g_app.message_delete.retry_at_ms <= now_ms)) {
        const u64 channel_id = g_app.message_delete.channel_id, message_id = g_app.message_delete.message_id;
        if (!application_message_delete_pending_available()) {
            notice_mutation_refused(ApplicationMessageMutationKind::erase, channel_id, message_id, nullptr, {});
            reset_message_delete();
            return refresh_account_text(true);
        }
        const RestClientSubmitResult submitted = host.api.message_delete(
            host.instance, channel_id, message_id, next_message_history_request_id());
        if (submitted.error == RestClientSubmitError::busy) {
            return true;
        }
        if (!submitted.ok()) {
            notice_mutation_refused(ApplicationMessageMutationKind::erase, channel_id, message_id,
                                    "message_delete_submit_error", submitted.error);
            reset_message_delete();
            return refresh_account_text(true);
        }
        g_app.message_history_request_owner = MessageHistoryRequestOwner::message_delete;
        return true;
    }

    if (!application_message_forward_synchronize())
        return false;
    application_message_send_captcha_resume();
    MessageComposerSubmit message_submit;
    const MessageComposerControllerResult prepared = message_composer_controller_prepare_submit(
        &g_app.account_ui->message_composer_controller, now_ms, &message_submit);
    if (prepared.ok()) {
        if (!application_thread_actions_channel_sendable(message_submit.channel_id) ||
            (message_submit.reply_message_id &&
             !application_channel_readable(g_app, message_submit.channel_id))) {
            // Preserve the retained outbox text and nonce as failed. Neither
            // this refusal nor a later explicit retry may bypass fresh access.
            const auto failed = message_composer_controller_submit_failed(
                &g_app.account_ui->message_composer_controller, message_submit.nonce, now_ms);
            g_app.message_rejection = {50013, true, false};
            g_app.message_rejection_channel_id = message_submit.channel_id;
            return failed.ok() && refresh_account_text(true);
        }
        const u64 request_id = next_message_history_request_id();
        application_action_captcha_arm(ApplicationActionCaptchaKind::message_send, message_submit.nonce, request_id,
                                       false);
        const RestClientSubmitResult submitted =
            message_submit.forward_reference.message_id
                ? host.api.message_forward(host.instance, message_submit.channel_id, message_submit.content,
                                           message_submit.nonce, message_submit.forward_reference,
                                           request_id)
                : host.api.message_submit(host.instance, message_submit.channel_id, message_submit.content,
                                          message_submit.nonce, message_submit.reply_message_id,
                                          {message_submit.attachments, message_submit.attachment_count},
                                          {message_submit.sticker_ids, message_submit.sticker_count},
                                          request_id, message_submit.mention_reply_author,
                                          (message_submit.suppress_notifications
                                               ? kMessageFlagSuppressNotifications
                                               : 0u) |
                                              // A recorded voice message is its only attachment.
                                              (message_submit.attachment_count == 1 &&
                                                       attachment_upload_is_voice(message_submit.attachments[0])
                                                   ? kMessageFlagVoiceMessage
                                                   : 0u));
        if (submitted.error == RestClientSubmitError::busy) {
            return true;
        }
        if (!submitted.ok()) {
            const MessageComposerControllerResult failed = message_composer_controller_submit_failed(
                &g_app.account_ui->message_composer_controller, message_submit.nonce, now_ms);
            log_u64(LogLevel::error, "rest", "message_send_submit_error",
                    static_cast<u64>(submitted.error));
            return failed.ok() && refresh_account_text(true);
        }
        const MessageComposerControllerResult marked = message_composer_controller_mark_submitted(
            &g_app.account_ui->message_composer_controller, message_submit.nonce);
        if (!marked.ok()) {
            log_u64(LogLevel::error, "message", "message_send_mark_submitted_error",
                    static_cast<u64>(marked.error));
            return false;
        }
        g_app.message_history_request_owner = MessageHistoryRequestOwner::message_send;
        return true;
    }
    if (prepared.error == MessageComposerControllerError::reply_unavailable)
        return refresh_account_text(true);
    if (prepared.error != MessageComposerControllerError::not_found) {
        log_u64(LogLevel::error, "message", "message_send_prepare_error", static_cast<u64>(prepared.error));
        return false;
    }

    if (application_attachment_url_pending(now_ms)) {
        bool submitted = false;
        if (!application_attachment_submit_url(next_message_history_request_id(), now_ms, &submitted))
            return false;
        if (submitted) {
            g_app.message_history_request_owner = MessageHistoryRequestOwner::attachment_upload;
            return true;
        }
    }
    const TypingPost typing_post =
        typing_store_take_local_post(&g_app.account_ui->typing_store, now_ms);
    if (typing_post.ready && typing_post.suppressed) {
        log_write(LogLevel::info, "typing", "post_suppressed_busy_channel");
    } else if (typing_post.ready && typing_post.channel_id != 0) {
        const u64 request_id = next_message_history_request_id();
        const RestClientSubmitResult submitted =
            host.api.message_typing(host.instance, typing_post.channel_id, request_id);
        if (!submitted.ok()) {
            log_u64(LogLevel::warning, "typing", "submit_error", static_cast<u64>(submitted.error));
        } else {
            g_app.message_history_request_owner = MessageHistoryRequestOwner::typing;
            return true;
        }
    }

    // Explicit result activation keeps its jump priority; ordinary search
    // submissions precede passive history paging and media URL refreshes.
    if (admit_primary_lanes(0, kPrimaryLaneSearch, now_ms) ||
        (!g_app.pending_message_jump_message_id &&
         admit_primary_lanes(kPrimaryLaneSearch, kPrimaryLaneMedia, now_ms)) ||
        admit_primary_lanes(kPrimaryLaneMedia, kPrimaryLaneFriends, now_ms))
        return true;
    if (application_thread_browser_read_blocked())
        return true;
    MessageHistoryQuery query{};
    u64 channel_id = 0;
    MessageHistoryRequestOwner owner = MessageHistoryRequestOwner::none;
    if (g_app.pending_message_jump_message_id != 0 &&
        g_app.pending_message_jump_channel_id == g_app.account_ui->active_channel_id) {
        query =
            MessageHistoryQuery{g_app.pending_message_jump_message_id, kMessageHistoryRequestLimit,
                                MessageHistoryPosition::around};
        channel_id = g_app.pending_message_jump_channel_id;
        owner = MessageHistoryRequestOwner::message_jump;
    } else if (catchup.state == MessageHistoryCatchupState::request_required) {
        if (!message_history_catchup_take_query(&catchup, &query)) {
            return false;
        }
        channel_id = catchup.channel_id;
        owner = MessageHistoryRequestOwner::catchup;
    } else if (message_history_browsing_ready(catchup.state) &&
               g_app.attachment_refresh.state == AttachmentRefreshState::request_required) {
        query = MessageHistoryQuery{g_app.attachment_refresh.message_id, 1,
                                    MessageHistoryPosition::around};
        channel_id = g_app.attachment_refresh.channel_id;
        owner = MessageHistoryRequestOwner::attachment_refresh;
    } else if (message_history_browsing_ready(catchup.state) &&
               g_app.reply_refresh.state == AttachmentRefreshState::request_required) {
        query = {g_app.reply_refresh.message_id, 1, MessageHistoryPosition::around};
        channel_id = g_app.reply_refresh.channel_id;
        owner = MessageHistoryRequestOwner::reply_refresh;
    } else if (message_history_browsing_ready(catchup.state) &&
               older.state == MessageHistoryOlderState::request_required) {
        if (!message_history_older_take_query(&older, &query)) {
            return false;
        }
        channel_id = older.channel_id;
        owner = MessageHistoryRequestOwner::older;
    }
    if (owner != MessageHistoryRequestOwner::none) {
        if (!application_channel_readable(g_app, channel_id)) {
            if (owner == MessageHistoryRequestOwner::attachment_refresh)
                message_history_attachment_reset(&g_app.attachment_refresh);
            if (owner == MessageHistoryRequestOwner::reply_refresh)
                reset_value(&g_app.reply_refresh);
            return true;
        }
        const u64 request_id = next_message_history_request_id();
        const RestClientSubmitResult submitted =
            host.api.history_submit(host.instance, channel_id, &query, request_id);
        if (!submitted.ok()) {
            const bool busy = submitted.error == RestClientSubmitError::busy;
            if (owner == MessageHistoryRequestOwner::message_jump &&
                (application_search_navigation_history_admission_failed(channel_id, query.cursor, busy) || busy))
                return true;
            if (owner == MessageHistoryRequestOwner::catchup) {
                message_history_catchup_fail(&catchup, MessageHistoryCatchupFailure::transport);
            } else if (owner == MessageHistoryRequestOwner::older) {
                message_history_older_fail(&older, MessageHistoryCatchupFailure::transport);
            } else if (owner == MessageHistoryRequestOwner::reply_refresh) {
                give_up_reply_refresh();
                return true;
            } else {
                message_history_attachment_reset(&g_app.attachment_refresh);
            }
            log_u64(LogLevel::error, "rest", "message_history_submit_error",
                    static_cast<u64>(submitted.error));
            return false;
        }
        if (owner == MessageHistoryRequestOwner::attachment_refresh) {
            g_app.attachment_refresh.state = AttachmentRefreshState::awaiting_response;
            ++g_app.attachment_refresh.requests;
        }
        if (owner == MessageHistoryRequestOwner::reply_refresh)
            g_app.reply_refresh.state = AttachmentRefreshState::awaiting_response;
        g_app.message_history_request_owner = owner;
        if (owner == MessageHistoryRequestOwner::message_jump)
            application_search_navigation_history_submitted(channel_id, query.cursor, request_id);
    } else if (auto* entry = media_url_next(&g_app.media_urls, now_ms)) {
        if (!application_media_url_authorized(g_app, {entry->original, entry->original_bytes})) {
            reset_value(entry);
            return true;
        }
        // Resolving media must not starve sends, interactions, or channel history.
        const u64 request_id = next_message_history_request_id();
        const auto submitted =
            host.api.media_url_refresh(host.instance, {entry->original, entry->original_bytes}, request_id);
        if (submitted.ok()) {
            entry->state = MediaUrlState::pending;
            entry->retry_at_ms = 0;
            ++entry->attempts;
            g_app.media_urls.pending_slot = static_cast<u32>(entry - g_app.media_urls.entries) + 1;
            g_app.message_history_request_owner = MessageHistoryRequestOwner::media_url_refresh;
        } else if (submitted.error != RestClientSubmitError::busy) {
            entry->state = MediaUrlState::failed;
        }
    } else if (const u64 prefetch_channel = application_channel_prefetch_take(now_ms)) {
        // Speculative hover prefetch: only when the lane has no other work.
        const u64 request_id = next_message_history_request_id();
        const MessageHistoryQuery prefetch_query{0, kMessageHistoryRequestLimit,
                                                 MessageHistoryPosition::latest};
        const auto submitted = host.api.history_submit(host.instance, prefetch_channel, &prefetch_query, request_id);
        if (submitted.ok()) {
            application_channel_prefetch_submitted(prefetch_channel, request_id, now_ms);
            g_app.message_history_request_owner = MessageHistoryRequestOwner::channel_prefetch;
        } else if (submitted.error != RestClientSubmitError::busy) {
            application_channel_prefetch_skip();
        }
    }
    return true;
}

// REST response consumers by request owner. A response that could not be taken reaches them as a
// transport failure with an empty body.
using ResponseConsumer = bool (*)(const RestClientResponse&, Utf8View body) noexcept;
using TimedResponseConsumer = bool (*)(const RestClientResponse&, Utf8View body, u64 now_ms) noexcept;
struct ResponseRoute {
    MessageHistoryRequestOwner owner;
    bool lenient; // A failed take never stops the pump.
    bool timed;
    union {
        ResponseConsumer plain;
        TimedResponseConsumer with_time;
    };

    constexpr ResponseRoute(MessageHistoryRequestOwner o, ResponseConsumer c, bool l = false) noexcept
        : owner(o), lenient(l), timed(false), plain(c) {}
    constexpr ResponseRoute(MessageHistoryRequestOwner o, TimedResponseConsumer c) noexcept
        : owner(o), lenient(false), timed(true), with_time(c) {}
    bool consume(const RestClientResponse& response, Utf8View body, u64 now_ms) const noexcept {
        return timed ? with_time(response, body, now_ms) : plain(response, body);
    }
};

static bool route_activity_sharing(const RestClientResponse& response, Utf8View body, u64) noexcept {
    application_activity_sharing_response(response, body);
    return true;
}

static bool route_read_state(const RestClientResponse& response, Utf8View body, u64 now_ms) noexcept {
    return application_read_state_response(response, reinterpret_cast<const u8*>(body.data), body.bytes,
                                           now_ms);
}

static bool route_scoped_read(const RestClientResponse& response, Utf8View body, u64 now_ms) noexcept {
    return application_scoped_read_response(response, body, now_ms);
}

static bool route_message_reaction(const RestClientResponse& response, Utf8View, u64) noexcept {
    return application_message_reaction_response(response);
}

static constexpr ResponseRoute kResponseRoutes[] = {
    {MessageHistoryRequestOwner::channel_prefetch, application_channel_prefetch_response},
    {MessageHistoryRequestOwner::forum_post, application_forum_post_response},
    {MessageHistoryRequestOwner::thread_creation, application_thread_creation_response},
    {MessageHistoryRequestOwner::thread_browser, application_thread_browser_response},
    {MessageHistoryRequestOwner::poll, application_poll_response},
    {MessageHistoryRequestOwner::member_moderation, application_member_moderation_response},
    {MessageHistoryRequestOwner::guild_invite, application_guild_invite_response},
    {MessageHistoryRequestOwner::thread_actions, application_thread_actions_response},
    {MessageHistoryRequestOwner::search_navigation, application_search_navigation_response},
    {MessageHistoryRequestOwner::channel_overview, application_channel_overview_response},
    {MessageHistoryRequestOwner::pinned_messages, application_pinned_messages_response},
    {MessageHistoryRequestOwner::self_status, application_self_status_response},
    {MessageHistoryRequestOwner::user_settings, application_user_settings_response},
    {MessageHistoryRequestOwner::guild_notification_settings, application_notification_controls_response},
    {MessageHistoryRequestOwner::custom_status, application_custom_status_response},
    {MessageHistoryRequestOwner::activity_sharing, route_activity_sharing},
    {MessageHistoryRequestOwner::inbox_mentions, application_inbox_response},
    {MessageHistoryRequestOwner::stream_preview, application_stream_preview_response},
    {MessageHistoryRequestOwner::stage, application_stage_response},
    {MessageHistoryRequestOwner::soundboard, application_soundboard_response},
    {MessageHistoryRequestOwner::message_components, application_message_components_response},
    {MessageHistoryRequestOwner::invite_cards, application_invite_cards_response},
    {MessageHistoryRequestOwner::account_settings, application_account_settings_response},
    {MessageHistoryRequestOwner::private_channel, application_private_channel_response},
    {MessageHistoryRequestOwner::media_catalog, application_media_picker_response},
    {MessageHistoryRequestOwner::expression_card, application_expression_card_response},
    {MessageHistoryRequestOwner::interaction_data, application_interaction_card_response},
    {MessageHistoryRequestOwner::gif_favorites, application_gif_favorites_response},
    {MessageHistoryRequestOwner::emoji_preferences, application_emoji_preferences_response},
    {MessageHistoryRequestOwner::message_search, application_search_response},
    {MessageHistoryRequestOwner::read_state_ack, route_read_state},
    {MessageHistoryRequestOwner::scoped_read, route_scoped_read},
    {MessageHistoryRequestOwner::friends, application_friends_response},
    {MessageHistoryRequestOwner::commands, application_commands_response},
    {MessageHistoryRequestOwner::user_profile, application_profile_response, true},
    {MessageHistoryRequestOwner::dm_profile, application_dm_profile_response, true},
    {MessageHistoryRequestOwner::message_reaction, route_message_reaction},
};

static const ResponseRoute* find_response_route(MessageHistoryRequestOwner owner) noexcept {
    for (const auto& entry : kResponseRoutes)
        if (entry.owner == owner)
            return &entry;
    return nullptr;
}

static bool pump_message_history_main(bool admit) noexcept {
    AccountHost& host = g_app.account;
    MessageHistoryCatchup& catchup = g_app.message_catchup;
    MessageHistoryOlder& older = g_app.message_older;
    // Before READY only a startup shell takes its prefetched page.
    const bool early = !host.status.running;
    if (!g_app.account_ui || !host.instance ||
        (early && (!g_app.startup_shell_provisional ||
                   g_app.message_history_request_owner != MessageHistoryRequestOwner::startup_history))) {
        return true;
    }
    const u64 now_ms = clock_monotonic_milliseconds();
    if (!early) {
        if (!application_commands_tick(now_ms))
            return false;
        application_message_components_tick(now_ms);
        bool send_changed = false;
        if (!message_composer_controller_tick(&g_app.account_ui->message_composer_controller, now_ms,
                                              &send_changed) ||
            !consume_message_send_completions()) {
            return false;
        }
        if (send_changed && g_app.window) {
            application_request_frame();
        }
        if (catchup.state == MessageHistoryCatchupState::waiting_retry) {
            message_history_catchup_tick(&catchup, now_ms);
        }
        if (older.state == MessageHistoryOlderState::waiting_retry) {
            message_history_older_tick(&older, now_ms);
        }
        message_history_attachment_tick(&g_app.attachment_refresh, now_ms);
        const auto* reply_channel = chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                                     g_app.account_ui->active_channel_id);
        if (!application_thread_browser_read_blocked() && reply_channel &&
            (reply_channel->actions & channel_action_read_history) &&
            g_app.message_history_request_owner != MessageHistoryRequestOwner::reply_refresh)
            message_history_reply_queue(&g_app.reply_refresh, g_app.message_view.timeline, reply_channel->id,
                                        now_ms);
    }
    // A startup page held until the restoration settled is presented as a
    // PRIMARY response once it has.
    const bool startup_replay = g_app.message_history_request_owner == MessageHistoryRequestOwner::none &&
                                application_startup_history_replay_due();
    if (g_app.message_history_request_owner != MessageHistoryRequestOwner::none ||
        g_app.details_request_owner != MessageHistoryRequestOwner::none || startup_replay) {
        const bool emoji_buffer =
            g_app.message_history_request_owner == MessageHistoryRequestOwner::emoji_preferences;
        const bool favorites_buffer =
            emoji_buffer || g_app.message_history_request_owner == MessageHistoryRequestOwner::gif_favorites;
        Slice<u8> body_output = emoji_buffer ? application_emoji_preferences_response_buffer()
                                : favorites_buffer
                                    ? application_gif_favorites_response_buffer()
                                    : Slice<u8>{g_app.account_ui->response_shape_scratch.response,
                                                kAccountHistoryResponseBytes};
        if (!body_output.data || !body_output.count)
            return false;
        RestClientResponse response{};
        RestClientTakeResult taken = startup_replay
                                         ? application_startup_history_replay(&response, body_output)
                                         : host.api.rest_take(host.instance, &response, body_output);
        // Declared before the erase guard so the bank is erased, then released.
        LargeResponseBank large{};
        if (taken.error == RestClientTakeError::output_too_small && large.allocate(taken.body_bytes)) {
            body_output = {large.data, large.bytes};
            taken = host.api.rest_take(host.instance, &response, body_output);
        }
        if (taken.error == RestClientTakeError::empty) {
            if (g_app.message_history_request_owner != MessageHistoryRequestOwner::none)
                return true;
        }
        if (taken.error != RestClientTakeError::empty) {
            const MessageHistoryResponseErase erase{
                body_output.data, minimum(taken.body_bytes, body_output.count), favorites_buffer};
            const bool details =
                response.request_id != 0 && response.request_id == g_app.details_request_id;
            if (!details && response.request_id != g_app.primary_request_id) {
                log_write(LogLevel::error, "rest", "unowned_response");
                return false;
            }
            MessageHistoryRequestOwner owner =
                details ? g_app.details_request_owner : g_app.message_history_request_owner;
            const i64 apply_started = clock_now();
            const u64 expected_id = details ? g_app.details_request_id : g_app.primary_request_id;
            if (details) {
                g_app.details_request_owner = MessageHistoryRequestOwner::none;
                g_app.details_request_id = 0;
            } else {
                g_app.message_history_request_owner = MessageHistoryRequestOwner::none;
                g_app.primary_request_id = 0;
            }
            // The restored channel's startup page is its latest-page catch-up.
            if (owner == MessageHistoryRequestOwner::startup_history) {
                const bool usable = taken.ok() && response.request_id == expected_id &&
                                    response.body_bytes == taken.body_bytes;
                const Utf8View startup_body =
                    usable ? Utf8View{reinterpret_cast<const char*>(body_output.data), taken.body_bytes}
                           : Utf8View{};
                if (!application_startup_history_take(response, startup_body))
                    return true;
                owner = MessageHistoryRequestOwner::catchup;
            }
            // A hovered channel that was selected meanwhile takes its prefetch as
            // the latest-page catch-up instead of issuing a second request.
            if (owner == MessageHistoryRequestOwner::channel_prefetch &&
                application_channel_prefetch_adopt(response.request_id))
                owner = MessageHistoryRequestOwner::catchup;
            if (!taken.ok() || response.request_id != expected_id ||
                response.body_bytes != taken.body_bytes) {
                if (owner == MessageHistoryRequestOwner::channel_prefetch)
                    return application_channel_prefetch_response(response, {});
                if (const ResponseRoute* handler = find_response_route(owner)) {
                    response.error = RestClientError::response;
                    return handler->consume(response, {}, now_ms) || handler->lenient;
                }
                if (owner == MessageHistoryRequestOwner::private_conversation) {
                    fail_private_conversation();
                } else if (owner == MessageHistoryRequestOwner::media_url_refresh) {
                    response.error = RestClientError::response;
                    media_url_complete(&g_app.media_urls, response, {}, now_ms,
                                       clock_unix_milliseconds() / 1000);
                    return true;
                }
                if (owner == MessageHistoryRequestOwner::attachment_upload) {
                    application_attachment_fail_url();
                    return refresh_message_composer_ui();
                }
                if (owner == MessageHistoryRequestOwner::typing) {
                    log_write(LogLevel::warning, "typing", "response_take_failed");
                    return true;
                } else if (owner == MessageHistoryRequestOwner::message_send) {
                    MessageComposerController& controller = g_app.account_ui->message_composer_controller;
                    if (controller.inflight_nonce != 0) {
                        message_composer_controller_submit_failed(&controller, controller.inflight_nonce,
                                                                  clock_monotonic_milliseconds());
                    }
                    refresh_account_text(true);
                } else if (owner == MessageHistoryRequestOwner::message_delete) {
                    reset_message_delete();
                } else if (owner == MessageHistoryRequestOwner::message_edit) {
                    end_message_edit_request();
                    refresh_message_composer_ui();
                } else if (owner == MessageHistoryRequestOwner::reaction_users) {
                    const ReactionViewerControllerResult failed = reaction_viewer_controller_submit_failed(
                        &g_app.reaction_viewer_controller, clock_monotonic_milliseconds());
                    log_u64(LogLevel::warning, "reaction_viewer", "response_take_error",
                            static_cast<u64>(failed.error));
                    refresh_account_text(true);
                } else if (owner == MessageHistoryRequestOwner::reaction_tooltip) {
                    application_reaction_tooltip_failed();
                } else if (owner == MessageHistoryRequestOwner::message_jump) {
                    response.error = RestClientError::response;
                    fail_message_jump(response, MessageHistoryRestError::transport);
                } else if (owner == MessageHistoryRequestOwner::catchup &&
                           catchup.state == MessageHistoryCatchupState::awaiting_page) {
                    message_history_catchup_fail(&catchup, MessageHistoryCatchupFailure::transport);
                } else if (owner == MessageHistoryRequestOwner::older &&
                           older.state == MessageHistoryOlderState::awaiting_page) {
                    message_history_older_fail(&older, MessageHistoryCatchupFailure::transport);
                } else if (owner == MessageHistoryRequestOwner::attachment_refresh) {
                    message_history_attachment_reset(&g_app.attachment_refresh);
                } else if (owner == MessageHistoryRequestOwner::reply_refresh) {
                    give_up_reply_refresh();
                }
                log_write(LogLevel::error, "rest", "message_history_take_failed");
                return false;
            }
            if (response.diagnostics.completions != 0) {
                char line[384]{};
                ProtocolWriter writer{{reinterpret_cast<u8*>(line), sizeof(line) - 1}, 0};
                const auto field = [&](const char* label, u64 value) noexcept {
                    protocol_writer_append_c_string(&writer, label);
                    protocol_writer_append_u64(&writer, value);
                };
                field("completion request=", response.request_id);
                field(" route=", response.diagnostics.route_id);
                field(" method=", static_cast<u64>(response.diagnostics.method));
                field(" status=", response.status_code);
                field(" transport=", static_cast<u64>(response.error));
                field(" has_code=", response.rejection.has_code);
                field(" code=", response.rejection.code);
                field(" challenge=", response.rejection.captcha);
                field(" attempts=", response.diagnostics.attempts);
                field(" completions=", response.diagnostics.completions);
                log_write(response.error != RestClientError::none || response.status_code >= 400
                              ? LogLevel::warning
                              : LogLevel::info,
                          "rest", line);
            }

            const Utf8View taken_body{reinterpret_cast<const char*>(body_output.data), taken.body_bytes};
            if (const ResponseRoute* handler = find_response_route(owner)) {
                if (!handler->consume(response, taken_body, now_ms))
                    return false;
            } else if (owner == MessageHistoryRequestOwner::attachment_upload) {
                if (!application_attachment_accept_url(response, taken_body))
                    return false;
            } else if (owner == MessageHistoryRequestOwner::private_conversation) {
                if (!accept_private_conversation(response, taken_body))
                    return false;
            } else if (owner == MessageHistoryRequestOwner::media_url_refresh) {
                const u32 slot = g_app.media_urls.pending_slot;
                media_url_complete(&g_app.media_urls, response, taken_body, now_ms,
                                   clock_unix_milliseconds() / 1000);
                // One record per bounded REST attempt, never the URL/signature or body.
                if (slot != 0 && slot <= 32) {
                    const auto state = g_app.media_urls.entries[slot - 1].state;
                    log_write(state == MediaUrlState::failed ? LogLevel::warning : LogLevel::info, "media",
                              state == MediaUrlState::ready    ? "attachment_url_refreshed"
                              : state == MediaUrlState::queued ? "attachment_url_refresh_deferred"
                                                               : "attachment_url_refresh_failed");
                    if (state != MediaUrlState::ready)
                        log_u64(LogLevel::warning, "media", "attachment_url_refresh_status",
                                response.status_code);
                }
                message_media_controller_defer_scan(&g_app.message_media);
            } else if (owner == MessageHistoryRequestOwner::typing) {
                if (!response.transport_ok()) {
                    log_u64(LogLevel::warning, "typing", "transport_error",
                            static_cast<u64>(response.error));
                } else if (response.status_code != 200 && response.status_code != 204) {
                    log_u64(LogLevel::warning, "typing", "http_status", response.status_code);
                }
            } else if (owner == MessageHistoryRequestOwner::message_send) {
                if (!accept_message_send_response(response, taken_body))
                    return false;
            } else if (owner == MessageHistoryRequestOwner::message_delete) {
                if (!accept_message_delete_response(response)) {
                    return false;
                }
            } else if (owner == MessageHistoryRequestOwner::message_edit) {
                if (!accept_message_edit_response(response)) {
                    return false;
                }
            } else if (owner == MessageHistoryRequestOwner::reaction_users) {
                const ReactionViewerControllerResult accepted = reaction_viewer_controller_accept_response(
                    &g_app.reaction_viewer_controller, response,
                    ConstSlice<u8>{body_output.data, response.body_bytes},
                    Slice<u32>{g_app.account_ui->message_history_page_structurals,
                               kMessageHistoryPageStructuralCount},
                    Slice<u32>{g_app.account_ui->message_history_object_structurals,
                               kMessageHistoryObjectStructuralCount});
                if (!accepted.ok()) {
                    const ReactionViewerController& controller = g_app.reaction_viewer_controller;
                    const u64 failed[] = {static_cast<u64>(accepted.error), controller.last_http_status,
                                          static_cast<u64>(controller.last_transport_error),
                                          static_cast<u64>(controller.last_decode_error),
                                          controller.last_decode_offset};
                    log_u64_fields(LogLevel::warning, "reaction_viewer",
                                   "response_error\0http_status\0transport_error\0decode_error\0"
                                   "decode_error_offset",
                                   failed);
                } else if (accepted.stale) {
                    log_write(LogLevel::info, "reaction_viewer", "stale_response_discarded");
                }
                if (!refresh_account_text(true) || (g_app.window && !application_request_frame())) {
                    return false;
                }
            } else if (owner == MessageHistoryRequestOwner::reaction_tooltip) {
                // The tooltip shows the names at its next synchronize.
                application_reaction_tooltip_response(
                    response, ConstSlice<u8>{body_output.data, response.body_bytes},
                    Slice<u32>{g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount},
                    Slice<u32>{g_app.account_ui->message_history_object_structurals,
                               kMessageHistoryObjectStructuralCount});
                if (g_app.window && !application_request_frame())
                    return false;
            } else {
                const u64 history_channel =
                    owner == MessageHistoryRequestOwner::catchup ? catchup.channel_id
                    : owner == MessageHistoryRequestOwner::older ? older.channel_id
                    : owner == MessageHistoryRequestOwner::attachment_refresh
                        ? g_app.attachment_refresh.channel_id
                    : owner == MessageHistoryRequestOwner::reply_refresh ? g_app.reply_refresh.channel_id
                    : owner == MessageHistoryRequestOwner::message_jump
                        ? g_app.pending_message_jump_channel_id
                        : 0;
                const bool history_readable =
                    history_channel && history_channel == g_app.account_ui->active_channel_id &&
                    history_channel == g_app.account_ui->chat_state.selected_channel_id &&
                    application_channel_readable(g_app, history_channel);
                // A navigation response must match the channel session that issued it
                // before decoding or replacing any cached history.
                // Matching IDs alone do not cover leaving and returning to it.
                const bool jump_current = owner != MessageHistoryRequestOwner::message_jump ||
                                          application_message_navigation_history_current(
                                              response.request_id, g_app.pending_message_jump_channel_id,
                                              g_app.pending_message_jump_message_id);
                const bool expected =
                    history_readable && jump_current &&
                    ((owner == MessageHistoryRequestOwner::catchup &&
                      catchup.state == MessageHistoryCatchupState::awaiting_page) ||
                     (owner == MessageHistoryRequestOwner::message_jump &&
                      g_app.pending_message_jump_channel_id == g_app.account_ui->active_channel_id &&
                      g_app.pending_message_jump_message_id != 0) ||
                     (owner == MessageHistoryRequestOwner::attachment_refresh &&
                      g_app.attachment_refresh.state == AttachmentRefreshState::awaiting_response) ||
                     (owner == MessageHistoryRequestOwner::reply_refresh &&
                      g_app.reply_refresh.channel_id == g_app.account_ui->active_channel_id &&
                      g_app.reply_refresh.state == AttachmentRefreshState::awaiting_response) ||
                     (owner == MessageHistoryRequestOwner::older &&
                      older.state == MessageHistoryOlderState::awaiting_page));
                if (!history_readable) {
                    // The response worker has finished above; cancel only its still-matching
                    // history request before touching body scratch.
                    if (owner == MessageHistoryRequestOwner::catchup &&
                        catchup.state == MessageHistoryCatchupState::awaiting_page) {
                        catchup.state = MessageHistoryCatchupState::paused;
                        catchup.retry_at_ms = 0;
                    }
                    if (owner == MessageHistoryRequestOwner::older &&
                        older.state == MessageHistoryOlderState::awaiting_page) {
                        older.state = MessageHistoryOlderState::idle;
                        older.retry_at_ms = 0;
                    }
                    if (owner == MessageHistoryRequestOwner::attachment_refresh)
                        message_history_attachment_reset(&g_app.attachment_refresh);
                    if (owner == MessageHistoryRequestOwner::reply_refresh)
                        reset_value(&g_app.reply_refresh);
                    if (owner == MessageHistoryRequestOwner::message_jump)
                        g_app.pending_message_jump_channel_id = g_app.pending_message_jump_message_id = 0;
                }
                MessageHistoryRestResult accepted{};
                if (expected) {
                    const Utf8View body{reinterpret_cast<const char*>(body_output.data),
                                        response.body_bytes};
                    MessageHistoryDecodeScratch scratch{};
                    if (owner == MessageHistoryRequestOwner::catchup) {
                        if (!application_message_history_catchup_response(
                                *g_app.account_ui, catchup, g_app.message_pump, response, body, now_ms,
                                &accepted, &scratch)) {
                            log_write(LogLevel::error, "message",
                                      "history_decode_storage_admission_failed");
                            return false;
                        }
                    } else {
                        if (response.transport_ok() && response.status_code == 200 && body.data &&
                            body.bytes && body.bytes == response.body_bytes &&
                            !application_message_decode_scratch(*g_app.account_ui, &scratch, body)) {
                            if (owner == MessageHistoryRequestOwner::message_jump) {
                                fail_message_jump(response, MessageHistoryRestError::decode);
                                return true;
                            }
                            log_write(LogLevel::error, "message",
                                      "history_decode_storage_admission_failed");
                            return false;
                        }
                        if (owner == MessageHistoryRequestOwner::older) {
                            accepted = message_history_older_accept_rest(
                                &older, &g_app.message_view.timeline, &g_app.account_ui->user_store,
                                &g_app.account_ui->chat_state, &g_app.account_ui->guild_member_store,
                                response, body, scratch, MessageContentAvailability::unknown, now_ms);
                        } else if (owner == MessageHistoryRequestOwner::message_jump) {
                            accepted = message_history_jump_accept_rest(
                                &g_app.message_view.timeline, &g_app.account_ui->user_store,
                                &g_app.account_ui->guild_member_store, &g_app.account_ui->chat_state,
                                response, body, g_app.pending_message_jump_channel_id,
                                g_app.pending_message_jump_message_id, scratch,
                                MessageContentAvailability::unknown);
                            if (accepted.accepted()) {
                                // The replaced window owns fresh before/after cursors; a
                                // window short of the present stays detached until forward
                                // paging (or Jump to Present) reaches the live tail.
                                const ConstSlice<GatewayMessageEvent> page{scratch.messages.data,
                                                                           accepted.decode.page.count};
                                const ChannelRecord* jumped = chat_state_store_find_channel(
                                    g_app.account_ui->chat_state, g_app.pending_message_jump_channel_id);
                                const MessageHistoryJumpEdges edges = message_history_jump_edges(
                                    page, g_app.pending_message_jump_message_id, kMessageHistoryRequestLimit,
                                    jumped ? jumped->last_message_id : 0);
                                if (!message_history_jump_settle(&catchup, &older, &g_app.message_pump,
                                                                 g_app.pending_message_jump_channel_id, page,
                                                                 edges) ||
                                    !scroll_to_resident_message(g_app.pending_message_jump_message_id)) {
                                    accepted.error = MessageHistoryRestError::apply;
                                    accepted.disposition = MessageHistoryRestDisposition::failed;
                                }
                            }
                            end_message_jump(response, accepted);
                        } else if (owner == MessageHistoryRequestOwner::reply_refresh) {
                            accepted = message_history_reply_accept_rest(
                                &g_app.reply_refresh, &g_app.message_view.timeline,
                                &g_app.account_ui->user_store, response, body, scratch);
                        } else {
                            accepted = message_history_attachment_accept_rest(
                                &g_app.attachment_refresh, &g_app.message_view.timeline, response, body,
                                scratch, MessageContentAvailability::unknown);
                        }
                    }
                    if (accepted.accepted() && accepted.apply.ok() &&
                        accepted.disposition == MessageHistoryRestDisposition::applied) {
                        // Degraded but loaded: one diagnostic per affected page.
                        if (accepted.decode.degraded_messages)
                            log_u64(LogLevel::warning, "rest", "message_history_rich_decode_degraded",
                                    accepted.decode.degraded_messages);
                        if (accepted.apply.skipped)
                            log_u64(LogLevel::warning, "rest", "message_history_messages_skipped",
                                    accepted.apply.skipped);
                        if (accepted.apply.rich_skipped)
                            log_u64(LogLevel::warning, "rest", "message_history_rich_skipped",
                                    accepted.apply.rich_skipped);
                        // Counts only: a page that leaves decoded messages out of the window.
                        if (accepted.apply.inserted + accepted.apply.resident < accepted.decode.page.count) {
                            TraceRecord shortfall("history_shortfall");
                            shortfall.field("request", response.request_id);
                            shortfall.field("work", rest_owner_name(owner));
                            const u64 fields[] = {accepted.decode.page.count, accepted.apply.inserted,
                                                  accepted.apply.resident, accepted.apply.skipped,
                                                  accepted.apply.evicted};
                            trace_fields(shortfall, "decoded\0inserted\0resident\0skipped\0evicted", fields);
                            log_write(LogLevel::warning, "rest", shortfall.text);
                        }
                        MessagePollPageInput inputs[kMessageHistoryRequestLimit]{};
                        MessagePollResultTicket tickets[kMessageHistoryRequestLimit]{};
                        u32 poll_count = 0;
                        auto& ui = *g_app.account_ui;
                        for (u32 i = 0; i < accepted.decode.page.count; ++i) {
                            const auto& message = scratch.messages[i];
                            if (!(message.fields & gateway_message_field_poll))
                                continue;
                            const auto* existing =
                                message_poll_store_find(ui.message_poll_store, message.id);
                            if (existing && existing->last_gateway_serial > ui.message_history_poll_serial)
                                continue;
                            tickets[poll_count] = message_poll_store_result_ticket(
                                ui.message_poll_store, message.id, ui.message_history_poll_serial);
                            inputs[poll_count] = {message.id, message.channel_id, &message.poll, 0,
                                                  &tickets[poll_count]};
                            ++poll_count;
                        }
                        const auto applied_polls =
                            message_poll_store_apply_page(&ui.message_poll_store, {inputs, poll_count});
                        if (!applied_polls.ok()) {
                            log_write(LogLevel::warning, "poll", "history_poll_publish_failed");
                        }
                        if (!application_poll_synchronize())
                            return false;
                        const u64 channel_id = g_app.account_ui->active_channel_id;
                        if (channel_id &&
                            channel_id == g_app.account_ui->chat_state.selected_channel_id) {
                            u64 newest_id = 0;
                            for (u32 index = 0; index < accepted.decode.page.count; ++index) {
                                const auto& message = scratch.messages[index];
                                if (message.channel_id == channel_id)
                                    newest_id = maximum(newest_id, message.id);
                            }
                            if (newest_id &&
                                (!chat_state_store_note_history_latest(&g_app.account_ui->chat_state,
                                                                          channel_id, newest_id) ||
                                 !synchronize_navigation_view())) {
                                log_write(LogLevel::error, "read", "history_latest_reconcile_failed");
                                return false;
                            }
                        }
                        if (owner == MessageHistoryRequestOwner::catchup)
                            message_history_older_observe_catchup(older, catchup, accepted.decode.page);
                        bool confirmed = false;
                        if (!message_composer_controller_observe_history(
                                &g_app.account_ui->message_composer_controller,
                                {scratch.messages.data, accepted.decode.page.count}, &confirmed)) {
                            log_write(LogLevel::error, "message", "message_send_history_reconcile_failed");
                            return false;
                        }
                        if (confirmed && !consume_message_send_completions())
                            return false;
                    }
                }
                if (!expected) {
                    if (owner == MessageHistoryRequestOwner::message_jump)
                        application_search_navigation_history_response(response, accepted);
                    log_u64(LogLevel::info, "rest", "message_history_stale_response",
                            static_cast<u64>(owner));
                } else if (!accepted.accepted()) {
                    if (accepted.apply.destructive_failure) {
                        g_app.message_pump.resync_required = true;
                    }
                    const u64 failed[] = {static_cast<u64>(accepted.error),
                                          static_cast<u64>(accepted.apply.error),
                                          static_cast<u64>(accepted.decode.error),
                                          static_cast<u64>(accepted.decode.message_error),
                                          static_cast<u64>(accepted.decode.read_error),
                                          static_cast<u64>(accepted.decode.scan_error),
                                          accepted.decode.error_offset};
                    log_u64_fields(LogLevel::error, "rest",
                                   "message_history_error\0message_history_apply_error\0"
                                   "message_history_decode_error\0message_history_message_error\0"
                                   "message_history_read_error\0message_history_scan_error\0"
                                   "message_history_error_offset",
                                   failed);
                    if (accepted.decode.schema_field[0])
                        log_write(LogLevel::error, "message_schema_field", accepted.decode.schema_field);
                    const u64 transport[] = {static_cast<u64>(response.error), response.native_error,
                                             response.status_code};
                    log_u64_fields(LogLevel::error, "rest",
                                   "message_history_transport_error\0message_history_native_error\0"
                                   "message_history_http_status",
                                   transport);
                    return false;
                } else if (accepted.disposition == MessageHistoryRestDisposition::retry_scheduled) {
                    // One diagnostic per completed attempt, never per frame or pump.
                    const u64 retry[] = {static_cast<u64>(response.error), response.native_error,
                                         response.status_code, response.reused_connection};
                    log_u64_fields(LogLevel::warning, "rest",
                                   "message_history_retry_transport_error\0message_history_retry_native_error\0"
                                   "message_history_retry_http_status\0message_history_retry_reused_connection",
                                   retry);
                } else if (accepted.disposition == MessageHistoryRestDisposition::applied && g_app.window) {
                    TraceRecord applied("history_applied");
                    applied.field("request", response.request_id);
                    applied.field("work", rest_owner_name(owner));
                    const u64 fields[] = {accepted.decode.page.count, accepted.apply.inserted,
                                          accepted.apply.resident, g_app.load_trace.channel_epoch,
                                          clock_microseconds(g_app.clock, apply_started, clock_now())};
                    trace_fields(applied, "decoded\0inserted\0resident\0channel_epoch\0decode_apply_us", fields);
                    applied.write("load_trace");
                    if (owner == MessageHistoryRequestOwner::catchup ||
                        owner == MessageHistoryRequestOwner::message_jump) {
                        g_app.load_trace.history_ready = true;
                        application_trace_load_stage("history_ready");
                    }
                    // A history page is a presentation commit boundary. Timeline reset and
                    // page application can reproduce the previous mutation counters, so the
                    // stale-key test is insufficient after a channel switch. Force a scene/text
                    // rebuild before asking Windows to paint the new page.
                    if (!refresh_account_text(true)) {
                        return false;
                    }
                    // A small page may leave the reader inside the lookahead band.
                    // Refill now, without waiting for another wheel event at the edge.
                    // This only queues work; the single-flight/rate-limit
                    // scheduler below still submits the request.
                    const MessageTimeline& timeline = g_app.message_view.timeline;
                    const ChannelRecord* channel = chat_state_store_find_channel(
                        g_app.account_ui->chat_state, g_app.account_ui->active_channel_id);
                    if ((owner == MessageHistoryRequestOwner::older ||
                         owner == MessageHistoryRequestOwner::catchup) &&
                        accepted.apply.inserted != 0 &&
                        catchup.state == MessageHistoryCatchupState::complete &&
                        message_history_older_prefetch_needed(timeline) && channel &&
                        (channel->actions & channel_action_read_history) != 0 &&
                        !message_history_older_demand(&older, &timeline, channel->id,
                                                      kMessageHistoryRequestLimit)) {
                        return false;
                    }
                    if (!application_request_frame()) {
                        log_write(LogLevel::error, "ui", "message_history_repaint_request_failed");
                        return false;
                    }
                }
            }
        }
    }
    if (early || !admit || g_app.message_history_request_owner != MessageHistoryRequestOwner::none) {
        return true;
    }

    if (!begin_required_catchup())
        return false;
    auto& private_request = g_app.account_ui->private_conversation;
    if (private_conversation_due(private_request, now_ms) &&
        private_conversation_validate_target(&private_request, g_app.account_ui->chat_state)) {
        const u64 request_id = next_message_history_request_id();
        const auto submitted = host.api.private_conversation(host.instance, private_request.operation,
                                                             private_request.target_id, request_id);
        if (submitted.error == RestClientSubmitError::busy) {
            private_request.phase = PrivateConversationPhase::retry_wait;
            private_request.retry_at_ms = deadline_after(now_ms, 100);
            return true;
        }
        if (!submitted.ok()) {
            fail_private_conversation();
            return true;
        }
        private_request.request_id = request_id;
        private_request.phase = PrivateConversationPhase::awaiting;
        g_app.message_history_request_owner = MessageHistoryRequestOwner::private_conversation;
        return true;
    }

    ReactionViewerQuery reaction_users{};
    const u64 read_wake = application_read_state_next_wake();
    if (read_wake && read_wake <= now_ms) {
        if (application_read_state_submit(next_message_history_request_id(), now_ms))
            g_app.message_history_request_owner = MessageHistoryRequestOwner::read_state_ack;
        return true;
    }
    const u64 scoped_wake = application_scoped_read_next_wake();
    if (scoped_wake && scoped_wake <= now_ms &&
        application_scoped_read_submit(next_message_history_request_id(), now_ms)) {
        g_app.message_history_request_owner = MessageHistoryRequestOwner::scoped_read;
        return true;
    }
    if (admit_primary_lanes(kPrimaryLaneFriends, kPrimaryLaneEnd, now_ms))
        return true;
    if (reaction_viewer_controller_prepare_request(&g_app.reaction_viewer_controller, now_ms,
                                                   &reaction_users)) {
        const u64 request_id = next_message_history_request_id();
        const RestClientSubmitResult submitted =
            host.api.reaction_users(host.instance, reaction_users.channel_id, reaction_users.message_id,
                                    reaction_users.emoji_id, reaction_users.emoji_name, reaction_users.type,
                                    reaction_users.after_user_id, reaction_users.limit, request_id);
        if (submitted.error == RestClientSubmitError::busy) {
            return true;
        }
        if (!submitted.ok()) {
            const ReactionViewerControllerResult failed =
                reaction_viewer_controller_submit_failed(&g_app.reaction_viewer_controller, now_ms);
            log_u64(LogLevel::warning, "reaction_viewer", "submit_error",
                    static_cast<u64>(submitted.error));
            log_u64(LogLevel::warning, "reaction_viewer", "submit_failure_state",
                    static_cast<u64>(failed.error));
            return true;
        }
        const ReactionViewerControllerResult marked = reaction_viewer_controller_mark_submitted(
            &g_app.reaction_viewer_controller, reaction_users.epoch);
        if (!marked.ok()) {
            log_u64(LogLevel::error, "reaction_viewer", "mark_submitted_error",
                    static_cast<u64>(marked.error));
            return false;
        }
        g_app.message_history_request_owner = MessageHistoryRequestOwner::reaction_users;
        return true;
    }
    // The first reactors of a hovered reaction pill.
    ReactionTooltipQuery reactors{};
    if (application_reaction_tooltip_query(&reactors)) {
        const u64 request_id = next_message_history_request_id();
        const RestClientSubmitResult submitted =
            host.api.reaction_users(host.instance, reactors.channel_id, reactors.message_id, reactors.emoji_id,
                                    reactors.emoji_name, reactors.type, 0, kReactionTooltipNames, request_id);
        if (submitted.error == RestClientSubmitError::busy)
            return true;
        if (!submitted.ok()) {
            application_reaction_tooltip_failed();
            log_u64(LogLevel::warning, "reaction_tooltip", "submit_error", static_cast<u64>(submitted.error));
            return true;
        }
        application_reaction_tooltip_submitted(reactors.epoch);
        g_app.message_history_request_owner = MessageHistoryRequestOwner::reaction_tooltip;
        return true;
    }

    const bool yield_reaction = reaction_primary_yield();
    if (!yield_reaction && admit_message_reaction(now_ms))
        return true;
    const bool lower = pump_message_history_lower(now_ms);
    if (!lower)
        return false;
    if (g_app.message_history_request_owner != MessageHistoryRequestOwner::none) {
        application_message_history_reset_reaction_turn();
        return true;
    }
    // No lower-priority request was accepted: the ready reaction gets one attempt in this
    // same pump, rather than relying on another input event or polling wake.
    if (yield_reaction) {
        application_message_history_reset_reaction_turn();
        admit_message_reaction(now_ms);
    }
    return true;
}

bool pump_message_history() noexcept {
    if (!application_message_reaction_synchronize())
        return false;
    if (!g_app.account_ui || !g_app.account.instance)
        return true;
    if (!g_app.account.status.running)
        return pump_message_history_main(false);
    application_thread_actions_synchronize();
    if (!application_thread_browser_synchronize())
        return false;
    if (!application_thread_creation_synchronize())
        return false;
    if (!application_forum_post_synchronize())
        return false;
    if (!application_poll_synchronize())
        return false;
    application_search_navigation_synchronize();
    // At most two completions. Decode/model application stays ordered on the
    // UI thread and reuses one response/scratch arena, not one arena per worker.
    for (u32 pass = 0; pass < 2; ++pass) {
        const u64 before = g_app.account_ui->message_history_request_id;
        const u64 poll_serial = g_app.account_ui->poll_gateway_serial;
        if (!pump_message_history_main(pass == 1))
            return false;
        if (g_app.message_history_request_owner != MessageHistoryRequestOwner::none &&
            before != g_app.account_ui->message_history_request_id) {
            g_app.primary_request_id = g_app.account_ui->message_history_request_id;
            g_app.account_ui->message_history_poll_serial = poll_serial;
            if (g_app.message_history_request_owner != MessageHistoryRequestOwner::message_reaction)
                application_message_history_reset_reaction_turn();
        }
    }
    if (g_app.details_request_owner == MessageHistoryRequestOwner::none) {
        const u64 now = clock_monotonic_milliseconds();
        admit_details_request(now);
    }
    return true;
}

bool scroll_to_resident_message(u64 message_id) noexcept {
    if (!g_app.account_ui || message_id == 0 || !g_app.message_view.timeline.list) {
        return false;
    }
    const MessageStoreResult found = message_store_find(g_app.message_view.store, message_id);
    UiVariableList* const list = g_app.message_view.timeline.list;
    if (!found.ok() || found.order_index >= list->count) {
        return false;
    }
    const u64 item_top = ui_variable_list_prefix_units(*list, found.order_index);
    const u64 item_height = list->item_units[found.order_index];
    const u64 viewport = g_app.message_view.timeline.viewport_units;
    const u64 centered =
        item_top + item_height / 2u > viewport / 2u ? item_top + item_height / 2u - viewport / 2u : 0u;
    const u64 maximum_scroll = message_timeline_maximum_scroll(g_app.message_view.timeline);
    if (!message_timeline_set_scroll(&g_app.message_view.timeline, minimum(centered, maximum_scroll),
                                     false)) {
        return false;
    }
    const MessageSceneResult updated =
        message_scene_update(&g_app.message_scene, g_app.message_scene.viewport);
    if (!updated.ok() || !message_scene_focus_message(&g_app.message_scene, message_id))
        return false;
    g_app.context_focus = &g_app.message_scene.widgets;
    application_message_highlight_begin(message_id);
    return true;
}

} // namespace maicord::app_detail
