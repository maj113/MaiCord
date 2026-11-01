#pragma once

#include "../../../src/app/shell/toast.hpp"
#include "maicord/app/account/user_account_coordinator.hpp"

namespace maicord::app_detail {
namespace account_pump_detail {

extern bool fixture_logout_blocked;

extern u64 fixture_logout_wake;

extern u32 refreshes, forced, history_calls, stops, login_returns, order;

extern u32 presentation_failures;

extern bool forum_sync_ok;

extern u32 normal_media_pumps, forum_preview_pumps, covered_media_pumps;

extern bool fixture_forum_active, fixture_preview_ok;

extern bool navigation_ok, history_ok, order_ok, rejected;

extern AccountModuleStatus next_status;

extern u32 failures, plugin_restores, scoped_read_syncs, scoped_read_menu_refreshes;

extern u64 scoped_read_wake;

extern bool scoped_read_menu_ok;

extern u32 diagnostic_source_reads, diagnostic_pops, diagnostic_logs, normal_pops, notification_callbacks;

extern GatewayClientEvent scoped_gateway_event;

extern bool scoped_gateway_event_ready;

extern GatewayClientEvent poll_follow_event;

extern bool poll_follow_ready;

extern bool poll_seed_selected_channel;

extern u8 poll_follow_payload[4096];

extern u32 poll_follow_bytes;

extern GatewayClientEvent poll_batch_events[4];

extern u32 poll_batch_count, poll_batch_next;

extern bool scoped_gateway_replace_scope;

extern u8 scoped_gateway_payload[kGatewayBulkDeleteMaximumIds * sizeof(u64)];

extern u32 scoped_gateway_payload_bytes;

extern u32 navigation_fetches, navigation_cancels, navigation_moves;

extern u64 navigation_cancelled_id;

extern bool navigation_cancel_before_reset;

extern u32 forum_gateway_notifications;

extern u32 browser_gateway_notifications, forum_source_observations, forum_delete_observations;

extern bool forum_observation_needed;

extern u64 forum_message_channel, forum_message_id;

extern u64 forum_gateway_guild, forum_gateway_parent, forum_gateway_thread;

extern bool reaction_observation_needed;

extern u32 reaction_observations, reaction_source_observations, reaction_delete_observations;

extern u64 reaction_wake;

extern MessageHandle observed_reaction_handle;

extern u32 observed_reaction_revision;

extern bool observed_reaction_applied;

extern u32 diagnostic_positions[8], diagnostic_events;

extern GatewayClientFailureStage diagnostic_stage;

extern GatewayClientPopError diagnostic_pop_error;

extern bool diagnostic_order_ok, diagnostic_refill, diagnostic_null_source, diagnostic_overlarge;

extern u64 profile_wake, command_wake, form_wake, friends_wake, recovery_wake, reset_wake, phone_wake, notification_wake, video_wake, tooltip_wake, surface_wake, toast_wake, reconnect_wake;

void check(bool valid, const char* label) noexcept;

AccountModuleStatus WINAPI pump(void*, u64, u64) noexcept;

void reset_counts() noexcept;

RestClientSubmitResult WINAPI navigation_fetch(void*, ChannelOverviewOperation operation,
                                               u64 channel, const ChannelOverviewPatch* patch,
                                               u64 request) noexcept;

RestClientSubmitResult WINAPI navigation_cancel(void*, const MessagePinsRequest* request,
                                                u64 id) noexcept;

void* WINAPI diagnostic_source(void*, u32 index) noexcept;

GatewayClientPopResult WINAPI diagnostic_pop(void* source, GatewayClientEvent* event,
                                             Slice<u8> payload) noexcept;

} // namespace account_pump_detail

using namespace account_pump_detail;

MessageReadBadge application_notifications_read_badge(void*, const GatewayClientEvent&,
                                                      ConstSlice<u8>) noexcept;

extern u64 loading_spinner_wake;

extern u64 search_request_wake, search_caret_wake, self_status_wake;

extern u64 overview_request_wake, overview_caret_wake;

extern u64 pins_request_wake;

extern u64 browser_request_wake;

extern u64 creation_request_wake;

extern u64 forum_request_wake;

extern bool browser_blocks_read;

} // namespace maicord::app_detail
