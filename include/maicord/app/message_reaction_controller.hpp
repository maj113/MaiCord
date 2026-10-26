#pragma once
#include "maicord/model/message_store.hpp"
#include "maicord/protocol/message_reaction.hpp"

namespace maicord {
inline constexpr u32 kMessageReactionMaximumIntents = 64;
inline constexpr u32 kMessageReactionMaximumRetries = 3;
inline constexpr u64 kMessageReactionIntentLifetimeMs = 120'000;
struct MessageReactionActor {
    u64 account_scope{};
    u64 profile_id{};
    u64 self_user_id{};
    usize instance{};
};

struct MessageReactionSource {
    MessageHandle handle{};
    u32 revision{};
    u16 channel_generation{};
};

struct MessageReactionTarget {
    u64 channel_id{};
    u64 message_id{};
    u64 emoji_id{};
    char name[kMessageReactionMaximumEmojiNameBytes]{};
    u16 name_bytes{};
    MessageReactionType type{};
    bool animated{};
};
// Server-confirmed aggregate, independent of the optimistic loaded model.
struct MessageReactionAuthority {
    u32 total{};
    u32 normal{};
    u32 burst{};
    bool mine_normal{};
    bool mine_burst{};
    bool present{};
    bool known{};
};

struct MessageReactionIntent {
    MessageReactionTarget target{};
    MessageReactionSource source{};
    MessageReactionAuthority authority{};
    u64 serial{};
    u64 mine_revision{};
    u64 due_at_ms{};
    u64 deadline_at_ms{};
    u8 retries{};
    bool desired_mine{};
    // A successful HTTP result keeps this slot until its final own
    // echo/snapshot or original deadline, without scheduling another request.
    bool awaiting_echo{};
    bool echo_matches{};
};

struct MessageReactionTicket {
    u64 request_id{};
    u64 generation{};
    u64 serial{};
};

struct MessageReactionFlight {
    MessageReactionIntent intent{};
    MessageReactionActor actor{};
    MessageReactionTicket ticket{};
    bool active{};
    bool retired{};
};

struct MessageReactionCooldown {
    u64 channel_id{};
    u64 until_ms{};
};

struct MessageReactionController {
    static constexpr bool kZeroDefault = true; // Every member defaults to zero: reset_value clears bytes.
    Slice<MessageReactionIntent> entries{}; // writable backed prefix; fixed address
    MessageReactionActor actor{};
    MessageReactionFlight flight{};
    MessageReactionCooldown cooldowns[kMessageReactionMaximumIntents]{};
    u64 serial{};
    u64 generation{};
    u64 mine_revision{};
    u64 global_cooldown_ms{};
    u32 count{};
    u32 revision{};
};
enum class MessageReactionError : u8 {
    none,
    invalid_input,
    capacity,
    serial_exhausted
};
enum class MessageReactionCompletion : u8 {
    success,
    retry,
    terminal
};
enum class MessageReactionRetryScope : u8 {
    intent,
    channel,
    global
};
enum class MessageReactionOutcomeKind : u8 {
    ignored,
    queued,
    settled,
    retained,
    failed,
    retry,
    retired,
    expired
};

struct MessageReactionProjection {
    MessageReactionAuthority authority{};
    bool desired_normal{};
    bool desired_burst{};
    bool optimistic{};
};

struct MessageReactionOutcome {
    MessageReactionTarget target{};
    MessageReactionSource source{};
    MessageReactionProjection projection{};
    u64 serial{};
    MessageReactionOutcomeKind kind{};
    MessageReactionError error{};
    bool project{};
    bool authority_conflict{};
    constexpr bool ok() const noexcept {
        return error == MessageReactionError::none;
    }
};
bool message_reaction_target_equal(const MessageReactionTarget&, const MessageReactionTarget&) noexcept;
bool message_reaction_controller_initialize(MessageReactionController*, Slice<MessageReactionIntent>,
                                            MessageReactionActor) noexcept;
// Host backs additional stable slots first. Does not read/clear unbacked slots.
bool message_reaction_controller_grow_storage(MessageReactionController*,
                                              Slice<MessageReactionIntent>) noexcept;
// Queue before projecting. Capacity/validation failure leaves all desired and
// flight state unchanged. Custom emoji identity is ID only; Unicode identity
// is exact host-canonical bytes. The core never strips variation selectors.
MessageReactionOutcome message_reaction_controller_queue(MessageReactionController*,
                                                         const MessageReactionTarget&,
                                                         MessageReactionSource, MessageReactionAuthority,
                                                         bool desired_mine, u64 now_ms) noexcept;
bool message_reaction_controller_project(const MessageReactionController&, const MessageReactionTarget&,
                                         MessageReactionProjection*) noexcept;
bool message_reaction_controller_next_due(const MessageReactionController&, u64* due_at_ms) noexcept;
bool message_reaction_controller_peek_due(const MessageReactionController&, u64 now_ms,
                                          MessageReactionIntent*) noexcept;
// Call only after transport accepts the peek and nonzero request ID.
bool message_reaction_controller_admit(MessageReactionController*, const MessageReactionIntent&,
                                       u64 request_id, u64 now_ms, MessageReactionTicket*) noexcept;
MessageReactionOutcome message_reaction_controller_complete(
    MessageReactionController*, MessageReactionTicket, MessageReactionCompletion, u64 now_ms,
    u64 retry_at_ms = 0, MessageReactionRetryScope = MessageReactionRetryScope::intent) noexcept;
// queue and complete filling a caller-owned outcome in place.
void message_reaction_controller_queue(MessageReactionOutcome*, MessageReactionController*,
                                       const MessageReactionTarget&, MessageReactionSource,
                                       MessageReactionAuthority, bool desired_mine, u64 now_ms) noexcept;
void message_reaction_controller_complete(MessageReactionOutcome*, MessageReactionController*,
                                          MessageReactionTicket, MessageReactionCompletion, u64 now_ms,
                                          u64 retry_at_ms, MessageReactionRetryScope) noexcept;
// Host supplies only validated actor/source-current Gateway observations.
// Foreign events change counts, never the user's own membership. Own events
// are idempotent against the stored server mine flag, independently of UI overlays.
bool message_reaction_controller_observe(MessageReactionController*, const MessageReactionTarget&, bool add,
                                         bool current_user) noexcept;
// Release expired settled guards before accepting a new event. Pending
// intents keep their baseline until the host applies the event and expiry.
bool message_reaction_controller_observe(MessageReactionController*, const MessageReactionTarget&, bool add,
                                         bool current_user, u64 now_ms) noexcept;
bool message_reaction_controller_snapshot(MessageReactionController*, const MessageReactionTarget&,
                                          MessageReactionAuthority) noexcept;
bool message_reaction_controller_rebind_source(MessageReactionController*, const MessageReactionTarget&,
                                               MessageReactionSource) noexcept;
// Clear/history/message removal barriers. Clears matching intents and
// cancels matching in-flight requests without releasing the transport request.
bool message_reaction_controller_cancel(MessageReactionController*, u64 channel_id, u64 message_id,
                                        const MessageReactionTarget* emoji = nullptr) noexcept;
bool message_reaction_controller_expire(MessageReactionController*, u64 now_ms,
                                        MessageReactionOutcome*) noexcept;
// Actor/READY reset keeps the old in-flight tombstone until its response arrives.
// Cooldowns belong to the actor and are cleared on a different actor.
void message_reaction_controller_reset(MessageReactionController*, MessageReactionActor) noexcept;
} // namespace maicord
