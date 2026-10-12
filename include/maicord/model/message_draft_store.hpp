#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {

inline constexpr u32 kMessageDraftStoreMaximumRecords = 256;

enum class MessageDraftStoreError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    record_capacity,
    text_capacity,
    not_found,
};

struct MessageDraftRecord {
    u64 channel_id;
    u64 touched;
    u64 reply_message_id;
    u32 text_offset;
    u32 text_bytes;
    u32 anchor;
    u32 caret;
    u32 revision;
    bool occupied;
    bool mention_reply_author;
};

static_assert(sizeof(MessageDraftRecord) == 48);

struct MessageDraftStore {
    MessageDraftRecord* records;
    u8* text;
    u32 capacity;
    u32 text_capacity;
    u32 count;
    u32 text_used;
    u32 text_wasted;
    u32 revision;
    u32 saved;
    u32 deleted;
    u32 compactions;
    u32 rejected;
    u64 clock;
};

struct MessageDraftInput {
    u64 channel_id;
    Utf8View text;
    u32 anchor;
    u32 caret;
    // Empty text is retained when a reply target exists. No target uses ping=true.
    u64 reply_message_id = 0;
    bool mention_reply_author = true;
};

struct MessageDraftSnapshot {
    Utf8View text;
    u64 channel_id;
    u64 touched;
    u32 anchor;
    u32 caret;
    u32 revision;
    u64 reply_message_id = 0;
    bool mention_reply_author = true;
};

struct MessageDraftStoreResult {
    u32 index;
    MessageDraftStoreError error;
    bool changed;
    bool erased;

    constexpr bool ok() const noexcept {
        return error == MessageDraftStoreError::none;
    }
};

bool message_draft_store_initialize(MessageDraftStore* store,
                                    Slice<MessageDraftRecord> records,
                                    Slice<u8> text) noexcept;
// Requires zero-filled text storage (a fresh committed allocation) and leaves
// those bytes untouched, so the bank's pages commit only when drafts are saved.
bool message_draft_store_initialize_sparse(MessageDraftStore* store,
                                           Slice<MessageDraftRecord> records,
                                           Slice<u8> text) noexcept;
MessageDraftStoreResult message_draft_store_save(
    MessageDraftStore* store, const MessageDraftInput& input) noexcept;
// Clear only this channel's exact reply target, retaining any text/selection.
MessageDraftStoreResult message_draft_store_clear_reply(
    MessageDraftStore* store, u64 channel_id, u64 reply_message_id) noexcept;
MessageDraftStoreResult message_draft_store_find(
    const MessageDraftStore& store, u64 channel_id) noexcept;
bool message_draft_store_snapshot(const MessageDraftStore& store,
                                  u64 channel_id,
                                  MessageDraftSnapshot* snapshot) noexcept;
bool message_draft_store_delete(MessageDraftStore* store,
                                u64 channel_id) noexcept;
bool message_draft_store_reset(MessageDraftStore* store) noexcept;

} // namespace maicord
