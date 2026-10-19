#pragma once

#include "maicord/protocol/gateway_events.hpp"
#include "maicord/protocol/message_reaction.hpp"

namespace maicord {

enum class GatewayMessageReactionEventKind : u8 {
    add,
    remove,
    remove_all,
    remove_emoji,
};

struct GatewayMessageReactionEvent {
    u64 message_id;
    u64 channel_id;
    u64 user_id;
    u64 emoji_id;
    Utf8View name;
    MessageReactionType type;
    GatewayMessageReactionEventKind kind;
    bool animated;
};

GatewayEventDecodeResult gateway_decode_message_reaction(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> name_output, GatewayMessageReactionEventKind kind,
    GatewayMessageReactionEvent* output) noexcept;

struct GatewayReactionBatchVisitResult {
    GatewayEventDecodeResult decoded;
    u32 visited;
    bool cancelled;

    constexpr bool ok() const noexcept {
        return decoded.ok() && !cancelled;
    }
};

// Validate the complete debounced batch before the first synchronous callback,
// then visit one normal add per user entry in wire order, including repeats.
// Names borrow name_scratch until the next callback. The callback must not
// modify or invalidate data, structural_scratch or name_scratch during the
// visit. False stops immediately; visited counts only accepted callbacks.
// Scratch buffers must be disjoint from each other and the source document.
GatewayReactionBatchVisitResult gateway_visit_message_reaction_add_many(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> name_scratch,
    void* context, bool (*visit)(void*, const GatewayMessageReactionEvent&) noexcept) noexcept;

struct GatewayMessagePollVoteEvent;
// MESSAGE_POLL_VOTE_ADD_MANY (debounced votes, {channel_id, message_id,
// guild_id?, votes: [{answer_id, users: [id]}]}) under the same contract:
// the whole batch validates first, then one vote add per user entry is
// visited in wire order with batched set.
GatewayReactionBatchVisitResult gateway_visit_message_poll_vote_add_many(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, void* context,
    bool (*visit)(void*, const GatewayMessagePollVoteEvent&) noexcept) noexcept;

} // namespace maicord
