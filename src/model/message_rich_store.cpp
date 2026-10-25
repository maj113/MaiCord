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

bool visit_string(MessageRichString* value, StringVisitor visitor, void* context) noexcept {
    return visitor(context, value);
}

bool walk_embed_media_strings(MessageRichEmbedMedia* media, StringVisitor visitor, void* context) noexcept {
    return visit_string(&media->url, visitor, context) &&
           visit_string(&media->proxy_url, visitor, context) &&
           visit_string(&media->content_type, visitor, context) &&
           visit_string(&media->description, visitor, context);
}

bool walk_strings(MessageRichRecord* record, u32 fields, StringVisitor visitor, void* context) noexcept {
    if ((fields & gateway_message_field_interaction) != 0 &&
        (!visit_string(&record->interaction.user_name, visitor, context) ||
         !visit_string(&record->interaction.user_avatar_hash, visitor, context) ||
         !visit_string(&record->interaction.command_name, visitor, context)))
        return false;
    if ((fields & gateway_message_field_thread) && !visit_string(&record->thread.name, visitor, context))
        return false;
    if ((fields & gateway_message_field_mentions) && !visit_string(&record->system.target_name, visitor, context))
        return false;
    if (record->forwarded_snapshot_child && (fields & gateway_message_field_message_snapshots) &&
        !visit_string(&record->snapshot.content, visitor, context))
        return false;
    if (!record->forwarded_snapshot_child && (fields & kReferenceFields) != 0) {
        if (!visit_string(&record->reference.author_name, visitor, context) ||
            !visit_string(&record->reference.author_avatar_hash, visitor, context) ||
            !visit_string(&record->reference.content, visitor, context)) {
            return false;
        }
    }
    if ((fields & gateway_message_field_reactions) != 0) {
        for (u32 index = 0; index < record->reaction_count; ++index) {
            if (!visit_string(&record->reactions[index].name, visitor, context)) {
                return false;
            }
        }
    }
    if ((fields & gateway_message_field_attachments) != 0) {
        for (u32 index = 0; index < record->attachment_count; ++index) {
            MessageRichAttachment& item = record->attachments[index];
            if (!visit_string(&item.filename, visitor, context) ||
                !visit_string(&item.title, visitor, context) ||
                !visit_string(&item.description, visitor, context) ||
                !visit_string(&item.content_type, visitor, context) ||
                !visit_string(&item.url, visitor, context) ||
                !visit_string(&item.waveform, visitor, context)) {
                return false;
            }
        }
    }
    if ((fields & gateway_message_field_embeds) != 0) {
        for (u32 index = 0; index < record->embed_count; ++index) {
            MessageRichEmbed& item = record->embeds[index];
            if (!visit_string(&item.title, visitor, context) ||
                !visit_string(&item.description, visitor, context) ||
                !visit_string(&item.url, visitor, context) ||
                !visit_string(&item.author_name, visitor, context) ||
                !visit_string(&item.footer_text, visitor, context) ||
                !visit_string(&item.author_url, visitor, context) ||
                !visit_string(&item.author_icon, visitor, context) ||
                !visit_string(&item.footer_icon, visitor, context) ||
                !visit_string(&item.provider_name, visitor, context) ||
                !visit_string(&item.provider_url, visitor, context) ||
                !walk_embed_media_strings(&item.image, visitor, context) ||
                !walk_embed_media_strings(&item.thumbnail, visitor, context) ||
                !walk_embed_media_strings(&item.video, visitor, context)) {
                return false;
            }
        }
        for (u32 index = 0; index < record->embed_field_count; ++index) {
            if (!visit_string(&record->embed_fields[index].name, visitor, context) ||
                !visit_string(&record->embed_fields[index].value, visitor, context)) {
                return false;
            }
        }
    }
    if ((fields & gateway_message_field_sticker_items) != 0) {
        for (u32 index = 0; index < record->sticker_count; ++index) {
            if (!visit_string(&record->stickers[index].name, visitor, context)) {
                return false;
            }
        }
    }
    if ((fields & gateway_message_field_components) != 0) {
        for (u32 index = 0; index < record->component_count; ++index) {
            MessageRichComponent& item = record->components[index];
            if (!visit_string(&item.label, visitor, context) ||
                !visit_string(&item.content, visitor, context) ||
                !visit_string(&item.custom_id, visitor, context) ||
                !visit_string(&item.url, visitor, context) ||
                !visit_string(&item.emoji_name, visitor, context)) {
                return false;
            }
        }
    }
    return true;
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
    for (u32 byte = 0; byte < value->bytes; ++byte) {
        store.text[context->destination_base + context->cursor + byte] = store.text[value->offset + byte];
    }
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
        if (slot >= store.record_extent || !store.records[slot].occupied) {
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

bool add_bytes(u64* total, Utf8View value) noexcept {
    if ((!value.data && value.bytes != 0) || value.bytes > 0xFFFFu ||
        !utf8_validate(value).valid || *total > ~0ull - value.bytes) {
        return false;
    }
    *total += value.bytes;
    return true;
}

bool validate_embed_media(const GatewayMessageEmbedMedia& media, u64* desired_bytes) noexcept {
    return (!media.has_width || media.width != 0) &&
           (!media.has_height || media.height != 0) && add_bytes(desired_bytes, media.url) &&
           add_bytes(desired_bytes, media.proxy_url) && add_bytes(desired_bytes, media.content_type) &&
           add_bytes(desired_bytes, media.description);
}

bool validate_attachments(ConstSlice<GatewayMessageAttachment> attachments, u64* desired_bytes) noexcept {
    if (attachments.count > kGatewayMessageAttachmentMaximum ||
        (!attachments.data && attachments.count))
        return false;
    for (u32 index = 0; index < attachments.count; ++index) {
        const auto& item = attachments[index];
        if (!item.id || !add_bytes(desired_bytes, item.filename) || !add_bytes(desired_bytes, item.title) ||
            !add_bytes(desired_bytes, item.description) || !add_bytes(desired_bytes, item.content_type) ||
            !add_bytes(desired_bytes, item.url) || !gateway_message_waveform_valid(item.waveform) ||
            !add_bytes(desired_bytes, item.waveform) || (!item.has_duration && item.duration_ms))
            return false;
    }
    return true;
}
bool validate_input(const GatewayMessageEvent& input, u32 fields, u64* desired_bytes) noexcept {
    if (input.reactions.count > kGatewayMessageReactionMaximum ||
        input.attachments.count > kGatewayMessageAttachmentMaximum ||
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
        (!add_bytes(desired_bytes, input.interaction.user_name) ||
         !add_bytes(desired_bytes, input.interaction.user_avatar_hash) ||
         !add_bytes(desired_bytes, input.interaction.command_name)))
        return false;
    if ((fields & gateway_message_field_thread) && !add_bytes(desired_bytes, input.thread.name))
        return false;
    if ((fields & gateway_message_field_mentions) && input.mention_users.count &&
        !add_bytes(desired_bytes, input.mention_users[0].name))
        return false;
    if ((fields & kReferenceFields) != 0) {
        if (!add_bytes(desired_bytes, input.reference.author_name) ||
            !add_bytes(desired_bytes, input.reference.author_avatar_hash) ||
            !add_bytes(desired_bytes, input.reference.content)) {
            return false;
        }
    }
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
            !add_bytes(desired_bytes, item.title) || !add_bytes(desired_bytes, item.description) ||
            !add_bytes(desired_bytes, item.url) || !add_bytes(desired_bytes, item.author_name) ||
            !add_bytes(desired_bytes, item.footer_text) || !add_bytes(desired_bytes, item.author_url) ||
            !add_bytes(desired_bytes, item.author_icon) || !add_bytes(desired_bytes, item.footer_icon) ||
            !add_bytes(desired_bytes, item.provider_name) || !add_bytes(desired_bytes, item.provider_url) ||
            !validate_embed_media(item.image, desired_bytes) ||
            !validate_embed_media(item.thumbnail, desired_bytes) ||
            !validate_embed_media(item.video, desired_bytes)) {
            return false;
        }
    }
    for (u32 index = 0; index < input.embed_fields.count; ++index) {
        if (input.embed_fields[index].embed_index >= input.embeds.count ||
            !add_bytes(desired_bytes, input.embed_fields[index].name) ||
            !add_bytes(desired_bytes, input.embed_fields[index].value)) {
            return false;
        }
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
            !add_bytes(desired_bytes, item.label) || !add_bytes(desired_bytes, item.content) ||
            !add_bytes(desired_bytes, item.custom_id) || !add_bytes(desired_bytes, item.url) ||
            !add_bytes(desired_bytes, item.emoji_name)) {
            return false;
        }
    }
    return true;
}

