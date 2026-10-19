#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {

inline constexpr u32 kMaximumMessageContentBytes = 16 * 1024;
inline constexpr u32 kMaximumMessageRecords = 1'048'576;

enum class MessageContentAvailability : u8 {
    unknown,
    available,
    restricted,
};

enum class MessageDeliveryState : u8 {
    authoritative,
    pending,
    failed,
};

enum MessagePresentationFlag : u32 {
    message_presentation_none = 0,
    message_presentation_mentions_current_user = 1u << 0,
    message_presentation_pinned = 1u << 1,
    // Flag only; poll content lives in the poll store.
    message_presentation_has_poll = 1u << 2,
    message_presentation_has_call = 1u << 3,
    message_presentation_has_activity = 1u << 4,
    // Live CREATE badge decision; raw mention styling remains independent.
    message_presentation_read_badge_known = 1u << 5,
    message_presentation_read_badge = 1u << 6,
};

inline constexpr u32 kMessagePresentationForwardingObstacles =
    message_presentation_has_poll | message_presentation_has_call | message_presentation_has_activity;
inline constexpr u32 kMessagePresentationKnownFlags = message_presentation_mentions_current_user |
    message_presentation_pinned | kMessagePresentationForwardingObstacles |
    message_presentation_read_badge_known | message_presentation_read_badge;

struct MessageHandle {
    u32 slot;
    u16 generation;
};

struct MessageRecord {
    u64 id;
    u64 channel_id;
    u64 author_id;
    u64 timestamp_ms;
    u64 edited_timestamp_ms;
    u32 content_offset;
    u32 content_bytes;
    u32 flags;
    u32 revision;
    u16 generation;
    bool occupied;
    MessageContentAvailability content_availability;
    MessageDeliveryState delivery;
    u8 presentation_flags;
    u16 type; // message type, independent of flags
};

struct MessageStore {
    MessageRecord* records;
    u32* order;
    u8* text;
    // Growing banks reserve text_capacity in this arena and commit
    // text_committed of it; fixed banks are fully committed and have none.
    Arena* text_arena;
    u32 capacity;
    u32 count;
    u32 text_capacity;
    u32 text_committed;
    u32 text_used;
    u32 text_wasted;
    u32 rejected;
    u32 created;
    u32 updated;
    u32 deleted;
    u32 compactions;
};

struct MessageInput {
    u64 id;
    u64 channel_id;
    u64 author_id;
    u64 timestamp_ms;
    u64 edited_timestamp_ms;
    Utf8View content;
    u32 flags;
    MessageContentAvailability content_availability =
        MessageContentAvailability::available;
    MessageDeliveryState delivery = MessageDeliveryState::authoritative;
    u32 presentation_flags = message_presentation_none;
    u16 type = 0; // local ordinary messages default to type 0
};

enum class MessageStoreError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    record_capacity,
    text_capacity,
    not_found,
};

struct MessageStoreResult {
    MessageHandle handle;
    u32 order_index;
    MessageStoreError error;
    bool inserted;

    constexpr bool ok() const noexcept {
        return error == MessageStoreError::none;
    }
};

bool message_store_initialize(MessageStore* store, Slice<MessageRecord> records,
                              Slice<u32> order, Slice<u8> text) noexcept;
// Text lives in `text_arena` (its whole reservation is the capacity). The
// arena is trimmed back to its first kMessageStoreTextCommitStep bytes, and
// appends commit more only when compaction cannot make room.
inline constexpr u32 kMessageStoreTextCommitStep = 16 * 1024;
bool message_store_initialize_growing(MessageStore* store, Slice<MessageRecord> records,
                                      Slice<u32> order, Arena* text_arena) noexcept;
MessageStoreError message_store_validate_input(
    const MessageStore* store, const MessageInput& input) noexcept;
MessageStoreResult message_store_upsert(MessageStore* store,
                                        const MessageInput& input) noexcept;
MessageStoreResult message_store_find(const MessageStore& store, u64 id) noexcept;
bool message_store_delete(MessageStore* store, u64 id) noexcept;
bool message_store_set_delivery(MessageStore* store, u64 id,
                                MessageDeliveryState delivery) noexcept;
bool message_store_set_presentation_flags(MessageStore* store, u64 id,
                                          u32 flags) noexcept;
const MessageRecord* message_store_resolve(const MessageStore& store,
                                           MessageHandle handle) noexcept;
MessageHandle message_store_ordered_handle(const MessageStore& store,
                                           u32 order_index) noexcept;
Utf8View message_store_content(const MessageStore& store,
                               MessageHandle handle) noexcept;
bool message_store_compact_in_place(MessageStore* store) noexcept;
u64 snowflake_timestamp_ms(u64 snowflake) noexcept;

} // namespace maicord
