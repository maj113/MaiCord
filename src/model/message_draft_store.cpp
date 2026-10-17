#include "maicord/model/message_draft_store.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

MessageDraftStoreResult draft_error(MessageDraftStoreError error) noexcept {
    return MessageDraftStoreResult{~0u, error, false, false};
}

bool store_valid(const MessageDraftStore& store) noexcept {
    return store.records && store.text && store.capacity != 0 &&
           store.capacity <= kMessageDraftStoreMaximumRecords && store.count <= store.capacity &&
           store.text_used <= store.text_capacity && store.text_wasted <= store.text_used;
}

bool text_boundary(Utf8View text, u32 offset) noexcept {
    return offset <= text.bytes &&
           (offset == 0 || offset == text.bytes || (static_cast<u8>(text.data[offset]) & 0xC0u) != 0x80u);
}

MessageDraftStoreError validate_input(const MessageDraftStore* store,
                                      const MessageDraftInput& input) noexcept {
    if (!store || !store_valid(*store) || input.channel_id == 0 ||
        (!input.text.data && input.text.bytes != 0) || input.text.bytes > store->text_capacity ||
        input.anchor > input.text.bytes || input.caret > input.text.bytes) {
        return MessageDraftStoreError::invalid_input;
    }
    if (!utf8_validate(input.text).valid || !text_boundary(input.text, input.anchor) ||
        !text_boundary(input.text, input.caret)) {
        return MessageDraftStoreError::invalid_utf8;
    }
    const u64 source_begin = reinterpret_cast<u64>(input.text.data);
    const u64 source_end = source_begin + input.text.bytes;
    const u64 bank_begin = reinterpret_cast<u64>(store->text);
    const u64 bank_end = bank_begin + store->text_capacity;
    if (input.text.bytes != 0 && source_begin < bank_end && source_end > bank_begin) {
        return MessageDraftStoreError::invalid_input;
    }
    return MessageDraftStoreError::none;
}

u32 find_index(const MessageDraftStore& store, u64 channel_id) noexcept {
    for (u32 index = 0; index < store.capacity; ++index) {
        if (store.records[index].occupied && store.records[index].channel_id == channel_id) {
            return index;
        }
    }
    return store.capacity;
}

u32 empty_index(const MessageDraftStore& store) noexcept {
    for (u32 index = 0; index < store.capacity; ++index) {
        if (!store.records[index].occupied) {
            return index;
        }
    }
    return store.capacity;
}

bool text_equal(const MessageDraftStore& store, const MessageDraftRecord& record, Utf8View text) noexcept {
    if (record.text_bytes != text.bytes || record.text_offset > store.text_used ||
        record.text_bytes > store.text_used - record.text_offset) {
        return false;
    }
    return bytes_equal(store.text + record.text_offset, text.data, text.bytes);
}

bool compact(MessageDraftStore* store) noexcept {
    if (!store_valid(*store)) {
        return false;
    }
    u32 write_offset = 0;
    for (u32 index = 0; index < store->capacity; ++index) {
        if (store->records[index].occupied && store->records[index].text_bytes == 0) {
            store->records[index].text_offset = 0;
        }
    }
    u32 previous_source = 0;
    bool has_previous = false;
    for (u32 processed = 0; processed < store->count; ++processed) {
        u32 selected = store->capacity;
        u32 selected_source = ~0u;
        for (u32 index = 0; index < store->capacity; ++index) {
            const MessageDraftRecord& record = store->records[index];
            if (!record.occupied || record.text_bytes == 0 ||
                (has_previous && record.text_offset <= previous_source) ||
                record.text_offset >= selected_source) {
                continue;
            }
            selected = index;
            selected_source = record.text_offset;
        }
        if (selected == store->capacity) {
            break;
        }
        MessageDraftRecord& record = store->records[selected];
        if (record.text_offset > store->text_used ||
            record.text_bytes > store->text_used - record.text_offset) {
            return false;
        }
        copy_bytes(store->text + write_offset, store->text + record.text_offset, record.text_bytes);
        record.text_offset = write_offset;
        write_offset += record.text_bytes;
        previous_source = selected_source;
        has_previous = true;
    }
    clear_bytes(store->text + write_offset, store->text_used - write_offset);
    store->text_used = write_offset;
    store->text_wasted = 0;
    ++store->compactions;
    return true;
}

u64 touch(MessageDraftStore* store) noexcept {
    ++store->clock;
    if (store->clock == 0) {
        store->clock = 1;
    }
    return store->clock;
}

bool erase_index(MessageDraftStore* store, u32 index) noexcept {
    if (!store_valid(*store) || index >= store->capacity || !store->records[index].occupied) {
        return false;
    }
    MessageDraftRecord& record = store->records[index];
    clear_bytes(store->text + record.text_offset, record.text_bytes);
    store->text_wasted += record.text_bytes;
    record = MessageDraftRecord{};
    --store->count;
    ++store->deleted;
    ++store->revision;
    return true;
}

