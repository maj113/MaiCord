#include "maicord/app/message_snapshot_cache.hpp"
#include "maicord/app/gateway_client.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {

struct MessageSnapshotScratch {
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
    MessageRecord records[1];
    u32 order[1];
    u32 item_units[1];
    u64 fenwick[1];
    u16 rich_order[2];
    MessageRichRecord rich_records[2];
    u8 text[kMaximumMessageContentBytes];
    u8 rich_text[2 * kMessageSnapshotRichBankBytes];
};

namespace {

inline constexpr u32 kScratchItemUnits = 64;

u32 align8(u32 value) noexcept {
    return (value + 7u) & ~7u;
}

MessageSnapshotRow* row_at(MessageSnapshotCache* cache, u32 offset) noexcept {
    return reinterpret_cast<MessageSnapshotRow*>(cache->pool.base + offset);
}

const MessageSnapshotRow* row_at(const MessageSnapshotCache& cache, u32 offset) noexcept {
    return reinterpret_cast<const MessageSnapshotRow*>(cache.pool.base + offset);
}

Utf8View row_content(const MessageSnapshotRow& row) noexcept {
    return {reinterpret_cast<const char*>(&row + 1), row.content_bytes};
}

ConstSlice<u8> row_rich(const MessageSnapshotRow& row) noexcept {
    return {reinterpret_cast<const u8*>(&row + 1) + row.content_bytes, row.rich_bytes};
}

MessageSnapshotEntry* find_entry(MessageSnapshotCache* cache, u64 channel_id) noexcept {
    for (auto& entry : cache->entries)
        if (entry.occupied && entry.channel_id == channel_id)
            return &entry;
    return nullptr;
}

bool pool_reserve(MessageSnapshotCache* cache, u32 used) noexcept {
    if (used > cache->budget)
        return false;
    if (!cache->pool.base && !arena_initialize(&cache->pool, cache->budget, 0, 64 * 1024))
        return false;
    if (used > cache->pool.offset && !arena_allocate(&cache->pool, used - cache->pool.offset, 1))
        return false;
    return true;
}

void release_memory(MessageSnapshotCache* cache) noexcept {
    arena_release(&cache->pool);
    arena_release(&cache->scratch_arena);
    cache->scratch = nullptr;
    cache->used = 0;
}

// Moves every byte after the entry so its region becomes new_bytes long.
bool resize_region(MessageSnapshotCache* cache, MessageSnapshotEntry* entry, u32 new_bytes) noexcept {
    const u32 end = entry->offset + entry->bytes;
    const u32 tail = cache->used - end;
    if (new_bytes > entry->bytes) {
        const u32 grow = new_bytes - entry->bytes;
        if (!pool_reserve(cache, cache->used + grow))
            return false;
        copy_bytes(cache->pool.base + end + grow, cache->pool.base + end, tail);
        cache->used += grow;
    } else {
        const u32 shrink = entry->bytes - new_bytes;
        copy_bytes(cache->pool.base + end - shrink, cache->pool.base + end, tail);
        cache->used -= shrink;
    }
    for (auto& other : cache->entries)
        if (other.occupied && &other != entry && other.offset >= end)
            other.offset = other.offset + new_bytes - entry->bytes;
    entry->bytes = new_bytes;
    return true;
}

void remove_entry(MessageSnapshotCache* cache, MessageSnapshotEntry* entry) noexcept {
    resize_region(cache, entry, 0);
    *entry = MessageSnapshotEntry{};
    if (message_snapshot_cache_count(*cache) == 0)
        release_memory(cache);
}

MessageSnapshotEntry* victim(MessageSnapshotCache* cache, const MessageSnapshotEntry* protect) noexcept {
    MessageSnapshotEntry* selected = nullptr;
    for (auto& entry : cache->entries) {
        if (!entry.occupied || &entry == protect)
            continue;
        if (!selected || (entry.speculative && !selected->speculative) ||
            (entry.speculative == selected->speculative && entry.used_tick < selected->used_tick))
            selected = &entry;
    }
    return selected;
}

bool make_room(MessageSnapshotCache* cache, u32 bytes, const MessageSnapshotEntry* protect,
               bool need_entry) noexcept {
    for (;;) {
        bool free_entry = !need_entry;
        for (const auto& entry : cache->entries)
            free_entry = free_entry || !entry.occupied;
        if (free_entry && bytes <= cache->budget - cache->used)
            return true;
        MessageSnapshotEntry* evicted = victim(cache, protect);
        if (!evicted)
            return false;
        remove_entry(cache, evicted);
        ++cache->evictions;
    }
}

MessageSnapshotEntry* free_entry(MessageSnapshotCache* cache) noexcept {
    for (auto& entry : cache->entries)
        if (!entry.occupied)
            return &entry;
    return nullptr;
}

u32 row_bytes(u32 content_bytes, u32 rich_bytes) noexcept {
    return align8(static_cast<u32>(sizeof(MessageSnapshotRow)) + content_bytes + rich_bytes);
}

bool write_row(u8* destination, u32 bytes, const MessageRecord& record, Utf8View content,
               const MessageRichStore* rich, u32 rich_bytes, u32 item_units) noexcept {
    auto* row = reinterpret_cast<MessageSnapshotRow*>(destination);
    *row = MessageSnapshotRow{record.id, record.author_id, record.timestamp_ms, record.edited_timestamp_ms,
                              bytes, record.flags, item_units, content.bytes, rich_bytes, record.type,
                              record.content_availability, record.presentation_flags};
    copy_bytes(row + 1, content.data, content.bytes);
    u8* packed = reinterpret_cast<u8*>(row + 1) + content.bytes;
    u32 written = 0;
    if (rich_bytes && (!rich || !message_rich_store_pack(*rich, record.id, {packed, rich_bytes}, &written) ||
                       written != rich_bytes))
        return false;
    clear_bytes(packed + rich_bytes, bytes - static_cast<u32>(sizeof(MessageSnapshotRow)) - content.bytes - rich_bytes);
    return true;
}

bool record_rich_bytes(const MessageRichStore* rich, const MessageRecord& record, u32* bytes) noexcept {
    *bytes = 0;
    return !rich || message_rich_store_pack(*rich, record.id, {}, bytes);
}

MessageInput row_input(const MessageSnapshotRow& row, u64 channel_id) noexcept {
    MessageInput input{row.id, channel_id, row.author_id, row.timestamp_ms, row.edited_timestamp_ms,
                       row_content(row), row.flags};
    input.content_availability = row.content_availability;
    input.delivery = MessageDeliveryState::authoritative;
    input.presentation_flags = row.presentation_flags;
    input.type = row.type;
    return input;
}

MessageSnapshotScratch* scratch(MessageSnapshotCache* cache) noexcept {
    if (cache->scratch)
        return cache->scratch;
    if (!arena_initialize(&cache->scratch_arena, sizeof(MessageSnapshotScratch), 0, 64 * 1024))
        return nullptr;
    auto* value = arena_allocate<MessageSnapshotScratch>(&cache->scratch_arena);
    if (!value || !message_store_initialize(&value->store, {value->records, 1}, {value->order, 1},
                                            {value->text, kMaximumMessageContentBytes}) ||
        !ui_variable_list_initialize(&value->list, {value->item_units, 1}, {value->fenwick, 1}, 0,
                                     kScratchItemUnits) ||
        !message_timeline_initialize(&value->timeline, &value->store, &value->list, kScratchItemUnits,
                                     kScratchItemUnits, false) ||
        !message_rich_store_initialize(&value->rich, {value->rich_records, 2}, {value->rich_order, 2},
                                       {value->rich_text, 2 * kMessageSnapshotRichBankBytes}) ||
        !message_timeline_bind_rich_store(&value->timeline, &value->rich)) {
        arena_release(&cache->scratch_arena);
        return nullptr;
    }
    cache->scratch = value;
    return value;
}

void scratch_clear(MessageSnapshotScratch* value) noexcept {
    if (value->store.count) {
        const MessageRecord* record = message_store_resolve(value->store, message_store_ordered_handle(value->store, 0));
        if (record) {
            const u64 id = record->id;
            message_rich_store_delete(&value->rich, id);
            message_store_delete(&value->store, id);
        }
    }
    if (value->list.count)
        ui_variable_list_remove(&value->list, 0, 1);
}

bool scratch_load(MessageSnapshotScratch* value, const MessageSnapshotRow& row, u64 channel_id) noexcept {
    scratch_clear(value);
    const MessageStoreResult inserted = message_store_upsert(&value->store, row_input(row, channel_id));
    const MessageRecord* record = inserted.ok() ? message_store_resolve(value->store, inserted.handle) : nullptr;
    if (!record || !ui_variable_list_insert(&value->list, 0, 1, kScratchItemUnits) ||
        (row.rich_bytes && !message_rich_store_unpack(&value->rich, row.id, record->revision, row_rich(row)))) {
        scratch_clear(value);
        return false;
    }
    return true;
}

// Locates the row for message_id; *offset is the pool offset of that row.
bool find_row(const MessageSnapshotCache& cache, const MessageSnapshotEntry& entry, u64 message_id,
              u32* offset) noexcept {
    u32 cursor = entry.offset;
    for (u32 index = 0; index < entry.count; ++index) {
        const MessageSnapshotRow* row = row_at(cache, cursor);
        if (row->id == message_id) {
            *offset = cursor;
            return true;
        }
        if (row->id > message_id)
            return false;
        cursor += row->bytes;
    }
    return false;
}

// Replaces one row's bytes inside the entry region.
bool splice_row(MessageSnapshotCache* cache, MessageSnapshotEntry* entry, u32 offset, u32 new_bytes) noexcept {
    const u32 old_bytes = row_at(cache, offset)->bytes;
    const u32 relative = offset - entry->offset;
    const u32 after = entry->bytes - relative - old_bytes;
    if (new_bytes > old_bytes) {
        const u32 grow = new_bytes - old_bytes;
        if (!make_room(cache, grow, entry, false) || !resize_region(cache, entry, entry->bytes + grow))
            return false;
        u8* row = cache->pool.base + entry->offset + relative;
        copy_bytes(row + new_bytes, row + old_bytes, after);
    } else {
        u8* row = cache->pool.base + entry->offset + relative;
        copy_bytes(row + new_bytes, row + old_bytes, after);
        resize_region(cache, entry, entry->bytes - (old_bytes - new_bytes));
    }
    return true;
}

void drop_stale(MessageSnapshotCache* cache, MessageSnapshotEntry* entry) noexcept {
    remove_entry(cache, entry);
    ++cache->event_drops;
}

// Writes the single scratch row back over the entry row at offset.
bool scratch_store_row(MessageSnapshotCache* cache, MessageSnapshotEntry* entry, u32 offset,
                       MessageSnapshotScratch* value) noexcept {
    const MessageRecord* record =
        value->store.count ? message_store_resolve(value->store, message_store_ordered_handle(value->store, 0))
                           : nullptr;
    u32 rich_bytes = 0;
    if (!record || !record_rich_bytes(&value->rich, *record, &rich_bytes))
        return false;
    const Utf8View content = message_store_content(value->store, message_store_ordered_handle(value->store, 0));
    const u32 item_units = row_at(cache, offset)->item_units;
    const u32 bytes = row_bytes(content.bytes, rich_bytes);
    const u32 relative = offset - entry->offset;
    if (!splice_row(cache, entry, offset, bytes))
        return false;
    return write_row(cache->pool.base + entry->offset + relative, bytes, *record, content, &value->rich,
                     rich_bytes, item_units);
}

bool delete_row(MessageSnapshotCache* cache, MessageSnapshotEntry* entry, u64 message_id) noexcept {
    u32 offset = 0;
    if (!find_row(*cache, *entry, message_id, &offset))
        return false;
    splice_row(cache, entry, offset, 0);
    --entry->count;
    if (entry->anchor_id == message_id)
        entry->anchor_units = 0;
    return true;
}

const MessageRecord* ordered_record(const MessageStore& store, u32 index) noexcept {
    return message_store_resolve(store, message_store_ordered_handle(store, index));
}

} // namespace

