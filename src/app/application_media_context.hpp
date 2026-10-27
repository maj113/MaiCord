#pragma once

#include "maicord/model/chat_state.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/relationship_store.hpp"
#include "maicord/ui/message_media_policy.hpp"

namespace maicord::app_detail {

struct ApplicationMediaContext {
    u64 author_id;
    MessageMediaContext context;
    bool self_authored;
};

// A forward keeps its destination context, but never inherits its forwarder's
// identity. Only an accepted source record in the matching channel can supply
// its author. A cache miss cannot inherit friend or self-author privileges.
inline ApplicationMediaContext application_media_context_resolve(
    const MessageMediaOwner& media, u64 self_user_id, const ChatStateStore* channels,
    const RelationshipStore* relationships, const MessageStore* live_messages,
    const MessageStore* search_messages) noexcept {
    u64 author_id = media.forwarded ? 0 : media.author_id;
    if (media.forwarded && media.source_channel_id && media.source_message_id) {
        const MessageStore* stores[]{live_messages, search_messages};
        for (const auto* messages : stores) {
            if (!messages)
                continue;
            const auto found = message_store_find(*messages, media.source_message_id);
            const auto* source = found.ok() ? message_store_resolve(*messages, found.handle) : nullptr;
            if (source && source->channel_id == media.source_channel_id) {
                author_id = source->author_id;
                break;
            }
        }
    }
    const auto* channel = channels ? chat_state_store_find_channel(*channels, media.channel_id) : nullptr;
    MessageMediaContext context = MessageMediaContext::other_dm;
    if (channel && channel->guild_id) {
        context = MessageMediaContext::guild;
    } else if (channel && channel_is_private(channel->type) && author_id && relationships) {
        const auto* relationship = relationship_store_find(*relationships, author_id);
        if (relationship && relationship->type == RelationshipType::friend_)
            context = MessageMediaContext::friend_dm;
    }
    return {author_id, context, self_user_id && author_id == self_user_id};
}

} // namespace maicord::app_detail
