#include "maicord/app/plugins/keep.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/utf.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

constexpr u32 kNone = ~0u;

struct Block {
    u32 offset, bytes;
};

struct Record {
    u64 channel_id, message_id, author_id, timestamp_ms, edited_ms;
    u64 stamp; // insertion order; the smallest is evicted first
    Block content, name;
    Block edits[kKeepEditsMax]; // edits[0] is the most recent earlier text
    u8 edit_count;
    bool deleted, occupied;
};

struct Store {
    Record records[kKeepMessagesMax];
    u32 count;
    u32 used, wasted;
    u64 stamp;
    u32 evicted;
    u8* pool;
};

Store* g_store;

bool ensure() noexcept {
    if (g_store) return true;
    const u32 bytes = (static_cast<u32>(sizeof(Store)) + 4095u) & ~4095u;
    auto* store = static_cast<Store*>(commit_pages(bytes));
    if (!store) return false;
    store->pool = static_cast<u8*>(commit_pages(kKeepPoolBytes));
    if (!store->pool) {
        pages_release(store);
        return false;
    }
    g_store = store;
    return true;
}

Record* find(u64 message_id) noexcept {
    if (!g_store) return nullptr;
    for (auto& record : g_store->records)
        if (record.occupied && record.message_id == message_id) return &record;
    return nullptr;
}
Record* find(u64 channel_id, u64 message_id) noexcept {
    Record* record = find(message_id);
    return record && record->channel_id == channel_id ? record : nullptr;
}

Utf8View view(const Block& block) noexcept {
    return {reinterpret_cast<const char*>(g_store->pool + block.offset), block.bytes};
}

u32 record_bytes(const Record& record) noexcept {
    u32 bytes = record.content.bytes + record.name.bytes;
    for (u32 i = 0; i < record.edit_count; ++i) bytes += record.edits[i].bytes;
    return bytes;
}

void release(Record* record) noexcept {
    g_store->wasted += record_bytes(*record);
    reset_value(record);
    --g_store->count;
}

Record* oldest(u64 channel_id, const Record* protect) noexcept {
    Record* best = nullptr;
    for (auto& record : g_store->records)
        if (record.occupied && &record != protect && (!channel_id || record.channel_id == channel_id) &&
            (!best || record.stamp < best->stamp))
            best = &record;
    return best;
}

void evict(Record* record) noexcept {
    ++g_store->evicted;
    release(record);
}

// Moves every live block to the front of a fresh pool.
bool compact() noexcept {
    auto* fresh = static_cast<u8*>(commit_pages(kKeepPoolBytes));
    if (!fresh) return false;
    u32 used = 0;
    const auto move = [&](Block* block) noexcept {
        copy_bytes(fresh + used, g_store->pool + block->offset, block->bytes);
        block->offset = used;
        used += block->bytes;
    };
    for (auto& record : g_store->records) {
        if (!record.occupied) continue;
        move(&record.content);
        move(&record.name);
        for (u32 i = 0; i < record.edit_count; ++i) move(&record.edits[i]);
    }
    pages_release(g_store->pool);
    g_store->pool = fresh;
    g_store->used = used;
    g_store->wasted = 0;
    return true;
}

// Room for bytes more; the oldest messages other than `protect` make way. Offset kNone when impossible.
u32 allocate(u32 bytes, const Record* protect) noexcept {
    if (bytes > kKeepPoolBytes) return kNone;
    for (;;) {
        if (g_store->used + bytes <= kKeepPoolBytes) {
            const u32 at = g_store->used;
            g_store->used += bytes;
            return at;
        }
        if (g_store->wasted && compact()) continue;
        Record* victim = oldest(0, protect);
        if (!victim) return kNone;
        evict(victim);
    }
}

bool store_text(Block* block, const Utf8View& text, const Record* protect) noexcept {
    const u32 at = allocate(text.bytes, protect);
    if (at == kNone) return false;
    copy_bytes(g_store->pool + at, text.data, text.bytes);
    *block = {at, text.bytes};
    return true;
}

bool valid(const KeepMessage& message) noexcept {
    return message.channel_id && message.message_id && message.content.bytes &&
           message.content.bytes <= kKeepContentMax && utf8_validate(message.content).valid &&
           utf8_validate(message.author_name).valid;
}

Record* claim(const KeepMessage& message) noexcept {
    Record* existing = find(message.message_id);
    if (existing) return existing;
    u32 in_channel = 0;
    for (const auto& record : g_store->records)
        in_channel += record.occupied && record.channel_id == message.channel_id;
    if (in_channel >= kKeepPerChannelMax) evict(oldest(message.channel_id, nullptr));
    if (g_store->count >= kKeepMessagesMax) evict(oldest(0, nullptr));
    for (auto& record : g_store->records) {
        if (record.occupied) continue;
        reset_value(&record);
        record.occupied = true;
        record.stamp = ++g_store->stamp;
        record.channel_id = message.channel_id;
        record.message_id = message.message_id;
        ++g_store->count;
        return &record;
    }
    return nullptr;
}

