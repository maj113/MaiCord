#include "application_downloads_inactive.hpp"
#include "application_poll_inactive.hpp"
#include "application_member_moderation_inactive.hpp"
#include "application_guild_invite_inactive.hpp"
#include "../src/app/application_account_pump.hpp"
#include "../src/app/application_account_module.hpp"
#include "../src/app/application_account_logout.hpp"
#include "../src/app/application_account_reconnect.hpp"
#include "../src/app/application_scoped_read.hpp"
#include "../src/app/application_scoped_read_menu.hpp"
#include "../src/app/application_media.hpp"
#include "../src/app/application_state.hpp"
#include "../src/app/application_scheduler.hpp"
#include "../src/app/application_channel_overview.hpp"
#include "../src/app/application_thread_browser.hpp"
#include "../src/app/application_thread_creation.hpp"
#include "../src/app/application_forum_post.hpp"
#include "../src/app/application_notifications.hpp"
#include "../src/app/application_thread_header_activation.hpp"
#include "../src/app/application_video.hpp"
#include "../src/app/application_toast.hpp"
#include "../src/app/gateway_dispatch_common.hpp"
#include "maicord/app/user_account_coordinator.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/gateway_events.hpp"
#include "application_account_text_retry_runtime.hpp"
#include "../src/app/application_search_navigation.cpp"
#include "application_action_captcha_stubs.hpp"
#include "test_output.hpp"

