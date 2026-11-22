#include "maicord/model/chat/message_store.hpp"
#include "maicord/sys/simd.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/model/chat/sorted_search.hpp"
#include "maicord/core/time.hpp"

namespace maicord {
namespace {

static_assert(sizeof(MessageRecord) == 64);
static_assert(sizeof(MessageInput) == 72);

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
    if (resident == incoming) {
        return true;
    }
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

constexpr bool valid_content_availability(MessageContentAvailability availability) noexcept {
    return availability == MessageContentAvailability::unknown ||
           availability == MessageContentAvailability::available ||
           availability == MessageContentAvailability::restricted;
}

constexpr bool valid_delivery_state(MessageDeliveryState delivery) noexcept {
    return delivery == MessageDeliveryState::authoritative || delivery == MessageDeliveryState::pending ||
           delivery == MessageDeliveryState::failed || delivery == MessageDeliveryState::retained;
}

u32 find_order_index(const MessageStore& store, u64 id, bool* found) noexcept {
    const u32 first = sorted_lower_bound(
        store.count, [&](u32 at) noexcept { return store.records[store.order[at]].id < id; });
    *found = first < store.count && store.records[store.order[first]].id == id;
    return first;
}

// The store holds fewer than `capacity` records, so a free slot exists.
u32 find_empty_slot(const MessageStore& store) noexcept {
    // The common append-only path keeps slots [0, count) occupied. Avoid an
    // otherwise quadratic scan while seeding or catching up a large store;
    // deletions still fall back to the hole search below.
    if (!store.records[store.count].occupied)
        return store.count;
    u32 index = 0;
    while (store.records[index].occupied)
        ++index;
    return index;
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

enum class AppendRoom : u8 { ready, full, failed };

// Makes `bytes` appendable at text_used. Compaction comes first whenever it
// frees enough committed text; a growing bank commits more only otherwise.
// `full` leaves the store unchanged; `failed` may follow a compaction.
AppendRoom append_room(MessageStore* store, u32 bytes, Utf8View source) noexcept {
    if (bytes <= store->text_committed - store->text_used)
        return AppendRoom::ready;
    const u32 live = store->text_used - store->text_wasted;
    if (bytes > store->text_capacity - live || message_store_content_overlaps(*store, source))
        return AppendRoom::full;
    if (bytes <= store->text_committed - live || store->text_used + bytes > store->text_capacity) {
        message_store_compact_in_place(store);
        if (bytes <= store->text_committed - store->text_used)
            return AppendRoom::ready;
    }
    Arena* arena = store->text_arena;
    if (!arena)
        return AppendRoom::failed;
    const u32 target = minimum(store->text_capacity, (store->text_used + bytes + kMessageStoreTextCommitStep - 1) &
                                                         ~(kMessageStoreTextCommitStep - 1));
    if (target > arena->offset && !arena_allocate(arena, target - arena->offset, 1))
        return AppendRoom::failed;
    store->text_committed = static_cast<u32>(arena->offset);
    return AppendRoom::ready;
}

// Metadata an upsert replaces on new and resident records alike.
void assign_metadata(MessageRecord& record, const MessageInput& input) noexcept {
    record.channel_id = input.channel_id;
    record.author_id = input.author_id;
    record.timestamp_ms = input.timestamp_ms != 0 ? input.timestamp_ms : snowflake_timestamp_ms(input.id);
    record.edited_timestamp_ms = input.edited_timestamp_ms;
    record.flags = input.flags;
    record.presentation_flags = static_cast<u8>(input.presentation_flags);
    record.type = input.type;
    record.content_availability = input.content_availability;
    record.delivery = input.delivery;
}

} // namespace

// Initialization and text compaction run at start-up and when the text storage is full: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool message_store_initialize(MessageStore* store, Slice<MessageRecord> records, Slice<u32> order,
                              Slice<u8> text) noexcept {
    reset_value<MessageStore>(store);
    store->records = records.data;
    store->order = order.data;
    store->text = text.data;
    store->capacity = records.count;
    store->text_capacity = text.count;
    store->text_committed = text.count;
    for (u32 index = 0; index < records.count; ++index) {
        reset_value<MessageRecord>(&records[index]);
        order[index] = ~0u;
    }
    return true;
}

bool message_store_initialize_growing(MessageStore* store, Slice<MessageRecord> records, Slice<u32> order,
                                      Arena* text_arena) noexcept {
    // The one arena (MessageViewRuntime::text_storage) reserves kMessageViewTextCapacity, a u32 above the step.
    // The first step stays committed. A trim to zero costs a decommit, a commit and four page faults at each
    // channel switch, and then the store commits the same step again.
    const usize kept = minimum<usize>(text_arena->offset, kMessageStoreTextCommitStep);
    if (!arena_trim(text_arena, kept) ||
        (kept < kMessageStoreTextCommitStep && !arena_allocate(text_arena, kMessageStoreTextCommitStep - kept, 1)) ||
        !message_store_initialize(store, records, order,
                                  {text_arena->base, static_cast<u32>(text_arena->reserved)}))
        return false;
    store->text_arena = text_arena;
    store->text_committed = kMessageStoreTextCommitStep;
    return true;
}
#pragma clang attribute pop

// Everything but the content's encoding.
static MessageStoreError validate_fields(const MessageStore* store, const MessageInput& input) noexcept {
    if (!store || !store->records || !store->order || !store->text || input.id == 0 ||
        input.channel_id == 0 || input.author_id == 0 ||
        (!input.content.data && input.content.bytes != 0) ||
        input.content.bytes > kMaximumMessageContentBytes ||
        !valid_content_availability(input.content_availability) || !valid_delivery_state(input.delivery) ||
        (input.presentation_flags & ~kMessagePresentationKnownFlags) != 0) {
        return MessageStoreError::invalid_input;
    }
    return MessageStoreError::none;
}

MessageStoreError message_store_validate_input(const MessageStore* store,
                                               const MessageInput& input) noexcept {
    const MessageStoreError fields = validate_fields(store, input);
    if (fields != MessageStoreError::none)
        return fields;
    return utf8_validate(input.content).valid ? MessageStoreError::none : MessageStoreError::invalid_utf8;
}

MessageStoreResult message_store_upsert_located(MessageStore* store, const MessageInput& input,
                                                MessageStoreSlot at) noexcept {
    const MessageStoreError validation = validate_fields(store, input);
    if (validation != MessageStoreError::none)
        return store_error(validation);
    const u32 order_index = at.order_index;
    const bool found = at.found;
    const bool unchanged =
        found && content_equal(*store, store->records[store->order[order_index]], input.content);
    // Content equal to the stored, already validated text needs no re-check.
    if (!unchanged && !utf8_validate(input.content).valid)
        return store_error(MessageStoreError::invalid_utf8);
    if (found) {
        const u32 slot = store->order[order_index];
        MessageRecord& record = store->records[slot];
        if (!unchanged) {
            if (input.content.bytes <= record.content_bytes) {
                copy_bytes(store->text + record.content_offset,
                           reinterpret_cast<const u8*>(input.content.data), input.content.bytes);
                store->text_wasted += record.content_bytes - input.content.bytes;
                record.content_bytes = input.content.bytes;
            } else if (input.content.bytes <= store->text_committed - store->text_used) {
                const u32 new_offset = store->text_used;
                copy_bytes(store->text + new_offset, reinterpret_cast<const u8*>(input.content.data),
                           input.content.bytes);
                store->text_used += input.content.bytes;
                store->text_wasted += record.content_bytes;
                record.content_offset = new_offset;
                record.content_bytes = input.content.bytes;
            } else {
                const u32 previous = record.content_bytes;
                store->text_wasted += previous;
                record.content_bytes = 0;
                const AppendRoom room = append_room(store, input.content.bytes, input.content);
                if (room == AppendRoom::full) {
                    store->text_wasted -= previous;
                    record.content_bytes = previous;
                    return store_error(message_store_content_overlaps(*store, input.content)
                                           ? MessageStoreError::invalid_input
                                           : MessageStoreError::text_capacity);
                }
                if (room == AppendRoom::failed) {
                    return store_error(MessageStoreError::text_capacity);
                }
                record.content_offset = store->text_used;
                record.content_bytes = input.content.bytes;
                copy_bytes(store->text + store->text_used, reinterpret_cast<const u8*>(input.content.data),
                           input.content.bytes);
                store->text_used += input.content.bytes;
            }
        }
        assign_metadata(record, input);
        saturating_increment(record.revision);
        ++store->updated;
        return result_from_slot(*store, slot, order_index, false);
    }
    if (store->count >= store->capacity)
        return store_error(MessageStoreError::record_capacity);
    if (append_room(store, input.content.bytes, input.content) != AppendRoom::ready)
        return store_error(MessageStoreError::text_capacity);
    const u32 slot = find_empty_slot(*store);
    sorted_insert_gap(store->order, store->count, order_index);
    store->order[order_index] = slot;
    MessageRecord& record = store->records[slot];
    record.id = input.id;
    assign_metadata(record, input);
    record.content_offset = store->text_used;
    record.content_bytes = input.content.bytes;
    record.revision = 1;
    record.generation = next_generation(record.generation);
    record.occupied = true;
    copy_bytes(store->text + store->text_used, reinterpret_cast<const u8*>(input.content.data),
               input.content.bytes);
    store->text_used += input.content.bytes;
    ++store->count;
    ++store->created;
    return result_from_slot(*store, slot, order_index, true);
}

MessageStoreResult message_store_upsert(MessageStore* store, const MessageInput& input) noexcept {
    return message_store_upsert_located(store, input, store ? message_store_locate(*store, input.id)
                                                            : MessageStoreSlot{});
}

MessageStoreSlot message_store_locate(const MessageStore& store, u64 id) noexcept {
    MessageStoreSlot at{};
    if (store.records && store.order && id != 0)
        at.order_index = find_order_index(store, id, &at.found);
    return at;
}

// An uninitialized store holds no records, and the store refuses id 0, so neither needs a check.
const MessageRecord* message_store_find_record(const MessageStore& store, u64 id) noexcept {
    bool found = false;
    const u32 order_index = find_order_index(store, id, &found);
    return found ? &store.records[store.order[order_index]] : nullptr;
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

void message_store_delete_at(MessageStore* store, u32 order_index) noexcept {
    MessageRecord& record = store->records[store->order[order_index]];
    store->text_wasted += record.content_bytes;
    record.occupied = false;
    sorted_erase_gap(store->order, store->count, order_index);
    --store->count;
    store->order[store->count] = ~0u;
    ++store->deleted;
}

bool message_store_delete(MessageStore* store, u64 id) noexcept {
    if (id == 0)
        return false;
    bool found = false;
    const u32 order_index = find_order_index(*store, id, &found);
    if (found)
        message_store_delete_at(store, order_index);
    return found;
}

bool message_store_set_delivery(MessageStore* store, u64 id, MessageDeliveryState delivery) noexcept {
    if (id == 0)
        return false;
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

bool message_store_set_presentation_flags(MessageStore* store, MessageHandle handle, u32 flags) noexcept {
    if ((flags & ~kMessagePresentationKnownFlags) != 0 || !message_store_resolve(*store, handle)) {
        return false;
    }
    MessageRecord& record = store->records[handle.slot];
    if (record.presentation_flags != flags) {
        record.presentation_flags = static_cast<u8>(flags);
        ++store->updated;
    }
    return true;
}

bool message_store_set_presentation_flags(MessageStore* store, u64 id, u32 flags) noexcept {
    return message_store_set_presentation_flags(store, message_store_find(*store, id).handle, flags);
}

const MessageRecord* message_store_resolve(const MessageStore& store, MessageHandle handle) noexcept {
    // An uninitialized store has no capacity.
    if (handle.generation == 0 || handle.slot >= store.capacity)
        return nullptr;
    const MessageRecord& record = store.records[handle.slot];
    return record.occupied && record.generation == handle.generation ? &record : nullptr;
}

MessageHandle message_store_ordered_handle(const MessageStore& store, u32 order_index) noexcept {
    if (order_index >= store.count)
        return MessageHandle{~0u, 0};
    const u32 slot = store.order[order_index];
    return MessageHandle{slot, store.records[slot].generation};
}

const MessageRecord* message_store_ordered_record(const MessageStore& store, u32 order_index) noexcept {
    // The order holds the occupied slots alone, so each of its first `count` entries names a live record.
    return order_index < store.count ? &store.records[store.order[order_index]] : nullptr;
}

Utf8View message_store_content(const MessageStore& store, MessageHandle handle) noexcept {
    const MessageRecord* record = message_store_resolve(store, handle);
    return record ? Utf8View{reinterpret_cast<const char*>(store.text + record->content_offset),
                             record->content_bytes}
                  : Utf8View{};
}

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
void message_store_compact_in_place(MessageStore* store) noexcept {
    // Appends keep text in message order; text that grows on an edit moves to the end. In message order each copy
    // moves text down over text already moved; otherwise the copies go in source order.
    bool ordered = true;
    u32 previous_end = 0;
    for (u32 index = 0; ordered && index < store->count; ++index) {
        const MessageRecord& record = store->records[store->order[index]];
        ordered = !record.content_bytes || record.content_offset >= previous_end;
        if (record.content_bytes)
            previous_end = record.content_offset + record.content_bytes;
    }
    u32 write_offset = 0;
    if (ordered) {
        for (u32 index = 0; index < store->count; ++index) {
            MessageRecord& record = store->records[store->order[index]];
            copy_bytes(store->text + write_offset, store->text + record.content_offset, record.content_bytes);
            record.content_offset = write_offset;
            write_offset += record.content_bytes;
        }
    } else {
        // Records never share text, so the nearest source at or after the end of the last one moved comes next.
        for (u32 source = 0;;) {
            MessageRecord* next = nullptr;
            for (u32 index = 0; index < store->count; ++index) {
                MessageRecord& record = store->records[store->order[index]];
                if (record.content_bytes && record.content_offset >= source &&
                    (!next || record.content_offset < next->content_offset))
                    next = &record;
            }
            if (!next)
                break;
            source = next->content_offset + next->content_bytes;
            copy_bytes(store->text + write_offset, store->text + next->content_offset, next->content_bytes);
            next->content_offset = write_offset;
            write_offset += next->content_bytes;
        }
        for (u32 index = 0; index < store->count; ++index) {
            MessageRecord& record = store->records[store->order[index]];
            if (!record.content_bytes)
                record.content_offset = write_offset;
        }
    }
    store->text_used = write_offset;
    store->text_wasted = 0;
    ++store->compactions;
}
#pragma clang attribute pop

u64 snowflake_timestamp_ms(u64 snowflake) noexcept {
    return (snowflake >> 22) + kSnowflakeEpochMilliseconds;
}

} // namespace maicord
