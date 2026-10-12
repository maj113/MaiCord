#pragma once

#include "maicord/core/base.hpp"
#include "maicord/protocol/message_reaction.hpp"
#include "maicord/protocol/gateway_reaction_events.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/model/message_store.hpp"

namespace maicord {

struct RestClientResponse;

namespace app_detail {

enum class MessageReactionRequestPhase : u8 {
    idle,
    queued,
    submitting,
    retry_wait,
};

struct MessageReactionRequest {
    u64 channel_id;
    u64 message_id;
    u64 emoji_id;
    u64 retry_at_ms;
    u16 name_bytes;
    MessageReactionType type;
    MessageReactionRequestPhase phase;
    u8 retries;
    bool add;
    bool animated;
    char name[kMessageReactionMaximumEmojiNameBytes];
};

static_assert(sizeof(MessageReactionRequest) == 168);

enum class ApplicationMessageReactionEnqueueResult : u8 {
    accepted,
    coalesced,
    capacity,
    permission,
    stale,
    invalid,
    storage
};
ApplicationMessageReactionEnqueueResult application_queue_message_reaction_emoji(u64 message_id, u64 emoji_id,
                                                                             Utf8View name,
                                                                             bool animated) noexcept;
ApplicationMessageReactionEnqueueResult application_queue_message_reaction(u64 message_id,
                                                                       u32 reaction_index) noexcept;
Utf8View application_message_reaction_admission_text(ApplicationMessageReactionEnqueueResult) noexcept;
Utf8View application_message_reaction_status(u64 message_id, bool* error) noexcept;
bool application_message_reaction_pending(u64 now_ms) noexcept;
bool application_message_reaction_submit(u64 request_id, u64 now_ms) noexcept;
bool application_message_reaction_response(const RestClientResponse&) noexcept;
bool application_message_reaction_synchronize() noexcept;
bool application_message_reaction_observation_needed() noexcept;
struct ApplicationMessageReactionMetrics {
    u32 owner_bytes, owner_committed_bytes, intent_bytes, intent_committed_bytes;
    u32 intent_count, backed_slots, revision;
    bool active, flight_active;
};
ApplicationMessageReactionMetrics application_message_reaction_metrics() noexcept;
u64 application_message_reaction_next_wake() noexcept;
void application_message_reaction_observe(const GatewayMessageReactionEvent&, MessageHandle source,
                                          u32 message_revision, bool model_applied) noexcept;
void application_message_reaction_message_observe(const GatewayMessageEvent&, MessageHandle source,
                                                  u32 message_revision, bool model_applied) noexcept;
void application_message_reaction_message_deleted(u64 channel_id, u64 message_id) noexcept;

void reset_message_reaction() noexcept;
bool queue_message_reaction(u64 message_id, u32 reaction_index) noexcept;
bool queue_message_reaction_emoji(u64 message_id, u64 emoji_id, Utf8View name, bool animated) noexcept;

} // namespace app_detail
} // namespace maicord
