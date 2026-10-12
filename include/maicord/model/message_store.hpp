#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {

inline constexpr u32 kMaximumMessageContentBytes = 16 * 1024;
inline constexpr u32 kMaximumMessageRecords = 1'048'576;
inline constexpr u64 kSnowflakeEpochMilliseconds = 1'420'070'400'000ull;

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
    // Retain source eligibility without admitting a second rich object tree.
    message_presentation_has_poll = 1u << 2,
    message_presentation_has_call = 1u << 3,
    message_presentation_has_activity = 1u << 4,
};

inline constexpr u32 kMessagePresentationForwardingObstacles =
    message_presentation_has_poll | message_presentation_has_call | message_presentation_has_activity;
inline constexpr u32 kMessagePresentationKnownFlags = message_presentation_mentions_current_user |
    message_presentation_pinned | kMessagePresentationForwardingObstacles;

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
    u16 type; // existing tail padding; the message type is independent of flags
};

struct MessageStore {
    MessageRecord* records;
    u32* order;
    u8* text;
    u32 capacity;
    u32 count;
    u32 text_capacity;
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
    u16 type = 0; // existing tail padding; local ordinary messages default to type 0
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
bool message_store_compact(MessageStore* store, Slice<u8> scratch) noexcept;
bool message_store_compact_in_place(MessageStore* store) noexcept;
u64 snowflake_timestamp_ms(u64 snowflake) noexcept;

} // namespace maicord
