#include "account_pump_internal.hpp"
#include "maicord/protocol/guild/channel_overview.hpp"
#include "maicord/model/voice/stage_instance_store.hpp"
#include "../../../src/app/media/media.hpp"
#include "../../../src/app/plugins/catalog_service.hpp"
#include "../../support/test_output.hpp"
#include "../../../src/app/account/account_ui_runtime.hpp"
#include "../../../src/app/chat/threads/actions.hpp"
#include "maicord/model/notifications/message_notifications.hpp"
#include "maicord/protocol/gateway/reaction_events.hpp"

namespace maicord::app_detail {
namespace account_pump_detail {

bool fixture_logout_blocked;
u64 fixture_logout_wake;

} // namespace account_pump_detail

// The background clock asks the effects module (src/app/shell/background.hpp); no effect is ever selected here.
bool application_effects_selected() noexcept {
    return false;
}
bool application_effects_motion_allowed() noexcept {
    return true;
}
bool application_effects_preview_running() noexcept {
    return false;
}
u32 application_effects_interval_ms() noexcept {
    return 42;
}
u64 application_effects_transition_wake() noexcept {
    return 0;
}

bool application_account_logout_blocked() noexcept {
    return fixture_logout_blocked;
}
u64 application_account_logout_next_wake() noexcept {
    return fixture_logout_wake;
}
ApplicationThreadActionView application_thread_actions_view(u64) noexcept {
    return {};
}
u32 application_thread_actions_publication_revision() noexcept {
    return 0;
}
bool application_thread_actions_synchronize() noexcept {
    return false;
}
ApplicationThreadActionEnqueueResult application_thread_actions_fetch(u64,
                                                                  ApplicationThreadActionTicket*) noexcept {
    return ApplicationThreadActionEnqueueResult::stale;
}
ApplicationThreadMembershipStatus application_thread_actions_membership(
    const ApplicationThreadActionTicket&) noexcept {
    return ApplicationThreadMembershipStatus::stale;
}
Utf8View application_thread_actions_admission_text(ApplicationThreadActionEnqueueResult) noexcept {
    return utf8_literal("Unavailable");
}
bool application_search_navigation_source(const ApplicationSearchSourceTicket&,
                                          ApplicationSearchNavigationSource*) noexcept {
    return false;
}
bool application_search_source_notice(const ApplicationSearchSourceTicket&, Utf8View) noexcept {
    return false;
}
u64 application_thread_actions_next_wake() noexcept {
    return 0;
}
u64 application_channel_prefetch_next_wake() noexcept {
    return 0;
}
u64 application_startup_history_next_wake() noexcept {
    return 0;
}
bool application_startup_shell_gateway_ready(u64) noexcept {
    return false;
}
void application_startup_shell_ready() noexcept {}
bool application_startup_shell_take_release() noexcept {
    return false;
}
void application_startup_shell_pump(u64) noexcept {}
u64 application_startup_shell_next_wake() noexcept {
    return 0;
}
void application_channel_snapshots_reset() noexcept {}
void application_channel_snapshots_observe(void*, const GatewayClientEvent&, ConstSlice<u8>,
                                           MessageContentAvailability) noexcept {}

bool application_refresh_thread_actions_menu() noexcept {
    return true;
}

namespace account_pump_detail {

u32 refreshes, forced, history_calls, stops, login_returns, order;
u32 presentation_failures;
bool forum_sync_ok = true;
u32 normal_media_pumps, forum_preview_pumps, covered_media_pumps;
bool fixture_forum_active, fixture_preview_ok = true;
bool navigation_ok = true, history_ok = true, order_ok = true, rejected;
AccountModuleStatus next_status{};
u32 failures, catalog_restores, scoped_read_syncs, scoped_read_menu_refreshes;
u64 scoped_read_wake;
bool scoped_read_menu_ok = true;
u32 diagnostic_source_reads, diagnostic_pops, diagnostic_logs, normal_pops, notification_callbacks;
GatewayClientEvent scoped_gateway_event{};
bool scoped_gateway_event_ready;
GatewayClientEvent poll_follow_event{};
bool poll_follow_ready;
bool poll_seed_selected_channel;
u8 poll_follow_payload[4096]{};
u32 poll_follow_bytes;
// Payload-free votes expanded from one batch, popped in order.
GatewayClientEvent poll_batch_events[4]{};
u32 poll_batch_count, poll_batch_next;
bool scoped_gateway_replace_scope;
u8 scoped_gateway_payload[kGatewayBulkDeleteMaximumIds * sizeof(u64)]{};
u32 scoped_gateway_payload_bytes;
u32 navigation_fetches, navigation_cancels, navigation_moves;
u64 navigation_cancelled_id;
bool navigation_cancel_before_reset;
u32 forum_gateway_notifications;
u32 browser_gateway_notifications, forum_source_observations, forum_delete_observations;
bool forum_observation_needed;
u64 forum_message_channel, forum_message_id;
u64 forum_gateway_guild, forum_gateway_parent, forum_gateway_thread;
bool reaction_observation_needed;
u32 reaction_observations, reaction_source_observations, reaction_delete_observations;
u64 reaction_wake;
MessageHandle observed_reaction_handle;
u32 observed_reaction_revision;
bool observed_reaction_applied;
u32 diagnostic_position, diagnostic_events;
GatewayClientFailureStage diagnostic_stage;
GatewayClientPopError diagnostic_pop_error;
bool diagnostic_order_ok, diagnostic_refill, diagnostic_null_source, diagnostic_overlarge;
u64 profile_wake, command_wake, form_wake, friends_wake, recovery_wake, reset_wake, phone_wake,
    notification_wake, video_wake, tooltip_wake, surface_wake, toast_wake, reconnect_wake;
bool theme_transition;

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
AccountModuleStatus WINAPI pump(void*, u64, u64) noexcept {
    return next_status;
}
void reset_counts() noexcept {
    refreshes = forced = history_calls = stops = login_returns = order = 0;
    scoped_read_syncs = scoped_read_menu_refreshes = 0;
    scoped_read_menu_ok = true;
    order_ok = navigation_ok = history_ok = true;
    rejected = false;
    diagnostic_source_reads = diagnostic_pops = diagnostic_logs = normal_pops = notification_callbacks = 0;
    diagnostic_position = 0;
    diagnostic_events = 0;
    diagnostic_stage = GatewayClientFailureStage::none;
    diagnostic_pop_error = GatewayClientPopError::none;
    diagnostic_order_ok = true;
    diagnostic_refill = diagnostic_null_source = diagnostic_overlarge = false;
}
RestClientSubmitResult WINAPI navigation_fetch(void*, ChannelOverviewOperation operation,
                                               u64 channel, const ChannelOverviewPatch* patch,
                                               u64 request) noexcept {
    check(operation == ChannelOverviewOperation::fetch && channel == 999 && !patch && request,
          "timeline navigation metadata uses actual GET owner and exact destination");
    ++navigation_fetches;
    return {};
}
RestClientSubmitResult WINAPI navigation_cancel(void*, const MessagePinsRequest* request,
                                                u64 id) noexcept {
    check(!request && id, "timeline navigation cancellation uses exact shared DETAILS ID");
    ++navigation_cancels;
    navigation_cancelled_id = id;
    navigation_cancel_before_reset =
        g_app.account_ui &&
        chat_state_store_find_channel(g_app.account_ui->chat_state, 70) != nullptr;
    return {};
}
void* WINAPI diagnostic_source(void*) noexcept {
    ++diagnostic_source_reads;
    return diagnostic_null_source ? nullptr : reinterpret_cast<void*>(1);
}
void* WINAPI no_gateway_source(void*) noexcept {
    return nullptr;
}
GatewayClientPopResult WINAPI diagnostic_pop(void*, GatewayClientEvent* event,
                                             Slice<u8> payload) noexcept {
    ++diagnostic_pops;
    auto& position = diagnostic_position;
    if (!diagnostic_refill && position >= diagnostic_events)
        return {0, GatewayClientPopError::empty};
    if (payload.count)
        payload[0] = 0x5a;
    *event = {};
    event->kind = position == 1   ? GatewayClientEventKind::closed
                  : position == 2 ? GatewayClientEventKind::failed
                                  : GatewayClientEventKind::dispatch;
    event->error = event->kind == GatewayClientEventKind::failed ? GatewayClientError::session
                                                                 : GatewayClientError::none;
    event->failure_stage = GatewayClientFailureStage::ready_user_route;
    ++position;
    return {diagnostic_overlarge ? payload.count + 1 : 1, diagnostic_pop_error};
}

} // namespace account_pump_detail

void application_log_account_observations(const AccountModuleStatus&, u32) noexcept {
}
bool refresh_message_composer_ui() noexcept {
    return true;
}
bool application_navigate_known_message(u64 channel, u64 message) noexcept {
    if (!g_app.account_ui ||
        !chat_state_store_select_channel(&g_app.account_ui->chat_state, channel).ok())
        return false;
    ++navigation_moves;
    g_app.account_ui->active_channel_id = channel;
    g_app.pending_message_jump_channel_id = channel;
    g_app.pending_message_jump_message_id = message;
    return true;
}
GatewayClientPopResult application_pop_account_gateway(void*, GatewayClientEvent* event,
                                                       Slice<u8> payload) noexcept {
    ++normal_pops;
    if (event)
        *event = {};
    if (!scoped_gateway_event_ready && poll_batch_next < poll_batch_count && event) {
        *event = poll_batch_events[poll_batch_next++];
        return {0, GatewayClientPopError::none};
    }
    if (!scoped_gateway_event_ready && poll_follow_ready && event) {
        if (poll_seed_selected_channel) {
            poll_seed_selected_channel = false;
            // The offline UI boundary admits a real readable DM projection
            // after READY cleared the previous session, before the next receipt.
            auto& state = g_app.account_ui->chat_state;
            const auto seeded = chat_state_store_upsert_channel(&state,
                {70, 0, 0, utf8_literal("Poll conversation"), 0, 0, 1});
            check(seeded.ok() && chat_state_store_select_channel(&state, 70).ok(),
                "poll receipt follows validated READY with real selected readable Core channel");
            g_app.account_ui->active_channel_id = 70;
        }
        *event = poll_follow_event;
        poll_follow_ready = false;
        if (poll_follow_bytes > payload.count) return {0, GatewayClientPopError::output_too_small};
        CopyMemory(payload.data, poll_follow_payload, poll_follow_bytes);
        return {poll_follow_bytes, GatewayClientPopError::none};
    }
    if (scoped_gateway_event_ready && event) {
        *event = scoped_gateway_event;
        scoped_gateway_event_ready = false;
        if (scoped_gateway_replace_scope) {
            ++g_app.plugin_account_scope_serial;
            scoped_gateway_replace_scope = false;
        }
        if (scoped_gateway_payload_bytes > payload.count)
            return {0, GatewayClientPopError::output_too_small};
        if (scoped_gateway_payload_bytes)
            CopyMemory(payload.data, scoped_gateway_payload, scoped_gateway_payload_bytes);
        return {scoped_gateway_payload_bytes, GatewayClientPopError::none};
    }
    return {0, GatewayClientPopError::empty};
}
void application_log_gateway_event(const GatewayClientEvent& event) noexcept {
    ++diagnostic_logs;
    diagnostic_order_ok &= !login_returns && !refreshes && !history_calls;
    if (event.kind == GatewayClientEventKind::failed)
        diagnostic_stage = event.failure_stage;
}
bool consume_message_send_completions() noexcept {
    order_ok &= order++ == 0;
    return true;
}
bool synchronize_navigation_view() noexcept {
    order_ok &= order++ == 1;
    return navigation_ok;
}
bool synchronize_selected_channel() noexcept {
    order_ok &= order++ == 2;
    return true;
}
bool pump_message_history() noexcept {
    ++history_calls;
    order_ok &= order++ == 3;
    return history_ok;
}
bool present_private_conversation_error() noexcept {
    return true;
}
bool application_private_channel_present_error() noexcept {
    return true;
}
bool application_synchronize_login_view() noexcept {
    return true;
}
void application_catalog_restore() noexcept {
    ++catalog_restores;
}
bool application_plugin_surfaces_stale() noexcept {
    return false;
}
void application_plugin_surfaces_rendered() noexcept {
}
void application_plugin_surfaces_prepare() noexcept {
}
void application_notifications_message(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept {
    ++notification_callbacks;
}
MessageReadBadge application_notifications_read_badge(void*, const GatewayClientEvent&,
                                                      ConstSlice<u8>) noexcept {
    // This pump fixture has no notification-settings or relationship owner.
    // It admits no badge counts; policy behavior uses the real host fixture.
    return MessageReadBadge::none;
}
void application_notifications_selection(Application*) noexcept {
    ++notification_callbacks;
}
void application_notifications_wake(Application*) noexcept {
}
u64 application_notifications_next_wake(const Application*) noexcept {
    return notification_wake;
}
bool application_message_reaction_observation_needed() noexcept {
    return reaction_observation_needed;
}
u64 application_message_reaction_next_wake() noexcept {
    return reaction_wake;
}
void application_message_reaction_observe(const GatewayMessageReactionEvent&, MessageHandle source,
                                          u32 revision, bool applied) noexcept {
    ++reaction_observations;
    observed_reaction_handle = source;
    observed_reaction_revision = revision;
    observed_reaction_applied = applied;
}
void application_message_reaction_message_observe(const GatewayMessageEvent&, MessageHandle source,
                                                  u32 revision, bool applied) noexcept {
    ++reaction_source_observations;
    observed_reaction_handle = source;
    observed_reaction_revision = revision;
    observed_reaction_applied = applied;
}
void application_message_reaction_message_deleted(u64 channel_id, u64 message_id) noexcept {
    if (channel_id == 70 && message_id == 900)
        ++reaction_delete_observations;
}
bool application_stop_account() noexcept {
    ++stops;
    return true;
}
void application_account_stop_recoverable(const char*) noexcept {
    application_stop_account();
}
void application_account_connection_observe(GatewayClientEventKind) noexcept {}
u64 application_account_reconnect_next_wake() noexcept {
    return reconnect_wake;
}
void application_record_presentation_failure(const char*) noexcept {
    ++presentation_failures;
}
bool application_account_return_to_login(bool invalid) noexcept {
    ++login_returns;
    rejected = invalid;
    return true;
}
ApplicationMediaPumpResult application_pump_visible_media(Application*) noexcept {
    ++normal_media_pumps;
    return {false, false, false, true};
}
ApplicationMediaPumpResult application_pump_forum_preview_media(Application*) noexcept {
    ++forum_preview_pumps;
    return {true, true, true, fixture_preview_ok};
}
ApplicationMediaPumpResult application_pump_covered_media(Application*) noexcept {
    ++covered_media_pumps;
    // Animation advances may repaint, but never request chat text or history.
    return {true, true, true, true};
}
bool application_forum_form_active() noexcept {
    return fixture_forum_active;
}
u64 application_profile_next_wake() noexcept {
    return profile_wake;
}
u64 application_commands_next_wake() noexcept {
    return command_wake;
}
bool application_commands_bar_visible() noexcept {
    return false;
}
u64 application_video_next_wake() noexcept {
    return video_wake;
}
u64 application_tooltip_next_wake() noexcept {
    return tooltip_wake;
}
u64 application_toast_next_wake() noexcept {
    return toast_wake;
}
u64 application_guild_banner_next_wake() noexcept {
    return 0;
}
u64 application_surface_motion_next_wake() noexcept {
    return surface_wake;
}
u64 loading_spinner_wake;
u64 application_loading_spinner_wake() noexcept {
    return loading_spinner_wake;
}
bool application_loading_spinner_due(u64 now_ms) noexcept {
    return loading_spinner_wake && now_ms >= loading_spinner_wake;
}
u64 application_author_hydration_next_wake() noexcept {
    // This isolated pump fixture has no author planner or materialized rows.
    return 0;
}
u64 application_mention_member_query_next_wake() noexcept {
    // No composer here: no '@' member search waits on its debounce.
    return 0;
}
u64 search_request_wake, search_caret_wake, self_status_wake;
u64 overview_request_wake, overview_caret_wake;
u64 pins_request_wake;
u64 browser_request_wake;
u64 creation_request_wake;
bool application_thread_creation_synchronize() noexcept {
    return true;
}
void application_thread_creation_gateway(u64, u64, u64) noexcept {
}
u64 application_thread_creation_next_wake() noexcept {
    return creation_request_wake;
}
u64 forum_request_wake;
bool application_forum_post_synchronize() noexcept {
    return forum_sync_ok;
}
bool application_forum_post_observation_needed() noexcept {
    return forum_observation_needed;
}
void application_forum_post_message_observe(const GatewayMessageEvent& event) noexcept {
    ++forum_source_observations;
    forum_message_channel = event.channel_id;
    forum_message_id = event.id;
}
void application_forum_post_message_deleted(u64 channel, u64 message) noexcept {
    ++forum_delete_observations;
    forum_message_channel = channel;
    forum_message_id = message;
}
void application_forum_post_gateway(u64 guild, u64 parent, u64 thread) noexcept {
    ++forum_gateway_notifications;
    forum_gateway_guild = guild;
    forum_gateway_parent = parent;
    forum_gateway_thread = thread;
}
u64 application_forum_post_next_wake() noexcept {
    return forum_request_wake;
}
u64 application_forum_form_next_wake() noexcept {
    return 0;
}
u64 application_forum_preview_next_wake() noexcept {
    return 0;
}
u64 application_composer_preview_next_wake() noexcept {
    return 0;
}
bool browser_blocks_read;
bool application_thread_browser_synchronize() noexcept {
    return true;
}
bool application_thread_browser_read_blocked() noexcept {
    return browser_blocks_read;
}
void application_thread_browser_gateway(u64, u64) noexcept {
    ++browser_gateway_notifications;
}
u64 application_thread_browser_next_wake() noexcept {
    return browser_request_wake;
}
u64 application_pinned_messages_next_wake() noexcept {
    return pins_request_wake;
}
u64 application_channel_overview_next_wake() noexcept {
    return overview_request_wake;
}
u64 application_channel_overview_caret_wake() noexcept {
    return overview_caret_wake;
}
u64 application_self_status_next_wake() noexcept {
    return self_status_wake;
}
u64 application_search_next_wake() noexcept {
    return search_request_wake;
}
u64 application_media_picker_next_wake() noexcept {
    return 0;
}
bool application_thread_starters_sync() noexcept {
    return false;
}
u64 application_emoji_preferences_next_wake() noexcept {
    return 0;
}
u64 application_gif_favorites_next_wake() noexcept {
    return 0;
}
u64 application_custom_status_next_wake() noexcept {
    return 0;
}
u64 application_activity_next_wake() noexcept {
    return 0;
}
u64 application_notification_controls_next_wake() noexcept {
    return 0;
}
u64 application_stream_preview_next_wake() noexcept {
    return 0;
}
u64 application_stage_next_wake(bool) noexcept {
    return 0;
}
u64 application_message_components_next_wake() noexcept {
    return 0;
}
u64 application_invite_cards_next_wake() noexcept {
    return 0;
}
const StageInstanceStore& application_stage_instances() noexcept {
    static StageInstanceStore empty;
    return empty;
}
u64 application_inbox_next_wake() noexcept {
    return 0;
}
u64 application_user_settings_next_wake() noexcept {
    return 0;
}
u64 application_guild_folders_request_wake() noexcept {
    return 0;
}
u64 application_account_settings_next_wake() noexcept {
    return 0;
}
u64 application_media_picker_caret_wake() noexcept {
    return 0;
}
u64 application_search_edit_next_wake() noexcept {
    return search_caret_wake;
}
u64 application_quick_switcher_caret_wake() noexcept {
    return 0;
}
u64 application_message_forward_caret_wake() noexcept {
    return 0;
}
u64 application_friends_next_wake() noexcept {
    return friends_wake;
}
bool application_read_state_eligible() noexcept {
    return false;
}
// Scoped controller/transport are outside this pump fixture. Observe only the
// exact production synchronize/menu/deadline integration boundaries.
void application_scoped_read_synchronize() noexcept {
    ++scoped_read_syncs;
}
u64 application_scoped_read_next_wake() noexcept {
    return scoped_read_wake;
}
bool application_scoped_read_menu_refresh() noexcept {
    ++scoped_read_menu_refreshes;
    return scoped_read_menu_ok;
}
bool application_read_state_observe() noexcept {
    return true;
}
u64 application_read_state_next_wake() noexcept {
    return 0;
}
void application_read_state_cancel(void*, u64) noexcept {
}
void application_read_state_ack_observed(void*, u64, u64, bool) noexcept {
}
void application_read_state_reset_token(void*) noexcept {
}
u64 application_friends_search_next_wake() noexcept {
    return 0;
}
u64 application_message_interaction_next_wake(u64) noexcept {
    return 0;
}
Utf8View application_composer_status_notice() noexcept {
    return {};
}
bool application_composer_status_notice_is_error() noexcept {
    return false;
}
u64 application_text_form_next_wake() noexcept {
    return form_wake;
}
u64 application_theme_schedule_wake() noexcept {
    return 0;
}
bool application_theme_transition_active() noexcept {
    return theme_transition;
}
u64 application_login_auth_deadline() noexcept {
    return 0;
}
u64 application_login_recovery_deadline() noexcept {
    return recovery_wake;
}
u64 application_login_reset_deadline() noexcept {
    return reset_wake;
}
u64 application_login_phone_deadline() noexcept {
    return phone_wake;
}
u64 application_login_captcha_deadline() noexcept {
    return 0;
}

} // namespace maicord::app_detail