MessageRichString store_string(MessageRichStore* store, Utf8View value, bool* truncated) noexcept {
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

bool count_wasted(void* opaque, MessageRichString* value) noexcept {
    auto* total = static_cast<u64*>(opaque);
    if (*total > ~0ull - value->bytes) {
        return false;
    }
    *total += value->bytes;
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
    reset_value(&store->records[slot]);
    store->records[slot].generation = static_cast<u16>(generation == 0xFFFFu ? 1u : generation + 1u);
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
    reset_value<MessageRichRecord>(&store->records[slot]);
    store->records[slot].generation = generation == 0xFFFFu ? 1u : static_cast<u16>(generation + 1u);
    for (u32 index = order_index + 1; index < store->count; ++index) {
        store->order[index - 1] = store->order[index];
    }
    --store->count;
    ++store->deleted;
    return true;
}

bool initialize_store(MessageRichStore* store, MessageRichRecord* records, StableStorage* record_storage,
                      u32 capacity, u32 record_extent, Slice<u16> order, Slice<u8> text,
                      u32 physical_capacity, bool sparse_text = false) noexcept {
    if (!store || !records || capacity == 0 || physical_capacity > 0xFFFFu ||
        capacity > physical_capacity || record_extent > physical_capacity || !order.data ||
        order.count < capacity || !text.data || text.count < 2 || (text.count & 1u) != 0) {
        return false;
    }
    reset_value<MessageRichStore>(store);
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
        reset_value<MessageRichRecord>(&records[index]);
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
        reset_value<MessageRichRecord>(&store->records[index]);
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
    if (store->record_storage) {
        stable_storage_release(store->record_storage);
    }
    reset_value<MessageRichStore>(store);
}

MediaAttachmentFormat message_rich_attachment_audio_format(
    const MessageRichStore& store, const MessageRichAttachment& attachment) noexcept {
    const auto equal_case = [](Utf8View value, Utf8View suffix, bool tail) noexcept {
        if (!value.data || value.bytes < suffix.bytes || (!tail && value.bytes != suffix.bytes))
            return false;
        const u32 start = tail ? value.bytes - suffix.bytes : 0;
        for (u32 index = 0; index < suffix.bytes; ++index) {
            char byte = value.data[start + index];
            if (byte >= 'A' && byte <= 'Z')
                byte += 'a' - 'A';
            if (byte != suffix.data[index])
                return false;
        }
        return true;
    };
    auto type = message_rich_store_string(store, attachment.content_type);
    const auto name = message_rich_store_string(store, attachment.filename);
    // Parameters describe the same MIME token; display strings remain intact.
    for (u32 i = 0; i < type.bytes; ++i)
        if (type.data[i] == ';') {
            type.bytes = i;
            break;
        }
    while (type.bytes && (type.data[type.bytes - 1] == ' ' || type.data[type.bytes - 1] == '\t'))
        --type.bytes;
    if (equal_case(type, utf8_literal("audio/ogg"), false))
        return MediaAttachmentFormat::ogg_opus;
    if (equal_case(type, utf8_literal("audio/mpeg"), false) ||
        equal_case(type, utf8_literal("audio/mp3"), false))
        return MediaAttachmentFormat::mp3;
    if (equal_case(type, utf8_literal("audio/wav"), false) ||
        equal_case(type, utf8_literal("audio/x-wav"), false) ||
        equal_case(type, utf8_literal("audio/vnd.wave"), false))
        return MediaAttachmentFormat::wav;
    const bool generic = !type.bytes || equal_case(type, utf8_literal("application/octet-stream"), false) ||
                         equal_case(type, utf8_literal("binary/octet-stream"), false);
    if (generic && equal_case(name, utf8_literal(".mp3"), true))
        return MediaAttachmentFormat::mp3;
    if (generic && equal_case(name, utf8_literal(".wav"), true))
        return MediaAttachmentFormat::wav;
    // MPEG-4 audio: AAC in an audio-only MP4/QuickTime container.
    if (equal_case(type, utf8_literal("audio/mp4"), false) || equal_case(type, utf8_literal("audio/x-m4a"), false) ||
        equal_case(type, utf8_literal("audio/m4a"), false) ||
        (generic && equal_case(name, utf8_literal(".m4a"), true)))
        return MediaAttachmentFormat::m4a;
    // Ogg/Opus is admitted by filename alone; the demuxer rejects non-Ogg data
    // before any native playback resource is opened.
    if (equal_case(name, utf8_literal(".ogg"), true) || equal_case(name, utf8_literal(".opus"), true))
        return MediaAttachmentFormat::ogg_opus;
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
    if (!store->records || !store->order || !store->text || store->capacity == 0 ||
        message_id == 0 || message_revision == 0 || input.id != message_id) {
        result.error = MessageRichStoreError::invalid_input;
        ++store->rejected;
        return result;
    }
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
        result.error = MessageRichStoreError::invalid_utf8;
        ++store->rejected;
        return result;
    }
    if (!child && (fields & gateway_message_field_message_snapshots) && input.snapshot.present)
        desired_bytes -= input.snapshot.content.bytes; // retained by the child, not the parent
    if (!search.found && fields == 0) {
        return result;
    }
    if (!search.found && !child && store->count >= store->capacity) {
        result.error = MessageRichStoreError::record_capacity;
        ++store->rejected;
        return result;
    }
    u16 slot = search.slot;
    if (!search.found) {
        slot = 0xFFFFu;
        for (u32 index = 0; index < store->record_extent; ++index) {
            if (!store->records[index].occupied) {
                slot = static_cast<u16>(index);
                break;
            }
        }
        if (slot == 0xFFFFu && extend_records(store)) {
            slot = static_cast<u16>(store->record_extent - 1u);
        }
        if (slot == 0xFFFFu) {
            result.error = MessageRichStoreError::record_capacity;
            ++store->rejected;
            return result;
        }
    }

    const u32 capacity = bank_capacity(*store);
    if (store->text_used > capacity) {
        result.error = MessageRichStoreError::corrupt_state;
        ++store->rejected;
        return result;
    }
    const u32 previous_compactions = store->compactions;
    if (desired_bytes > capacity - store->text_used &&
        !compact_excluding(store, slot, search.found, fields)) {
        result.error = MessageRichStoreError::corrupt_state;
        ++store->rejected;
        return result;
    }
    MessageRichRecord* record = &store->records[slot];
    if (!search.found) {
        const u16 generation = record->generation == 0 ? 1 : record->generation;
        reset_value<MessageRichRecord>(record);
        record->forwarded_snapshot_child = child;
        record->generation = generation;
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
            result.error = MessageRichStoreError::corrupt_state;
            ++store->rejected;
            return result;
        }
        store->text_wasted += static_cast<u32>(wasted);
    }
    clear_replaced(record, fields);
    record->truncations &= ~replaced_truncation_mask(fields);
    bool storage_truncated = desired_bytes > capacity - store->text_used;
    copy_input(store, record, input, fields, &storage_truncated);
    if (input.fields & gateway_message_field_flags)
        record->voice_message = (input.flags & kGatewayMessageVoiceFlag) != 0;
    if (!child && (input.fields & gateway_message_field_type))
        record->system.message_type = input.type;
    record->message_revision = message_revision;
    record->truncations |= input.rich_truncations;
    if (storage_truncated) {
        record->truncations |= gateway_message_rich_truncated_text;
    }
    ++store->updated;
    result.storage_truncated = storage_truncated;

    if (!record_has_content(*record)) {
        if (!erase_slot(store, search.order_index, slot)) {
            result.error = MessageRichStoreError::corrupt_state;
            ++store->rejected;
            return result;
        }
        return result;
    }
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
        slot = 0xFFFFu;
        for (u32 index = 0; index < store->record_extent; ++index)
            if (!store->records[index].occupied) {
                slot = static_cast<u16>(index);
                break;
            }
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
    const u8 type = static_cast<u8>(mutation.type);
    const bool needs_emoji = mutation.kind != MessageRichReactionMutationKind::remove_all;
    if (!store || !store->records || !store->order || !store->text || message_id == 0 ||
        message_revision == 0 || type > static_cast<u8>(MessageReactionType::vote) ||
        (needs_emoji && ((!mutation.name.data && mutation.name.bytes != 0) ||
                         (mutation.name.bytes == 0 && mutation.emoji_id == 0) ||
                         mutation.name.bytes > kMessageReactionMaximumEmojiNameBytes ||
                         !utf8_validate(mutation.name).valid))) {
        result.error = MessageRichStoreError::invalid_input;
        if (store) {
            ++store->rejected;
        }
        return result;
    }

    const SearchResult search = find_record(*store, message_id);
    const MessageRichRecord* existing = search.found ? &store->records[search.slot] : nullptr;
    if (existing && existing->message_revision != message_revision) {
        result.error = MessageRichStoreError::invalid_input;
        ++store->rejected;
        return result;
    }
    if (!existing && mutation.kind != MessageRichReactionMutationKind::add) {
        return result;
    }

    GatewayMessageReaction reactions[kGatewayMessageReactionMaximum]{};
    char names[kGatewayMessageReactionMaximum][kMessageReactionMaximumEmojiNameBytes]{};
    u32 reaction_count = existing ? existing->reaction_count : 0;
    u32 name_cursor = 0;
    u32 match = reaction_count;
    for (u32 index = 0; index < reaction_count; ++index) {
        const MessageRichReaction& source = existing->reactions[index];
        const Utf8View source_name = message_rich_store_string(*store, source.name);
        if ((!source_name.data && source_name.bytes) || (!source_name.bytes && !source.emoji_id) ||
            source_name.bytes > kMessageReactionMaximumEmojiNameBytes) {
            result.error = MessageRichStoreError::corrupt_state;
            ++store->rejected;
            return result;
        }
        copy_bytes(names[name_cursor], source_name.data, source_name.bytes);
        GatewayMessageReaction& destination = reactions[index];
        destination.emoji_id = source.emoji_id;
        destination.name = Utf8View{names[name_cursor], source_name.bytes};
        destination.count = source.count;
        destination.normal_count = source.normal_count;
        destination.burst_count = source.burst_count;
        destination.me = (source.flags & 1u) != 0;
        destination.me_burst = (source.flags & 2u) != 0;
        destination.animated = (source.flags & 4u) != 0;
        const bool same_id = source.emoji_id == mutation.emoji_id;
        bool same_name = source_name.bytes == mutation.name.bytes;
        for (u32 byte = 0; same_name && byte < source_name.bytes; ++byte) {
            same_name = source_name.data[byte] == mutation.name.data[byte];
        }
        // Custom names are mutable wire metadata. Their snowflake owns
        // identity; Unicode has no snowflake and retains exact wire bytes.
        if (same_id && (source.emoji_id != 0 || same_name)) {
            match = index;
        }
        ++name_cursor;
    }

    if (mutation.kind == MessageRichReactionMutationKind::remove_all) {
        reaction_count = 0;
    } else if (mutation.kind == MessageRichReactionMutationKind::remove_emoji) {
        if (match == reaction_count) {
            result.record = existing;
            return result;
        }
        for (u32 index = match + 1; index < reaction_count; ++index) {
            reactions[index - 1] = reactions[index];
        }
        --reaction_count;
    } else if (mutation.kind == MessageRichReactionMutationKind::add) {
        if (match == reaction_count) {
            if (reaction_count >= kGatewayMessageReactionMaximum) {
                result.error = MessageRichStoreError::record_capacity;
                ++store->rejected;
                return result;
            }
            copy_bytes(names[name_cursor], mutation.name.data, mutation.name.bytes);
            match = reaction_count++;
            reactions[match].emoji_id = mutation.emoji_id;
            reactions[match].name = Utf8View{names[name_cursor], mutation.name.bytes};
            reactions[match].animated = mutation.animated;
        }
        GatewayMessageReaction& item = reactions[match];
        const bool already_owned = mutation.current_user &&
                                   ((mutation.type == MessageReactionType::normal && item.me) ||
                                    (mutation.type == MessageReactionType::burst && item.me_burst));
        if (already_owned) {
            result.record = existing;
            return result;
        }
        ++item.count;
        if (mutation.type == MessageReactionType::normal) {
            ++item.normal_count;
            item.me = item.me || mutation.current_user;
        } else if (mutation.type == MessageReactionType::burst) {
            ++item.burst_count;
            item.me_burst = item.me_burst || mutation.current_user;
        }
    } else {
        if (match == reaction_count) {
            result.record = existing;
            return result;
        }
        GatewayMessageReaction& item = reactions[match];
        const bool owned = !mutation.current_user ||
                           (mutation.type == MessageReactionType::normal && item.me) ||
                           (mutation.type == MessageReactionType::burst && item.me_burst);
        const u32 vote_count = item.count - item.normal_count - item.burst_count;
        const bool has_type =
            (mutation.type == MessageReactionType::normal && item.normal_count != 0) ||
            (mutation.type == MessageReactionType::burst && item.burst_count != 0) ||
            (mutation.type == MessageReactionType::vote && vote_count != 0);
        if (!owned || !has_type || item.count == 0) {
            result.record = existing;
            return result;
        }
        --item.count;
        if (mutation.type == MessageReactionType::normal) {
            --item.normal_count;
            item.me = item.me && !mutation.current_user;
        } else if (mutation.type == MessageReactionType::burst) {
            --item.burst_count;
            item.me_burst = item.me_burst && !mutation.current_user;
        }
        if (item.count == 0) {
            for (u32 index = match + 1; index < reaction_count; ++index) {
                reactions[index - 1] = reactions[index];
            }
            --reaction_count;
        }
    }

    GatewayMessageEvent input{};
    input.id = message_id;
    input.fields = gateway_message_field_reactions;
    input.reactions = ConstSlice<GatewayMessageReaction>{reactions, reaction_count};
    return message_rich_store_apply(store, message_id, message_revision, input);
}