void message_snapshot_cache_initialize(MessageSnapshotCache* cache, u32 budget_bytes) noexcept {
    if (!cache)
        return;
    *cache = MessageSnapshotCache{};
    cache->budget = budget_bytes;
}

void message_snapshot_cache_release(MessageSnapshotCache* cache) noexcept {
    if (!cache)
        return;
    for (auto& entry : cache->entries)
        entry = MessageSnapshotEntry{};
    release_memory(cache);
}

void message_snapshot_cache_drop(MessageSnapshotCache* cache, u64 channel_id) noexcept {
    if (MessageSnapshotEntry* entry = cache && channel_id ? find_entry(cache, channel_id) : nullptr)
        remove_entry(cache, entry);
}

void message_snapshot_cache_prune(MessageSnapshotCache* cache, bool (*keep)(void*, u64), void* context) noexcept {
    if (!cache || !keep)
        return;
    for (auto& entry : cache->entries)
        if (entry.occupied && !keep(context, entry.channel_id))
            remove_entry(cache, &entry);
}

const MessageSnapshotEntry* message_snapshot_cache_find(const MessageSnapshotCache& cache,
                                                        u64 channel_id) noexcept {
    for (const auto& entry : cache.entries)
        if (entry.occupied && entry.channel_id == channel_id && channel_id)
            return &entry;
    return nullptr;
}

