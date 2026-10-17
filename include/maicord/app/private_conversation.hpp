#pragma once
#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/protocol/private_conversation.hpp"

namespace maicord {
struct RestClientResponse;
enum class PrivateConversationPhase : u8 { idle, queued, awaiting, retry_wait, failed };
enum class PrivateConversationError : u8 { none, transport, http, response, capacity };
struct PrivateConversationRequest {
    u64 target_id;
    u64 request_id;
    u64 retry_at_ms;
    u32 intent_revision;
    u16 channel_generation;
    u16 status_code;
    PrivateConversationOperation operation;
    PrivateConversationPhase phase;
    PrivateConversationError error;
    bool feedback_shown;
};
u64 private_conversation_cached(const ChatStateStore& state, u64 user_id) noexcept;
bool private_conversation_begin(PrivateConversationRequest* request, const ChatStateStore& state,
    PrivateConversationOperation operation, u64 target_id) noexcept;
bool private_conversation_due(const PrivateConversationRequest& request, u64 now_ms) noexcept;
bool private_conversation_validate_target(PrivateConversationRequest* request, const ChatStateStore& state) noexcept;
bool private_conversation_accept(PrivateConversationRequest* request, ChatStateStore* state, UserStore* users,
    const RestClientResponse& response, Utf8View body, Slice<u32> scratch, Slice<char> packet, Slice<u64> ids) noexcept;
}