const MessageRichRecord* message_rich_store_find(const MessageRichStore& store, u64 message_id) noexcept {
    if (!store.records || !store.order || message_id == 0) {
        return nullptr;
    }
    const SearchResult search = find_record(store, message_id);
    return search.found ? &store.records[search.slot] : nullptr;
}

bool message_rich_store_set_attachment_dimensions(MessageRichStore* store, u64 message_id,
                                                  u32 message_revision, u64 attachment_id, u32 width,
                                                  u32 height, bool* changed, bool snapshot) noexcept {
    if (!changed) {
        return false;
    }
    *changed = false;
    if (!store || !store->records || !store->order || message_id == 0 || message_revision == 0 ||
        attachment_id == 0 || width == 0 || height == 0) {
        return false;
    }
    const SearchResult search = find_record(*store, message_id);
    if (!search.found) {
        return false;
    }
    if (snapshot && !message_rich_store_snapshot(*store, message_id))
        return false;
    MessageRichRecord& record =
        store->records[snapshot ? store->records[search.slot].forwarded_snapshot_slot : search.slot];
    if (!record.occupied || record.message_revision != message_revision) {
        return false;
    }
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
    if (!store || !store->records || !store->order || !message_id || !message_revision || !width || !height)
        return false;
    const SearchResult search = find_record(*store, message_id);
    if (!search.found)
        return false;
    if (snapshot && !message_rich_store_snapshot(*store, message_id))
        return false;
    auto& record =
        store->records[snapshot ? store->records[search.slot].forwarded_snapshot_slot : search.slot];
    if (!record.occupied || record.message_revision != message_revision ||
        embed_index >= record.embed_count)
        return false;
    auto& embed = record.embeds[embed_index];
    MessageRichEmbedMedia* media = nullptr;
    switch (slot) {
    case MessageRichEmbedMediaSlot::image:
        media = &embed.image;
        break;
    case MessageRichEmbedMediaSlot::thumbnail:
        media = &embed.thumbnail;
        break;
    case MessageRichEmbedMediaSlot::video:
        media = &embed.video;
        break;
    default:
        return false;
    }
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
    if (value.bytes == 0) {
        return Utf8View{};
    }
    const u32 base = bank_base(store);
    if (!store.text || value.offset < base || value.offset > base + store.text_used ||
        value.bytes > base + store.text_used - value.offset) {
        return Utf8View{};
    }
    const Utf8View result{reinterpret_cast<const char*>(store.text + value.offset), value.bytes};
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
                            store->record_capacity, sparse_text);
}

