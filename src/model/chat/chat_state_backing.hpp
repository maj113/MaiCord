#pragma once

#include "maicord/model/chat/chat_state.hpp"

// Shared by the chat-state and custom-emoji translation units. chat_state.cpp defines it once.
namespace maicord::chat_state_detail {

// Backs the first `required` of `capacity` units of one backing (bank `bank`) before use;
// a store without a backing callback is fully backed.
bool back_range(const ChatStateStore& store, ChatStateBacking backing, u32 bank, u32& backed, u32 capacity,
                u32 required) noexcept;

} // namespace maicord::chat_state_detail
