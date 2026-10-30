#include "maicord/model/message_rich_store.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/media/asset_key.hpp"

namespace maicord {
namespace {

inline constexpr u32 kReferenceFields =
    gateway_message_field_message_reference | gateway_message_field_referenced_message;
inline constexpr u32 kRichFields =
    kReferenceFields | gateway_message_field_reactions | gateway_message_field_attachments |
    gateway_message_field_embeds | gateway_message_field_sticker_items | gateway_message_field_components |
    gateway_message_field_interaction | gateway_message_field_message_snapshots | gateway_message_field_thread |
    gateway_message_field_mentions | gateway_message_field_call;
// Only recipient add/remove system messages retain their mentioned user.
constexpr bool system_target_type(u16 type) noexcept {
    return type == 1 || type == 2;
}

using StringVisitor = bool (*)(void*, MessageRichString*) noexcept;

template <typename... Strings> bool visit_all(StringVisitor visitor, void* context, Strings*... values) noexcept {
    return (visitor(context, values) && ...);
}

bool walk_embed_media_strings(MessageRichEmbedMedia* media, StringVisitor visitor, void* context) noexcept {
    return visit_all(visitor, context, &media->url, &media->proxy_url, &media->content_type, &media->description);
}

// Item arrays live in fixed-size side blocks: per array kind, a small class and a class holding the
// protocol maximum. Blocks recycle through a per-class free list; nothing moves, so item pointers
// stay valid until their record's array is replaced. Kinds follow the record's member order.
enum ArrayKind : u32 { kind_reaction, kind_attachment, kind_embed, kind_field, kind_sticker, kind_component };
constexpr u32 kItemBytes[kMessageRichSideKinds] = {
    sizeof(MessageRichReaction), sizeof(MessageRichAttachment), sizeof(MessageRichEmbed),
    sizeof(MessageRichEmbedField), sizeof(MessageRichSticker), sizeof(MessageRichComponent)};
constexpr u32 kSmallItems[kMessageRichSideKinds] = {4, 2, 1, 4, kGatewayMessageStickerMaximum, 4};
constexpr u32 kLargeItems[kMessageRichSideKinds] = {
    kGatewayMessageReactionMaximum, kGatewayMessageAttachmentMaximum, kGatewayMessageEmbedMaximum,
    kGatewayMessageEmbedFieldMaximum, kGatewayMessageStickerMaximum, kGatewayMessageComponentMaximum};
// The event field that replaces each kind; fields belong to the embeds they follow.
constexpr u32 kKindFields[kMessageRichSideKinds] = {
    gateway_message_field_reactions, gateway_message_field_attachments, gateway_message_field_embeds,
    gateway_message_field_embeds,    gateway_message_field_sticker_items, gateway_message_field_components};
constexpr u32 kSideMagic = 0x53524D43u;

// The record keeps its six array pointers and six counts contiguously, in kind order.
constexpr u32 kPointerBytes = sizeof(void*);
static_assert(__builtin_offsetof(MessageRichRecord, components) - __builtin_offsetof(MessageRichRecord, reactions) ==
              5 * kPointerBytes);
static_assert(__builtin_offsetof(MessageRichRecord, component_count) -
                  __builtin_offsetof(MessageRichRecord, reaction_count) == 5);

// Shared zero items that an empty array points at; never written.
alignas(8) MessageRichReaction g_no_reactions[kGatewayMessageReactionMaximum];
alignas(8) MessageRichAttachment g_no_attachments[kGatewayMessageAttachmentMaximum];
alignas(8) MessageRichEmbed g_no_embeds[kGatewayMessageEmbedMaximum];
alignas(8) MessageRichEmbedField g_no_fields[kGatewayMessageEmbedFieldMaximum];
alignas(8) MessageRichSticker g_no_stickers[kGatewayMessageStickerMaximum];
alignas(8) MessageRichComponent g_no_components[kGatewayMessageComponentMaximum];
void* const kNoItems[kMessageRichSideKinds] = {g_no_reactions, g_no_attachments, g_no_embeds,
                                               g_no_fields,    g_no_stickers,    g_no_components};

void* block_of(const MessageRichRecord& record, u32 kind) noexcept {
    void* block;
    copy_bytes(&block, reinterpret_cast<const u8*>(&record.reactions) + kind * kPointerBytes, kPointerBytes);
    return block;
}
void set_block(MessageRichRecord* record, u32 kind, void* block) noexcept {
    copy_bytes(reinterpret_cast<u8*>(&record->reactions) + kind * kPointerBytes, &block, kPointerBytes);
}
u32 count_of(const MessageRichRecord& record, u32 kind) noexcept {
    return (&record.reaction_count)[kind];
}

// The strings of one array item, in storage order; `item` points at an item of `kind`.
bool walk_item(u32 kind, void* item, StringVisitor visitor, void* context) noexcept {
    switch (kind) {
    case kind_reaction:
        return visitor(context, &static_cast<MessageRichReaction*>(item)->name);
    case kind_attachment: {
        auto* value = static_cast<MessageRichAttachment*>(item);
        return visit_all(visitor, context, &value->filename, &value->title, &value->description,
                         &value->content_type, &value->url, &value->waveform);
    }
    case kind_embed: {
        auto* value = static_cast<MessageRichEmbed*>(item);
        return visit_all(visitor, context, &value->title, &value->description, &value->url, &value->author_name,
                         &value->footer_text, &value->author_url, &value->author_icon, &value->footer_icon,
                         &value->provider_name, &value->provider_url) &&
               walk_embed_media_strings(&value->image, visitor, context) &&
               walk_embed_media_strings(&value->thumbnail, visitor, context) &&
               walk_embed_media_strings(&value->video, visitor, context);
    }
    case kind_field: {
        auto* value = static_cast<MessageRichEmbedField*>(item);
        return visit_all(visitor, context, &value->name, &value->value);
    }
    case kind_sticker:
        return visitor(context, &static_cast<MessageRichSticker*>(item)->name);
    default: {
        auto* value = static_cast<MessageRichComponent*>(item);
        return visit_all(visitor, context, &value->label, &value->content, &value->custom_id, &value->url,
                         &value->emoji_name);
    }
    }
}

// The strings outside the item arrays: interaction, thread, target, snapshot or reference.
bool walk_head_strings(MessageRichRecord* record, u32 fields, StringVisitor visitor, void* context) noexcept {
    if ((fields & gateway_message_field_interaction) != 0 &&
        !visit_all(visitor, context, &record->interaction.user_name, &record->interaction.user_avatar_hash,
                   &record->interaction.command_name))
        return false;
    if ((fields & gateway_message_field_thread) && !visitor(context, &record->thread.name))
        return false;
    if ((fields & gateway_message_field_mentions) && !visitor(context, &record->system.target_name))
        return false;
    if (record->forwarded_snapshot_child && (fields & gateway_message_field_message_snapshots) &&
        !visitor(context, &record->snapshot.content))
        return false;
    return record->forwarded_snapshot_child || (fields & kReferenceFields) == 0 ||
           visit_all(visitor, context, &record->reference.author_name, &record->reference.author_avatar_hash,
                     &record->reference.content);
}

bool walk_strings(MessageRichRecord* record, u32 fields, StringVisitor visitor, void* context) noexcept {
    if (!walk_head_strings(record, fields, visitor, context))
        return false;
    #pragma clang loop unroll(disable)
    for (u32 kind = 0; kind < kMessageRichSideKinds; ++kind) {
        if (!(fields & kKindFields[kind]))
            continue;
        u8* item = static_cast<u8*>(block_of(*record, kind));
        for (u32 index = 0, count = count_of(*record, kind); index < count; ++index, item += kItemBytes[kind])
            if (!walk_item(kind, item, visitor, context))
                return false;
    }
    return true;
}

bool slab_owns(const MessageRichSlab& slab, const void* block) noexcept {
    const u8* base = slab.storage.arena.base;
    return base && static_cast<const u8*>(block) >= base &&
           static_cast<const u8*>(block) < base + usize{slab.storage.reserved_capacity} * slab.block_bytes;
}

// A zeroed block, or null when the slab cannot commit more.
void* slab_take(MessageRichSlab* slab) noexcept {
    u8* base = stable_storage_data<u8>(slab->storage);
    u32 index = slab->top;
    if (slab->free_head) {
        index = slab->free_head - 1;
        copy_bytes(&slab->free_head, base + usize{index} * slab->block_bytes, sizeof(u32));
    } else if (index >= slab->storage.reserved_capacity || !stable_storage_reserve(&slab->storage, index + 1)) {
        return nullptr;
    } else {
        ++slab->top;
    }
    u8* block = base + usize{index} * slab->block_bytes;
    clear_bytes(block, slab->block_bytes);
    return block;
}

void slab_give(MessageRichSlab* slab, void* block) noexcept {
    const u32 index = static_cast<u32>((static_cast<u8*>(block) - stable_storage_data<u8>(slab->storage)) /
                                       slab->block_bytes);
    copy_bytes(block, &slab->free_head, sizeof(u32));
    slab->free_head = index + 1;
}

// How many items a block holds; zero for the shared empty items and foreign pointers.
u32 block_items(const MessageRichSide& side, u32 kind, const void* block) noexcept {
    return slab_owns(side.slabs[kind][0], block)   ? kSmallItems[kind]
           : slab_owns(side.slabs[kind][1], block) ? kLargeItems[kind]
                                                   : 0;
}

void release_block(MessageRichSide* side, u32 kind, void* block) noexcept {
    if (slab_owns(side->slabs[kind][0], block))
        slab_give(&side->slabs[kind][0], block);
    else if (slab_owns(side->slabs[kind][1], block))
        slab_give(&side->slabs[kind][1], block);
}

// A block for `count` items: `current` when it already fits, the shared empty items for none, else a
// fresh zeroed block (*fresh) in the smallest fitting class. Null when none can be committed.
void* fit_block(MessageRichSide* side, u32 kind, void* current, u32 count, bool* fresh) noexcept {
    *fresh = false;
    if (count == 0)
        return kNoItems[kind];
    if (count <= block_items(*side, kind, current))
        return current;
    void* block = slab_take(&side->slabs[kind][count <= kSmallItems[kind] ? 0 : 1]);
    *fresh = block != nullptr;
    return block;
}

// Blocks for every array a record update replaces, taken before the record changes.
struct ArrayPlan {
    void* block[kMessageRichSideKinds];
    bool replace[kMessageRichSideKinds];
    bool fresh[kMessageRichSideKinds];
};

void discard_plan(MessageRichSide* side, const ArrayPlan& plan) noexcept {
    #pragma clang loop unroll(disable)
    for (u32 kind = 0; kind < kMessageRichSideKinds; ++kind)
        if (plan.replace[kind] && plan.fresh[kind])
            release_block(side, kind, plan.block[kind]);
}

bool plan_arrays(MessageRichSide* side, const MessageRichRecord& record, const GatewayMessageEvent& input,
                 u32 fields, ArrayPlan* plan) noexcept {
    const u32 counts[kMessageRichSideKinds] = {input.reactions.count,  input.attachments.count,
                                               input.embeds.count,     input.embed_fields.count,
                                               input.stickers.count,   input.components.count};
    #pragma clang loop unroll(disable)
    for (u32 kind = 0; kind < kMessageRichSideKinds; ++kind) {
        plan->replace[kind] = (fields & kKindFields[kind]) != 0;
        if (!plan->replace[kind])
            continue;
        plan->block[kind] = fit_block(side, kind, block_of(record, kind), counts[kind], &plan->fresh[kind]);
        if (!plan->block[kind]) {
            plan->replace[kind] = false;
            discard_plan(side, *plan);
            return false;
        }
    }
    return true;
}

void commit_plan(MessageRichSide* side, MessageRichRecord* record, const ArrayPlan& plan) noexcept {
    #pragma clang loop unroll(disable)
    for (u32 kind = 0; kind < kMessageRichSideKinds; ++kind) {
        void* const previous = block_of(*record, kind);
        if (!plan.replace[kind] || previous == plan.block[kind])
            continue;
        set_block(record, kind, plan.block[kind]);
        release_block(side, kind, previous);
    }
}

// Returns every block of a record to its slab and points the arrays back at the empty items.
void release_arrays(MessageRichSide* side, MessageRichRecord* record) noexcept {
    #pragma clang loop unroll(disable)
    for (u32 kind = 0; kind < kMessageRichSideKinds; ++kind) {
        release_block(side, kind, block_of(*record, kind));
        set_block(record, kind, kNoItems[kind]);
    }
}

void reset_slot(MessageRichStore* store, MessageRichRecord* record, u16 generation) noexcept {
    release_arrays(&store->side, record);
    message_rich_record_clear(record);
    record->generation = generation;
}

void side_release(MessageRichSide* side) noexcept {
    if (side->initialized == kSideMagic)
        for (auto& pair : side->slabs)
            for (auto& slab : pair)
                stable_storage_release(&slab.storage);
    reset_value(side);
}

// One block per live record per kind, plus one while a record changes class.
bool side_initialize(MessageRichSide* side, u32 records) noexcept {
    reset_value(side);
    side->initialized = kSideMagic;
    #pragma clang loop unroll(disable)
    for (u32 kind = 0; kind < kMessageRichSideKinds; ++kind) {
#pragma clang loop unroll(disable)
        for (u32 large = 0; large < 2; ++large) {
            if (large && kSmallItems[kind] == kLargeItems[kind])
                continue;
            MessageRichSlab& slab = side->slabs[kind][large];
            slab.block_bytes = (large ? kLargeItems[kind] : kSmallItems[kind]) * kItemBytes[kind];
            if (!stable_storage_initialize(&slab.storage, records + 1, 0, slab.block_bytes, 8)) {
                side_release(side);
                return false;
            }
        }
    }
    return true;
}

// Every block is free again; committed pages stay for reuse.
void side_clear(MessageRichSide* side) noexcept {
    for (auto& pair : side->slabs)
        for (auto& slab : pair)
            slab.free_head = slab.top = 0;
}

u32 bank_capacity(const MessageRichStore& store) noexcept {
    return store.text_capacity / 2;
}

u32 bank_base(const MessageRichStore& store) noexcept {
    return static_cast<u32>(store.active_bank) * bank_capacity(store);
}

struct TotalContext {
    const MessageRichStore* store;
    u64 total;
};

bool total_string(void* opaque, MessageRichString* value) noexcept {
    auto* context = static_cast<TotalContext*>(opaque);
    if (value->bytes == 0) {
        return value->offset == 0;
    }
    const MessageRichStore& store = *context->store;
    const u32 base = bank_base(store);
    if (value->offset < base || value->offset > base + store.text_used ||
        value->bytes > base + store.text_used - value->offset) {
        return false;
    }
    const Utf8View text{reinterpret_cast<const char*>(store.text + value->offset), value->bytes};
    if (!utf8_validate(text).valid || context->total > ~0ull - value->bytes) {
        return false;
    }
    context->total += value->bytes;
    return true;
}

struct RelocateContext {
    MessageRichStore* store;
    u32 destination_base;
    u32 cursor;
};

bool relocate_string(void* opaque, MessageRichString* value) noexcept {
    auto* context = static_cast<RelocateContext*>(opaque);
    if (value->bytes == 0) {
        return value->offset == 0;
    }
    MessageRichStore& store = *context->store;
    const u32 source_base = bank_base(store);
    const u32 capacity = bank_capacity(store);
    if (value->offset < source_base || value->offset > source_base + store.text_used ||
        value->bytes > source_base + store.text_used - value->offset || context->cursor > capacity ||
        value->bytes > capacity - context->cursor) {
        return false;
    }
    copy_bytes(store.text + context->destination_base + context->cursor, store.text + value->offset, value->bytes);
    value->offset = context->destination_base + context->cursor;
    context->cursor += value->bytes;
    return true;
}

struct SearchResult {
    u32 order_index;
    u16 slot;
    bool found;
};

SearchResult find_record(const MessageRichStore& store, u64 message_id) noexcept {
    u32 low = 0;
    u32 high = store.count;
    while (low < high) {
        const u32 middle = low + (high - low) / 2;
        const u16 slot = store.order[middle];
        // order lists occupied slots only, so a probe reads just the id (the
        // 8 KiB record's first line); occupancy is checked on the final candidate.
        if (slot >= store.record_extent) {
            return SearchResult{};
        }
        if (store.records[slot].message_id < message_id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < store.count) {
        const u16 slot = store.order[low];
        if (slot < store.record_extent && store.records[slot].occupied &&
            store.records[slot].message_id == message_id) {
            return SearchResult{low, slot, true};
        }
    }
    return SearchResult{low, 0, false};
}

// The text a string names, or empty when it lies outside the used bank.
Utf8View stored_text(const MessageRichStore& store, MessageRichString value) noexcept {
    const u32 base = bank_base(store);
    if (value.bytes == 0 || !store.text || value.offset < base || value.offset > base + store.text_used ||
        value.bytes > base + store.text_used - value.offset)
        return Utf8View{};
    return Utf8View{reinterpret_cast<const char*>(store.text + value.offset), value.bytes};
}

// First unoccupied slot at or after `from` (wrapping), other than `skip`; 0xFFFF when none.
u32 free_slot(const MessageRichStore& store, u32 from, u32 skip) noexcept {
    for (u32 pass = 0; pass < 2; ++pass) {
        const u32 end = pass ? minimum(from, store.record_extent) : store.record_extent;
        for (u32 index = pass ? 0 : from; index < end; ++index)
            if (!store.records[index].occupied && index != skip)
                return index;
    }
    return 0xFFFFu;
}

bool compact_excluding(MessageRichStore* store, u16 target_slot, bool has_target,
                       u32 replaced_fields) noexcept {
    TotalContext total{store, 0};
    for (u32 slot = 0; slot < store->record_extent; ++slot) {
        if (!store->records[slot].occupied)
            continue;
        const u32 fields = has_target && slot == target_slot ? kRichFields & ~replaced_fields : kRichFields;
        if (!walk_strings(&store->records[slot], fields, total_string, &total)) {
            return false;
        }
    }
    const u32 capacity = bank_capacity(*store);
    if (total.total > capacity) {
        return false;
    }
    const u8 destination_bank = store->active_bank == 0 ? 1 : 0;
    RelocateContext relocate{store, static_cast<u32>(destination_bank) * capacity, 0};
    for (u32 slot = 0; slot < store->record_extent; ++slot) {
        if (!store->records[slot].occupied)
            continue;
        const u32 fields = has_target && slot == target_slot ? kRichFields & ~replaced_fields : kRichFields;
        if (!walk_strings(&store->records[slot], fields, relocate_string, &relocate)) {
            return false;
        }
    }
    store->active_bank = destination_bank;
    store->text_used = relocate.cursor;
    store->text_wasted = 0;
    ++store->compactions;
    return true;
}

bool add_bytes(u64* total, const Utf8View& value) noexcept {
    if ((!value.data && value.bytes != 0) || value.bytes > 0xFFFFu ||
        !utf8_validate(value).valid || *total > ~0ull - value.bytes) {
        return false;
    }
    *total += value.bytes;
    return true;
}

template <typename... Views> bool add_all(u64* total, const Views&... values) noexcept {
    return (add_bytes(total, values) && ...);
}

bool validate_embed_media(const GatewayMessageEmbedMedia& media, u64* desired_bytes) noexcept {
    return (!media.has_width || media.width != 0) && (!media.has_height || media.height != 0) &&
           add_all(desired_bytes, media.url, media.proxy_url, media.content_type, media.description);
}

bool validate_attachments(ConstSlice<GatewayMessageAttachment> attachments, u64* desired_bytes) noexcept {
    if (attachments.count > kGatewayMessageAttachmentMaximum ||
        (!attachments.data && attachments.count))
        return false;
    for (u32 index = 0; index < attachments.count; ++index) {
        const auto& item = attachments[index];
        if (!item.id ||
            !add_all(desired_bytes, item.filename, item.title, item.description, item.content_type, item.url) ||
            !gateway_message_waveform_valid(item.waveform) || !add_bytes(desired_bytes, item.waveform) ||
            (!item.has_duration && item.duration_ms))
            return false;
    }
    return true;
}
bool validate_input(const GatewayMessageEvent& input, u32 fields, u64* desired_bytes) noexcept {
    if (input.reactions.count > kGatewayMessageReactionMaximum ||
        input.embeds.count > kGatewayMessageEmbedMaximum ||
        input.embed_fields.count > kGatewayMessageEmbedFieldMaximum ||
        input.stickers.count > kGatewayMessageStickerMaximum ||
        input.components.count > kGatewayMessageComponentMaximum ||
        ((fields & gateway_message_field_reactions) == 0 && input.reactions.count != 0) ||
        ((fields & gateway_message_field_attachments) == 0 && input.attachments.count != 0) ||
        ((fields & gateway_message_field_embeds) == 0 &&
         (input.embeds.count != 0 || input.embed_fields.count != 0)) ||
        ((fields & gateway_message_field_sticker_items) == 0 && input.stickers.count != 0) ||
        ((fields & gateway_message_field_components) == 0 && input.components.count != 0)) {
        return false;
    }
    *desired_bytes = 0;
    if ((fields & gateway_message_field_message_snapshots) && input.snapshot.present &&
        !add_bytes(desired_bytes, input.snapshot.content))
        return false;
    if ((fields & gateway_message_field_interaction) != 0 &&
        !add_all(desired_bytes, input.interaction.user_name, input.interaction.user_avatar_hash,
                 input.interaction.command_name))
        return false;
    if ((fields & gateway_message_field_thread) && !add_bytes(desired_bytes, input.thread.name))
        return false;
    if ((fields & gateway_message_field_mentions) && input.mention_users.count &&
        !add_bytes(desired_bytes, input.mention_users[0].name))
        return false;
    if ((fields & kReferenceFields) != 0 &&
        !add_all(desired_bytes, input.reference.author_name, input.reference.author_avatar_hash,
                 input.reference.content))
        return false;
    for (u32 index = 0; index < input.reactions.count; ++index) {
        if (input.reactions[index].count == 0 ||
            (!input.reactions[index].emoji_id && !input.reactions[index].name.bytes) ||
            input.reactions[index].normal_count > input.reactions[index].count ||
            input.reactions[index].burst_count >
                input.reactions[index].count - input.reactions[index].normal_count ||
            !add_bytes(desired_bytes, input.reactions[index].name)) {
            return false;
        }
    }
    if (!validate_attachments(input.attachments, desired_bytes))
        return false;
    for (u32 index = 0; index < input.embeds.count; ++index) {
        const GatewayMessageEmbed& item = input.embeds[index];
        if (item.field_begin > input.embed_fields.count ||
            item.field_count > input.embed_fields.count - item.field_begin ||
            !add_all(desired_bytes, item.title, item.description, item.url, item.author_name, item.footer_text,
                     item.author_url, item.author_icon, item.footer_icon, item.provider_name,
                     item.provider_url) ||
            !validate_embed_media(item.image, desired_bytes) ||
            !validate_embed_media(item.thumbnail, desired_bytes) ||
            !validate_embed_media(item.video, desired_bytes)) {
            return false;
        }
    }
    for (u32 index = 0; index < input.embed_fields.count; ++index) {
        if (input.embed_fields[index].embed_index >= input.embeds.count ||
            !add_all(desired_bytes, input.embed_fields[index].name, input.embed_fields[index].value))
            return false;
    }
    for (u32 index = 0; index < input.stickers.count; ++index) {
        if (input.stickers[index].id == 0 || input.stickers[index].format_type == 0 ||
            input.stickers[index].format_type > 4 ||
            !add_bytes(desired_bytes, input.stickers[index].name)) {
            return false;
        }
    }
    for (u32 index = 0; index < input.components.count; ++index) {
        const GatewayMessageComponent& item = input.components[index];
        if (item.type == 0 || (item.parent != 0xFFFFu && item.parent >= index) ||
            (item.has_accent_color && item.accent_color > 0xFFFFFFu) ||
            !add_all(desired_bytes, item.label, item.content, item.custom_id, item.url, item.emoji_name)) {
            return false;
        }
    }
    return true;
}

MessageRichString store_string(MessageRichStore* store, const Utf8View& value_view, bool* truncated) noexcept {
    const Utf8View value = value_view; // the store is written below
    if (value.bytes == 0) {
        return MessageRichString{};
    }
    const u32 capacity = bank_capacity(*store);
    if (value.bytes > 0xFFFFu || store->text_used > capacity || value.bytes > capacity - store->text_used) {
        *truncated = true;
        return MessageRichString{};
    }
    const u32 offset = bank_base(*store) + store->text_used;
    copy_bytes(store->text + offset, value.data, value.bytes);
    store->text_used += value.bytes;
    return MessageRichString{offset, static_cast<u16>(value.bytes), 0};
}

void copy_embed_media(MessageRichStore* store, const GatewayMessageEmbedMedia& source,
                      MessageRichEmbedMedia* destination, bool* truncated) noexcept {
    reset_value<MessageRichEmbedMedia>(destination);
    destination->url = store_string(store, source.url, truncated);
    destination->proxy_url = store_string(store, source.proxy_url, truncated);
    destination->content_type = store_string(store, source.content_type, truncated);
    destination->description = store_string(store, source.description, truncated);
    destination->width = source.width;
    destination->height = source.height;
    destination->flags = source.flags;
    destination->state = static_cast<u8>((source.has_width ? 1u : 0u) | (source.has_height ? 2u : 0u));
}

// Strings come from at most a few hundred 16-bit lengths, so the sum cannot wrap.
bool count_wasted(void* opaque, MessageRichString* value) noexcept {
    *static_cast<u64*>(opaque) += value->bytes;
    return true;
}

void clear_replaced(MessageRichRecord* record, u32 fields) noexcept {
    if ((fields & gateway_message_field_interaction) != 0)
        reset_value(&record->interaction);
    if ((fields & gateway_message_field_thread) != 0)
        reset_value(&record->thread);
    if ((fields & gateway_message_field_mentions) != 0) {
        record->system.target_user_id = 0;
        reset_value(&record->system.target_name);
    }
    if ((fields & gateway_message_field_call) != 0) {
        record->system.call_ended_ms = 0;
        record->system.call_present = record->system.call_ended = record->system.call_participated = false;
    }
    if ((fields & kReferenceFields) != 0) {
        reset_value<MessageRichReference>(&record->reference);
    }
    if ((fields & gateway_message_field_reactions) != 0) {
        record->reaction_count = 0;
    }
    if ((fields & gateway_message_field_attachments) != 0) {
        record->attachment_count = 0;
    }
    if ((fields & gateway_message_field_embeds) != 0) {
        record->embed_count = 0;
        record->embed_field_count = 0;
    }
    if ((fields & gateway_message_field_sticker_items) != 0) {
        record->sticker_count = 0;
    }
    if ((fields & gateway_message_field_components) != 0) {
        record->component_count = 0;
    }
}

bool record_has_content(const MessageRichRecord& record) noexcept {
    return record.forwarded_snapshot_child || record.has_forwarded_snapshot || record.interaction.id != 0 ||
           record.thread.id != 0 || record.system.target_user_id != 0 || record.system.call_present ||
           record.reference.state != GatewayMessageReferenceState::none || record.reaction_count != 0 ||
           record.attachment_count != 0 || record.embed_count != 0 || record.sticker_count != 0 ||
           record.component_count != 0 || (record.truncations & ~gateway_message_rich_truncated_text) != 0;
}

u32 replaced_truncation_mask(u32 fields) noexcept {
    u32 mask = 0;
    if ((fields & gateway_message_field_reactions) != 0) {
        mask |= gateway_message_rich_truncated_reactions;
    }
    if ((fields & gateway_message_field_attachments) != 0) {
        mask |= gateway_message_rich_truncated_attachments;
    }
    if ((fields & gateway_message_field_embeds) != 0) {
        mask |= gateway_message_rich_truncated_embeds | gateway_message_rich_truncated_embed_fields;
    }
    if ((fields & gateway_message_field_sticker_items) != 0) {
        mask |= gateway_message_rich_truncated_stickers;
    }
    if ((fields & gateway_message_field_components) != 0) {
        mask |= gateway_message_rich_truncated_components;
    }
    return mask;
}

void copy_input(MessageRichStore* store, MessageRichRecord* record, const GatewayMessageEvent& input,
                u32 fields, bool* truncated) noexcept {
    if (fields & gateway_message_field_message_snapshots) {
        if (record->forwarded_snapshot_child) {
            record->snapshot = {store_string(store, input.snapshot.content, truncated),
                                input.snapshot.timestamp_ms,
                                input.snapshot.edited_timestamp_ms,
                                input.snapshot.flags,
                                input.snapshot.type,
                                input.snapshot.present};
        } else
            record->has_forwarded_snapshot = input.snapshot.present;
    }
    if ((fields & gateway_message_field_thread) && input.thread.present) {
        record->thread.id = input.thread.id;
        record->thread.last_message_id = input.thread.last_message_id;
        record->thread.message_count = input.thread.message_count;
        record->thread.member_count = input.thread.member_count;
        record->thread.archived = input.thread.archived;
        record->thread.locked = input.thread.locked;
        record->thread.name = store_string(store, input.thread.name, truncated);
    }
    if ((fields & gateway_message_field_mentions) != 0 && input.mention_users.count != 0) {
        record->system.target_user_id = input.mention_users[0].id;
        record->system.target_name = store_string(store, input.mention_users[0].name, truncated);
    }
    if ((fields & gateway_message_field_call) != 0 && input.call.present) {
        record->system.call_present = true;
        record->system.call_ended = input.call.ended;
        record->system.call_participated = input.call.participated;
        record->system.call_ended_ms = input.call.ended_timestamp_ms;
    }
    if ((fields & gateway_message_field_interaction) != 0) {
        record->interaction.id = input.interaction.id;
        record->interaction.user_id = input.interaction.user_id;
        record->interaction.type = input.interaction.type;
        record->interaction.user_name = store_string(store, input.interaction.user_name, truncated);
        record->interaction.user_avatar_hash =
            store_string(store, input.interaction.user_avatar_hash, truncated);
        record->interaction.command_name = store_string(store, input.interaction.command_name, truncated);
    }
    if ((fields & kReferenceFields) != 0) {
        record->reference.message_id = input.reference.message_id;
        record->reference.channel_id = input.reference.channel_id;
        record->reference.guild_id = input.reference.guild_id;
        record->reference.author_id = input.reference.author_id;
        record->reference.state = input.reference.state;
        record->reference.type = input.reference.type;
        record->reference.content_flags = input.reference.content_flags;
        record->reference.quote_flags = input.reference.quote_flags;
        record->reference.message_type = input.reference.message_type;
        record->reference.author_name = store_string(store, input.reference.author_name, truncated);
        record->reference.author_avatar_hash =
            store_string(store, input.reference.author_avatar_hash, truncated);
        record->reference.content = store_string(store, input.reference.content, truncated);
    }
    if ((fields & gateway_message_field_reactions) != 0) {
        record->reaction_count = static_cast<u8>(input.reactions.count);
        for (u32 index = 0; index < input.reactions.count; ++index) {
            const GatewayMessageReaction& source = input.reactions[index];
            MessageRichReaction& destination = record->reactions[index];
            reset_value<MessageRichReaction>(&destination);
            destination.emoji_id = source.emoji_id;
            destination.name = store_string(store, source.name, truncated);
            destination.count = source.count;
            destination.normal_count = source.normal_count;
            destination.burst_count = source.burst_count;
            destination.flags = static_cast<u8>((source.me ? 1u : 0u) | (source.me_burst ? 2u : 0u) |
                                                (source.animated ? 4u : 0u));
        }
    }
    if ((fields & gateway_message_field_attachments) != 0) {
        record->attachment_count = static_cast<u8>(input.attachments.count);
        for (u32 index = 0; index < input.attachments.count; ++index) {
            const GatewayMessageAttachment& source = input.attachments[index];
            MessageRichAttachment& destination = record->attachments[index];
            reset_value<MessageRichAttachment>(&destination);
            destination.id = source.id;
            destination.size = source.size;
            destination.filename = store_string(store, source.filename, truncated);
            destination.title = store_string(store, source.title, truncated);
            destination.description = store_string(store, source.description, truncated);
            destination.content_type = store_string(store, source.content_type, truncated);
            destination.url = store_string(store, source.url, truncated);
            destination.waveform = store_string(store, source.waveform, truncated);
            destination.duration_ms = source.duration_ms;
            destination.width = source.width;
            destination.height = source.height;
            destination.flags = source.flags;
            destination.state =
                static_cast<u8>((source.has_width ? 1u : 0u) | (source.has_height ? 2u : 0u) |
                                (source.ephemeral ? 4u : 0u) | (source.has_duration ? 8u : 0u));
        }
    }
    if ((fields & gateway_message_field_embeds) != 0) {
        record->embed_count = static_cast<u8>(input.embeds.count);
        record->embed_field_count = static_cast<u8>(input.embed_fields.count);
        for (u32 index = 0; index < input.embeds.count; ++index) {
            const GatewayMessageEmbed& source = input.embeds[index];
            MessageRichEmbed& destination = record->embeds[index];
            reset_value<MessageRichEmbed>(&destination);
            destination.title = store_string(store, source.title, truncated);
            destination.description = store_string(store, source.description, truncated);
            destination.url = store_string(store, source.url, truncated);
            destination.author_name = store_string(store, source.author_name, truncated);
            destination.footer_text = store_string(store, source.footer_text, truncated);
            destination.author_url = store_string(store, source.author_url, truncated);
            destination.author_icon = store_string(store, source.author_icon, truncated);
            destination.footer_icon = store_string(store, source.footer_icon, truncated);
            destination.provider_name = store_string(store, source.provider_name, truncated);
            destination.provider_url = store_string(store, source.provider_url, truncated);
            destination.timestamp_ms = source.timestamp_ms;
            copy_embed_media(store, source.image, &destination.image, truncated);
            copy_embed_media(store, source.thumbnail, &destination.thumbnail, truncated);
            copy_embed_media(store, source.video, &destination.video, truncated);
            destination.color = source.color;
            destination.field_begin = source.field_begin;
            destination.field_count = source.field_count;
            destination.type = source.type;
            destination.has_color = source.has_color;
            destination.has_title = source.has_title || source.title.bytes != 0;
            destination.has_author = source.has_author || source.author_name.bytes != 0;
            destination.flags = source.flags;
        }
        for (u32 index = 0; index < input.embed_fields.count; ++index) {
            const GatewayMessageEmbedField& source = input.embed_fields[index];
            MessageRichEmbedField& destination = record->embed_fields[index];
            reset_value<MessageRichEmbedField>(&destination);
            destination.name = store_string(store, source.name, truncated);
            destination.value = store_string(store, source.value, truncated);
            destination.embed_index = source.embed_index;
            destination.inline_field = source.inline_field;
        }
    }
    if ((fields & gateway_message_field_sticker_items) != 0) {
        record->sticker_count = static_cast<u8>(input.stickers.count);
        for (u32 index = 0; index < input.stickers.count; ++index) {
            const GatewayMessageSticker& source = input.stickers[index];
            MessageRichSticker& destination = record->stickers[index];
            reset_value<MessageRichSticker>(&destination);
            destination.id = source.id;
            destination.name = store_string(store, source.name, truncated);
            destination.format_type = source.format_type;
        }
    }
    if ((fields & gateway_message_field_components) != 0) {
        record->component_count = static_cast<u8>(input.components.count);
        for (u32 index = 0; index < input.components.count; ++index) {
            const GatewayMessageComponent& source = input.components[index];
            MessageRichComponent& destination = record->components[index];
            reset_value<MessageRichComponent>(&destination);
            destination.label = store_string(store, source.label, truncated);
            destination.content = store_string(store, source.content, truncated);
            destination.custom_id = store_string(store, source.custom_id, truncated);
            destination.url = store_string(store, source.url, truncated);
            destination.emoji_name = store_string(store, source.emoji_name, truncated);
            destination.emoji_id = source.emoji_id;
            destination.min_values = source.min_values;
            destination.max_values = source.max_values;
            destination.flags = source.flags;
            destination.id = source.id;
            destination.parent = source.parent;
            destination.order = source.order;
            destination.type = source.type;
            destination.style = source.style;
            destination.disabled = source.disabled;
            destination.accent_color = source.accent_color;
            destination.has_accent_color = source.has_accent_color;
        }
    }
}

bool erase_snapshot_slot(MessageRichStore* store, u16 slot) noexcept {
    if (slot >= store->record_extent || !store->records[slot].occupied ||
        !store->records[slot].forwarded_snapshot_child || !store->snapshot_count)
        return false;
    u64 wasted = 0;
    if (!walk_strings(&store->records[slot], kRichFields, count_wasted, &wasted) ||
        wasted > ~0u - store->text_wasted)
        return false;
    const u16 generation = store->records[slot].generation;
    reset_slot(store, &store->records[slot], static_cast<u16>(generation == 0xFFFFu ? 1u : generation + 1u));
    store->text_wasted += static_cast<u32>(wasted);
    --store->snapshot_count;
    return true;
}

bool erase_slot(MessageRichStore* store, u32 order_index, u16 slot) noexcept {
    if (slot >= store->record_extent || order_index >= store->count ||
        store->order[order_index] != slot || !store->records[slot].occupied) {
        return false;
    }
    u64 bytes = 0;
    if (!walk_strings(&store->records[slot], kRichFields, count_wasted, &bytes) ||
        bytes > ~0u - store->text_wasted) {
        return false;
    }
    if (store->records[slot].has_forwarded_snapshot &&
        !erase_snapshot_slot(store, store->records[slot].forwarded_snapshot_slot))
        return false;
    store->text_wasted += static_cast<u32>(bytes);
    const u16 generation = store->records[slot].generation;
    reset_slot(store, &store->records[slot], generation == 0xFFFFu ? u16{1} : static_cast<u16>(generation + 1u));
    for (u32 index = order_index + 1; index < store->count; ++index) {
        store->order[index - 1] = store->order[index];
    }
    --store->count;
    ++store->deleted;
    return true;
}

// `keep_side` reuses the store's live side slabs (reset); otherwise they are created, replacing any
// the same store object held before.
bool initialize_store(MessageRichStore* store, MessageRichRecord* records, StableStorage* record_storage,
                      u32 capacity, u32 record_extent, Slice<u16> order, Slice<u8> text,
                      u32 physical_capacity, bool sparse_text = false, bool keep_side = false) noexcept {
    if (!store || !records || capacity == 0 || physical_capacity > 0xFFFFu ||
        capacity > physical_capacity || record_extent > physical_capacity || !order.data ||
        order.count < capacity || !text.data || text.count < 2 || (text.count & 1u) != 0) {
        return false;
    }
    MessageRichSide side = store->side;
    if (!keep_side)
        side_release(&side);
    reset_value<MessageRichStore>(store);
    store->side = side;
    if (keep_side)
        side_clear(&store->side);
    else if (!side_initialize(&store->side, physical_capacity))
        return false;
    store->records = records;
    store->record_storage = record_storage;
    store->order = order.data;
    store->text = text.data;
    store->capacity = capacity;
    store->record_capacity = physical_capacity;
    store->record_extent = record_extent;
    store->text_capacity = text.count;
    store->sparse_text = sparse_text;
    for (u32 index = 0; index < record_extent; ++index) {
        message_rich_record_clear(&records[index]);
        records[index].generation = 1;
    }
    for (u32 index = 0; index < capacity; ++index) {
        order[index] = 0;
    }
    if (sparse_text) {
        text[0] = 0;
        text[text.count / 2] = 0;
    } else
        for (u32 index = 0; index < text.count; ++index)
            text[index] = 0;
    return true;
}

bool extend_records(MessageRichStore* store) noexcept {
    if (!store->record_storage || store->record_extent >= store->record_capacity ||
        !stable_storage_reserve(store->record_storage, store->record_extent + 1u)) {
        return false;
    }
    const u32 previous_extent = store->record_extent;
    store->records = stable_storage_data<MessageRichRecord>(*store->record_storage);
    store->record_extent = store->record_storage->capacity;
    for (u32 index = previous_extent; index < store->record_extent; ++index) {
        message_rich_record_clear(&store->records[index]);
        store->records[index].generation = 1;
    }
    return true;
}

} // namespace

bool message_rich_store_initialize(MessageRichStore* store, Slice<MessageRichRecord> records,
                                   Slice<u16> order, Slice<u8> text) noexcept {
    return initialize_store(store, records.data, nullptr, minimum(records.count, order.count),
                            records.count, order, text, records.count);
}

bool message_rich_store_initialize_owned_sparse(MessageRichStore* store, StableStorage* record_storage,
                                                u32 record_capacity, Slice<u16> order,
                                                Slice<u8> reserved_text) noexcept {
    if (!store || !record_storage || record_storage->arena.base || record_capacity == 0 ||
        record_capacity > 0xFFFFu || !order.data || order.count < record_capacity || !reserved_text.data ||
        reserved_text.count < 2 || (reserved_text.count & 1u) != 0 ||
        !stable_storage_initialize(record_storage, minimum<u32>(record_capacity * 2u, 0xFFFFu), 0,
                                   sizeof(MessageRichRecord), alignof(MessageRichRecord), false)) {
        return false;
    }
    if (!initialize_store(store, stable_storage_data<MessageRichRecord>(*record_storage), record_storage,
                          record_capacity, 0, order, reserved_text, minimum<u32>(record_capacity * 2u, 0xFFFFu),
                          true)) {
        stable_storage_release(record_storage);
        return false;
    }
    return true;
}

void message_rich_store_release(MessageRichStore* store) noexcept {
    if (!store) {
        return;
    }
    side_release(&store->side);
    if (store->record_storage) {
        stable_storage_release(store->record_storage);
    }
    reset_value<MessageRichStore>(store);
}

void message_rich_record_clear(MessageRichRecord* record) noexcept {
    reset_value<MessageRichRecord>(record);
    #pragma clang loop unroll(disable)
    for (u32 kind = 0; kind < kMessageRichSideKinds; ++kind)
        set_block(record, kind, kNoItems[kind]);
}

namespace {
// Checked in order. Suffix rules read the filename; the generic ones apply
// only when the content type is absent or an opaque binary.
enum class AudioRuleKind : u8 { mime, generic_suffix, suffix };
struct AudioRule {
    Utf8View text;
    MediaAttachmentFormat format;
    AudioRuleKind kind;
};
constexpr AudioRule kAudioRules[] = {
    {utf8_literal("audio/ogg"), MediaAttachmentFormat::ogg_opus, AudioRuleKind::mime},
    {utf8_literal("audio/mpeg"), MediaAttachmentFormat::mp3, AudioRuleKind::mime},
    {utf8_literal("audio/mp3"), MediaAttachmentFormat::mp3, AudioRuleKind::mime},
    {utf8_literal("audio/wav"), MediaAttachmentFormat::wav, AudioRuleKind::mime},
    {utf8_literal("audio/x-wav"), MediaAttachmentFormat::wav, AudioRuleKind::mime},
    {utf8_literal("audio/vnd.wave"), MediaAttachmentFormat::wav, AudioRuleKind::mime},
    {utf8_literal(".mp3"), MediaAttachmentFormat::mp3, AudioRuleKind::generic_suffix},
    {utf8_literal(".wav"), MediaAttachmentFormat::wav, AudioRuleKind::generic_suffix},
    // MPEG-4 audio: AAC in an audio-only MP4/QuickTime container.
    {utf8_literal("audio/mp4"), MediaAttachmentFormat::m4a, AudioRuleKind::mime},
    {utf8_literal("audio/x-m4a"), MediaAttachmentFormat::m4a, AudioRuleKind::mime},
    {utf8_literal("audio/m4a"), MediaAttachmentFormat::m4a, AudioRuleKind::mime},
    {utf8_literal(".m4a"), MediaAttachmentFormat::m4a, AudioRuleKind::generic_suffix},
    // Ogg/Opus is admitted by filename alone; the demuxer rejects non-Ogg data
    // before any native playback resource is opened.
    {utf8_literal(".ogg"), MediaAttachmentFormat::ogg_opus, AudioRuleKind::suffix},
    {utf8_literal(".opus"), MediaAttachmentFormat::ogg_opus, AudioRuleKind::suffix},
};

// ASCII case-insensitive; `text` is lower case.
bool equal_lower(Utf8View value, Utf8View text, bool tail) noexcept {
    if (value.bytes < text.bytes || (!tail && value.bytes != text.bytes))
        return false;
    const char* at = value.data + (tail ? value.bytes - text.bytes : 0);
    for (u32 index = 0; index < text.bytes; ++index) {
        const char byte = at[index];
        if ((byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte) != text.data[index])
            return false;
    }
    return true;
}
} // namespace

MediaAttachmentFormat message_rich_attachment_audio_format(
    const MessageRichStore& store, const MessageRichAttachment& attachment) noexcept {
    Utf8View type = message_rich_store_string(store, attachment.content_type);
    const Utf8View name = message_rich_store_string(store, attachment.filename);
    // Parameters describe the same MIME token; display strings remain intact.
    for (u32 i = 0; i < type.bytes; ++i)
        if (type.data[i] == ';') {
            type.bytes = i;
            break;
        }
    while (type.bytes && (type.data[type.bytes - 1] == ' ' || type.data[type.bytes - 1] == '\t'))
        --type.bytes;
    const bool generic = !type.bytes || equal_lower(type, utf8_literal("application/octet-stream"), false) ||
                         equal_lower(type, utf8_literal("binary/octet-stream"), false);
    for (const AudioRule& rule : kAudioRules) {
        const bool mime = rule.kind == AudioRuleKind::mime;
        if ((rule.kind != AudioRuleKind::generic_suffix || generic) &&
            equal_lower(mime ? type : name, rule.text, !mime))
            return rule.format;
    }
    return MediaAttachmentFormat::unknown;
}
bool message_rich_attachment_audio_supported(const MessageRichStore& store,
                                             const MessageRichAttachment& attachment) noexcept {
    return message_rich_attachment_audio_format(store, attachment) != MediaAttachmentFormat::unknown;
}

bool message_rich_store_validate_attachments(ConstSlice<GatewayMessageAttachment> attachments) noexcept {
    u64 bytes = 0;
    return validate_attachments(attachments, &bytes);
}
bool message_rich_store_advance_revision(MessageRichStore* store, u64 message_id, u32 expected_revision,
                                         u32 next_revision) noexcept {
    if (!store || !store->records || !store->order || !message_id || !expected_revision || !next_revision)
        return false;
    const auto found = find_record(*store, message_id);
    if (!found.found || store->records[found.slot].message_revision != expected_revision)
        return false;
    auto& parent = store->records[found.slot];
    if (parent.has_forwarded_snapshot) {
        if (!message_rich_store_snapshot(*store, message_id))
            return false;
        store->records[parent.forwarded_snapshot_slot].message_revision = next_revision;
    }
    if (expected_revision != next_revision) {
        store->records[found.slot].message_revision = next_revision;
        ++store->updated;
    }
    return true;
}
struct RichValidationPlan {
    u64 bytes;
    u64 reference_bytes;
    u32 fields;
    u32 mention_bytes;
};

static MessageRichStoreResult apply_body(MessageRichStore* store, u64 message_id, u32 message_revision,
                                         const GatewayMessageEvent& input, bool child = false,
                                         u16 child_slot = 0, const RichValidationPlan* validated = nullptr) noexcept {
    MessageRichStoreResult result{};
    const auto reject = [&](MessageRichStoreError error) noexcept {
        result.error = error;
        ++store->rejected;
        return result;
    };
    if (!store->text || store->capacity == 0)
        return reject(MessageRichStoreError::invalid_input);
    u32 fields = input.fields & kRichFields & (child ? ~static_cast<u32>(gateway_message_field_thread) : ~0u);
    SearchResult search = child ? SearchResult{0, child_slot, store->records[child_slot].occupied}
                                : find_record(*store, message_id);
    // System metadata: mentions only for recipient changes, call objects only
    // while present (or to clear a retained one). Snapshots never carry them.
    if (child || !(input.fields & gateway_message_field_type) || !system_target_type(input.type))
        fields &= ~static_cast<u32>(gateway_message_field_mentions);
    if (child || (!input.call.present && !(search.found && store->records[search.slot].system.call_present)))
        fields &= ~static_cast<u32>(gateway_message_field_call);
    // An omitted referenced_message is not a replacement for a cached quote.
    // Retain it only while the reference still names the same target.
    if (search.found && (fields & gateway_message_field_message_reference) &&
        !(fields & gateway_message_field_referenced_message) &&
        input.reference.state == GatewayMessageReferenceState::unknown) {
        const auto& previous = store->records[search.slot].reference;
        if (previous.state != GatewayMessageReferenceState::none &&
            previous.message_id == input.reference.message_id &&
            previous.channel_id == input.reference.channel_id && previous.type == input.reference.type)
            fields &= ~kReferenceFields;
    }
    u64 desired_bytes = 0;
    constexpr u32 removable = gateway_message_field_mentions | gateway_message_field_call | kReferenceFields;
    if (!child && validated && !(fields & ~validated->fields) &&
        !((validated->fields & ~fields) & ~removable)) {
        desired_bytes = validated->bytes;
        if (!(fields & gateway_message_field_mentions)) desired_bytes -= validated->mention_bytes;
        if (!(fields & kReferenceFields)) desired_bytes -= validated->reference_bytes;
    } else if (!validate_input(input, fields, &desired_bytes)) {
        return reject(MessageRichStoreError::invalid_utf8);
    }
    if (!child && (fields & gateway_message_field_message_snapshots) && input.snapshot.present)
        desired_bytes -= input.snapshot.content.bytes; // retained by the child, not the parent
    if (!search.found && fields == 0)
        return result;
    if (!search.found && !child && store->count >= store->capacity)
        return reject(MessageRichStoreError::record_capacity);
    u16 slot = search.slot;
    if (!search.found) {
        slot = static_cast<u16>(free_slot(*store, 0, 0xFFFFu));
        if (slot == 0xFFFFu && extend_records(store))
            slot = static_cast<u16>(store->record_extent - 1u);
        if (slot == 0xFFFFu)
            return reject(MessageRichStoreError::record_capacity);
    }

    const u32 capacity = bank_capacity(*store);
    if (store->text_used > capacity)
        return reject(MessageRichStoreError::corrupt_state);
    const u32 previous_compactions = store->compactions;
    if (desired_bytes > capacity - store->text_used && !compact_excluding(store, slot, search.found, fields))
        return reject(MessageRichStoreError::corrupt_state);
    // Blocks for the replaced arrays come first, so running out of side storage rejects the update
    // before the record changes.
    ArrayPlan plan{};
    if (!plan_arrays(&store->side, store->records[slot], input, fields, &plan))
        return reject(MessageRichStoreError::record_capacity);
    MessageRichRecord* record = &store->records[slot];
    if (!search.found) {
        // A free slot is zero except for its generation (erase_slot,
        // erase_snapshot_slot, abandon, initialize_store and extend_records
        // leave it so), which stays.
        record->forwarded_snapshot_child = child;
        record->message_id = message_id;
        record->occupied = true;
        for (u32 index = !child ? store->count : 0; index > search.order_index; --index) {
            store->order[index] = store->order[index - 1];
        }
        if (!child) {
            store->order[search.order_index] = slot;
            ++store->count;
        } else
            ++store->snapshot_count;
        result.inserted = true;
    } else if (fields != 0 && store->compactions == previous_compactions) {
        u64 wasted = 0;
        if (!walk_strings(record, fields, count_wasted, &wasted) || wasted > ~0u - store->text_wasted) {
            discard_plan(&store->side, plan);
            return reject(MessageRichStoreError::corrupt_state);
        }
        store->text_wasted += static_cast<u32>(wasted);
    }
    clear_replaced(record, fields);
    commit_plan(&store->side, record, plan);
    record->truncations &= ~replaced_truncation_mask(fields);
    bool storage_truncated = desired_bytes > capacity - store->text_used;
    copy_input(store, record, input, fields, &storage_truncated);
    if (input.fields & gateway_message_field_flags) {
        constexpr u32 kComponentsV2Flag = 1u << 15;
        record->voice_message = (input.flags & kGatewayMessageVoiceFlag) != 0;
        record->components_v2 = (input.flags & kComponentsV2Flag) != 0;
    }
    if (!child && (input.fields & gateway_message_field_type))
        record->system.message_type = input.type;
    record->message_revision = message_revision;
    record->truncations |= input.rich_truncations;
    if (storage_truncated) {
        record->truncations |= gateway_message_rich_truncated_text;
    }
    ++store->updated;
    result.storage_truncated = storage_truncated;

    if (!record_has_content(*record))
        return erase_slot(store, search.order_index, slot) ? result : reject(MessageRichStoreError::corrupt_state);
    result.record = record;
    return result;
}

const MessageRichRecord* message_rich_store_snapshot(const MessageRichStore& store,
                                                     u64 outer_message_id) noexcept {
    if (!store.records || !store.order)
        return nullptr;
    const auto found = find_record(store, outer_message_id);
    if (!found.found)
        return nullptr;
    const auto& outer = store.records[found.slot];
    if (!outer.has_forwarded_snapshot || outer.forwarded_snapshot_child ||
        outer.forwarded_snapshot_slot >= store.record_extent)
        return nullptr;
    const auto& child = store.records[outer.forwarded_snapshot_slot];
    return child.occupied && child.forwarded_snapshot_child && child.snapshot.present &&
                   child.message_id == outer.message_id && child.message_revision == outer.message_revision
               ? &child
               : nullptr;
}

MessageRichStoreResult message_rich_store_apply(MessageRichStore* store, u64 message_id,
                                                u32 message_revision,
                                                const GatewayMessageEvent& input) noexcept {
    if (!store || !store->records || !store->order || !message_id || !message_revision ||
        input.id != message_id)
        return {nullptr, MessageRichStoreError::invalid_input, false, false};
    const auto found = find_record(*store, message_id);
    const auto* outer = found.found ? &store->records[found.slot] : nullptr;
    const auto* previous = outer ? message_rich_store_snapshot(*store, message_id) : nullptr;
    if (outer && outer->has_forwarded_snapshot && !previous)
        return {nullptr, MessageRichStoreError::corrupt_state, false, false};
    const bool supplied = (input.fields & gateway_message_field_message_snapshots) != 0;
    if (!supplied && input.snapshot.present)
        return {nullptr, MessageRichStoreError::invalid_input, false, false};
    if (!outer && (!(input.fields & kRichFields) || store->count >= store->capacity))
        return apply_body(store, message_id, message_revision, input);
    const u16 previous_slot = previous ? outer->forwarded_snapshot_slot : 0;
    GatewayMessageEvent body{};
    constexpr u32 body_fields = gateway_message_field_attachments | gateway_message_field_embeds |
                                gateway_message_field_sticker_items | gateway_message_field_components |
                                gateway_message_field_message_snapshots | gateway_message_field_flags;
    if (supplied && input.snapshot.present) {
        body.id = message_id;
        body.fields = body_fields;
        body.flags = input.snapshot.flags;
        body.snapshot = input.snapshot;
        body.attachments = input.snapshot.attachments;
        body.embeds = input.snapshot.embeds;
        body.embed_fields = input.snapshot.embed_fields;
        body.stickers = input.snapshot.stickers;
        body.components = input.snapshot.components;
        body.rich_truncations = input.snapshot.rich_truncations;
        u64 bytes = 0;
        if (input.snapshot.fields & gateway_message_field_message_snapshots)
            return {nullptr, MessageRichStoreError::invalid_input, false, false};
        if (!validate_input(body, body_fields, &bytes)) {
            ++store->rejected;
            return {nullptr, MessageRichStoreError::invalid_utf8, false, false};
        }
    }
    u64 bytes = 0;
    if (!validate_input(input, input.fields & kRichFields, &bytes)) {
        ++store->rejected;
        return {nullptr, MessageRichStoreError::invalid_utf8, false, false};
    }
    // Empty array fields cannot retain a new record. Preserve the logical
    // insert/delete counters without a physical slot or two full record clears.
    constexpr u32 empty_arrays = gateway_message_field_reactions | gateway_message_field_attachments |
                                 gateway_message_field_embeds | gateway_message_field_sticker_items |
                                 gateway_message_field_components;
    if (!outer && store->count < store->record_capacity &&
        store->snapshot_count < store->record_capacity - store->count && (input.fields & empty_arrays) &&
        !(input.fields & kRichFields & ~(empty_arrays | gateway_message_field_mentions)) &&
        !(input.reactions.count | input.attachments.count | input.embeds.count | input.embed_fields.count |
          input.stickers.count | input.components.count | input.mention_users.count | input.rich_truncations) &&
        !(input.flags & kGatewayMessageVoiceFlag) && store->text && store->text_used <= bank_capacity(*store)) {
        ++store->updated;
        ++store->deleted;
        return {nullptr, MessageRichStoreError::none, true, false};
    }
    // Full validation already checked these borrowed strings. Capture scalar
    // contributions before publication; apply_body can omit them without a rescan.
    const u32 validated_fields = input.fields & kRichFields;
    const RichValidationPlan validated{
        bytes,
        (validated_fields & kReferenceFields)
            ? static_cast<u64>(input.reference.author_name.bytes) + input.reference.author_avatar_hash.bytes +
                  input.reference.content.bytes
            : 0,
        validated_fields,
        (validated_fields & gateway_message_field_mentions) && input.mention_users.count
            ? input.mention_users[0].name.bytes : 0};
    // Admit every physical slot before publishing the parent or changing text.
    const u32 needed = (!outer ? 1u : 0u) + (supplied && input.snapshot.present && !previous ? 1u : 0u);
    while (needed) {
        u32 free = 0;
        for (u32 index = 0; index < store->record_extent; ++index)
            free += !store->records[index].occupied;
        if (free >= needed)
            break;
        if (!extend_records(store))
            return {nullptr, MessageRichStoreError::record_capacity, false, false};
    }
    const bool removed_snapshot = supplied && !input.snapshot.present && previous;
    if (removed_snapshot) {
        if (!erase_snapshot_slot(store, previous_slot))
            return {nullptr, MessageRichStoreError::corrupt_state, false, false};
        store->records[found.slot].has_forwarded_snapshot = false;
    }
    auto result = apply_body(store, message_id, message_revision, input, false, 0,
                             removed_snapshot ? nullptr : &validated);
    if (!result.ok() || !result.record)
        return result;
    const auto resident = find_record(*store, message_id);
    if (!resident.found)
        return {nullptr, MessageRichStoreError::corrupt_state, false, false};
    if (!supplied) {
        if (previous)
            store->records[previous_slot].message_revision = message_revision;
        return result;
    }
    if (!input.snapshot.present)
        return result;
    u16 slot = previous_slot;
    if (!previous) {
        slot = static_cast<u16>(free_slot(*store, 0, 0xFFFFu));
        if (slot == 0xFFFFu)
            return {nullptr, MessageRichStoreError::corrupt_state, false, false};
    }
    const auto copied = apply_body(store, message_id, message_revision, body, true, slot);
    if (!copied.ok())
        return copied;
    store->records[resident.slot].forwarded_snapshot_slot = slot;
    store->records[resident.slot].has_forwarded_snapshot = true;
    result.record = &store->records[resident.slot];
    result.storage_truncated = result.storage_truncated || copied.storage_truncated;
    return result;
}

MessageRichStoreResult message_rich_store_mutate_reaction(
    MessageRichStore* store, u64 message_id, u32 message_revision,
    const MessageRichReactionMutation& mutation) noexcept {
    MessageRichStoreResult result{};
    const auto reject = [&](MessageRichStoreError error) noexcept {
        result.error = error;
        if (store)
            ++store->rejected;
        return result;
    };
    const bool needs_emoji = mutation.kind != MessageRichReactionMutationKind::remove_all;
    if (!store || !store->records || !store->order || !store->text || message_id == 0 ||
        message_revision == 0 ||
        (needs_emoji && ((!mutation.name.data && mutation.name.bytes != 0) ||
                         (mutation.name.bytes == 0 && mutation.emoji_id == 0) ||
                         mutation.name.bytes > kMessageReactionMaximumEmojiNameBytes ||
                         !utf8_validate(mutation.name).valid)))
        return reject(MessageRichStoreError::invalid_input);

    const SearchResult search = find_record(*store, message_id);
    if (!search.found) {
        if (mutation.kind != MessageRichReactionMutationKind::add)
            return result;
        // The first reaction creates the record through the ordinary path.
        GatewayMessageReaction reaction{};
        reaction.emoji_id = mutation.emoji_id;
        reaction.name = mutation.name;
        reaction.count = 1;
        reaction.animated = mutation.animated;
        if (mutation.type == MessageReactionType::normal) {
            reaction.normal_count = 1;
            reaction.me = mutation.current_user;
        } else if (mutation.type == MessageReactionType::burst) {
            reaction.burst_count = 1;
            reaction.me_burst = mutation.current_user;
        }
        GatewayMessageEvent input{};
        input.id = message_id;
        input.fields = gateway_message_field_reactions;
        input.reactions = ConstSlice<GatewayMessageReaction>{&reaction, 1};
        return message_rich_store_apply(store, message_id, message_revision, input);
    }
    MessageRichRecord* const record = &store->records[search.slot];
    if (record->message_revision != message_revision)
        return reject(MessageRichStoreError::invalid_input);

    // Custom names are mutable wire metadata. Their snowflake owns identity;
    // Unicode has no snowflake and retains exact wire bytes.
    u32 match = record->reaction_count;
    for (u32 index = 0; index < record->reaction_count; ++index) {
        const MessageRichReaction& item = record->reactions[index];
        if (item.emoji_id != mutation.emoji_id)
            continue;
        const Utf8View name = stored_text(*store, item.name);
        if (item.emoji_id != 0 || (name.bytes == mutation.name.bytes && bytes_equal(name.data, mutation.name.data, name.bytes)))
            match = index;
    }
    const auto drop = [&](u32 index) noexcept {
        store->text_wasted += record->reactions[index].name.bytes;
        for (u32 next = index + 1; next < record->reaction_count; ++next)
            record->reactions[next - 1] = record->reactions[next];
        --record->reaction_count;
    };

    bool truncated = false;
    if (mutation.kind == MessageRichReactionMutationKind::remove_all) {
        while (record->reaction_count != 0)
            drop(record->reaction_count - 1);
    } else if (mutation.kind == MessageRichReactionMutationKind::remove_emoji) {
        if (match == record->reaction_count) {
            result.record = record;
            return result;
        }
        drop(match);
    } else if (mutation.kind == MessageRichReactionMutationKind::add) {
        if (match == record->reaction_count) {
            if (record->reaction_count >= kGatewayMessageReactionMaximum)
                return reject(MessageRichStoreError::record_capacity);
            const u32 capacity = bank_capacity(*store);
            if (store->text_used > capacity ||
                (mutation.name.bytes > capacity - store->text_used &&
                 !compact_excluding(store, search.slot, false, 0)))
                return reject(MessageRichStoreError::corrupt_state);
            bool fresh = false;
            void* const grown = fit_block(&store->side, kind_reaction, record->reactions,
                                          record->reaction_count + 1u, &fresh);
            if (!grown)
                return reject(MessageRichStoreError::record_capacity);
            if (fresh) {
                copy_bytes(grown, record->reactions, record->reaction_count * static_cast<u32>(sizeof(MessageRichReaction)));
                release_block(&store->side, kind_reaction, record->reactions);
                record->reactions = static_cast<MessageRichReaction*>(grown);
            }
            MessageRichReaction& item = record->reactions[match];
            reset_value<MessageRichReaction>(&item);
            item.emoji_id = mutation.emoji_id;
            item.name = store_string(store, mutation.name, &truncated);
            item.flags = mutation.animated ? u8{4} : u8{0};
            ++record->reaction_count;
        }
        MessageRichReaction& item = record->reactions[match];
        const bool owned = mutation.current_user && ((mutation.type == MessageReactionType::normal && (item.flags & 1u)) ||
                                                      (mutation.type == MessageReactionType::burst && (item.flags & 2u)));
        if (owned) {
            result.record = record;
            return result;
        }
        ++item.count;
        if (mutation.type == MessageReactionType::normal) {
            ++item.normal_count;
            if (mutation.current_user)
                item.flags = static_cast<u8>(item.flags | 1u);
        } else if (mutation.type == MessageReactionType::burst) {
            ++item.burst_count;
            if (mutation.current_user)
                item.flags = static_cast<u8>(item.flags | 2u);
        }
    } else {
        if (match == record->reaction_count) {
            result.record = record;
            return result;
        }
        MessageRichReaction& item = record->reactions[match];
        const bool owned = !mutation.current_user ||
                           (mutation.type == MessageReactionType::normal && (item.flags & 1u)) ||
                           (mutation.type == MessageReactionType::burst && (item.flags & 2u));
        const u32 vote_count = item.count - item.normal_count - item.burst_count;
        const bool has_type =
            (mutation.type == MessageReactionType::normal && item.normal_count != 0) ||
            (mutation.type == MessageReactionType::burst && item.burst_count != 0) ||
            (mutation.type == MessageReactionType::vote && vote_count != 0);
        if (!owned || !has_type || item.count == 0) {
            result.record = record;
            return result;
        }
        --item.count;
        if (mutation.type == MessageReactionType::normal) {
            --item.normal_count;
            if (mutation.current_user)
                item.flags = static_cast<u8>(item.flags & ~1u);
        } else if (mutation.type == MessageReactionType::burst) {
            --item.burst_count;
            if (mutation.current_user)
                item.flags = static_cast<u8>(item.flags & ~2u);
        }
        if (item.count == 0)
            drop(match);
    }

    if (record->reaction_count == 0) {
        release_block(&store->side, kind_reaction, record->reactions);
        record->reactions = static_cast<MessageRichReaction*>(kNoItems[kind_reaction]);
    }
    record->truncations &= ~gateway_message_rich_truncated_reactions;
    if (truncated)
        record->truncations |= gateway_message_rich_truncated_text;
    ++store->updated;
    result.storage_truncated = truncated;
    if (!record_has_content(*record))
        return erase_slot(store, search.order_index, search.slot) ? result : reject(MessageRichStoreError::corrupt_state);
    result.record = record;
    return result;
}

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
    for (u32 index = store->count; index > search.order_index; --index)
        store->order[index] = store->order[index - 1];
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