u32 message_snapshot_cache_count(const MessageSnapshotCache& cache) noexcept {
    u32 count = 0;
    for (const auto& entry : cache.entries)
        count += entry.occupied;
    return count;
}

bool message_snapshot_cache_capture(MessageSnapshotCache* cache, const MessageTimeline& timeline,
                                    const MessageSnapshotCaptureInput& input) noexcept {
    if (!cache || !cache->budget || !input.channel_id || !timeline.store || !timeline.list ||
        timeline.store->count != timeline.list->count)
        return false;
    const MessageStore& store = *timeline.store;
    const MessageRichStore* rich = timeline.rich_store;
    u64 total = 0;
    u32 count = 0;
    u64 newest = 0;
    for (u32 index = 0; index < store.count; ++index) {
        const MessageRecord* record = ordered_record(store, index);
        if (!record || record->channel_id != input.channel_id)
            return false;
        if (record->delivery != MessageDeliveryState::authoritative)
            continue;
        u32 rich_bytes = 0;
        if (!record_rich_bytes(rich, *record, &rich_bytes))
            return false;
        total += row_bytes(record->content_bytes, rich_bytes);
        newest = record->id;
        ++count;
    }
    if (!count)
        return false;
    message_snapshot_cache_drop(cache, input.channel_id);
    if (total > cache->budget / 2 || !make_room(cache, static_cast<u32>(total), nullptr, true))
        return false;
    MessageSnapshotEntry* entry = free_entry(cache);
    const u32 offset = cache->used;
    if (!entry || !pool_reserve(cache, offset + static_cast<u32>(total))) {
        ++cache->failures;
        return false;
    }
    u32 cursor = offset;
    for (u32 index = 0; index < store.count; ++index) {
        const MessageHandle handle = message_store_ordered_handle(store, index);
        const MessageRecord* record = message_store_resolve(store, handle);
        if (record->delivery != MessageDeliveryState::authoritative)
            continue;
        u32 rich_bytes = 0;
        record_rich_bytes(rich, *record, &rich_bytes);
        const Utf8View content = message_store_content(store, handle);
        const u32 bytes = row_bytes(content.bytes, rich_bytes);
        if (!write_row(cache->pool.base + cursor, bytes, *record, content, rich, rich_bytes,
                       ui_variable_list_item_units(*timeline.list, index))) {
            ++cache->failures;
            return false;
        }
        cursor += bytes;
    }
    *entry = MessageSnapshotEntry{};
    entry->channel_id = input.channel_id;
    entry->newest_id = newest;
    entry->older_cursor = input.older_cursor;
    entry->used_tick = ++cache->tick;
    entry->offset = offset;
    entry->bytes = static_cast<u32>(total);
    entry->count = count;
    entry->occupied = true;
    entry->follow_bottom = timeline.follow_bottom;
    entry->live_edge = input.live_edge;
    entry->older_exhausted = input.older_exhausted;
    entry->speculative = input.speculative;
    cache->used = offset + static_cast<u32>(total);
    if (!timeline.follow_bottom) {
        UiScrollAnchor anchor{};
        if (ui_variable_list_capture_anchor(*timeline.list, timeline.scroll_units, &anchor) && anchor.valid) {
            for (u32 index = anchor.index; index < store.count; ++index) {
                const MessageRecord* record = ordered_record(store, index);
                if (record->delivery != MessageDeliveryState::authoritative)
                    continue;
                entry->anchor_id = record->id;
                entry->anchor_units = index == anchor.index ? anchor.within_item_units : 0;
                break;
            }
        }
        if (!entry->anchor_id)
            entry->follow_bottom = true;
    }
    ++cache->captures;
    return true;
}

