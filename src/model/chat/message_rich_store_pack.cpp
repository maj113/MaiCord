#include "message_rich_store_internal.hpp"

namespace maicord {
using namespace message_rich_store_detail;

const MessageRichRecord* message_rich_store_find(const MessageRichStore& store, u64 message_id) noexcept {
    if (!store.records || !store.order || message_id == 0) {
        return nullptr;
    }
    const SearchResult search = find_record(store, message_id);
    return search.found ? &store.records[search.slot] : nullptr;
}

// The record a dimension update edits (the message itself or its forwarded snapshot) when
// it is resident at message_revision.
static MessageRichRecord* dimension_target(MessageRichStore* store, u64 message_id, u32 message_revision,
                                           bool snapshot) noexcept {
    if (!store || !store->records || !store->order || !message_id || !message_revision)
        return nullptr;
    const SearchResult search = find_record(*store, message_id);
    if (!search.found || (snapshot && !message_rich_store_snapshot(*store, message_id)))
        return nullptr;
    MessageRichRecord& record =
        store->records[snapshot ? store->records[search.slot].forwarded_snapshot_slot : search.slot];
    return record.occupied && record.message_revision == message_revision ? &record : nullptr;
}

bool message_rich_store_set_attachment_dimensions(MessageRichStore* store, u64 message_id,
                                                  u32 message_revision, u64 attachment_id, u32 width,
                                                  u32 height, bool* changed, bool snapshot) noexcept {
    if (!changed)
        return false;
    *changed = false;
    MessageRichRecord* target = attachment_id && width && height
                                    ? dimension_target(store, message_id, message_revision, snapshot)
                                    : nullptr;
    if (!target)
        return false;
    MessageRichRecord& record = *target;
    for (u32 index = 0; index < record.attachment_count; ++index) {
        MessageRichAttachment& attachment = record.attachments[index];
        if (attachment.id == attachment_id) {
            if (attachment.width != width || attachment.height != height || (attachment.state & 3u) != 3u) {
                attachment.width = width;
                attachment.height = height;
                attachment.state = static_cast<u8>(attachment.state | 3u);
                *changed = true;
            }
            return true;
        }
    }
    return false;
}

bool message_rich_store_set_embed_dimensions(MessageRichStore* store, u64 message_id, u32 message_revision,
                                             u32 embed_index, MessageRichEmbedMediaSlot slot, u32 width,
                                             u32 height, bool* changed, bool snapshot) noexcept {
    if (!changed)
        return false;
    *changed = false;
    MessageRichRecord* target =
        width && height ? dimension_target(store, message_id, message_revision, snapshot) : nullptr;
    if (!target || embed_index >= target->embed_count)
        return false;
    MessageRichRecord& record = *target;
    auto& embed = record.embeds[embed_index];
    MessageRichEmbedMedia* media = slot == MessageRichEmbedMediaSlot::image       ? &embed.image
                                   : slot == MessageRichEmbedMediaSlot::thumbnail ? &embed.thumbnail
                                                                                   : &embed.video;
    // Native providers can supply playback through the card URL alone.
    if (!media->url.bytes && !media->proxy_url.bytes &&
        !(slot == MessageRichEmbedMediaSlot::video && embed.url.bytes))
        return false;
    *changed = media->width != width || media->height != height || (media->state & 3u) != 3u;
    media->width = width;
    media->height = height;
    media->state = static_cast<u8>(media->state | 3u);
    return true;
}

Utf8View message_rich_store_string(const MessageRichStore& store, MessageRichString value) noexcept {
    const Utf8View result = stored_text(store, value);
    return utf8_validate(result).valid ? result : Utf8View{};
}

bool message_rich_store_delete(MessageRichStore* store, u64 message_id) noexcept {
    if (!store || message_id == 0) {
        return false;
    }
    const SearchResult search = find_record(*store, message_id);
    return !search.found || erase_slot(store, search.order_index, search.slot);
}

bool message_rich_store_reset(MessageRichStore* store) noexcept {
    if (!store || !store->records || !store->order || !store->text) {
        return false;
    }
    MessageRichRecord* records = store->records;
    StableStorage* record_storage = store->record_storage;
    u16* order = store->order;
    u8* text = store->text;
    const u32 capacity = store->capacity;
    const u32 record_extent = store->record_extent;
    const u32 text_capacity = store->text_capacity;
    const bool sparse_text = store->sparse_text;
    return initialize_store(store, records, record_storage, capacity, record_extent,
                            Slice<u16>{order, capacity}, Slice<u8>{text, text_capacity},
                            store->record_capacity, sparse_text, true);
}

namespace {

struct PackedRichHeader {
    u32 bytes;
    u32 string_bytes;
};

// Head and tail are copied verbatim, so fields added outside the bounded
// arrays are carried without changing the snapshot format.
inline constexpr u32 kRichHead = __builtin_offsetof(MessageRichRecord, reactions);
inline constexpr u32 kRichTailBegin = __builtin_offsetof(MessageRichRecord, generation);
inline constexpr u32 kRichTail = sizeof(MessageRichRecord) - kRichTailBegin;
inline constexpr u32 kPackedRichMinimum = sizeof(PackedRichHeader) + kRichHead + kRichTail;

u32 packed_fixed_bytes(const MessageRichRecord& record) noexcept {
    return kPackedRichMinimum + record.reaction_count * sizeof(MessageRichReaction) +
           record.attachment_count * sizeof(MessageRichAttachment) +
           record.embed_count * sizeof(MessageRichEmbed) +
           record.embed_field_count * sizeof(MessageRichEmbedField) +
           record.sticker_count * sizeof(MessageRichSticker) +
           record.component_count * sizeof(MessageRichComponent);
}

struct PackStringContext {
    const MessageRichStore* store;
    u8* output;
    u32 capacity;
    u32 cursor;
};

bool pack_string(void* opaque, MessageRichString* value) noexcept {
    auto* context = static_cast<PackStringContext*>(opaque);
    if (value->bytes == 0)
        return value->offset == 0;
    const MessageRichStore& store = *context->store;
    const u32 base = bank_base(store);
    if (value->offset < base || value->offset > base + store.text_used ||
        value->bytes > base + store.text_used - value->offset || value->bytes > ~0u - context->cursor)
        return false;
    if (context->cursor + value->bytes > context->capacity)
        return false;
    copy_bytes(context->output + context->cursor, store.text + value->offset, value->bytes);
    value->offset = context->cursor;
    context->cursor += value->bytes;
    return true;
}

// Appends the record's items of every kind, each through a local copy that carries relative string offsets.
bool write_items(const MessageRichRecord& source, u8** cursor, PackStringContext* strings) noexcept {
    alignas(8) u8 item[sizeof(MessageRichEmbed)];
    #pragma clang loop unroll(disable)
    for (u32 kind = 0; kind < kMessageRichSideKinds; ++kind) {
        const u8* from = static_cast<const u8*>(block_of(source, kind));
        for (u32 index = 0, count = count_of(source, kind); index < count; ++index, from += kItemBytes[kind]) {
            copy_bytes(item, from, kItemBytes[kind]);
            if (!walk_item(kind, item, pack_string, strings))
                return false;
            copy_bytes(*cursor, item, kItemBytes[kind]);
            *cursor += kItemBytes[kind];
        }
    }
    return true;
}

const u8* read_items(const u8* cursor, void* items, u32 bytes) noexcept {
    copy_bytes(items, cursor, bytes);
    return cursor + bytes;
}

// A null output only sizes the record. Items go through local copies that carry relative string
// offsets; the head and tail come from a stack copy.
bool pack_record(const MessageRichStore& store, const MessageRichRecord& source, u8* output, u32 capacity,
                 u32* written) noexcept {
    const u32 fixed = packed_fixed_bytes(source);
    if (!output) {
        u64 string_bytes = 0;
        if (!walk_strings(const_cast<MessageRichRecord*>(&source), kRichFields, count_wasted, &string_bytes) ||
            string_bytes > ~0u - fixed)
            return false;
        *written = fixed + static_cast<u32>(string_bytes);
        return true;
    }
    if (fixed > capacity)
        return false;
    MessageRichRecord copy;
    copy_bytes(&copy, &source, kRichHead);
    copy_bytes(reinterpret_cast<u8*>(&copy) + kRichTailBegin, reinterpret_cast<const u8*>(&source) + kRichTailBegin,
               kRichTail);
    // Slot and revision bindings are reassigned by unpack.
    copy.message_revision = 0;
    copy.generation = 0;
    copy.forwarded_snapshot_slot = 0;
    PackStringContext strings{&store, output + fixed, capacity - fixed, 0};
    u8* cursor = output + sizeof(PackedRichHeader);
    if (!walk_head_strings(&copy, kRichFields, pack_string, &strings))
        return false;
    copy_bytes(cursor, &copy, kRichHead);
    cursor += kRichHead;
    copy_bytes(cursor, reinterpret_cast<const u8*>(&copy) + kRichTailBegin, kRichTail);
    cursor += kRichTail;
    if (!write_items(source, &cursor, &strings) ||
        strings.cursor > ~0u - fixed)
        return false;
    *written = fixed + strings.cursor;
    const PackedRichHeader header{*written, strings.cursor};
    copy_bytes(output, &header, sizeof(header));
    return true;
}

struct UnpackStringContext {
    u32 string_bytes;
    u32 destination;
};

bool unpack_string(void* opaque, MessageRichString* value) noexcept {
    auto* context = static_cast<UnpackStringContext*>(opaque);
    if (value->bytes == 0)
        return value->offset == 0;
    if (value->offset > context->string_bytes || value->bytes > context->string_bytes - value->offset)
        return false;
    value->offset += context->destination;
    return true;
}

// Copies the head and tail into `record`, then takes a side block per counted array and reads the
// items into it. On failure the caller resets the slot, which returns any blocks taken.
bool parse_record(MessageRichStore* store, ConstSlice<u8> input, MessageRichRecord* record,
                  ConstSlice<u8>* strings, u32* consumed) noexcept {
    PackedRichHeader header{};
    if (!input.data || input.count < kPackedRichMinimum)
        return false;
    copy_bytes(&header, input.data, sizeof(header));
    if (header.bytes < kPackedRichMinimum || header.bytes > input.count || header.string_bytes > header.bytes)
        return false;
    // The slot is free, hence cleared beyond what the head and tail copies set.
    const u8* cursor = input.data + sizeof(header);
    copy_bytes(record, cursor, kRichHead);
    cursor += kRichHead;
    copy_bytes(reinterpret_cast<u8*>(record) + kRichTailBegin, cursor, kRichTail);
    cursor += kRichTail;
    // The store branches on these flags; a byte other than 0 or 1 is not a bool.
    const auto* raw = reinterpret_cast<const u8*>(record);
    if (raw[__builtin_offsetof(MessageRichRecord, occupied)] > 1 ||
        raw[__builtin_offsetof(MessageRichRecord, forwarded_snapshot_child)] > 1 ||
        raw[__builtin_offsetof(MessageRichRecord, has_forwarded_snapshot)] > 1 ||
        raw[__builtin_offsetof(MessageRichRecord, voice_message)] > 1)
        return false;
    if (record->reaction_count > kGatewayMessageReactionMaximum ||
        record->attachment_count > kGatewayMessageAttachmentMaximum ||
        record->embed_count > kGatewayMessageEmbedMaximum ||
        record->embed_field_count > kGatewayMessageEmbedFieldMaximum ||
        record->sticker_count > kGatewayMessageStickerMaximum ||
        record->component_count > kGatewayMessageComponentMaximum ||
        packed_fixed_bytes(*record) + header.string_bytes != header.bytes)
        return false;
    #pragma clang loop unroll(disable)
    for (u32 kind = 0; kind < kMessageRichSideKinds; ++kind) {
        const u32 count = count_of(*record, kind);
        bool fresh = false;
        void* block = fit_block(&store->side, kind, block_of(*record, kind), count, &fresh);
        if (!block)
            return false;
        set_block(record, kind, block);
        cursor = read_items(cursor, block, count * kItemBytes[kind]);
    }
    *strings = ConstSlice<u8>{cursor, header.string_bytes};
    *consumed = header.bytes;
    return true;
}

} // namespace

bool message_rich_store_pack(const MessageRichStore& store, u64 message_id, Slice<u8> output,
                             u32* written) noexcept {
    if (!written)
        return false;
    *written = 0;
    if (!store.records || !store.order || !store.text || message_id == 0)
        return false;
    const SearchResult found = find_record(store, message_id);
    if (!found.found)
        return true;
    const MessageRichRecord& outer = store.records[found.slot];
    const MessageRichRecord* child =
        outer.has_forwarded_snapshot ? message_rich_store_snapshot(store, message_id) : nullptr;
    if (outer.has_forwarded_snapshot && !child)
        return false;
    u8* data = output.data && output.count ? output.data : nullptr;
    u32 cursor = sizeof(u32);
    u32 bytes = 0;
    if (data && output.count < cursor)
        return false;
    if (!pack_record(store, outer, data ? data + cursor : nullptr, data ? output.count - cursor : 0, &bytes))
        return false;
    cursor += bytes;
    if (child) {
        if (!pack_record(store, *child, data ? data + cursor : nullptr, data ? output.count - cursor : 0,
                         &bytes) ||
            bytes > ~0u - cursor)
            return false;
        cursor += bytes;
    }
    if (data) {
        const u32 flags = child ? 1u : 0u;
        copy_bytes(data, &flags, sizeof(flags));
    }
    *written = cursor;
    return true;
}

bool message_rich_store_unpack(MessageRichStore* store, u64 message_id, u32 message_revision,
                               ConstSlice<u8> packed, u32* slot_hint) noexcept {
    if (!store || !store->records || !store->order || !store->text || message_id == 0 || message_revision == 0 ||
        !packed.data || packed.count < sizeof(u32))
        return false;
    const SearchResult search = find_record(*store, message_id);
    if (search.found || store->count >= store->capacity)
        return false;
    u32 flags = 0;
    copy_bytes(&flags, packed.data, sizeof(flags));
    if (flags > 1u)
        return false;
    const bool has_child = flags != 0;
    const u32 parts = has_child ? 2u : 1u;
    PackedRichHeader headers[2]{};
    u32 cursor = sizeof(u32);
    u32 string_total = 0;
    for (u32 index = 0; index < parts; ++index) {
        if (packed.count - cursor < sizeof(PackedRichHeader))
            return false;
        copy_bytes(&headers[index], packed.data + cursor, sizeof(PackedRichHeader));
        if (headers[index].bytes < kPackedRichMinimum || headers[index].bytes > packed.count - cursor)
            return false;
        cursor += headers[index].bytes;
        string_total += headers[index].string_bytes;
    }
    const u32 capacity = bank_capacity(*store);
    if (cursor != packed.count || store->text_used > capacity || string_total > capacity)
        return false;
    if (string_total > capacity - store->text_used &&
        (!compact_excluding(store, 0, false, 0) || string_total > capacity - store->text_used))
        return false;
    // Admit every physical slot before publishing either record.
    const u32 from = slot_hint ? *slot_hint : 0;
    u32 slots[2]{0xFFFFu, 0xFFFFu};
    for (;;) {
        slots[0] = free_slot(*store, from, 0xFFFFu);
        slots[1] = has_child && slots[0] != 0xFFFFu ? free_slot(*store, slots[0] + 1u, slots[0]) : 0;
        if (slots[0] != 0xFFFFu && slots[1] != 0xFFFFu)
            break;
        if (!extend_records(store))
            return false;
    }
    const auto abandon = [&](u32 count) noexcept {
        for (u32 index = 0; index < count; ++index) {
            MessageRichRecord& record = store->records[slots[index]];
            const u16 generation = record.generation;
            reset_slot(store, &record, generation == 0 ? u16{1} : generation);
        }
        return false;
    };
    const u32 base = bank_base(*store);
    u32 destination = base + store->text_used;
    cursor = sizeof(u32);
    for (u32 index = 0; index < parts; ++index) {
        MessageRichRecord& record = store->records[slots[index]];
        const u16 generation = record.generation;
        ConstSlice<u8> strings{};
        u32 consumed = 0;
        UnpackStringContext context{headers[index].string_bytes, destination};
        if (!parse_record(store, {packed.data + cursor, packed.count - cursor}, &record, &strings, &consumed) ||
            record.has_forwarded_snapshot != (index == 0 && has_child) ||
            record.forwarded_snapshot_child != (index != 0) ||
            !walk_strings(&record, kRichFields, unpack_string, &context)) {
            record.generation = generation;
            return abandon(index + 1);
        }
        record.generation = generation == 0 ? 1 : generation;
        record.message_id = message_id;
        record.message_revision = message_revision;
        record.occupied = false;
        record.forwarded_snapshot_slot = index == 0 && has_child ? static_cast<u16>(slots[1]) : 0;
        copy_bytes(store->text + destination, strings.data, strings.count);
        destination += strings.count;
        cursor += consumed;
    }
    for (u32 index = 0; index < parts; ++index)
        store->records[slots[index]].occupied = true;
    store->text_used = destination - base;
    sorted_insert_gap(store->order, store->count, search.order_index);
    store->order[search.order_index] = static_cast<u16>(slots[0]);
    ++store->count;
    if (has_child)
        ++store->snapshot_count;
    ++store->updated;
    if (slot_hint)
        *slot_hint = slots[has_child ? 1 : 0] + 1u;
    return true;
}
} // namespace maicord
