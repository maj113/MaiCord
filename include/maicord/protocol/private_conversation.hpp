#pragma once
#include "maicord/protocol/rest.hpp"

namespace maicord {
enum class PrivateConversationOperation : u8 { open, close };
struct PrivateConversationWriteResult { u32 target_bytes; u32 body_bytes; bool ok; };
PrivateConversationWriteResult private_conversation_write(PrivateConversationOperation operation, u64 id,
    Slice<u8> target, Slice<u8> body) noexcept;
RestRouteKey private_conversation_route(PrivateConversationOperation operation, u64 id) noexcept;

// Group DMs hold ten members including the current user.
inline constexpr u32 kPrivateChannelGroupMaximum = 10;
inline constexpr u32 kPrivateChannelRecipientMaximum = kPrivateChannelGroupMaximum - 1;
inline constexpr u32 kPrivateChannelNameMaximumCodepoints = 100;
inline constexpr u32 kPrivateChannelNameMaximumBytes = 400;
enum class PrivateChannelActionKind : u8 {
    none,
    create,           // POST /users/@me/channels {"recipients":[...]}
    add_recipient,    // PUT /channels/{id}/recipients/{user}
    remove_recipient, // DELETE /channels/{id}/recipients/{user}
    rename,           // PATCH /channels/{id} {"name":...}
    leave,            // DELETE /channels/{id}?silent=...
    ring,             // POST /channels/{id}/call/ring {"recipients":null|[...]}
    stop_ringing,     // POST /channels/{id}/call/stop-ringing {} or {"recipients":[...]}
    // Message requests and DM safety (account ABI 113).
    accept_request,   // PUT /channels/{id}/recipients/@me {"consent_status":2}
    ignore_request,   // DELETE /channels/{id}/recipients/@me
    ignore_requests,  // PUT /channels/recipients/@me/batch-reject {"channel_ids":[recipients...]}
    acknowledge_safety, // POST /channels/{id}/safety-warnings/ack {"warning_ids":[recipients...]}
};
struct PrivateChannelActionRequest {
    u64 channel_id;
    u64 user_id;
    u64 recipients[kPrivateChannelRecipientMaximum];
    char name[kPrivateChannelNameMaximumBytes];
    u16 name_bytes;
    u8 recipient_count;
    PrivateChannelActionKind kind;
    bool silent;
};
static_assert(sizeof(PrivateChannelActionRequest) == 496);
inline constexpr u32 kPrivateChannelActionTargetBytes = 96;
inline constexpr u32 kPrivateChannelActionBodyBytes = 640;
bool private_channel_action_valid(const PrivateChannelActionRequest& request) noexcept;
PrivateConversationWriteResult private_channel_action_write(const PrivateChannelActionRequest& request,
    Slice<u8> target, Slice<u8> body) noexcept;
RestRouteKey private_channel_action_route(const PrivateChannelActionRequest& request) noexcept;
}