bool initialize_store(MessageDraftStore* store, Slice<MessageDraftRecord> records, Slice<u8> text,
                      bool clear_text) noexcept {
    if (!store || !records.data || records.count == 0 || records.count > kMessageDraftStoreMaximumRecords ||
        !text.data || text.count == 0) {
        return false;
    }
    *store = MessageDraftStore{};
    store->records = records.data;
    store->text = text.data;
    store->capacity = records.count;
    store->text_capacity = text.count;
    for (u32 index = 0; index < records.count; ++index) {
        records[index] = MessageDraftRecord{};
    }
    if (clear_text)
        clear_bytes(text.data, text.count);
    return true;
}
} // namespace

bool message_draft_store_initialize(MessageDraftStore* store, Slice<MessageDraftRecord> records,
                                    Slice<u8> text) noexcept {
    return initialize_store(store, records, text, true);
}

bool message_draft_store_initialize_sparse(MessageDraftStore* store, Slice<MessageDraftRecord> records,
                                           Slice<u8> text) noexcept {
    return initialize_store(store, records, text, false);
}

MessageDraftStoreResult message_draft_store_save(MessageDraftStore* store,
                                                 const MessageDraftInput& input) noexcept {
    const MessageDraftStoreError validation = validate_input(store, input);
    if (validation != MessageDraftStoreError::none) {
        if (store) {
            ++store->rejected;
        }
        return draft_error(validation);
    }
    const u32 found = find_index(*store, input.channel_id);
    if (input.text.bytes == 0 && input.reply_message_id == 0) {
        if (found == store->capacity) {
            return MessageDraftStoreResult{~0u, MessageDraftStoreError::none, false, false};
        }
        return erase_index(store, found)
                   ? MessageDraftStoreResult{found, MessageDraftStoreError::none, true, true}
                   : draft_error(MessageDraftStoreError::invalid_input);
    }
    if (found != store->capacity) {
        MessageDraftRecord& record = store->records[found];
        const bool mention_reply_author = input.reply_message_id == 0 || input.mention_reply_author;
        const bool same_text = text_equal(*store, record, input.text);
        if (same_text && record.anchor == input.anchor && record.caret == input.caret &&
            record.reply_message_id == input.reply_message_id &&
            record.mention_reply_author == mention_reply_author) {
            record.touched = touch(store);
            return MessageDraftStoreResult{found, MessageDraftStoreError::none, false, false};
        }
        const u32 live_without_previous = store->text_used - store->text_wasted - record.text_bytes;
        if (input.text.bytes > store->text_capacity - live_without_previous) {
            ++store->rejected;
            return draft_error(MessageDraftStoreError::text_capacity);
        }
        const bool tail = record.text_offset + record.text_bytes == store->text_used;
        if (same_text) {
            // Reply and selection changes do not consume or move text bytes.
        } else if (tail && input.text.bytes <= record.text_bytes) {
            copy_bytes(store->text + record.text_offset, reinterpret_cast<const u8*>(input.text.data),
                       input.text.bytes);
            clear_bytes(store->text + record.text_offset + input.text.bytes,
                        record.text_bytes - input.text.bytes);
            store->text_used -= record.text_bytes - input.text.bytes;
            record.text_bytes = input.text.bytes;
        } else if (tail &&
                   input.text.bytes - record.text_bytes <= store->text_capacity - store->text_used) {
            copy_bytes(store->text + record.text_offset, reinterpret_cast<const u8*>(input.text.data),
                       input.text.bytes);
            store->text_used += input.text.bytes - record.text_bytes;
            record.text_bytes = input.text.bytes;
        } else if (input.text.bytes <= record.text_bytes) {
            copy_bytes(store->text + record.text_offset, reinterpret_cast<const u8*>(input.text.data),
                       input.text.bytes);
            clear_bytes(store->text + record.text_offset + input.text.bytes,
                        record.text_bytes - input.text.bytes);
            store->text_wasted += record.text_bytes - input.text.bytes;
            record.text_bytes = input.text.bytes;
        } else {
            clear_bytes(store->text + record.text_offset, record.text_bytes);
            store->text_wasted += record.text_bytes;
            record.text_bytes = 0;
            if (input.text.bytes > store->text_capacity - store->text_used && !compact(store)) {
                ++store->rejected;
                return draft_error(MessageDraftStoreError::text_capacity);
            }
            record.text_offset = store->text_used;
            record.text_bytes = input.text.bytes;
            copy_bytes(store->text + store->text_used, reinterpret_cast<const u8*>(input.text.data),
                       input.text.bytes);
            store->text_used += input.text.bytes;
        }
        record.anchor = input.anchor;
        record.caret = input.caret;
        record.reply_message_id = input.reply_message_id;
        record.mention_reply_author = mention_reply_author;
        if (record.text_bytes == 0) {
            record.text_offset = 0;
        }
        record.touched = touch(store);
        saturating_increment(record.revision);
        ++store->saved;
        ++store->revision;
        return MessageDraftStoreResult{found, MessageDraftStoreError::none, true, false};
    }
    if (store->count == store->capacity) {
        ++store->rejected;
        return draft_error(MessageDraftStoreError::record_capacity);
    }
    if (input.text.bytes > store->text_capacity - store->text_used &&
        (!compact(store) || input.text.bytes > store->text_capacity - store->text_used)) {
        ++store->rejected;
        return draft_error(MessageDraftStoreError::text_capacity);
    }
    const u32 index = empty_index(*store);
    if (index == store->capacity) {
        ++store->rejected;
        return draft_error(MessageDraftStoreError::record_capacity);
    }
    MessageDraftRecord& record = store->records[index];
    record = MessageDraftRecord{};
    record.channel_id = input.channel_id;
    record.touched = touch(store);
    record.reply_message_id = input.reply_message_id;
    record.mention_reply_author = input.reply_message_id == 0 || input.mention_reply_author;
    record.text_offset = input.text.bytes == 0 ? 0 : store->text_used;
    record.text_bytes = input.text.bytes;
    record.anchor = input.anchor;
    record.caret = input.caret;
    record.revision = 1;
    record.occupied = true;
    copy_bytes(store->text + store->text_used, reinterpret_cast<const u8*>(input.text.data),
               input.text.bytes);
    store->text_used += input.text.bytes;
    ++store->count;
    ++store->saved;
    ++store->revision;
    return MessageDraftStoreResult{index, MessageDraftStoreError::none, true, false};
}

