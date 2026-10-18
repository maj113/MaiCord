#pragma once
#include "maicord/protocol/private_conversation.hpp"
#include "maicord/app/rest_client.hpp"
namespace maicord::app_detail {
bool queue_private_conversation(PrivateConversationOperation operation, u64 id) noexcept;
// After the selected private channel or the conversation list changed.
bool private_navigation_changed() noexcept;
bool accept_private_conversation(const RestClientResponse& response, Utf8View body) noexcept;
void fail_private_conversation() noexcept;
bool present_private_conversation_error() noexcept;
}
