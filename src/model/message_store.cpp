#include "maicord/model/message_store.hpp"
#include <emmintrin.h>

#include "maicord/core/memory.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

static_assert(sizeof(MessageRecord) == 64);
static_assert(sizeof(MessageInput) == 72);

constexpr u16 next_generation(u16 generation) noexcept {
    ++generation;
    return generation == 0 ? 1 : generation;
}

MessageStoreResult store_error(MessageStoreError error) noexcept {
    return MessageStoreResult{MessageHandle{~0u, 0}, 0, error, false};
}

bool content_equal(const MessageStore& store, const MessageRecord& record, Utf8View content) noexcept {
    if (record.content_bytes != content.bytes) {
        return false;
    }
    if (content.bytes == 0) {
        return true;
    }
    const u8* resident = store.text + record.content_offset;
    const auto* incoming = reinterpret_cast<const u8*>(content.data);
    if (!resident || !incoming) {
        return false;
    }
    if (resident == incoming) {
        return true;
    }
    // Preserve the early exit for a changed first byte.
    if (resident[0] != incoming[0]) {
        return false;
    }
    u32 offset = 0;
    while (content.bytes - offset >= 16) {
        const __m128i left = _mm_loadu_si128(reinterpret_cast<const __m128i*>(resident + offset));
        const __m128i right = _mm_loadu_si128(reinterpret_cast<const __m128i*>(incoming + offset));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(left, right)) != 0xFFFF) {
            return false;
        }
        offset += 16;
    }
    for (; offset < content.bytes; ++offset) {
        if (resident[offset] != incoming[offset]) {
            return false;
        }
    }
    return true;
}

bool content_overlaps_text(const MessageStore& store, Utf8View content) noexcept {
    if (!content.data || content.bytes == 0) {
        return false;
    }
    const u64 content_begin = reinterpret_cast<u64>(content.data);
    const u64 text_begin = reinterpret_cast<u64>(store.text);
    return content_begin < text_begin ? text_begin - content_begin < content.bytes
                                      : content_begin - text_begin < store.text_capacity;
}

constexpr bool valid_content_availability(MessageContentAvailability availability) noexcept {
    return availability == MessageContentAvailability::unknown ||
           availability == MessageContentAvailability::available ||
           availability == MessageContentAvailability::restricted;
}

constexpr bool valid_delivery_state(MessageDeliveryState delivery) noexcept {
    return delivery == MessageDeliveryState::authoritative || delivery == MessageDeliveryState::pending ||
           delivery == MessageDeliveryState::failed;
}