bool message_snapshot_cache_capture_page(MessageSnapshotCache* cache, u64 channel_id,
                                         ConstSlice<GatewayMessageEvent> messages,
                                         const ChatStateStore* state,
                                         MessageContentAvailability empty_content_availability,
                                         u32 default_item_units, bool exhausted) noexcept {
    if (!cache || !cache->budget || !channel_id || !messages.count || !messages.data || !default_item_units)
        return false;
    MessageSnapshotScratch* value = scratch(cache);
    if (!value)
        return false;
    // Each message passes through the canonical history apply on one scratch
    // row: once to size the entry, once to write it.
    const auto stage = [&](const GatewayMessageEvent& message, u32* rich_bytes) noexcept {
        scratch_clear(value);
        if (message.channel_id != channel_id)
            return false;
        // A snapshot restore is not followed by a refetch of the same page, so
        // a message that history apply would degrade (left out, or rich data
        // dropped) must not be captured; the channel then loads from REST.
        const auto applied = message_timeline_apply_history_page(
            &value->timeline, {&message, 1}, MessageHistoryDirection::newer, empty_content_availability);
        return applied.ok() && !applied.skipped && !applied.rich_skipped &&
               message_history_apply_mention_flags(&value->timeline, state, {&message, 1}) &&
               value->store.count == 1 && record_rich_bytes(&value->rich, *ordered_record(value->store, 0), rich_bytes);
    };
    u64 total = 0;
    for (u32 index = 0; index < messages.count; ++index) {
        u32 rich_bytes = 0;
        if (!stage(messages[index], &rich_bytes)) {
            scratch_clear(value);
            ++cache->failures;
            return false;
        }
        total += row_bytes(message_store_content(value->store, message_store_ordered_handle(value->store, 0)).bytes,
                           rich_bytes);
    }
    scratch_clear(value);
    // Dropping or evicting the last entry releases the scratch with the pool.
    message_snapshot_cache_drop(cache, channel_id);
    if (total > cache->budget / 2 || !make_room(cache, static_cast<u32>(total), nullptr, true))
        return false;
    MessageSnapshotEntry* entry = free_entry(cache);
    const u32 offset = cache->used;
    if (!entry || !pool_reserve(cache, offset + static_cast<u32>(total)) || !(value = scratch(cache)))
        return false;
    u32 cursor = offset;
    for (u32 index = messages.count; index != 0; --index) {
        u32 rich_bytes = 0;
        const bool staged = stage(messages[index - 1], &rich_bytes);
        const MessageHandle handle = message_store_ordered_handle(value->store, 0);
        const Utf8View content = message_store_content(value->store, handle);
        const u32 bytes = row_bytes(content.bytes, rich_bytes);
        if (!staged || cursor + bytes > offset + total ||
            !write_row(cache->pool.base + cursor, bytes, *message_store_resolve(value->store, handle), content,
                       &value->rich, rich_bytes, default_item_units)) {
            scratch_clear(value);
            ++cache->failures;
            return false;
        }
        cursor += bytes;
    }
    scratch_clear(value);
    *entry = MessageSnapshotEntry{};
    entry->channel_id = channel_id;
    entry->newest_id = messages[0].id;
    entry->older_cursor = exhausted ? messages[messages.count - 1].id : 0;
    entry->used_tick = ++cache->tick;
    entry->offset = offset;
    entry->bytes = static_cast<u32>(total);
    entry->count = messages.count;
    entry->occupied = true;
    entry->follow_bottom = true;
    entry->live_edge = true;
    entry->older_exhausted = exhausted;
    entry->speculative = true;
    cache->used = offset + static_cast<u32>(total);
    ++cache->captures;
    return true;
}