namespace maicord::app_detail {
namespace {
bool fixture_logout_blocked;
u64 fixture_logout_wake;
} // namespace
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
void application_channel_snapshots_reset() noexcept {}
void application_channel_snapshots_observe(void*, const GatewayClientEvent&, ConstSlice<u8>,
                                           MessageContentAvailability) noexcept {}
bool application_refresh_thread_actions_menu() noexcept {
    return true;
}
namespace {
u32 refreshes, forced, history_calls, stops, login_returns, order;
u32 presentation_failures;
bool forum_sync_ok = true;
u32 normal_media_pumps, forum_preview_pumps, covered_media_pumps;
bool fixture_forum_active, fixture_preview_ok = true;
bool navigation_ok = true, history_ok = true, order_ok = true, rejected;
AccountModuleStatus next_status{};
u32 failures, plugin_restores, scoped_read_syncs, scoped_read_menu_refreshes;
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
u32 navigation_fetches, navigation_cancels, navigation_moves, navigation_composer_refreshes;
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
u32 diagnostic_positions[8], diagnostic_events;
GatewayClientFailureStage diagnostic_stage;
GatewayClientPopError diagnostic_pop_error;
bool diagnostic_order_ok, diagnostic_refill, diagnostic_null_source, diagnostic_overlarge;
u64 profile_wake, command_wake, form_wake, friends_wake, recovery_wake, reset_wake, phone_wake,
    notification_wake, video_wake, tooltip_wake, surface_wake, toast_wake, reconnect_wake;
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
    for (u32& position : diagnostic_positions)
        position = 0;
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
void* WINAPI diagnostic_source(void*, u32 index) noexcept {
    ++diagnostic_source_reads;
    return diagnostic_null_source ? nullptr : reinterpret_cast<void*>(static_cast<usize>(index) + 1);
}
GatewayClientPopResult WINAPI diagnostic_pop(void* source, GatewayClientEvent* event,
                                             Slice<u8> payload) noexcept {
    ++diagnostic_pops;
    auto& position = diagnostic_positions[reinterpret_cast<usize>(source) - 1];
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
} // namespace
void application_log_account_observations(const AccountModuleStatus&, u32) noexcept {
}
bool refresh_message_composer_ui() noexcept {
    ++navigation_composer_refreshes;
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
bool refresh_account_text(bool force) noexcept {
    if (account_text_retry_runtime_test::active) {
        account_text_retry_runtime_test::last_force = force;
        return refresh_account_text_runtime_impl(force);
    }
    ++refreshes;
    forced += force ? 1u : 0u;
    order_ok &= order++ == 4;
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
bool application_plugins_restore() noexcept {
    ++plugin_restores;
    return true;
}
bool application_plugins_slots_stale() noexcept {
    return false;
}
void application_plugins_slots_rendered() noexcept {
}
void application_plugins_render_prepare() noexcept {
}
bool application_plugins_render_stale() noexcept {
    return false;
}
void application_plugins_render_rendered() noexcept {
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
void application_notifications_drain(Application*) noexcept {
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
u64 application_surface_motion_next_wake(u64) noexcept {
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
bool application_read_state_observe(u64) noexcept {
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

void test_frame_deadlines() noexcept {
    g_app.frame_pending = false;
    // Active but idle account, with no real workers. The production scheduler
    // decides which work should wake; the fixture controls unrelated UI timers.
    g_app.account.instance = reinterpret_cast<void*>(1);
    g_app.renderer.text_effect_animated = true;
    g_app.renderer.text_effect_next_ms = 900;
    check(application_visual_frame_requested(1000, 1000000), "overdue_effect_requests_display_slot");
    check(application_effect_frame_due(1000), "procedural_effect_needs_draw_without_media_completion");
    check(application_wait_timeout(1000, 1000000, true) == INFINITE,
          "overdue_effect_does_not_poll_while_waiting_for_dxgi");
    check(application_wait_timeout(1000, 1000000, false) == 0, "overdue_effect_wakes_without_dxgi_clock");
    check(!application_nonvisual_wake_due(1000), "visual_effect_does_not_pump_idle_account");
    reaction_wake = 1400;
    check(application_wait_timeout(1000, 1000000, true) == 400 && !application_nonvisual_wake_due(1000),
          "reaction_network_deadline_waits_without_polling_the_idle_account");
    reaction_wake = 1000;
    check(application_wait_timeout(1000, 1000000, true) == 0 && application_nonvisual_wake_due(1000),
          "due_reaction_deadline_wakes_the_account_without_a_visual_timer");
    reaction_wake = 0;
    g_app.renderer.text_effect_next_ms = 1042;
    check(!application_visual_frame_requested(1000, 1000000) && !application_effect_frame_due(1000) &&
              application_wait_timeout(1000, 1000000, false) == 42,
          "future_effect_uses_supplied_loop_time");
    form_wake = 1000;
    check(application_wait_timeout(1000, 1000000, true) == 0 && !application_nonvisual_wake_due(1000),
          "form_caret_wakes_ui_without_pumping_account");
    form_wake = 0;
    video_wake = 1032;
    check(!application_visual_frame_requested(1000, 1000000) &&
              application_wait_timeout(1000, 1000000, false) == 32 && !application_nonvisual_wake_due(1000),
          "buffering_spinner_uses_visual_deadline_without_account_polling");
    video_wake = 1000;
    check(application_visual_frame_requested(1000, 1000000) &&
              application_wait_timeout(1000, 1000000, true) == INFINITE &&
              application_wait_timeout(1000, 1000000, false) == 0 && !application_nonvisual_wake_due(1000),
          "overdue_spinner_waits_for_dxgi_and_has_bounded_timer_fallback");
    video_wake = 0;
    // Isolate tooltip deadlines from the deliberately active 42-ms text effect.
    g_app.renderer.text_effect_animated = false;
    tooltip_wake = 1400;
    check(!application_visual_frame_requested(1000, 1000000) &&
              application_wait_timeout(1000, 1000000, true) == INFINITE &&
              application_wait_timeout(1000, 1000000, false) == 400 &&
              !application_nonvisual_wake_due(1000),
          "pending_tooltip_has_one_visual_deadline_without_account_polling");
    tooltip_wake = 1000;
    check(application_visual_frame_requested(1000, 1000000) &&
              application_wait_timeout(1000, 1000000, true) == INFINITE &&
              application_wait_timeout(1000, 1000000, false) == 0 && !application_nonvisual_wake_due(1000),
          "due_tooltip_uses_dxgi_or_timer_fallback_without_network_work");
    tooltip_wake = 0;
    surface_wake = 1016;
    check(!application_visual_frame_requested(1000, 1000000) &&
              application_wait_timeout(1000, 1000000, false) == 16 &&
              application_wait_timeout(1000, 1000000, true) == INFINITE &&
              !application_nonvisual_wake_due(1000),
          "surface_motion_uses_visual_deadline_without_account_work");
    surface_wake = 1000;
    check(application_visual_frame_requested(1000, 1000000) &&
              application_wait_timeout(1000, 1000000, true) == INFINITE &&
              application_wait_timeout(1000, 1000000, false) == 0 && !application_nonvisual_wake_due(1000),
          "due_surface_motion_is_paced_by_dxgi_or_timer_fallback");
    surface_wake = 0;
    // Pins/inbox/thread-browser loading spinners share one visual frame clock.
    loading_spinner_wake = 1032;
    check(!application_visual_frame_requested(1000, 1000000) && !application_effect_frame_due(1000) &&
              application_wait_timeout(1000, 1000000, false) == 32 &&
              application_wait_timeout(1000, 1000000, true) == INFINITE && !application_nonvisual_wake_due(1000),
          "visible_loading_spinner_schedules_its_next_frame_without_account_work");
    loading_spinner_wake = 1000;
    check(application_visual_frame_requested(1000, 1000000) && application_effect_frame_due(1000) &&
              application_wait_timeout(1000, 1000000, true) == INFINITE &&
              application_wait_timeout(1000, 1000000, false) == 0 && !application_nonvisual_wake_due(1000),
          "due_loading_spinner_requests_one_paced_frame");
    loading_spinner_wake = 0;
    scoped_read_wake = 1400;
    check(application_wait_timeout(1000, 1000000, true) == 400 && !application_nonvisual_wake_due(1000) &&
              !application_visual_frame_requested(1000, 1000000),
          "scoped_read_future_deadline_is_nonvisual_account_work");
    scoped_read_wake = 1000;
    check(application_wait_timeout(1000, 1000000, true) == 0 && application_nonvisual_wake_due(1000) &&
              !application_visual_frame_requested(1000, 1000000),
          "scoped_read_due_deadline_wakes_account_even_with_dxgi_armed");
    scoped_read_wake = 0;
    search_caret_wake = 1400;
    check(application_wait_timeout(1000, 1000000, false) == 400 &&
              application_wait_timeout(1000, 1000000, true) == INFINITE &&
              !application_nonvisual_wake_due(1000),
          "search_caret_wait_is_visual_without_rest_work");
    search_caret_wake = 1000;
    check(application_visual_frame_requested(1000, 1000000) && !application_nonvisual_wake_due(1000) &&
              application_wait_timeout(1000, 1000000, true) == INFINITE,
          "due_search_caret_waits_for_dxgi_without_polling");
    search_caret_wake = 0;
    overview_request_wake = 1400;
    check(application_wait_timeout(1000, 1000000, true) == 400 &&
              application_wait_timeout(1000, 1000000, false) == 400 &&
              !application_nonvisual_wake_due(1000) && !application_visual_frame_requested(1000, 1000000),
          "overview_retry_deadline_waits_for_account_work_without_requesting_visual_frames");
    overview_request_wake = 1000;
    check(application_nonvisual_wake_due(1000) && application_wait_timeout(1000, 1000000, true) == 0 &&
              !application_visual_frame_requested(1000, 1000000),
          "overview_due_retry_pumps_account_even_when_dxgi_clock_is_armed");
    overview_request_wake = 0;
    overview_caret_wake = 1400;
    check(application_wait_timeout(1000, 1000000, false) == 400 &&
              application_wait_timeout(1000, 1000000, true) == INFINITE &&
              !application_nonvisual_wake_due(1000) && !application_visual_frame_requested(1000, 1000000),
          "overview_future_caret_is_visual_only_and_uses_timer_fallback_without_dxgi");
    overview_caret_wake = 1000;
    check(application_visual_frame_requested(1000, 1000000) && !application_nonvisual_wake_due(1000) &&
              application_wait_timeout(1000, 1000000, true) == INFINITE &&
              application_wait_timeout(1000, 1000000, false) == 0,
          "overview_due_caret_waits_for_display_slot_without_pumping_account");
    overview_caret_wake = 0;
    check(application_wait_timeout(1000, 1000000, false) == INFINITE &&
              !application_nonvisual_wake_due(1000) && !application_visual_frame_requested(1000, 1000000),
          "overview_cleared_retry_and_caret_deadlines_return_to_wait_driven_idle");
    // A stopped account keeps its shell; deadlines only the pump serves must not poll.
    scoped_read_wake = 1000;
    reconnect_wake = 1400;
    g_app.account.instance = nullptr;
    check(application_wait_timeout(1000, 1000000, true) == 400 && !application_nonvisual_wake_due(1000),
          "stopped_account_waits_for_its_reconnect_without_polling_pump_deadlines");
    g_app.account.instance = reinterpret_cast<void*>(1);
    scoped_read_wake = reconnect_wake = 0;
    // A shown toast's hold ends without a frame of its own.
    toast_wake = 1400;
    g_app.frame_pending = false;
    application_toast_tick(1000);
    check(!g_app.frame_pending && application_wait_timeout(1000, 1000000, false) == 400,
          "toast_future_phase_waits_without_a_frame");
    application_toast_tick(1400);
    check(g_app.frame_pending && application_visual_frame_requested(1400, 1400000),
          "due_toast_phase_requests_its_frame");
    toast_wake = 0;
    g_app.frame_pending = false;
    const auto original_primary = g_app.message_history_request_owner;
    const auto original_details = g_app.details_request_owner;
    g_app.message_history_request_owner = MessageHistoryRequestOwner::catchup;
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    pins_request_wake = 1400;
    check(application_wait_timeout(1000, 1000000, true) == 400 && !application_nonvisual_wake_due(1000) &&
              !application_visual_frame_requested(1000, 1000000),
          "pins_future_retry_uses_details_deadline_even_while_primary_history_lane_is_busy");
    pins_request_wake = 1000;
    check(application_nonvisual_wake_due(1000) && application_wait_timeout(1000, 1000000, true) == 0,
          "pins_due_retry_can_admit_independently_of_primary_history_work");
    g_app.details_request_owner = MessageHistoryRequestOwner::user_profile;
    check(!application_nonvisual_wake_due(1000) &&
              application_wait_timeout(1000, 1000000, true) == INFINITE &&
              !application_visual_frame_requested(1000, 1000000),
          "pins_due_retry_waits_for_busy_details_receipt_without_zero_timeout_polling");
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    check(application_nonvisual_wake_due(1000) && application_wait_timeout(1000, 1000000, true) == 0,
          "pins_due_retry_resumes_when_details_receipt_releases_lane");
    pins_request_wake = 0;
    browser_request_wake = 1400;
    check(application_wait_timeout(1000, 1000000, true) == 400 && !application_nonvisual_wake_due(1000),
          "thread_browser_future_retry_uses_details_deadline_while_primary_is_busy");
    browser_request_wake = 1000;
    check(application_nonvisual_wake_due(1000) && application_wait_timeout(1000, 1000000, true) == 0,
          "thread_browser_due_retry_wakes_free_details_lane");
    g_app.details_request_owner = MessageHistoryRequestOwner::thread_actions;
    check(!application_nonvisual_wake_due(1000) &&
              application_wait_timeout(1000, 1000000, true) == INFINITE,
          "thread_browser_due_retry_waits_for_exact_busy_details_receipt");
    // The browser has no field; the header search bar's caret serves it.
    search_caret_wake = 1000;
    check(application_visual_frame_requested(1000, 1000000) && !application_nonvisual_wake_due(1000) &&
              application_wait_timeout(1000, 1000000, true) == INFINITE &&
              application_wait_timeout(1000, 1000000, false) == 0,
          "header_search_caret_over_thread_browser_remains_visual_while_details_lane_is_busy");
    browser_request_wake = search_caret_wake = 0;
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    creation_request_wake = 1400;
    check(application_wait_timeout(1000, 1000000, true) == 400 && !application_nonvisual_wake_due(1000),
          "creation_future_reconcile_waits_for_its_details_deadline_while_primary_is_busy");
    creation_request_wake = 1;
    check(application_nonvisual_wake_due(1000) && application_wait_timeout(1000, 1000000, true) == 0,
          "creation_fresh_intent_uses_nonzero_immediate_wake_on_free_details_lane");
    g_app.details_request_owner = MessageHistoryRequestOwner::thread_browser;
    check(!application_nonvisual_wake_due(1000) &&
              application_wait_timeout(1000, 1000000, true) == INFINITE,
          "creation_intent_waits_for_exact_occupied_details_receipt_without_polling");
    creation_request_wake = 0;
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    forum_request_wake = 1400;
    check(application_wait_timeout(1000, 1000000, true) == 400 && !application_nonvisual_wake_due(1000),
          "forum_metadata_retry_waits_for_its_details_deadline_while_primary_is_busy");
    forum_request_wake = 1;
    check(application_nonvisual_wake_due(1000) && application_wait_timeout(1000, 1000000, true) == 0,
          "forum_fresh_metadata_intent_uses_nonzero_immediate_wake_on_free_details_lane");
    g_app.details_request_owner = MessageHistoryRequestOwner::thread_browser;
    check(!application_nonvisual_wake_due(1000) &&
              application_wait_timeout(1000, 1000000, true) == INFINITE,
          "forum_intent_waits_for_exact_occupied_details_receipt_without_polling");
    forum_request_wake = 0;
    g_app.message_history_request_owner = original_primary;
    g_app.details_request_owner = original_details;
    ThreadMemberListSubscriptionCommand thread_command{};
    auto& thread_interest = g_app.thread_member_list_subscription;
    check(
        thread_member_list_subscription_select(&thread_interest, 7, 71, true, 1000) &&
            thread_member_list_subscription_fail(&thread_interest, 1000) &&
            thread_interest.retry_at_ms == 1100 && !g_app.member_list_subscription.retry_at_ms &&
            application_wait_timeout(1000, 1000000, true) == 100 &&
            application_wait_timeout(1000, 1000000, false) == 100 &&
            !application_nonvisual_wake_due(1000) && !application_visual_frame_requested(1000, 1000000),
        "thread_only_refresh_backpressure_uses_bounded_account_deadline_without_ordinary_member_interest");
    check(!thread_member_list_subscription_command(thread_interest, 1099, &thread_command) &&
              application_wait_timeout(1099, 1099000, true) == 1 && !application_nonvisual_wake_due(1099) &&
              thread_member_list_subscription_command(thread_interest, 1100, &thread_command) &&
              thread_command.guild_id == 7 && thread_command.thread_id == 71 &&
              application_nonvisual_wake_due(1100) && application_wait_timeout(1100, 1100000, true) == 0 &&
              !application_visual_frame_requested(1100, 1100000),
          "thread_only_retry_becomes_nonvisual_account_work_at_exact_deadline");
    thread_member_list_subscription_sent(&thread_interest, thread_command, 1100);
    check(!thread_interest.retry_at_ms && application_wait_timeout(1100, 1100000, true) == INFINITE &&
              !application_nonvisual_wake_due(1100),
          "thread_only_refresh_admission_returns_to_idle_without_periodic_wakes");
    check(thread_member_list_subscription_select(&thread_interest, 0, 0, false, 1200) &&
              thread_member_list_subscription_fail(&thread_interest, 1200) &&
              application_wait_timeout(1200, 1200000, true) == 100 &&
              !application_nonvisual_wake_due(1200) &&
              thread_member_list_subscription_command(thread_interest, 1300, &thread_command) &&
              thread_command.guild_id == 7 && !thread_command.thread_id &&
              application_nonvisual_wake_due(1300),
          "hidden_thread_interest_release_retains_scoped_bounded_retry_until_clear_admitted");
    thread_member_list_subscription_sent(&thread_interest, thread_command, 1300);
    check(!thread_interest.sent_guild_id && !thread_interest.sent_thread_id &&
              !thread_interest.retry_at_ms && application_wait_timeout(1300, 1300000, true) == INFINITE &&
              !application_nonvisual_wake_due(1300),
          "hidden_thread_release_admission_has_no_residual_scheduler_deadline");
    thread_member_list_subscription_complete(&thread_interest);
    search_request_wake = 1400;
    check(application_wait_timeout(1000, 1000000, true) == 400 &&
              !application_visual_frame_requested(1000, 1000000),
          "search_indexing_retry_is_network_deadline_not_animation");
    search_request_wake = 1000;
    check(application_nonvisual_wake_due(1000) && application_wait_timeout(1000, 1000000, true) == 0,
          "search_indexing_retry_runs_account_work_when_due");
    search_request_wake = 0;
    self_status_wake = 1400;
    check(application_wait_timeout(1000, 1000000, true) == 400 &&
              !application_visual_frame_requested(1000, 1000000),
          "saved_status_retry_waits_on_network_deadline_without_animation");
    self_status_wake = 1000;
    check(application_nonvisual_wake_due(1000), "saved_status_deadline_runs_account_work");
    self_status_wake = 0;
    g_app.account.login_instance = reinterpret_cast<void*>(1);
    g_app.account.login_status.phase = AccountLoginPhase::browser_waiting;
    g_app.account.login_status.browser_deadline_ms = 1400;
    check(application_wait_timeout(1000, 1000000, true) == 400 && !application_nonvisual_wake_due(1000),
          "browser_receiver_expiry_waits_without_account_polling");
    g_app.account.login_status.browser_deadline_ms = 1000;
    check(application_wait_timeout(1000, 1000000, true) == 0 && !application_nonvisual_wake_due(1000),
          "browser_receiver_expiry_wakes_login_only");
    g_app.account.login_status.phase = AccountLoginPhase::browser_expired;
    check(application_login_browser_deadline() == 0, "terminal_browser_status_has_no_expiry_polling");
    g_app.account.login_status = {};
    g_app.account.login_instance = nullptr;
    g_app.renderer.text_effect_animated = true;
    command_wake = 1000;
    check(application_wait_timeout(1000, 1000000, true) == 0 && application_nonvisual_wake_due(1000),
          "command_deadline_still_runs_account_work_while_dxgi_waits");
    command_wake = 0;
    friends_wake = 1040;
    check(!application_nonvisual_wake_due(1000) && application_wait_timeout(1000, 1000000, true) == 40,
          "friends_retry_uses_deadline_without_polling");
    friends_wake = 1000;
    check(application_wait_timeout(1000, 1000000, true) == 0 && application_nonvisual_wake_due(1000),
          "friends_deadline_runs_account_work_while_dxgi_waits");
    friends_wake = 0;
    notification_wake = 1040;
    check(!application_nonvisual_wake_due(1000) && application_wait_timeout(1000, 1000000, true) == 40,
          "sound_trailing_deadline_waits_without_account_polling");
    notification_wake = 1000;
    check(application_wait_timeout(1000, 1000000, true) == 0 && application_nonvisual_wake_due(1000),
          "sound_trailing_deadline_runs_account_work_while_dxgi_waits");
    notification_wake = 0;
    recovery_wake = 50'001'000;
    check(application_wait_timeout(1000, 1000000, true) == 50'000'000 &&
              !application_nonvisual_wake_due(1000),
          "recovery_cooldown_keeps_long_server_deadline_without_account_polling");
    recovery_wake = 1000;
    check(application_wait_timeout(1000, 1000000, true) == 0 && !application_nonvisual_wake_due(1000),
          "recovery_cooldown_expiry_wakes_ui_without_account_work");
    recovery_wake = 0;
    reset_wake = 50'001'000;
    check(application_wait_timeout(1000, 1000000, true) == 50'000'000 &&
              !application_nonvisual_wake_due(1000),
          "reset_cooldown_keeps_server_deadline_without_account_polling");
    reset_wake = 1000;
    check(application_wait_timeout(1000, 1000000, true) == 0 && !application_nonvisual_wake_due(1000),
          "reset_cooldown_expiry_wakes_ui_without_submitting_or_pumping_account_work");
    reset_wake = 0;
    phone_wake = 50'001'000;
    check(application_wait_timeout(1000, 1000000, true) == 50'000'000 &&
              !application_nonvisual_wake_due(1000),
          "phone_cooldown_retains_server_deadline_without_account_polling");
    phone_wake = 1000;
    check(application_wait_timeout(1000, 1000000, true) == 0 && !application_nonvisual_wake_due(1000),
          "phone_cooldown_expiry_wakes_only_ui_without_automatic_verify_or_resend");
    phone_wake = 0;
    check(application_wait_timeout(1000, 1000000, true) == INFINITE,
          "cleared_phone_deadline_returns_to_wait_driven_idle");
    g_app.account.status.next_wake_at_ms = 1000;
    check(application_nonvisual_wake_due(1000), "account_deadline_still_wakes");
    g_app.account.status.next_wake_at_ms = 0;
    g_app.renderer.text_effect_animated = false;
    g_app.frame_rate.next_update_us = 1000501;
    check(application_wait_timeout(1000, 1000000, false) == 1,
          "fps_deadline_uses_supplied_qpc_time_and_rounds_up");
    g_app.frame_rate = {};
    g_app.frame_pending = true;
    check(application_wait_timeout(1000, 1000000, false) == 0 &&
              application_wait_timeout(1000, 1000000, true) == INFINITE,
          "dirty_frame_falls_back_without_spinning_on_dxgi");
    g_app.frame_pending = false;
    g_app.renderer.text_effect_next_ms = 0;
    g_app.account = {};
}

void test_member_role_refresh() noexcept {
    auto& ui = *g_app.account_ui;
    auto& event_pump = g_app.message_pump;
    const auto refresh = event_pump.member_roles_refresh;
    void* const context = event_pump.member_roles_refresh_context;
    check(refresh && context == &g_app, "role_refresh_callback_installed_by_production_account_pump");
    if (!refresh)
        return;
    GuildRecord guilds[1]{};
    ChannelRecord channels[1]{};
    RoleRecord roles[2]{};
    PermissionOverwriteRecord overwrites[1]{};
    GuildMemberRecord members[2]{};
    u16 guild_order[1]{}, channel_order[1]{};
    u32 member_order[2]{};
    u8 names[64]{};
    MessageTimeline timeline{};
    const bool initialized =
        chat_state_store_initialize(&ui.chat_state, {guilds, 1}, {guild_order, 1}, {channels, 1},
                                       {channel_order, 1}, {names, 64}) &&
        chat_state_store_bind_permissions(&ui.chat_state, {roles, 2}, {overwrites, 1}) &&
        guild_member_store_initialize(&ui.guild_member_store, {members, 2}, {member_order, 2});
    check(initialized, "role_refresh_production_callback_bounded_authority_fixture");
    if (!initialized)
        return;
    guilds[0].id = 7;
    guilds[0].owner_id = 42;
    guilds[0].member_roles_known = true;
    guilds[0].occupied = true;
    ui.chat_state.guild_count = 1;
    guild_order[0] = 0;
    channels[0].id = 70;
    channels[0].guild_id = 7;
    channels[0].occupied = true;
    ui.chat_state.channel_count = 1;
    channel_order[0] = 0;
    ui.chat_state.selected_channel_id = 70;
    ui.chat_state.self_user_id = 42;
    event_pump.timeline = &timeline;
    event_pump.state = &ui.chat_state;
    event_pump.members = &ui.guild_member_store;
    const u64 assigned[] = {17};
    const u64 now = clock_monotonic_milliseconds();
    ThreadMemberListSubscriptionCommand command{};
    const bool staged =
        chat_state_store_upsert_role(&ui.chat_state, {7, 7, 1ull << 10, {}}) &&
        chat_state_store_upsert_role(&ui.chat_state, {17, 7, 0, {}, 5, 0x9922CC, true}) &&
        guild_member_store_upsert(&ui.guild_member_store, ui.chat_state, 7, 20, {}, {assigned, 1},
                                          true, GuildMemberStatus::online, 0, 70)
            .ok() &&
        member_list_subscription_viewport(&g_app.member_list_subscription, 7, 70, 200, 10, 6, true, now) &&
        thread_member_list_subscription_select(&g_app.thread_member_list_subscription, 7, 71, true, now) &&
        thread_member_list_subscription_command(g_app.thread_member_list_subscription, now, &command);
    check(staged, "role_refresh_existing_member_and_thread_interest_staged");
    if (staged) {
        member_list_subscription_sent(&g_app.member_list_subscription);
        thread_member_list_subscription_sent(&g_app.thread_member_list_subscription, command, now);
        const auto range_count = g_app.member_list_subscription.range_count;
        GatewayClientEvent role{};
        role.kind = GatewayClientEventKind::dispatch;
        role.dispatch_kind = GatewayClientDispatchKind::guild_role_update;
        role.state.id = 17;
        role.state.guild_id = 7;
        role.state.position = 5;
        role.state.hoist = true;
        role.state.fields = gateway_client_state_field_id | gateway_client_state_field_guild_id |
                            gateway_client_state_field_permissions;
        const bool applied = message_event_pump_consume(&event_pump, role, {}).ok();
        const u64 deadline = g_app.member_list_subscription.retry_at_ms;
        const auto* member = guild_member_store_find(ui.guild_member_store, 7, 20);
        check(
            applied && member && !member->roles_known && !member->role_color && range_count &&
                !g_app.member_list_subscription.sent_range_count &&
                g_app.member_list_subscription.range_count == range_count && deadline >= now &&
                member_list_subscription_due(g_app.member_list_subscription, deadline) &&
                thread_member_list_subscription_command(g_app.thread_member_list_subscription, deadline,
                                                        &command) &&
                command.guild_id == 7 && command.thread_id == 71,
            "role_refresh_postcommit_invalidates_admitted_ranges_and_refreshes_only_matching_thread_scope");
        const auto list_before = g_app.member_list_subscription;
        const auto thread_before = g_app.thread_member_list_subscription;
        refresh(context, 8);
        refresh(nullptr, 7);
        ui.chat_state.selected_channel_id = 999;
        refresh(context, 7);
        ui.chat_state.selected_channel_id = 70;
        check(
            bytes_equal(&list_before, &g_app.member_list_subscription, sizeof(list_before)) &&
                bytes_equal(&thread_before, &g_app.thread_member_list_subscription, sizeof(thread_before)),
            "role_refresh_foreign_guild_foreign_context_and_missing_current_channel_do_not_change_"
            "interest");
        check(member_list_subscription_fail(&g_app.member_list_subscription, deadline) &&
                  thread_member_list_subscription_fail(&g_app.thread_member_list_subscription, deadline) &&
                  g_app.member_list_subscription.retry_at_ms ==
                      deadline + kMemberListSubscriptionRetryMinimumMs &&
                  !member_list_subscription_due(g_app.member_list_subscription, deadline) &&
                  application_wait_timeout(deadline, deadline * 1000, true) ==
                      kMemberListSubscriptionRetryMinimumMs &&
                  !application_nonvisual_wake_due(deadline),
              "role_refresh_transport_backpressure_uses_existing_bounded_retry_instead_of_hot_loop");
        member_list_subscription_sent(&g_app.member_list_subscription);
        thread_member_list_subscription_sent(&g_app.thread_member_list_subscription, command, deadline);
        check(!g_app.member_list_subscription.retry_at_ms &&
                  !thread_member_list_subscription_command(g_app.thread_member_list_subscription, deadline,
                                                           &command) &&
                  application_wait_timeout(deadline, deadline * 1000, true) == INFINITE,
              "role_refresh_successful_admission_returns_scheduler_to_wait_driven_idle");
        member_list_subscription_complete(&g_app.member_list_subscription);
        thread_member_list_subscription_complete(&g_app.thread_member_list_subscription);
        refresh(context, 7);
        check(!g_app.member_list_subscription.guild_id && !g_app.member_list_subscription.retry_at_ms &&
                  !g_app.thread_member_list_subscription.guild_id &&
                  !g_app.thread_member_list_subscription.retry_at_ms,
              "role_refresh_hidden_interest_does_not_create_a_subscription_or_wake");
    }
    const auto previous_gateway_source = g_app.account.api.gateway_source;
    const u32 previous_gateway_sources = g_app.account.status.gateway_sources;
    const u32 previous_next_gateway_sources = next_status.gateway_sources;
    g_app.account.api.gateway_source = diagnostic_source;
    g_app.account.status.gateway_sources = 1;
    next_status.gateway_sources = 1;
    scoped_gateway_event = {};
    scoped_gateway_event.kind = GatewayClientEventKind::dispatch;
    scoped_gateway_event.dispatch_kind = GatewayClientDispatchKind::channel_delete;
    scoped_gateway_event.state.id = 999;
    scoped_gateway_event.state.guild_id = 7;
    scoped_gateway_event.state.parent_id = 70;
    scoped_gateway_event.state.type = 11;
    scoped_gateway_event.state.fields =
        gateway_client_state_field_id | gateway_client_state_field_guild_id |
        gateway_client_state_field_parent_id | gateway_client_state_field_type;
    scoped_gateway_event_ready = true;
    forum_gateway_notifications = 0;
    check(application_pump_account() && forum_gateway_notifications == 1 && forum_gateway_guild == 7 &&
              forum_gateway_parent == 70 && forum_gateway_thread == 999,
          "forum flight receives exact uncached Gateway deletion while the rejected Core apply is skipped");
    event_pump.resync_required = false;
    scoped_gateway_event.dispatch_kind = GatewayClientDispatchKind::channel_update;
    scoped_gateway_event.state.id = 70;
    scoped_gateway_event.state.parent_id = 0;
    scoped_gateway_event.state.type = 15;
    scoped_gateway_event.state.flags = 16;
    scoped_gateway_event.state.fields |= gateway_client_state_field_flags;
    scoped_gateway_event_ready = true;
    forum_gateway_notifications = 0;
    check(application_pump_account() && forum_gateway_notifications == 1 && forum_gateway_guild == 7 &&
              forum_gateway_parent == 70 && !forum_gateway_thread,
          "forum parent metadata authority retires separately from exact child thread changes");
    scoped_gateway_event_ready = true;
    forum_gateway_notifications = 0;
    browser_gateway_notifications = 0;
    check(application_pump_account() && forum_gateway_notifications == 1 &&
              browser_gateway_notifications == 1,
          "repeated forum Gateway update conservatively advances Core and metadata authority once");
    scoped_gateway_event.state.type = 0;
    scoped_gateway_event_ready = true;
    forum_gateway_notifications = 0;
    check(application_pump_account() && forum_gateway_notifications == 1,
          "forum parent type withdrawal retires current metadata authority");
    scoped_gateway_event_ready = true;
    forum_gateway_notifications = 0;
    browser_gateway_notifications = 0;
    check(application_pump_account() && forum_gateway_notifications == 1 &&
              browser_gateway_notifications == 1,
          "repeated ordinary channel Gateway update retains current Core publication semantics");
    scoped_gateway_event.state.fields |= gateway_client_state_field_permission_overwrites;
    scoped_gateway_event_ready = true;
    forum_gateway_notifications = browser_gateway_notifications = 0;
    check(application_pump_account() && forum_gateway_notifications == 1 &&
              browser_gateway_notifications == 1,
          "explicit overwrite authority retains scoped browser and forum invalidation");
    g_app.account.api.gateway_source = previous_gateway_source;
    g_app.account.status.gateway_sources = previous_gateway_sources;
    next_status.gateway_sources = previous_next_gateway_sources;
    event_pump.timeline = nullptr;
    event_pump.state = nullptr;
    event_pump.members = nullptr;
    ui.chat_state = {};
    ui.guild_member_store = {};
    message_event_pump_reset(&event_pump);
    check(!event_pump.member_roles_refresh && !event_pump.member_roles_refresh_context &&
              !event_pump.message_create_observe && !event_pump.message_create_context &&
              !event_pump.read_badge && !event_pump.read_badge_context,
          "role_refresh_full_pump_reset_retires_borrowed_observer_contexts");
    reset_counts();
    check(application_pump_account() && event_pump.member_roles_refresh == refresh &&
              event_pump.member_roles_refresh_context == context &&
              event_pump.read_badge == application_notifications_read_badge &&
              event_pump.read_badge_context == &g_app,
          "role_refresh_new_account_drain_reinstalls_current_observer_after_full_reset");
    reset_counts();
}

void test_reaction_observer_binding() noexcept {
    auto& event_pump = g_app.message_pump;
    check(!event_pump.reaction_observe && event_pump.reaction_source_observe &&
              event_pump.reaction_delete_observe && event_pump.reaction_observe_context == &g_app &&
              event_pump.source_observe_poll_only,
          "empty reaction owner retains lazy poll source and deletion callbacks only");
    reaction_observation_needed = true;
    reset_counts();
    check(application_pump_account() && event_pump.reaction_observe && event_pump.reaction_source_observe &&
              event_pump.reaction_delete_observe && event_pump.reaction_observe_context == &g_app,
          "retained_reaction_intents_install_grouped_typed_observers_on_actual_account_pump");
    if (event_pump.reaction_observe && event_pump.reaction_source_observe &&
        event_pump.reaction_delete_observe) {
        const MessageHandle handle{3, 7};
        GatewayMessageReactionEvent reaction{};
        event_pump.reaction_observe(nullptr, reaction, handle, 19, true);
        check(!reaction_observations, "reaction_binder_rejects_a_foreign_callback_context");
        event_pump.reaction_observe(&g_app, reaction, handle, 19, true);
        check(reaction_observations == 1 && observed_reaction_handle.slot == 3 &&
                  observed_reaction_handle.generation == 7 && observed_reaction_revision == 19 &&
                  observed_reaction_applied,
              "reaction_binder_preserves_post_validation_handle_revision_and_application_state");
        GatewayMessageEvent message{};
        event_pump.reaction_source_observe(&g_app, message, handle, 20, false);
        event_pump.reaction_delete_observe(&g_app, 70, 900);
        check(reaction_source_observations == 1 && observed_reaction_revision == 20 &&
                  !observed_reaction_applied && reaction_delete_observations == 1,
              "reaction_binder_relays_nonresident_typed_sources_and_validated_deletion");
    }
    reaction_observation_needed = false;
    reset_counts();
    check(application_pump_account() && !event_pump.reaction_observe &&
              event_pump.reaction_source_observe && event_pump.reaction_delete_observe &&
              event_pump.reaction_observe_context == &g_app && event_pump.source_observe_poll_only,
          "settled reaction owner retires reaction callbacks while preserving lazy poll observers");
    forum_observation_needed = true;
    forum_source_observations = forum_delete_observations = 0;
    reset_counts();
    check(application_pump_account() && !event_pump.reaction_observe &&
              event_pump.reaction_source_observe && event_pump.reaction_delete_observe &&
              event_pump.reaction_observe_context == &g_app,
          "pending forum post installs decoded source and deletion observers without reaction intent");
    if (event_pump.reaction_source_observe && event_pump.reaction_delete_observe) {
        GatewayMessageEvent starter{};
        starter.channel_id = 901;
        starter.id = 902;
        event_pump.reaction_source_observe(nullptr, starter, {~u32{0}, 0}, 0, false);
        event_pump.reaction_delete_observe(nullptr, 901, 902);
        check(!forum_source_observations && !forum_delete_observations,
              "forum source and deletion binder rejects foreign callback contexts");
        event_pump.reaction_source_observe(&g_app, starter, {~u32{0}, 0}, 0, false);
        check(forum_source_observations == 1 && forum_message_channel == 901 && forum_message_id == 902,
              "forum source binder preserves exact nonresident starter identity");
        event_pump.reaction_delete_observe(&g_app, 901, 902);
        check(forum_delete_observations == 1 && forum_message_channel == 901 && forum_message_id == 902,
              "forum deletion binder preserves exact validated starter identity");
    }
    forum_observation_needed = false;
    reset_counts();
    check(application_pump_account() && event_pump.reaction_source_observe &&
              event_pump.reaction_delete_observe && event_pump.reaction_observe_context == &g_app &&
              event_pump.source_observe_poll_only,
          "settled forum post retires broad sources while preserving lazy poll observers");
    reset_counts();
}

void test_message_navigation_lifecycle() noexcept {
    // Exercise the production account drain, strict typed message decoders,
    // resident timeline/Core permissions and the shared navigation owner.
    // Only transport admission and final native selection are fixture seams.
    auto& ui = *g_app.account_ui;
    const auto previous_status = next_status;
    const auto previous_actual_status = g_app.account.status;
    const u8 previous_observed_phase = g_app.account.last_phase;
    const auto previous_overview = g_app.account.api.channel_overview;
    const auto previous_pins = g_app.account.api.pinned_messages;
    const auto previous_gateway_source = g_app.account.api.gateway_source;
    const u64 previous_scope = g_app.plugin_account_scope_serial;
    GuildRecord guilds[1]{};
    ChannelRecord channels[8]{};
    RoleRecord roles[2]{};
    PermissionOverwriteRecord overwrites[4]{};
    u16 guild_order[1]{}, channel_order[8]{};
    u8 names[4096]{}, text[1024]{};
    MessageRecord messages[8]{};
    u32 message_order[8]{}, item_units[8]{};
    u64 fenwick[8]{}, bulk_ids[kGatewayBulkDeleteMaximumIds]{}, ready_guilds[8]{};
    g_app.account.api.channel_overview = navigation_fetch;
    g_app.account.api.pinned_messages = navigation_cancel;
    g_app.account.api.gateway_source = diagnostic_source;
    next_status.gateway_sources = 1;
    next_status.running = true;
    next_status.profile_id = 5;
    next_status.coordinator_phase = static_cast<u8>(UserAccountCoordinatorPhase::running);
    g_app.account.status = next_status;
    g_app.plugin_account_scope_serial = 31;
    reaction_observation_needed = forum_observation_needed = false;
    constexpr auto held_body =
        utf8_literal(R"({"id":"999","guild_id":"7","type":0,"name":"Target","permission_overwrites":[]})");
    for (u32 scenario = 0; scenario < 11; ++scenario) {
        application_message_navigation_release();
        g_app.plugin_account_scope_serial = 31;
        g_app.pending_message_jump_channel_id = g_app.pending_message_jump_message_id = 0;
        bool initialized =
            chat_state_store_initialize(&ui.chat_state, {guilds, 1}, {guild_order, 1}, {channels, 8},
                                           {channel_order, 8}, {names, sizeof(names)}) &&
            chat_state_store_bind_permissions(&ui.chat_state, {roles, 2}, {overwrites, 4}) &&
            chat_state_store_upsert_guild(
                &ui.chat_state, {7, utf8_literal("Guild"), GuildMutationKind::create, 99})
                .ok() &&
            chat_state_store_upsert_channel(&ui.chat_state,
                                               {70, 7, 0, utf8_literal("Source"), 0, 0, 0})
                .ok() &&
            message_store_initialize(&g_app.message_view.store, {messages, 8}, {message_order, 8},
                                     {text, sizeof(text)}) &&
            message_store_upsert(&g_app.message_view.store,
                                 {900, 70, 42, 100, 0, utf8_literal("Permalink source")})
                .ok() &&
            ui_variable_list_initialize(&g_app.message_view.list, {item_units, 8}, {fenwick, 8}, 1, 100) &&
            message_timeline_initialize(&g_app.message_view.timeline, &g_app.message_view.store,
                                        &g_app.message_view.list, 100, 1000, true);
        ui.chat_state.self_user_id = 42;
        initialized = initialized &&
                      chat_state_store_upsert_role(
                          &ui.chat_state,
                          {7, 7, kPermissionViewChannel | kPermissionReadMessageHistory,
                           utf8_literal("Everyone")}) &&
                      chat_state_store_set_self_roles(&ui.chat_state, 7, 99, 42, {});
        chat_state_store_recompute_guild_permissions(&ui.chat_state, 7);
        initialized = initialized && chat_state_store_select_channel(&ui.chat_state, 70).ok();
        ui.active_channel_id = 70;
        g_app.message_pump.timeline = &g_app.message_view.timeline;
        g_app.message_pump.state = &ui.chat_state;
        g_app.message_pump.bulk_id_scratch = {bulk_ids, kGatewayBulkDeleteMaximumIds};
        g_app.message_pump.guild_id_scratch = {ready_guilds, 8};
        g_app.message_pump.payload_scratch = {ui.event_payload_scratch,
                                              kGatewayClientDispatchPayloadCapacity};
        g_app.message_pump.resync_required = false;
        const auto found = message_store_find(g_app.message_view.store, 900);
        const auto* record =
            found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
        ApplicationMessageNavigationSource source{};
        navigation_fetches = navigation_cancels = navigation_moves = 0;
        navigation_cancelled_id = 0;
        navigation_cancel_before_reset = false;
        check(
            initialized && record &&
                application_message_navigation_source(70, 900, found.handle, record ? record->revision : 0,
                                                      &source) &&
                application_message_navigation_link(source, {7, 999, 1001}) &&
                application_search_navigation_submit(500 + scenario, clock_monotonic_milliseconds()) &&
                navigation_fetches == 1 && application_message_navigation_observation_needed(),
            "timeline permalink actual shared owner admits one metadata GET from readable resident source");
        if (scenario == 10) {
            RestClientResponse limited{};
            limited.request_id = 510;
            limited.status_code = 429;
            limited.rate_limited = true;
            const u64 now = clock_monotonic_milliseconds();
            limited.retry_at_ms = now + 400;
            check(application_search_navigation_response(limited, {}) &&
                      application_wait_timeout(now, now * 1000, true) == 400,
                  "navigation_rate_limit_schedules_retry_wake");
            application_message_navigation_release();
            continue;
        }
        reset_counts();
        check(application_pump_account() && !g_app.message_pump.reaction_observe &&
                  g_app.message_pump.reaction_source_observe &&
                  g_app.message_pump.source_observe_poll_only &&
                  g_app.message_pump.reaction_delete_observe &&
                  g_app.message_pump.reaction_observe_context == &g_app,
              "timeline hydration alone installs validated deletion observer without reactions replies or "
              "forum intent");
        if (scenario == 7) {
            const auto observer = g_app.message_pump.reaction_delete_observe;
            observer(nullptr, 70, 900);
            observer(&g_app, 70, 900);
            check(!navigation_cancels && application_message_navigation_active(),
                  "navigation deletion binder rejects foreign and stale out-of-drain callbacks");
        }
        scoped_gateway_event = {};
        scoped_gateway_payload_bytes = 0;
        scoped_gateway_event.kind = GatewayClientEventKind::dispatch;
        if (scenario == 0 || scenario == 5 || scenario == 8) {
            scoped_gateway_event.dispatch_kind = GatewayClientDispatchKind::channel_delete;
            scoped_gateway_event.state.id = 999;
            scoped_gateway_event.state.guild_id = 7;
            scoped_gateway_event.state.parent_id = 70;
            scoped_gateway_event.state.type = 11;
            scoped_gateway_event.state.fields =
                gateway_client_state_field_id | gateway_client_state_field_guild_id |
                gateway_client_state_field_parent_id | gateway_client_state_field_type;
            if (scenario == 5)
                scoped_gateway_event.event_decode_error = GatewayEventDecodeError::invalid_input;
            if (scenario == 8)
                scoped_gateway_replace_scope = true;
        } else if (scenario == 1 || scenario == 2 || scenario == 6 || scenario == 7) {
            scoped_gateway_event.dispatch_kind = scenario == 2
                                                     ? GatewayClientDispatchKind::message_bulk_delete
                                                     : GatewayClientDispatchKind::message_delete;
            scoped_gateway_event.message.channel_id = 70;
            scoped_gateway_event.message.id = scenario == 7 ? 0 : 900;
            if (scenario == 6)
                scoped_gateway_event.event_decode_error = GatewayEventDecodeError::invalid_input;
            if (scenario == 2) {
                const u64 deleted[]{905, 900};
                CopyMemory(scoped_gateway_payload, deleted, sizeof(deleted));
                scoped_gateway_payload_bytes = sizeof(deleted);
                scoped_gateway_event.message.id_count = 2;
                scoped_gateway_event.dispatch_payload_bytes = sizeof(deleted);
            }
        } else if (scenario == 9) {
            scoped_gateway_event.kind = GatewayClientEventKind::ready;
            scoped_gateway_event.state.id = 42;
            scoped_gateway_event.state.record_count = 2;
            const u64 duplicated_guilds[]{7, 7};
            CopyMemory(scoped_gateway_payload, duplicated_guilds, sizeof(duplicated_guilds));
            scoped_gateway_payload_bytes = sizeof(duplicated_guilds);
            scoped_gateway_event.dispatch_payload_bytes = sizeof(duplicated_guilds);
        } else {
            scoped_gateway_event.kind =
                scenario == 3 ? GatewayClientEventKind::ready : GatewayClientEventKind::resumed;
            scoped_gateway_event.state.id = 42;
        }
        scoped_gateway_event_ready = true;
        reset_counts();
        const bool pumped = application_pump_account();
        if (scenario <= 3) {
            check(navigation_cancels == 1 && navigation_cancelled_id == 500 + scenario &&
                      !application_message_navigation_active() &&
                      application_message_navigation_observation_needed(),
                  "validated Gateway deletion or fresh READY cancels exact hydration but retains matching "
                  "completion fence");
            if (scenario == 0)
                check(pumped && !chat_state_store_find_channel(ui.chat_state, 999),
                      "uncached THREAD_DELETE tombstone reaches navigation and the rejected Core apply is "
                      "skipped without stopping the session");
            if (scenario == 1 || scenario == 2)
                check(pumped && !message_store_find(g_app.message_view.store, 900).ok(),
                      "validated single and bulk deletes apply real timeline deletion before owner "
                      "cancellation");
            if (scenario == 3)
                check(navigation_cancel_before_reset,
                      "fresh READY cancels metadata before Core replaces resident source authority");
        } else {
            check(!navigation_cancels && application_message_navigation_active(),
                  "resumed malformed or foreign-scope Gateway receipt cannot create navigation tombstone "
                  "or cancellation");
            if (scenario == 4) {
                const GatewayClientEventKind preserved[]{GatewayClientEventKind::reconnecting,
                                                         GatewayClientEventKind::closed};
                for (auto kind : preserved) {
                    scoped_gateway_event = {};
                    scoped_gateway_event.kind = kind;
                    scoped_gateway_event_ready = true;
                    reset_counts();
                    check(application_pump_account() && !navigation_cancels &&
                              application_message_navigation_active(),
                          "reconnect and closed lifecycle preserve intent until an actual fresh READY "
                          "arrives");
                }
                scoped_gateway_event = {};
                scoped_gateway_event.kind = GatewayClientEventKind::dispatch;
                scoped_gateway_event.dispatch_kind = GatewayClientDispatchKind::ready_state_complete;
                scoped_gateway_event_ready = true;
                reset_counts();
                check(application_pump_account() && !navigation_cancels &&
                          application_message_navigation_active(),
                      "READY state completion preserves an intent within the current restored session");
            }
            if (scenario != 5 && scenario != 7 && scenario != 8 && scenario != 9)
                application_message_navigation_cancel();
        }
        RestClientResponse held{};
        held.request_id = 500 + scenario;
        held.status_code = 200;
        held.body_bytes = held_body.bytes;
        const auto mutations = ui.chat_state.channel_mutations;
        const bool consumed = application_search_navigation_response(held, held_body);
        if (scenario == 5 || scenario == 7 || scenario == 9) {
            check(consumed && navigation_moves == 1 &&
                      chat_state_store_find_channel(ui.chat_state, 999),
                  "malformed Gateway receipt cannot tombstone valid held metadata or deny actual "
                  "destination admission");
            application_message_navigation_release();
        } else
            check(consumed && !navigation_moves &&
                      !chat_state_store_find_channel(ui.chat_state, 999) &&
                      ui.chat_state.channel_mutations == mutations &&
                      !application_message_navigation_observation_needed(),
                  "held exact 200 after retirement drains without resurrecting destination or selecting "
                  "timeline");
        reset_counts();
        check(application_pump_account() && g_app.message_pump.reaction_delete_observe &&
                  g_app.message_pump.reaction_observe_context == &g_app &&
                  g_app.message_pump.source_observe_poll_only &&
                  !application_message_navigation_observation_needed(),
              "drained navigation intent retains only lazy poll model observers on next account pump");
    }
    application_message_navigation_release();
    message_event_pump_reset(&g_app.message_pump);
    ui.chat_state = {};
    g_app.message_view = {};
    g_app.account.api.channel_overview = previous_overview;
    g_app.account.api.pinned_messages = previous_pins;
    g_app.account.api.gateway_source = previous_gateway_source;
    next_status = previous_status;
    g_app.account.status = previous_actual_status;
    // The fixture pumps a running account. Restore its observation stamp too,
    // so the surrounding unchanged-account case sees its original phase.
    g_app.account.last_phase = previous_observed_phase;
    g_app.plugin_account_scope_serial = previous_scope;
    scoped_gateway_event_ready = scoped_gateway_replace_scope = false;
    scoped_gateway_payload_bytes = 0;
    reset_counts();
}

void test_poll_gateway_receipts() noexcept {
    auto& ui = *g_app.account_ui;
    const auto old_status = next_status;
    const auto old_actual = g_app.account.status;
    const u8 old_phase = g_app.account.last_phase;
    const auto old_source = g_app.account.api.gateway_source;
    const auto old_scope = g_app.plugin_account_scope_serial;
    const HWND old_window = g_app.window;
    GuildRecord guilds[1]{}; ChannelRecord channels[2]{};
    u16 guild_order[1]{}, channel_order[2]{}; u8 names[128]{}, text[1024]{};
    MessageRecord messages[2]{}; u32 order_slots[2]{}, units[2]{};
    u64 fenwick[2]{}, bulk[kGatewayBulkDeleteMaximumIds]{}, ready[2]{};
    const bool initialized = chat_state_store_initialize(&ui.chat_state, {guilds, 1}, {guild_order, 1},
        {channels, 2}, {channel_order, 2}, {names, sizeof(names)}) &&
        message_store_initialize(&g_app.message_view.store, {messages, 2}, {order_slots, 2}, {text, sizeof(text)}) &&
        ui_variable_list_initialize(&g_app.message_view.list, {units, 2}, {fenwick, 2}, 0, 100) &&
        message_timeline_initialize(&g_app.message_view.timeline, &g_app.message_view.store, &g_app.message_view.list, 100, 1000, true) &&
        message_poll_store_initialize_owned(&ui.message_poll_store);
    check(initialized, "poll actual pump lazy model fixture initializes");
    if (!initialized) return;
    message_event_pump_reset(&g_app.message_pump);
    g_app.message_pump.timeline = &g_app.message_view.timeline;
    g_app.message_pump.state = &ui.chat_state;
    g_app.message_pump.bulk_id_scratch = {bulk, kGatewayBulkDeleteMaximumIds};
    g_app.message_pump.guild_id_scratch = {ready, 2};
    g_app.message_pump.payload_scratch = {ui.event_payload_scratch, kGatewayClientDispatchPayloadCapacity};
    next_status.running = true; next_status.profile_id = 5; next_status.gateway_sources = 1;
    next_status.coordinator_phase = static_cast<u8>(UserAccountCoordinatorPhase::running);
    g_app.account.status = next_status; g_app.plugin_account_scope_serial = 31;
    g_app.account.api.gateway_source = diagnostic_source;
    constexpr auto create = utf8_literal(R"({"op":0,"s":2,"t":"MESSAGE_CREATE","d":{"id":"900","channel_id":"70","author":{"id":"42"},"content":"","timestamp":"2026-10-05T00:00:00Z","poll":{"question":{"text":"Where?"},"answers":[{"answer_id":1,"poll_media":{"text":"Here"}}],"allow_multiselect":false,"layout_type":1,"expiry":null,"results":{"is_finalized":false,"answer_counts":[{"id":1,"count":0,"me_voted":false}]}}}})");
    u32 structurals[1024]{}, object_structurals[1024]{}; char mailbox[4096]{};
    GatewayEnvelope envelope{}; ConstSlice<u8> payload{};
    const bool routed = gateway_decode_envelope(create, {structurals, 1024}, 32, &envelope).ok() &&
        gateway_client_route_dispatch(envelope, {object_structurals, 1024}, 32, {mailbox, sizeof(mailbox)},
            {bulk, kGatewayBulkDeleteMaximumIds}, 42, &poll_follow_event, &payload).ok();
    check(routed && payload.count <= sizeof(poll_follow_payload), "poll create uses real Gateway mailbox encoding");
    if (routed && payload.count <= sizeof(poll_follow_payload)) {
        CopyMemory(poll_follow_payload, payload.data, payload.count); poll_follow_bytes = payload.count;
        poll_follow_ready = true; poll_seed_selected_channel = true;
        scoped_gateway_event = {}; scoped_gateway_event.kind = GatewayClientEventKind::ready;
        scoped_gateway_event.state.id = 42; scoped_gateway_event_ready = true; scoped_gateway_payload_bytes = 0;
        reset_counts();
        check(application_pump_account() && ui.chat_state.self_user_id == 42 &&
            message_poll_store_find(ui.message_poll_store, 900),
            "initial READY admits following received poll in the same source drain without selected owner");
        const auto queue = [&](Utf8View body) noexcept {
            ConstSlice<u8> encoded{};
            if (!gateway_decode_envelope(body, {structurals, 1024}, 32, &envelope).ok() ||
                !gateway_client_route_dispatch(envelope, {object_structurals, 1024}, 32,
                    {mailbox, sizeof(mailbox)}, {bulk, kGatewayBulkDeleteMaximumIds}, 42,
                    &poll_follow_event, &encoded).ok() || encoded.count > sizeof(poll_follow_payload)) return false;
            CopyMemory(poll_follow_payload, encoded.data, encoded.count);
            poll_follow_bytes = encoded.count; poll_follow_ready = true;
            return true;
        };
        constexpr auto vote = utf8_literal(R"({"op":0,"s":3,"t":"MESSAGE_POLL_VOTE_ADD","d":{"channel_id":"70","message_id":"900","user_id":"43","answer_id":1}})");
        reset_counts();
        check(queue(vote) && application_pump_account(), "real poll vote mailbox applies through account pump");
        const auto* first = message_poll_store_find(ui.message_poll_store, 900);
        const u32 first_count = first ? first->answers[0].count : 0;
        const u32 first_revision = ui.message_poll_store.revision;
        reset_counts();
        check(queue(vote) && application_pump_account() && first_count == 1 &&
            ui.message_poll_store.revision == first_revision &&
            message_poll_store_find(ui.message_poll_store, 900)->answers[0].count == first_count,
            "duplicate raw source sequence cannot count the same poll vote twice");
        // The own optimistic vote, then a debounced batch that echoes it beside two others.
        const bool own = message_poll_store_own_vote(&ui.message_poll_store, 900, 70, 42, 1).ok();
        constexpr auto votes = utf8_literal(R"({"channel_id":"70","message_id":"900","votes":[{"answer_id":1,"users":["42","44","45"]}]})");
        GatewayEnvelope batch{};
        batch.data = {votes, 0, JsonValueKind::object};
        batch.event_name = {utf8_literal("MESSAGE_POLL_VOTE_ADD_MANY"), 0, false};
        batch.sequence = 4;
        batch.has_sequence = batch.has_event_name = true;
        poll_batch_count = poll_batch_next = 0;
        const auto expanded = gateway_client_publish_poll_vote_batch(
            batch, {object_structurals, 1024}, 32, 1,
            [](void*, const GatewayClientEvent& vote, ConstSlice<u8>) noexcept {
                if (poll_batch_count == 4)
                    return false;
                poll_batch_events[poll_batch_count++] = vote;
                return true;
            },
            nullptr);
        reset_counts();
        const bool pumped = own && expanded.ok() && poll_batch_count == 3 && application_pump_account();
        const auto* batched = message_poll_store_find(ui.message_poll_store, 900);
        check(pumped && poll_batch_next == 3 && batched && batched->answers[0].count == first_count + 3 &&
                  batched->answers[0].me_voted,
              "poll vote batch applies every vote of its sequence and never recounts the own optimistic vote");
        poll_batch_count = poll_batch_next = 0;
        constexpr auto late = utf8_literal(R"({"op":0,"s":4,"t":"MESSAGE_POLL_VOTE_ADD","d":{"channel_id":"70","message_id":"900","user_id":"46","answer_id":1}})");
        reset_counts();
        check(queue(late) && application_pump_account() &&
                  message_poll_store_find(ui.message_poll_store, 900)->answers[0].count == first_count + 3,
              "a single vote reusing a batch sequence is still rejected as a replay");
        constexpr auto update = utf8_literal(R"({"op":0,"s":5,"t":"MESSAGE_UPDATE","d":{"id":"900","channel_id":"70","poll":{"results":{"is_finalized":true,"answer_counts":[{"id":1,"count":7,"me_voted":false}]}}}})");
        reset_counts(); g_app.frame_pending = false; g_app.window = reinterpret_cast<HWND>(1);
        check(queue(update) && application_pump_account() &&
            ui.message_poll_store.revision != first_revision && g_app.frame_pending,
            "poll-only nonresident model publication requests repaint without a selected poll owner");
    }
    poll_follow_ready = scoped_gateway_event_ready = poll_seed_selected_channel = false;
    message_poll_store_release(&ui.message_poll_store); message_event_pump_reset(&g_app.message_pump);
    ui.chat_state = {}; g_app.message_view = {}; ui.poll_source_sequence_mask = 0;
    g_app.account.api.gateway_source = old_source; next_status = old_status; g_app.account.status = old_actual;
    g_app.account.last_phase = old_phase;
    ui.active_channel_id = 0;
    g_app.plugin_account_scope_serial = old_scope; g_app.window = old_window; g_app.frame_pending = false; reset_counts();
}

u32 run_account_pump_tests() noexcept {
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui)
        return 1;
    g_app.account.module = reinterpret_cast<HMODULE>(1);
    g_app.account.instance = reinterpret_cast<void*>(1);
    g_app.account.api.pump = pump;
    g_app.login.status = RendererLoginStatus::idle;
    bool media_repaint = true;
    browser_blocks_read = true;
    media_repaint = false;
    check(application_pump_visible_static_media(&media_repaint) && media_repaint && !normal_media_pumps &&
              !forum_preview_pumps && covered_media_pumps == 1 && !refreshes && !history_calls,
          "browser-covered chrome animations pump without chat admission/text/history projection");
    fixture_forum_active = true;
    check(application_pump_visible_static_media(&media_repaint) && media_repaint &&
              forum_preview_pumps == 1 && !normal_media_pumps && covered_media_pumps == 1 && !refreshes &&
              !history_calls && !order,
          "browser-covered forum preview pumps without chat text/navigation/history/read projection");
    fixture_preview_ok = false;
    check(!application_pump_visible_static_media(&media_repaint) && forum_preview_pumps == 2 &&
              !normal_media_pumps && !refreshes && !history_calls,
          "browser-covered preview failure propagates without underlying projection");
    fixture_forum_active = false;
    fixture_preview_ok = true;
    browser_blocks_read = false;
    check(application_pump_visible_static_media(&media_repaint) && normal_media_pumps == 1 &&
              !media_repaint,
          "closing browser restores ordinary visible media pump");
    account_text_retry_runtime_test::run(check);
    reset_counts();
    next_status.profile_id = 1;
    next_status.coordinator_phase = static_cast<u8>(UserAccountCoordinatorPhase::gateway_connecting);
    check(application_pump_account() && refreshes == 1 && forced == 1 && history_calls == 1 && order_ok,
          "initial_account_state_admits_history_before_presentation");
    test_reaction_observer_binding();
    test_member_role_refresh();
    test_message_navigation_lifecycle();
    test_poll_gateway_receipts();
    reset_counts();
    check(application_pump_account() && refreshes == 1 && forced == 0 && history_calls == 1 && order_ok,
          "unchanged_account_state_keeps_conditional_refresh");
    check(scoped_read_syncs == 1 && scoped_read_menu_refreshes == 1,
          "unchanged_pump_observes_scoped_authority_and_menu_once_without_forced_rebuild");
    reset_counts();
    scoped_read_menu_ok = false;
    check(!application_pump_account() && scoped_read_syncs == 1 && scoped_read_menu_refreshes == 1 &&
              history_calls == 1 && !refreshes && !stops,
          "scoped_menu_refresh_failure_propagates_before_presentation_without_stopping_account");
    reset_counts();
    next_status.coordinator_phase = static_cast<u8>(UserAccountCoordinatorPhase::running);
    next_status.running = true;
    g_app.login.status = RendererLoginStatus::restoring_session;
    g_app.login.visible = true;
    check(application_pump_account() && refreshes == 1 && forced == 1 && history_calls == 1 && order_ok,
          "ready_transition_coalesces_model_and_phase_presentation");
    check(plugin_restores == 1 && !g_app.login.visible && g_app.login.status == RendererLoginStatus::idle,
          "remembered_plugins_restore_once_after_authenticated_shell_transition");
    reset_counts();
    history_ok = false;
    check(application_pump_account() && history_calls == 1 && refreshes == 1 && !stops && order_ok,
          "history_failure_keeps_presentation_and_account_alive");
    check(plugin_restores == 1, "unchanged_account_pump_never_polls_saved_plugins_again");
    reset_counts();
    forum_sync_ok = false;
    presentation_failures = 0;
    check(application_pump_account() && !stops && presentation_failures == 1 && history_calls == 1 &&
              refreshes == 1 && order_ok,
          "feature_surface_sync_failure_records_presentation_failure_and_keeps_account_alive");
    forum_sync_ok = true;
    reset_counts();
    navigation_ok = false;
    check(!application_pump_account() && stops == 1 && !refreshes && !history_calls,
          "model_failure_stops_before_presentation_or_history");
    reset_counts();
    next_status.failed = true;
    next_status.coordinator_error = static_cast<u8>(UserAccountCoordinatorError::authentication_rejected);
    check(application_pump_account() && login_returns == 1 && rejected && !refreshes && !history_calls,
          "authentication_rejection_returns_to_login_without_normal_model_drain");
    reset_counts();
    next_status.gateway_sources = 1;
    next_status.coordinator_error = static_cast<u8>(UserAccountCoordinatorError::gateway);
    g_app.account.api.gateway_source = diagnostic_source;
    g_app.account.api.gateway_pop = diagnostic_pop;
    g_app.message_pump.payload_scratch = {g_app.account_ui->event_payload_scratch,
                                          kGatewayClientDispatchPayloadCapacity};
    diagnostic_events = 3;
    check(application_pump_account() && login_returns == 1 && !rejected && diagnostic_source_reads == 1 &&
              diagnostic_pops == 4 && diagnostic_logs == 2 && diagnostic_order_ok &&
              diagnostic_stage == GatewayClientFailureStage::ready_user_route && !normal_pops &&
              !notification_callbacks && !refreshes && !history_calls &&
              g_app.message_pump.payload_scratch[0] == 0,
          "fatal_gateway_receipt_and_closed_metadata_logged_before_teardown_without_dispatch_model_"
          "callbacks_and_payload_erased");
    reset_counts();
    next_status.gateway_sources = 2;
    diagnostic_refill = true;
    check(application_pump_account() && login_returns == 1 && diagnostic_source_reads == 2 &&
              diagnostic_pops == 2 * kGatewayClientEventCapacity && diagnostic_logs == 4 && !normal_pops &&
              !notification_callbacks && !refreshes && !history_calls,
          "terminal_diagnostic_drain_has_fixed_per_source_budget_even_when_queue_replenishes");
    reset_counts();
    next_status.gateway_sources = 9;
    check(application_pump_account() && login_returns == 1 && !diagnostic_source_reads && !diagnostic_pops,
          "terminal_diagnostic_source_count_over_bound_aborts_diagnostics_before_native_queries");
    reset_counts();
    next_status.gateway_sources = 1;
    diagnostic_null_source = true;
    check(application_pump_account() && login_returns == 1 && diagnostic_source_reads == 1 &&
              !diagnostic_pops,
          "terminal_diagnostic_null_source_still_reaches_safe_teardown");
    for (u32 scenario = 0; scenario < 2; ++scenario) {
        reset_counts();
        if (!scenario)
            g_app.account.api.gateway_source = nullptr;
        else
            g_app.account.api.gateway_pop = nullptr;
        check(application_pump_account() && login_returns == 1 && !diagnostic_source_reads &&
                  !diagnostic_pops,
              "terminal_diagnostic_missing_hooks_abort_before_dispatch_without_skipping_teardown");
        g_app.account.api.gateway_source = diagnostic_source;
        g_app.account.api.gateway_pop = diagnostic_pop;
    }
    reset_counts();
    const auto valid_scratch = g_app.message_pump.payload_scratch;
    g_app.message_pump.payload_scratch = {};
    check(application_pump_account() && login_returns == 1 && !diagnostic_pops && !diagnostic_logs,
          "terminal_diagnostic_missing_scratch_aborts_without_new_storage_or_model_work");
    reset_counts();
    next_status.gateway_sources = 0;
    check(application_pump_account() && login_returns == 1 && !diagnostic_source_reads && !diagnostic_pops,
          "terminal_diagnostic_zero_sources_needs_no_scratch_or_hooks");
    g_app.message_pump.payload_scratch = valid_scratch;
    next_status.gateway_sources = 1;
    for (u32 scenario = 0; scenario < 2; ++scenario) {
        reset_counts();
        diagnostic_events = 3;
        if (!scenario)
            diagnostic_pop_error = GatewayClientPopError::output_too_small;
        else
            diagnostic_overlarge = true;
        check(application_pump_account() && login_returns == 1 && diagnostic_pops == 1 &&
                  !diagnostic_logs && !normal_pops && !notification_callbacks && valid_scratch[0] == 0,
              "terminal_diagnostic_capacity_or_corrupt_payload_stops_pop_loop_erases_scratch_and_tears_"
              "down_safely");
    }
    next_status.gateway_sources = 0;
    g_app.message_pump.payload_scratch = {};
    reset_counts();
    g_app.account.instance = nullptr;
    check(application_pump_account() && !refreshes && !history_calls && !login_returns,
          "inactive_account_does_no_presentation_work");
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    g_app.account = {};
    test_frame_deadlines();
    fixture_logout_blocked = true;
    fixture_logout_wake = 1125;
    g_app.account.status.next_wake_at_ms = 1;
    g_app.message_media.scan_pending = true;
    g_app.login.remote_restart_at_ms = 1;
    reset_counts();
    check(application_pump_account() && !refreshes && !history_calls && !login_returns &&
              application_wait_timeout(1000, 1'000'000, true) == 125 &&
              !application_nonvisual_wake_due(1000) && application_nonvisual_wake_due(1125) &&
              application_login_browser_deadline() == 0,
          "logout_uses_its_bounded_poll_while_suppressing_stale_account_and_login_deadlines");
    fixture_logout_wake = 0;
    check(application_wait_timeout(1000, 1'000'000, true) == INFINITE &&
              !application_nonvisual_wake_due(1000),
          "logout_teardown_waits_on_its_thread_without_account_timer_polling");
    fixture_logout_blocked = false;
    g_app.message_media.scan_pending = false;
    g_app.login.remote_restart_at_ms = 0;
    g_app.account = {};
    return failures;
}
void application_voice_pump() noexcept {
}
u64 application_voice_next_wake() noexcept {
    return 0;
}
u64 application_voice_timer_next_wake() noexcept {
    return 0;
}
u64 application_soundboard_next_wake(bool) noexcept {
    return 0;
}
} // namespace maicord::app_detail
#include "maicord/render/d3d11_renderer.hpp"
namespace maicord {
u64 renderer_media_next_trim_ms(const D3D11Renderer&) noexcept {
    return 0;
}
} // namespace maicord
extern "C" void maicord_account_pump_tests_entry() noexcept {
    ExitProcess(maicord::app_detail::run_account_pump_tests() ? 1 : 0);
}