void fill(Record* record, const KeepMessage& message) noexcept {
    record->author_id = message.author_id;
    record->timestamp_ms = message.timestamp_ms;
    record->edited_ms = message.edited_ms;
}

bool store_name(Record* record, Utf8View name) noexcept {
    if (name.bytes > kKeepNameMax) name.bytes = utf8_truncated_length(name, kKeepNameMax);
    return store_text(&record->name, name, record);
}

} // namespace

bool keep_deleted(const KeepMessage& message) noexcept {
    if (!valid(message) || !ensure()) return false;
    Record* existing = find(message.message_id);
    if (existing && existing->deleted) return true;
    Record* record = claim(message);
    if (!record) return false;
    if (!existing) {
        fill(record, message);
        if (!store_text(&record->content, message.content, record) || !store_name(record, message.author_name)) {
            release(record);
            return false;
        }
    }
    record->deleted = true;
    return true;
}

bool keep_edited(const KeepMessage& before, const Utf8View& after, u64 edited_ms) noexcept {
    if (!valid(before) || !after.bytes || after.bytes > kKeepContentMax || !utf8_validate(after).valid || !ensure())
        return false;
    Record* existing = find(before.message_id);
    Record* record = claim(before);
    if (!record) return false;
    Block earlier{}, latest{};
    if (!store_text(&earlier, before.content, record) || !store_text(&latest, after, record) ||
        (!existing && !store_name(record, before.author_name))) {
        // Blocks stored so far are dead weight until the next compaction.
        g_store->wasted += earlier.bytes + latest.bytes;
        if (!existing) release(record);
        return false;
    }
    if (!existing) fill(record, before);
    g_store->wasted += record->content.bytes;
    if (record->edit_count == kKeepEditsMax) {
        g_store->wasted += record->edits[kKeepEditsMax - 1].bytes;
        --record->edit_count;
    }
    for (u32 i = record->edit_count; i; --i) record->edits[i] = record->edits[i - 1];
    record->edits[0] = earlier;
    ++record->edit_count;
    record->content = latest;
    record->edited_ms = edited_ms;
    return true;
}

bool keep_is_deleted(u64 message_id) noexcept {
    const Record* record = find(message_id);
    return record && record->deleted;
}

bool keep_info(u64 channel_id, u64 message_id, KeepInfo* info) noexcept {
    const Record* record = find(channel_id, message_id);
    if (!record || !info) return false;
    *info = {record->author_id, record->timestamp_ms, record->edited_ms, record->edit_count, record->deleted};
    return true;
}

bool keep_text(u64 channel_id, u64 message_id, u32 version, Utf8View* text) noexcept {
    const Record* record = find(channel_id, message_id);
    if (!record || version > record->edit_count || !text) return false;
    *text = view(version ? record->edits[version - 1] : record->content);
    return true;
}

u32 keep_tombstones(u64 channel_id, KeepMessage* output, u32 capacity) noexcept {
    if (!g_store) return 0;
    u32 total = 0;
    for (const auto& record : g_store->records) {
        if (!record.occupied || !record.deleted || record.channel_id != channel_id) continue;
        const KeepMessage message{record.channel_id, record.message_id, record.author_id, record.timestamp_ms,
                                  record.edited_ms,  view(record.content), view(record.name)};
        // Insertion sort by id into the first `capacity` slots.
        u32 at = total < capacity ? total : capacity;
        while (at && output[at - 1].message_id > message.message_id) --at;
        if (at < capacity) {
            const u32 last = total < capacity ? total : capacity - 1;
            for (u32 i = last; i > at; --i) output[i] = output[i - 1];
            output[at] = message;
        }
        ++total;
    }
    return total;
}

bool keep_add_edit(u64 channel_id, u64 message_id, Utf8View text) noexcept {
    Record* record = find(channel_id, message_id);
    if (!record || record->edit_count == kKeepEditsMax || !text.bytes || text.bytes > kKeepContentMax ||
        !utf8_validate(text).valid)
        return false;
    Block block{};
    if (!store_text(&block, text, record)) return false;
    record->edits[record->edit_count++] = block;
    return true;
}

void keep_clear() noexcept {
    if (!g_store) return;
    pages_release(g_store->pool);
    pages_release(g_store);
    g_store = nullptr;
}

KeepStats keep_stats() noexcept {
    return g_store ? KeepStats{g_store->count, g_store->evicted} : KeepStats{};
}

} // namespace maicord