u32 find_order_index(const MessageStore& store, u64 id, bool* found) noexcept {
    u32 first = 0;
    u32 count = store.count;
    while (count != 0) {
        const u32 step = count / 2;
        const u32 middle = first + step;
        const MessageRecord& record = store.records[store.order[middle]];
        if (record.id < id) {
            first = middle + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    *found = first < store.count && store.records[store.order[first]].id == id;
    return first;
}

u32 find_empty_slot(const MessageStore& store) noexcept {
    // The common append-only path keeps slots [0, count) occupied. Avoid an
    // otherwise quadratic scan while seeding or catching up a large store;
    // deletions still fall back to the bounded hole search below.
    if (store.count < store.capacity && !store.records[store.count].occupied) {
        return store.count;
    }
    for (u32 index = 0; index < store.capacity; ++index) {
        if (!store.records[index].occupied) {
            return index;
        }
    }
    return store.capacity;
}

MessageStoreResult result_from_slot(const MessageStore& store, u32 slot, u32 order_index,
                                    bool inserted) noexcept {
    return MessageStoreResult{
        MessageHandle{slot, store.records[slot].generation},
        order_index,
        MessageStoreError::none,
        inserted,
    };
}

} // namespace

bool message_store_initialize(MessageStore* store, Slice<MessageRecord> records, Slice<u32> order,
                              Slice<u8> text) noexcept {
    if (!store || !records.data || records.count == 0 || records.count > kMaximumMessageRecords ||
        !order.data || order.count != records.count || !text.data || text.count == 0) {
        return false;
    }
    *store = MessageStore{};
    store->records = records.data;
    store->order = order.data;
    store->text = text.data;
    store->capacity = records.count;
    store->text_capacity = text.count;
    for (u32 index = 0; index < records.count; ++index) {
        records[index] = MessageRecord{};
        order[index] = ~0u;
    }
    return true;
}

MessageStoreError message_store_validate_input(const MessageStore* store,
                                               const MessageInput& input) noexcept {
    if (!store || !store->records || !store->order || !store->text || input.id == 0 ||
        input.channel_id == 0 || input.author_id == 0 ||
        (!input.content.data && input.content.bytes != 0) ||
        input.content.bytes > kMaximumMessageContentBytes ||
        !valid_content_availability(input.content_availability) || !valid_delivery_state(input.delivery) ||
        (input.presentation_flags & ~kMessagePresentationKnownFlags) != 0) {
        return MessageStoreError::invalid_input;
    }
    return utf8_validate(input.content).valid ? MessageStoreError::none : MessageStoreError::invalid_utf8;
}

MessageStoreResult message_store_upsert(MessageStore* store, const MessageInput& input) noexcept {
    const MessageStoreError validation = message_store_validate_input(store, input);
    if (validation != MessageStoreError::none) {
        if (store) {
            ++store->rejected;
        }
        return store_error(validation);
    }
    bool found = false;
    const u32 order_index = find_order_index(*store, input.id, &found);
    if (found) {
        const u32 slot = store->order[order_index];
        MessageRecord& record = store->records[slot];
        if (!content_equal(*store, record, input.content)) {
            if (input.content.bytes <= record.content_bytes) {
                copy_bytes(store->text + record.content_offset,
                           reinterpret_cast<const u8*>(input.content.data), input.content.bytes);
                store->text_wasted += record.content_bytes - input.content.bytes;
                record.content_bytes = input.content.bytes;
            } else if (input.content.bytes <= store->text_capacity - store->text_used) {
                const u32 new_offset = store->text_used;
                copy_bytes(store->text + new_offset, reinterpret_cast<const u8*>(input.content.data),
                           input.content.bytes);
                store->text_used += input.content.bytes;
                store->text_wasted += record.content_bytes;
                record.content_offset = new_offset;
                record.content_bytes = input.content.bytes;
            } else {
                const u32 live_without_previous =
                    store->text_used - store->text_wasted - record.content_bytes;
                if (input.content.bytes > store->text_capacity - live_without_previous ||
                    content_overlaps_text(*store, input.content)) {
                    ++store->rejected;
                    return store_error(input.content.bytes > store->text_capacity - live_without_previous
                                           ? MessageStoreError::text_capacity
                                           : MessageStoreError::invalid_input);
                }
                store->text_wasted += record.content_bytes;
                record.content_bytes = 0;
                if (!message_store_compact_in_place(store)) {
                    return store_error(MessageStoreError::text_capacity);
                }
                record.content_offset = store->text_used;
                record.content_bytes = input.content.bytes;
                copy_bytes(store->text + store->text_used, reinterpret_cast<const u8*>(input.content.data),
                           input.content.bytes);
                store->text_used += input.content.bytes;
            }
        }
        record.channel_id = input.channel_id;
        record.author_id = input.author_id;
        record.timestamp_ms =
            input.timestamp_ms != 0 ? input.timestamp_ms : snowflake_timestamp_ms(input.id);
        record.edited_timestamp_ms = input.edited_timestamp_ms;
        record.flags = input.flags;
        record.presentation_flags = static_cast<u8>(input.presentation_flags);
        record.type = input.type;
        record.content_availability = input.content_availability;
        record.delivery = input.delivery;
        saturating_increment(record.revision);
        ++store->updated;
        return result_from_slot(*store, slot, order_index, false);
    }
    if (store->count >= store->capacity) {
        ++store->rejected;
        return store_error(MessageStoreError::record_capacity);
    }
    if (input.content.bytes > store->text_capacity - store->text_used) {
        const u32 live_bytes = store->text_used - store->text_wasted;
        if (input.content.bytes > store->text_capacity - live_bytes) {
            ++store->rejected;
            return store_error(MessageStoreError::text_capacity);
        }
        if (!message_store_compact_in_place(store)) {
            return store_error(MessageStoreError::text_capacity);
        }
    }
    const u32 slot = find_empty_slot(*store);
    if (slot >= store->capacity) {
        ++store->rejected;
        return store_error(MessageStoreError::record_capacity);
    }
    for (u32 index = store->count; index > order_index; --index) {
        store->order[index] = store->order[index - 1];
    }
    store->order[order_index] = slot;
    MessageRecord& record = store->records[slot];
    const u16 generation = next_generation(record.generation);
    record = MessageRecord{};
    record.id = input.id;
    record.channel_id = input.channel_id;
    record.author_id = input.author_id;
    record.timestamp_ms =
        input.timestamp_ms != 0 ? input.timestamp_ms : snowflake_timestamp_ms(input.id);
    record.edited_timestamp_ms = input.edited_timestamp_ms;
    record.content_offset = store->text_used;
    record.content_bytes = input.content.bytes;
    record.flags = input.flags;
    record.presentation_flags = static_cast<u8>(input.presentation_flags);
    record.type = input.type;
    record.revision = 1;
    record.generation = generation;
    record.occupied = true;
    record.content_availability = input.content_availability;
    record.delivery = input.delivery;
    copy_bytes(store->text + store->text_used, reinterpret_cast<const u8*>(input.content.data),
               input.content.bytes);
    store->text_used += input.content.bytes;
    ++store->count;
    ++store->created;
    return result_from_slot(*store, slot, order_index, true);
}

MessageStoreResult message_store_find(const MessageStore& store, u64 id) noexcept {
    if (!store.records || !store.order || id == 0) {
        return store_error(MessageStoreError::invalid_input);
    }
    bool found = false;
    const u32 order_index = find_order_index(store, id, &found);
    return found ? result_from_slot(store, store.order[order_index], order_index, false)
                 : store_error(MessageStoreError::not_found);
}

bool message_store_delete(MessageStore* store, u64 id) noexcept {
    if (!store || !store->records || !store->order || id == 0) {
        if (store) {
            ++store->rejected;
        }
        return false;
    }
    bool found = false;
    const u32 order_index = find_order_index(*store, id, &found);
    if (!found) {
        ++store->rejected;
        return false;
    }
    MessageRecord& record = store->records[store->order[order_index]];
    store->text_wasted += record.content_bytes;
    record.occupied = false;
    for (u32 index = order_index + 1; index < store->count; ++index) {
        store->order[index - 1] = store->order[index];
    }
    --store->count;
    store->order[store->count] = ~0u;
    ++store->deleted;
    return true;
}

bool message_store_set_delivery(MessageStore* store, u64 id, MessageDeliveryState delivery) noexcept {
    if (!store || !valid_delivery_state(delivery) || id == 0) {
        if (store) {
            ++store->rejected;
        }
        return false;
    }
    const MessageStoreResult found = message_store_find(*store, id);
    if (!found.ok()) {
        return false;
    }
    MessageRecord& record = store->records[found.handle.slot];
    if (record.delivery == delivery) {
        return true;
    }
    record.delivery = delivery;
    saturating_increment(record.revision);
    ++store->updated;
    return true;
}

bool message_store_set_presentation_flags(MessageStore* store, u64 id, u32 flags) noexcept {
    if (!store || (flags & ~kMessagePresentationKnownFlags) != 0) {
        return false;
    }
    const MessageStoreResult found = message_store_find(*store, id);
    MessageRecord* record = found.ok() ? &store->records[found.handle.slot] : nullptr;
    if (!record || !record->occupied || record->generation != found.handle.generation) {
        return false;
    }
    if (record->presentation_flags != flags) {
        record->presentation_flags = static_cast<u8>(flags);
        ++store->updated;
    }
    return true;
}

const MessageRecord* message_store_resolve(const MessageStore& store, MessageHandle handle) noexcept {
    if (!store.records || handle.generation == 0 || handle.slot >= store.capacity) {
        return nullptr;
    }
    const MessageRecord& record = store.records[handle.slot];
    return record.occupied && record.generation == handle.generation ? &record : nullptr;
}

MessageHandle message_store_ordered_handle(const MessageStore& store, u32 order_index) noexcept {
    if (!store.records || !store.order || order_index >= store.count) {
        return MessageHandle{~0u, 0};
    }
    const u32 slot = store.order[order_index];
    return MessageHandle{slot, store.records[slot].generation};
}

Utf8View message_store_content(const MessageStore& store, MessageHandle handle) noexcept {
    const MessageRecord* record = message_store_resolve(store, handle);
    return record ? Utf8View{reinterpret_cast<const char*>(store.text + record->content_offset),
                             record->content_bytes}
                  : Utf8View{};
}

bool message_store_compact(MessageStore* store, Slice<u8> scratch) noexcept {
    if (!store || !store->records || !store->order || !store->text || !scratch.data ||
        scratch.data == store->text) {
        if (store) {
            ++store->rejected;
        }
        return false;
    }
    const u32 live_bytes = store->text_used - store->text_wasted;
    const u64 text_begin = reinterpret_cast<u64>(store->text);
    const u64 text_end = text_begin + store->text_capacity;
    const u64 scratch_begin = reinterpret_cast<u64>(scratch.data);
    const u64 scratch_end = scratch_begin + scratch.count;
    if (scratch.count < live_bytes || (scratch_begin < text_end && scratch_end > text_begin)) {
        ++store->rejected;
        return false;
    }
    u32 write_offset = 0;
    for (u32 index = 0; index < store->count; ++index) {
        MessageRecord& record = store->records[store->order[index]];
        copy_bytes(scratch.data + write_offset, store->text + record.content_offset, record.content_bytes);
        record.content_offset = write_offset;
        write_offset += record.content_bytes;
    }
    copy_bytes(store->text, scratch.data, write_offset);
    store->text_used = write_offset;
    store->text_wasted = 0;
    ++store->compactions;
    return true;
}

bool message_store_compact_in_place(MessageStore* store) noexcept {
    if (!store || !store->records || !store->order || !store->text ||
        store->text_wasted > store->text_used) {
        if (store) {
            ++store->rejected;
        }
        return false;
    }
    u64 live_bytes = 0;
    u32 nonempty_records = 0;
    u32 occupied_records = 0;
    for (u32 slot = 0; slot < store->capacity; ++slot) {
        const MessageRecord& record = store->records[slot];
        if (!record.occupied) {
            continue;
        }
        ++occupied_records;
        if (record.content_offset > store->text_used ||
            record.content_bytes > store->text_used - record.content_offset) {
            ++store->rejected;
            return false;
        }
        live_bytes += record.content_bytes;
        if (live_bytes > store->text_used) {
            ++store->rejected;
            return false;
        }
        nonempty_records += record.content_bytes != 0 ? 1u : 0u;
    }
    if (live_bytes != store->text_used - store->text_wasted) {
        ++store->rejected;
        return false;
    }
    // Initial history and live appends keep text in message order, even after slot reuse.
    // Validate the complete order and all adjacent ranges before the first write.
    bool ordered = store->count == occupied_records;
    u64 previous_id = 0;
    u32 previous_end = 0;
    for (u32 index = 0; ordered && index < store->count; ++index) {
        const u32 slot = store->order[index];
        if (slot >= store->capacity) {
            ordered = false;
            break;
        }
        const MessageRecord& record = store->records[slot];
        if (!record.occupied || record.id <= previous_id ||
            (record.content_bytes && record.content_offset < previous_end)) {
            ordered = false;
            break;
        }
        previous_id = record.id;
        if (record.content_bytes)
            previous_end = record.content_offset + record.content_bytes;
    }
    if (ordered) {
        u32 write_offset = 0;
        for (u32 index = 0; index < store->count; ++index) {
            MessageRecord& record = store->records[store->order[index]];
            if (!record.content_bytes) {
                record.content_offset = static_cast<u32>(live_bytes);
                continue;
            }
            copy_bytes(store->text + write_offset, store->text + record.content_offset, record.content_bytes);
            record.content_offset = write_offset;
            write_offset += record.content_bytes;
        }
        store->text_used = write_offset;
        store->text_wasted = 0;
        ++store->compactions;
        return true;
    }
    for (u32 left = 0; left < store->capacity; ++left) {
        const MessageRecord& first = store->records[left];
        if (!first.occupied || first.content_bytes == 0) {
            continue;
        }
        const u32 first_end = first.content_offset + first.content_bytes;
        for (u32 right = left + 1; right < store->capacity; ++right) {
            const MessageRecord& second = store->records[right];
            if (!second.occupied || second.content_bytes == 0) {
                continue;
            }
            const u32 second_end = second.content_offset + second.content_bytes;
            if (first.content_offset < second_end && second.content_offset < first_end) {
                ++store->rejected;
                return false;
            }
        }
    }

    u32 write_offset = 0;
    u32 previous_source = 0;
    bool has_previous = false;
    for (u32 processed = 0; processed < nonempty_records; ++processed) {
        u32 selected = store->capacity;
        u32 selected_source = ~0u;
        for (u32 slot = 0; slot < store->capacity; ++slot) {
            const MessageRecord& record = store->records[slot];
            if (!record.occupied || record.content_bytes == 0 ||
                (has_previous && record.content_offset <= previous_source) ||
                record.content_offset >= selected_source) {
                continue;
            }
            selected = slot;
            selected_source = record.content_offset;
        }
        if (selected == store->capacity) {
            ++store->rejected;
            return false;
        }
        MessageRecord& record = store->records[selected];
        copy_bytes(store->text + write_offset, store->text + selected_source, record.content_bytes);
        record.content_offset = write_offset;
        write_offset += record.content_bytes;
        previous_source = selected_source;
        has_previous = true;
    }
    for (u32 slot = 0; slot < store->capacity; ++slot) {
        MessageRecord& record = store->records[slot];
        if (record.occupied && record.content_bytes == 0) {
            record.content_offset = write_offset;
        }
    }
    store->text_used = write_offset;
    store->text_wasted = 0;
    ++store->compactions;
    return true;
}

u64 snowflake_timestamp_ms(u64 snowflake) noexcept {
    return (snowflake >> 22) + kSnowflakeEpochMilliseconds;
}

} // namespace maicord
