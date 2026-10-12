#pragma once
#include "maicord/model/chat_state.hpp"
#include "maicord/model/user_store.hpp"

namespace maicord {
// Borrows an explicit name, or returns generated UTF-8 in caller-owned storage.
Utf8View private_channel_title(const ChatStateStore& state, const UserStore* users,
    const ChannelRecord& channel, Slice<char> storage) noexcept;
u64 private_channel_peer(const ChatStateStore& state, u64 channel_id) noexcept;
}
