#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/ui/navigation_view.hpp"

namespace maicord::app_detail {
// DETAILS lane owner for group DM create, membership, rename, leave and ringing.
bool application_private_channel_pending(u64 now_ms) noexcept;
bool application_private_channel_submit(u64 request_id, u64 now_ms) noexcept;
bool application_private_channel_response(const RestClientResponse& response, Utf8View body) noexcept;
// One person opens or creates the DM; more people create a group.
bool application_private_channel_create(ConstSlice<u64> users) noexcept;
bool application_private_channel_add(u64 channel_id, ConstSlice<u64> users) noexcept;
// Picker for a new conversation (zero) or for people joining channel_id.
bool application_private_channel_picker(u64 channel_id) noexcept;
bool application_private_channel_rename(u64 channel_id) noexcept;
// Asks before leaving the group (silent: without notifying the members).
bool application_private_channel_leave(u64 channel_id, bool silent) noexcept;
// Context-menu rows for a DM or group row and for a group member.
u32 application_private_channel_menu_items(u64 channel_id, Slice<PopupItem> output) noexcept;
u32 application_private_channel_member_items(u64 user_id, Slice<PopupItem> output) noexcept;
// True when the action belonged to these menus (applied or not).
bool application_private_channel_menu_action(u32 action, u64 owner_id, bool shift, bool* applied) noexcept;
bool application_private_channel_present_error() noexcept;
// CALL_CREATE/UPDATE/DELETE: an incoming DM or group call prompts Join or Decline.
void application_private_channel_gateway(const GatewayClientEvent& event) noexcept;
// Borrowed presence/typing source for conversation rows; null without an account.
const UiPrivateRowSource* application_private_channel_rows() noexcept;
// Joins the conversation's call and rings the other members once connected.
bool application_private_channel_call(u64 channel_id) noexcept;
// The group name in the conversation header opens the rename dialog.
bool application_private_channel_title_hit(float x, float y) noexcept;
// The row's close button: closes a DM, or asks before leaving a group.
bool application_private_channel_row_close(u64 channel_id, float x, float y) noexcept;
// Message requests: accept or ignore one, or ignore every listed request.
bool application_private_channel_accept_request(u64 channel_id) noexcept;
bool application_private_channel_ignore_request(u64 channel_id) noexcept;
// Dismisses the DM safety warning banner of a conversation.
bool application_private_channel_dismiss_safety(u64 channel_id) noexcept;
// The conversation list shows message requests instead of conversations.
bool application_private_channel_requests_mode() noexcept;
void application_private_channel_set_requests_mode(bool requests) noexcept;
// Pending and spam request counts for the list's "Message requests" row.
u32 application_private_channel_request_count() noexcept;
// True while a request action for this channel is queued or in flight.
bool application_private_channel_request_busy(u64 channel_id) noexcept;
// Publishes the "Message requests" row and list mode to the navigation view.
void application_private_channel_navigation(NavigationView* view) noexcept;
} // namespace maicord::app_detail
