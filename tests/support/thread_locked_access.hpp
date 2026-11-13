#pragma once
#include "maicord/model/chat/chat_state.hpp"

namespace maicord {
// Sets or forgets a thread's locked state the way a test needs it. The product learns it from channel input.
inline bool chat_state_store_set_thread_locked(ChatStateStore* store, u64 thread_id, u64 self_user_id, bool locked,
                                               bool known = true) noexcept {
    auto* channel = const_cast<ChannelRecord*>(chat_state_store_find_channel(*store, thread_id));
    if (!channel || !channel->occupied || !channel_is_thread(channel->type) || self_user_id != store->self_user_id)
        return false;
    channel->thread_locked_known = known;
    if (known)
        channel->thread_locked = locked;
    channel->actions = chat_state_store_thread_actions(*store, *channel);
    ++channel->revision;
    ++store->channel_mutations;
    return true;
}
} // namespace maicord