MessageDraftStoreResult message_draft_store_clear_reply(MessageDraftStore* store, u64 channel_id,
                                                        u64 reply_message_id) noexcept {
    if (!store || !store_valid(*store) || channel_id == 0 || reply_message_id == 0) {
        if (store) {
            ++store->rejected;
        }
        return draft_error(MessageDraftStoreError::invalid_input);
    }
    const u32 index = find_index(*store, channel_id);
    if (index == store->capacity || store->records[index].reply_message_id != reply_message_id) {
        return MessageDraftStoreResult{~0u, MessageDraftStoreError::none, false, false};
    }
    MessageDraftRecord& record = store->records[index];
    if (record.text_bytes == 0) {
        return erase_index(store, index)
                   ? MessageDraftStoreResult{index, MessageDraftStoreError::none, true, true}
                   : draft_error(MessageDraftStoreError::invalid_input);
    }
    record.reply_message_id = 0;
    record.mention_reply_author = true;
    record.touched = touch(store);
    saturating_increment(record.revision);
    ++store->saved;
    ++store->revision;
    return MessageDraftStoreResult{index, MessageDraftStoreError::none, true, false};
}

MessageDraftStoreResult message_draft_store_find(const MessageDraftStore& store, u64 channel_id) noexcept {
    if (!store_valid(store) || channel_id == 0) {
        return draft_error(MessageDraftStoreError::invalid_input);
    }
    const u32 index = find_index(store, channel_id);
    return index != store.capacity
               ? MessageDraftStoreResult{index, MessageDraftStoreError::none, false, false}
               : draft_error(MessageDraftStoreError::not_found);
}

bool message_draft_store_snapshot(const MessageDraftStore& store, u64 channel_id,
                                  MessageDraftSnapshot* snapshot) noexcept {
    if (snapshot) {
        *snapshot = MessageDraftSnapshot{};
    }
    const MessageDraftStoreResult found = message_draft_store_find(store, channel_id);
    if (!snapshot || !found.ok()) {
        return false;
    }
    const MessageDraftRecord& record = store.records[found.index];
    if (record.text_offset > store.text_used || record.text_bytes > store.text_used - record.text_offset) {
        return false;
    }
    *snapshot = MessageDraftSnapshot{
        Utf8View{reinterpret_cast<const char*>(store.text + record.text_offset), record.text_bytes},
        record.channel_id,
        record.touched,
        record.anchor,
        record.caret,
        record.revision,
        record.reply_message_id,
        record.mention_reply_author,
    };
    return true;
}

bool message_draft_store_delete(MessageDraftStore* store, u64 channel_id) noexcept {
    if (!store || !store_valid(*store) || channel_id == 0) {
        if (store) {
            ++store->rejected;
        }
        return false;
    }
    const u32 index = find_index(*store, channel_id);
    return index == store->capacity || erase_index(store, index);
}

bool message_draft_store_reset(MessageDraftStore* store) noexcept {
    if (!store || !store_valid(*store)) {
        return false;
    }
    // Every save, shrink and compaction clears the bytes it frees, so the
    // text past text_used is already zero: scrub only the live prefix and
    // leave an unused bank's pages uncommitted.
    clear_bytes(store->text, store->text_used);
    return initialize_store(store, Slice<MessageDraftRecord>{store->records, store->capacity},
                            Slice<u8>{store->text, store->text_capacity}, false);
}

} // namespace maicord