MessageSnapshotRestoreResult message_snapshot_cache_restore(MessageSnapshotCache* cache, u64 channel_id,
                                                            u64 channel_last_message_id, u32 gap_limit,
                                                            MessageTimeline* timeline) noexcept {
    MessageSnapshotRestoreResult result{};
    result.status = MessageSnapshotRestoreStatus::missing;
    MessageSnapshotEntry* entry = cache && channel_id ? find_entry(cache, channel_id) : nullptr;
    if (!entry)
        return result;
    const bool missed_tail = entry->live_edge && !entry->creates && channel_last_message_id > entry->newest_id;
    if (entry->creates >= gap_limit || missed_tail) {
        remove_entry(cache, entry);
        ++cache->gaps;
        result.status = MessageSnapshotRestoreStatus::gap;
        return result;
    }
    result.status = MessageSnapshotRestoreStatus::failed;
    if (!timeline || !timeline->store || !timeline->list || timeline->store->count || timeline->list->count ||
        entry->count > timeline->store->capacity || entry->count > timeline->list->capacity) {
        if (timeline && timeline->store && timeline->list)
            message_timeline_reset(timeline);
        remove_entry(cache, entry);
        ++cache->failures;
        return result;
    }
    MessageStore& store = *timeline->store;
    UiVariableList& list = *timeline->list;
    bool ok = true;
    u32 cursor = entry->offset;
    u32 slot_hint = 0;
    for (u32 index = 0; ok && index < entry->count; ++index) {
        const MessageSnapshotRow& row = *row_at(*cache, cursor);
        const MessageStoreResult inserted = message_store_upsert(&store, row_input(row, channel_id));
        const MessageRecord* record = inserted.ok() ? message_store_resolve(store, inserted.handle) : nullptr;
        ok = record && inserted.inserted &&
             (!row.rich_bytes ||
              (timeline->rich_store &&
               message_rich_store_unpack(timeline->rich_store, row.id, record->revision, row_rich(row), &slot_hint)));
        cursor += row.bytes;
    }
    ok = ok && store.count == entry->count &&
         ui_variable_list_initialize(&list, {list.item_units, list.capacity}, {list.fenwick, list.capacity},
                                     store.count, timeline->default_item_units);
    cursor = entry->offset;
    for (u32 index = 0; ok && index < entry->count; ++index) {
        const MessageSnapshotRow& row = *row_at(*cache, cursor);
        ok = !row.item_units || ui_variable_list_set_height(&list, index, row.item_units);
        cursor += row.bytes;
    }
    if (ok) {
        if (entry->follow_bottom) {
            ok = message_timeline_set_scroll(timeline, 0, true);
        } else {
            const MessageStoreResult anchor = message_store_find(store, entry->anchor_id);
            const u32 index = minimum(anchor.order_index, store.count - 1);
            const u64 within = anchor.ok() ? minimum(entry->anchor_units, ui_variable_list_item_units(list, index)) : 0;
            ok = message_timeline_set_scroll(timeline, ui_variable_list_prefix_units(list, index) + within, false);
        }
    }
    if (!ok) {
        message_timeline_reset(timeline);
        remove_entry(cache, entry);
        ++cache->failures;
        return result;
    }
    result.status = MessageSnapshotRestoreStatus::restored;
    result.count = entry->count;
    result.older_cursor = entry->older_cursor;
    result.older_exhausted = entry->older_exhausted;
    result.live_edge = entry->live_edge;
    // The live timeline now owns this channel; a later capture replaces it.
    remove_entry(cache, entry);
    ++cache->restores;
    return result;
}

