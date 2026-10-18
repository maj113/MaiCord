#pragma once

#include "maicord/core/stable_storage.hpp"
#include "maicord/protocol/gateway_message_events.hpp"

namespace maicord {

inline constexpr u32 kMessagePollDefaultCapacity = 512;
inline constexpr u32 kMessagePollDefaultTextBytes = 256 * 1024;

struct MessagePollString {
    u32 offset;
    u16 bytes;
    u16 reserved;
};
struct MessagePollAnswer {
    u64 emoji_id;
    MessagePollString text;
    MessagePollString emoji_name;
    u32 id;
    u32 count;
    bool count_known;
    bool me_voted;
    bool me_known;
    bool emoji_animated;
};
struct MessagePollRecord {
    u64 message_id;
    u64 channel_id;
    u64 expiry_ms;
    u64 last_gateway_serial;
    MessagePollString question;
    MessagePollAnswer answers[kGatewayMessagePollMaximumAnswers];
    u32 content_revision;
    u32 result_revision;
    u32 fields;
    u16 layout_type;
    u8 answer_count;
    bool allow_multiselect;
    bool expiry_null;
    bool results_known;
    bool is_finalized;
};

// Timeline and Search have independent MessageRecord revisions. Poll content
// versions belong to this shared store; each scene separately tracks its source revision.
struct MessagePollResultTicket {
    u64 message_id;
    u64 gateway_serial;
    u32 content_revision;
};
struct MessagePollOwnedStorage;
struct MessagePollStore {
    MessagePollRecord* records;
    u8* text;
    u32 capacity;
    u32 count;
    u32 text_capacity;
    u32 text_used;
    u32 text_wasted;
    u32 revision;
    u32 content_revision;
    u32 rejected;
    u32 compactions;
    MessagePollOwnedStorage* owned;
};
enum class MessagePollStoreError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    incomplete,
    capacity,
    stale,
};
struct MessagePollStoreResult {
    const MessagePollRecord* record;
    MessagePollStoreError error;
    bool changed;
    bool results_ignored;
    constexpr bool ok() const noexcept {
        return error == MessagePollStoreError::none;
    }
};
struct MessagePollStoreMetrics {
    usize reserved_bytes;
    usize committed_bytes;
    u32 commit_count;
    u32 records;
    u32 text_bytes;
};
inline constexpr u32 kMessagePollPageMaximum = 125;
struct MessagePollPageInput {
    u64 message_id;
    u64 channel_id;
    const GatewayMessagePoll* poll;
    u64 gateway_serial;
    const MessagePollResultTicket* ticket;
};
struct MessagePollPageResult {
    MessagePollStoreError error;
    u32 applied;
    u32 ignored;
    bool changed;
    constexpr bool ok() const noexcept {
        return error == MessagePollStoreError::none;
    }
};

// Configures a demand-backed store. No reservation/commit occurs until the first
// valid poll; ordinary/absent message fields leave the store dormant.
bool message_poll_store_initialize_owned(MessagePollStore*, u32 capacity = kMessagePollDefaultCapacity,
                                         u32 text_bytes = kMessagePollDefaultTextBytes) noexcept;
void message_poll_store_release(MessagePollStore*) noexcept;
const MessagePollRecord* message_poll_store_find(const MessagePollStore&, u64 message_id) noexcept;
Utf8View message_poll_store_string(const MessagePollStore&, MessagePollString) noexcept;
const MessagePollAnswer* message_poll_record_answer(const MessagePollRecord&, u32 answer_id) noexcept;
MessagePollResultTicket message_poll_store_result_ticket(const MessagePollStore&, u64 message_id,
                                                         u64 gateway_serial) noexcept;

// Omitted fields keep prior values; present=false removes a poll.
// gateway_serial is the account-local ordered mutation serial, never a payload
// sequence or a MessageRecord revision. HTTP callers pass their captured revision.
// Newer Gateway observations or changed content invalidate HTTP result snapshots.
MessagePollStoreResult message_poll_store_apply(MessagePollStore*, u64 message_id, u64 channel_id,
                                                const GatewayMessagePoll&, u64 gateway_serial = 0,
                                                const MessagePollResultTicket* = nullptr) noexcept;
MessagePollStoreResult message_poll_store_vote(MessagePollStore*, const GatewayMessagePollVoteEvent&,
                                               u64 actor_id, u64 gateway_serial) noexcept;
// The accepted own vote (PUT answers/@me succeeded): answers in mask become
// me_voted, the others not, each changed count moving by one. Idempotent, so
// the matching MESSAGE_POLL_VOTE_ADD/REMOVE echo (before or after) never counts
// twice; a finalized poll keeps its results.
MessagePollStoreResult message_poll_store_own_vote(MessagePollStore*, u64 message_id, u64 channel_id,
                                                   u64 actor_id, u16 answer_mask) noexcept;
bool message_poll_store_delete(MessagePollStore*, u64 message_id) noexcept;
MessagePollStoreMetrics message_poll_store_metrics(const MessagePollStore&) noexcept;
// Actual poll inputs only: an ordinary page performs zero allocation. A private
// sparse clone is committed only after the whole candidate succeeds. Stale
// input revisions keep the newer loaded poll and do not reject unrelated rows.
// Committing changes global content_revision so all borrowed scene strings are
// rebound before rendering, even if individual poll versions stay unchanged.
MessagePollPageResult message_poll_store_apply_page(MessagePollStore*,
                                                    ConstSlice<MessagePollPageInput>) noexcept;

} // namespace maicord