namespace {

struct PackedRichHeader {
    u32 bytes;
    u32 string_bytes;
};

// Head and tail are copied verbatim, so fields added outside the bounded
// arrays are carried without changing the snapshot format.
inline constexpr u32 kRichHead = __builtin_offsetof(MessageRichRecord, reactions);
inline constexpr u32 kRichTailBegin = __builtin_offsetof(MessageRichRecord, components) +
                                      sizeof(MessageRichComponent) * kGatewayMessageComponentMaximum;
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

template <typename T> u8* write_items(u8* cursor, const T* items, u32 count) noexcept {
    copy_bytes(cursor, items, count * static_cast<u32>(sizeof(T)));
    return cursor + count * sizeof(T);
}

template <typename T> const u8* read_items(const u8* cursor, T* items, u32 count) noexcept {
    copy_bytes(items, cursor, count * static_cast<u32>(sizeof(T)));
    return cursor + count * sizeof(T);
}

bool count_string(void* opaque, MessageRichString* value) noexcept {
    auto* total = static_cast<u32*>(opaque);
    if (value->bytes > ~0u - *total)
        return false;
    *total += value->bytes;
    return true;
}

// A null output only sizes the record. The stack copy receives relative
// offsets; only the parts walk_strings and the format read are copied.
bool pack_record(const MessageRichStore& store, const MessageRichRecord& source, u8* output, u32 capacity,
                 u32* written) noexcept {
    const u32 fixed = packed_fixed_bytes(source);
    if (!output) {
        u32 string_bytes = 0;
        if (!walk_strings(const_cast<MessageRichRecord*>(&source), kRichFields, count_string, &string_bytes) ||
            string_bytes > ~0u - fixed)
            return false;
        *written = fixed + string_bytes;
        return true;
    }
    if (fixed > capacity)
        return false;
    MessageRichRecord copy;
    copy_bytes(&copy, &source, kRichHead);
    copy_bytes(reinterpret_cast<u8*>(&copy) + kRichTailBegin, reinterpret_cast<const u8*>(&source) + kRichTailBegin,
               kRichTail);
    copy_bytes(copy.reactions, source.reactions, source.reaction_count * static_cast<u32>(sizeof(MessageRichReaction)));
    copy_bytes(copy.attachments, source.attachments,
               source.attachment_count * static_cast<u32>(sizeof(MessageRichAttachment)));
    copy_bytes(copy.embeds, source.embeds, source.embed_count * static_cast<u32>(sizeof(MessageRichEmbed)));
    copy_bytes(copy.embed_fields, source.embed_fields,
               source.embed_field_count * static_cast<u32>(sizeof(MessageRichEmbedField)));
    copy_bytes(copy.stickers, source.stickers, source.sticker_count * static_cast<u32>(sizeof(MessageRichSticker)));
    copy_bytes(copy.components, source.components,
               source.component_count * static_cast<u32>(sizeof(MessageRichComponent)));
    // Slot and revision bindings are reassigned by unpack.
    copy.message_revision = 0;
    copy.generation = 0;
    copy.forwarded_snapshot_slot = 0;
    PackStringContext strings{&store, output + fixed, capacity - fixed, 0};
    if (!walk_strings(&copy, kRichFields, pack_string, &strings) || strings.cursor > ~0u - fixed)
        return false;
    *written = fixed + strings.cursor;
    const PackedRichHeader header{*written, strings.cursor};
    u8* cursor = output;
    copy_bytes(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    copy_bytes(cursor, &copy, kRichHead);
    cursor += kRichHead;
    copy_bytes(cursor, reinterpret_cast<const u8*>(&copy) + kRichTailBegin, kRichTail);
    cursor += kRichTail;
    cursor = write_items(cursor, copy.reactions, copy.reaction_count);
    cursor = write_items(cursor, copy.attachments, copy.attachment_count);
    cursor = write_items(cursor, copy.embeds, copy.embed_count);
    cursor = write_items(cursor, copy.embed_fields, copy.embed_field_count);
    cursor = write_items(cursor, copy.stickers, copy.sticker_count);
    write_items(cursor, copy.components, copy.component_count);
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

bool parse_record(ConstSlice<u8> input, MessageRichRecord* record, ConstSlice<u8>* strings,
                  u32* consumed) noexcept {
    PackedRichHeader header{};
    if (!input.data || input.count < kPackedRichMinimum)
        return false;
    copy_bytes(&header, input.data, sizeof(header));
    if (header.bytes < kPackedRichMinimum || header.bytes > input.count || header.string_bytes > header.bytes)
        return false;
    reset_value<MessageRichRecord>(record);
    const u8* cursor = input.data + sizeof(header);
    copy_bytes(record, cursor, kRichHead);
    cursor += kRichHead;
    copy_bytes(reinterpret_cast<u8*>(record) + kRichTailBegin, cursor, kRichTail);
    cursor += kRichTail;
    if (record->reaction_count > kGatewayMessageReactionMaximum ||
        record->attachment_count > kGatewayMessageAttachmentMaximum ||
        record->embed_count > kGatewayMessageEmbedMaximum ||
        record->embed_field_count > kGatewayMessageEmbedFieldMaximum ||
        record->sticker_count > kGatewayMessageStickerMaximum ||
        record->component_count > kGatewayMessageComponentMaximum ||
        packed_fixed_bytes(*record) + header.string_bytes != header.bytes)
        return false;
    cursor = read_items(cursor, record->reactions, record->reaction_count);
    cursor = read_items(cursor, record->attachments, record->attachment_count);
    cursor = read_items(cursor, record->embeds, record->embed_count);
    cursor = read_items(cursor, record->embed_fields, record->embed_field_count);
    cursor = read_items(cursor, record->stickers, record->sticker_count);
    cursor = read_items(cursor, record->components, record->component_count);
    *strings = ConstSlice<u8>{cursor, header.string_bytes};
    *consumed = header.bytes;
    return true;
}

u32 free_slot(const MessageRichStore& store, u32 from, u32 skip) noexcept {
    for (u32 pass = 0; pass < 2; ++pass) {
        const u32 end = pass ? minimum(from, store.record_extent) : store.record_extent;
        for (u32 index = pass ? 0 : from; index < end; ++index)
            if (!store.records[index].occupied && index != skip)
                return index;
    }
    return 0xFFFFu;
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
            reset_value<MessageRichRecord>(&record);
            record.generation = generation == 0 ? 1 : generation;
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
        if (!parse_record({packed.data + cursor, packed.count - cursor}, &record, &strings, &consumed) ||
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