void message_snapshot_cache_observe(MessageSnapshotCache* cache, const GatewayClientEvent& event,
                                    ConstSlice<u8> payload, Slice<u64> bulk_ids, u64 self_user_id,
                                    MessageContentAvailability content_availability) noexcept {
    if (!cache || !message_snapshot_cache_count(*cache))
        return;
    switch (event.dispatch_kind) {
    case GatewayClientDispatchKind::message_create:
        if (MessageSnapshotEntry* entry = find_entry(cache, event.message.channel_id))
            saturating_increment(entry->creates);
        return;
    case GatewayClientDispatchKind::message_delete: {
        GatewayMessageDeleteEvent message{};
        MessageSnapshotEntry* entry = gateway_client_event_as_message_delete(event, payload, &message)
                                          ? find_entry(cache, message.channel_id)
                                          : nullptr;
        if (entry && delete_row(cache, entry, message.id) && !entry->count)
            remove_entry(cache, entry);
        return;
    }
    case GatewayClientDispatchKind::message_bulk_delete: {
        GatewayMessageBulkDeleteEvent message{};
        MessageSnapshotEntry* entry = gateway_client_event_as_message_bulk_delete(event, payload, bulk_ids, &message)
                                          ? find_entry(cache, message.channel_id)
                                          : nullptr;
        for (u32 index = 0; entry && index < message.id_count; ++index)
            delete_row(cache, entry, bulk_ids[index]);
        if (entry && !entry->count)
            remove_entry(cache, entry);
        return;
    }
    case GatewayClientDispatchKind::message_update: {
        MessageSnapshotEntry* entry = find_entry(cache, event.message.channel_id);
        u32 offset = 0;
        if (!entry || !find_row(*cache, *entry, event.message.id, &offset))
            return;
        MessageSnapshotScratch* value = scratch(cache);
        const bool applied =
            value && scratch_load(value, *row_at(*cache, offset), entry->channel_id) &&
            message_timeline_apply(&value->timeline, event, payload, bulk_ids, content_availability).ok() &&
            value->store.count == 1 && scratch_store_row(cache, entry, offset, value);
        if (value)
            scratch_clear(value);
        if (applied)
            ++cache->event_updates;
        else if ((entry = find_entry(cache, event.message.channel_id)) != nullptr)
            drop_stale(cache, entry);
        return;
    }
    case GatewayClientDispatchKind::message_reaction_add:
    case GatewayClientDispatchKind::message_reaction_remove:
    case GatewayClientDispatchKind::message_reaction_remove_all:
    case GatewayClientDispatchKind::message_reaction_remove_emoji: {
        GatewayMessageReactionEvent reaction{};
        if (!gateway_client_event_as_message_reaction(event, payload, &reaction))
            return;
        MessageSnapshotEntry* entry = find_entry(cache, reaction.channel_id);
        u32 offset = 0;
        if (!entry || !find_row(*cache, *entry, reaction.message_id, &offset))
            return;
        const MessageRichReactionMutationKind kind =
            reaction.kind == GatewayMessageReactionEventKind::add      ? MessageRichReactionMutationKind::add
            : reaction.kind == GatewayMessageReactionEventKind::remove ? MessageRichReactionMutationKind::remove
            : reaction.kind == GatewayMessageReactionEventKind::remove_all
                ? MessageRichReactionMutationKind::remove_all
                : MessageRichReactionMutationKind::remove_emoji;
        const MessageRichReactionMutation mutation{reaction.emoji_id, reaction.name, reaction.type, kind,
                                                   reaction.user_id != 0 && reaction.user_id == self_user_id,
                                                   reaction.animated};
        MessageSnapshotScratch* value = scratch(cache);
        bool applied = value && scratch_load(value, *row_at(*cache, offset), entry->channel_id);
        if (applied) {
            const MessageRecord* record = ordered_record(value->store, 0);
            applied = record &&
                      message_rich_store_mutate_reaction(&value->rich, reaction.message_id, record->revision, mutation)
                          .ok() &&
                      scratch_store_row(cache, entry, offset, value);
        }
        if (value)
            scratch_clear(value);
        if (applied)
            ++cache->event_updates;
        else if ((entry = find_entry(cache, reaction.channel_id)) != nullptr)
            drop_stale(cache, entry);
        return;
    }
    default:
        return;
    }
}

