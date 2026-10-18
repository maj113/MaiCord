#pragma once
#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/protocol/private_conversation.hpp"

namespace maicord {
struct RestClientResponse;
enum class PrivateChannelActionPhase : u8 { idle, queued, awaiting, retry_wait, failed };
enum class PrivateChannelActionError : u8 { none, transport, http, response };
// One group DM request at a time. Adding several people queues one PUT per
// person; the gateway's CHANNEL_RECIPIENT_ADD/REMOVE and CHANNEL_UPDATE events
// remain the authority for membership and names.
struct PrivateChannelActionController {
    PrivateChannelActionRequest request;
    u64 queued[kPrivateChannelRecipientMaximum];
    u64 request_id;
    u64 retry_at_ms;
    u64 result_channel_id;
    u32 intent_revision;
    u16 status_code;
    u8 queued_count;
    PrivateChannelActionPhase phase;
    PrivateChannelActionError error;
    bool feedback_shown;
};

// Members including the current user; zero for unknown or guild channels.
u32 private_channel_member_count(const ChatStateStore& state, u64 channel_id) noexcept;
bool private_channel_has_member(const ChatStateStore& state, u64 channel_id, u64 user_id) noexcept;
u64 private_channel_owner(const ChatStateStore& state, u64 channel_id) noexcept;
// People that may still be added: a DM becomes a new group with its peer.
u32 private_channel_capacity_left(const ChatStateStore& state, u64 channel_id) noexcept;
// Only the group owner removes others; nobody removes themselves (leave instead).
bool private_channel_can_remove(const ChatStateStore& state, u64 channel_id, u64 user_id) noexcept;
bool private_channel_is_group(const ChatStateStore& state, u64 channel_id) noexcept;

bool private_channel_begin_create(PrivateChannelActionController* controller, const ChatStateStore& state,
                                  ConstSlice<u64> users) noexcept;
bool private_channel_begin_add(PrivateChannelActionController* controller, const ChatStateStore& state,
                               u64 channel_id, ConstSlice<u64> users) noexcept;
bool private_channel_begin_remove(PrivateChannelActionController* controller, const ChatStateStore& state,
                                  u64 channel_id, u64 user_id) noexcept;
bool private_channel_begin_rename(PrivateChannelActionController* controller, const ChatStateStore& state,
                                  u64 channel_id, Utf8View name) noexcept;
bool private_channel_begin_leave(PrivateChannelActionController* controller, const ChatStateStore& state,
                                 u64 channel_id, bool silent) noexcept;
bool private_channel_begin_ring(PrivateChannelActionController* controller, const ChatStateStore& state,
                                u64 channel_id, bool stop) noexcept;
// Message requests: accept keeps the DM (consent_status ACCEPTED), ignore
// leaves it. Ignoring several uses the batch endpoint (at most
// kPrivateChannelRecipientMaximum channels per request).
bool private_channel_begin_accept_request(PrivateChannelActionController* controller, const ChatStateStore& state,
                                          u64 channel_id) noexcept;
bool private_channel_begin_ignore_request(PrivateChannelActionController* controller, const ChatStateStore& state,
                                          u64 channel_id) noexcept;
// Dismisses the channel's current safety warning banner.
bool private_channel_begin_dismiss_safety(PrivateChannelActionController* controller, const ChatStateStore& state,
                                          u64 channel_id) noexcept;
bool private_channel_action_due(const PrivateChannelActionController& controller, u64 now_ms) noexcept;
bool private_channel_action_submitted(PrivateChannelActionController* controller, u64 request_id) noexcept;
// Applies a created or renamed channel from the response body and selects a
// created channel when navigation has not moved on. Leaving removes the
// channel locally. True when the response belonged to the controller.
bool private_channel_action_accept(PrivateChannelActionController* controller, ChatStateStore* state,
                                   UserStore* users, const RestClientResponse& response,
                                   Utf8View body, Slice<u32> scratch, Slice<char> packet, Slice<u64> ids) noexcept;

// The DM or group call the current user is being rung for, if any.
struct PrivateCallPrompt {
    u64 channel_id;
};
// Applies call_update/call_delete events; a call already joined never prompts.
// True when the prompt appeared, moved or cleared.
bool private_call_observe(PrivateCallPrompt* prompt, const ChatStateStore& state, const GatewayClientEvent& event,
                          u64 voice_channel_id) noexcept;
} // namespace maicord