void channel_prefetch_hover(ChannelPrefetch* prefetch, u64 channel_id, u64 now_ms) noexcept {
    if (!prefetch || prefetch->hover_channel_id == channel_id)
        return;
    if (prefetch->hover_channel_id && !prefetch->fired)
        ++prefetch->cancelled;
    if (prefetch->hover_channel_id && prefetch->hover_channel_id == prefetch->inflight_channel_id)
        prefetch->left_at_ms = now_ms;
    prefetch->hover_channel_id = channel_id;
    prefetch->hover_since_ms = now_ms;
    prefetch->fired = false;
    if (!channel_id)
        return;
    if (channel_id == prefetch->inflight_channel_id) {
        prefetch->fired = true;
        prefetch->left_at_ms = 0;
        return;
    }
    for (u32 index = 0; index < kChannelPrefetchRecent; ++index)
        if (prefetch->recent_channel_ids[index] == channel_id &&
            now_ms - prefetch->recent_at_ms[index] < kChannelPrefetchRepeatMs) {
            prefetch->fired = true;
            ++prefetch->deduplicated;
            return;
        }
}

u64 channel_prefetch_due(const ChannelPrefetch& prefetch, u64 now_ms) noexcept {
    const u64 wake = channel_prefetch_next_wake(prefetch);
    return wake && now_ms >= wake ? prefetch.hover_channel_id : 0;
}

u64 channel_prefetch_next_wake(const ChannelPrefetch& prefetch) noexcept {
    if (!prefetch.hover_channel_id || prefetch.fired || prefetch.inflight_request_id)
        return 0;
    const u64 dwell = prefetch.hover_since_ms + kChannelPrefetchDwellMs;
    const u64 interval = prefetch.last_submit_ms ? prefetch.last_submit_ms + kChannelPrefetchIntervalMs : 0;
    return maximum(dwell, interval);
}

void channel_prefetch_skip(ChannelPrefetch* prefetch) noexcept {
    if (prefetch)
        prefetch->fired = true;
}

void channel_prefetch_submitted(ChannelPrefetch* prefetch, u64 channel_id, u64 request_id, u64 now_ms) noexcept {
    if (!prefetch || !channel_id || !request_id)
        return;
    prefetch->inflight_channel_id = channel_id;
    prefetch->inflight_request_id = request_id;
    prefetch->last_submit_ms = now_ms;
    prefetch->left_at_ms = 0;
    if (prefetch->hover_channel_id == channel_id)
        prefetch->fired = true;
    u32 slot = 0;
    for (u32 index = 1; index < kChannelPrefetchRecent; ++index)
        if (prefetch->recent_at_ms[index] < prefetch->recent_at_ms[slot])
            slot = index;
    prefetch->recent_channel_ids[slot] = channel_id;
    prefetch->recent_at_ms[slot] = now_ms;
    ++prefetch->submitted;
}

bool channel_prefetch_adopt(ChannelPrefetch* prefetch, u64 request_id, u64 channel_id) noexcept {
    if (!prefetch || !request_id || request_id != prefetch->inflight_request_id ||
        channel_id != prefetch->inflight_channel_id)
        return false;
    prefetch->inflight_channel_id = prefetch->inflight_request_id = 0;
    prefetch->left_at_ms = 0;
    ++prefetch->adopted;
    return true;
}

bool channel_prefetch_complete(ChannelPrefetch* prefetch, u64 request_id, u64 now_ms) noexcept {
    if (!prefetch || !request_id || request_id != prefetch->inflight_request_id)
        return false;
    const bool wanted = prefetch->hover_channel_id == prefetch->inflight_channel_id ||
                        (prefetch->left_at_ms && now_ms - prefetch->left_at_ms <= kChannelPrefetchKeepMs);
    prefetch->inflight_channel_id = prefetch->inflight_request_id = 0;
    prefetch->left_at_ms = 0;
    if (!wanted)
        ++prefetch->discarded;
    return wanted;
}

} // namespace maicord
