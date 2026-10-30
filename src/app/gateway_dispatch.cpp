#include "maicord/model/user_activity.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/message_event_scratch.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/notification_settings.hpp"

#include "gateway_dispatch_common.hpp"
#include "gateway_user_payload.hpp"
#include "maicord/protocol/upload_allowance.hpp"
#include "gateway_private_dispatch.hpp"
#include "gateway_relationship_dispatch.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "../protocol/json_field_lists.hpp"
#include "maicord/protocol/account_requirement.hpp"
#include "maicord/model/user_profile.hpp"

namespace maicord {
namespace {

static_assert(sizeof(GatewayClientNamedUserMutation) == 16);
static_assert(sizeof(GatewayClientMessageTextRange) == 8);
static_assert(sizeof(GatewayClientMessageRichHeader) == 120);
static_assert(sizeof(GatewayClientMessagePollHeader) == 32);
static_assert(sizeof(GatewayClientMessagePollAnswerMutation) == 32);
static_assert(sizeof(GatewayClientMessagePollCountMutation) == 12);
static_assert(sizeof(GatewayMessagePoll) == 72);
static_assert(sizeof(GatewayMessagePollAnswer) == 56);
static_assert(sizeof(GatewayMessagePollAnswerCount) == 12);
static_assert(sizeof(GatewayMessagePollVoteEvent) == 40);
static_assert(sizeof(GatewayClientMessageReactionMutation) == 32);
static_assert(sizeof(GatewayClientMessageReactionEvent) == 40);
static_assert(sizeof(GatewayClientTypingStartEvent) == 32);
static_assert(sizeof(GatewayClientMessageAttachmentMutation) == 88);
static_assert(sizeof(GatewayClientMessageEmbedMediaMutation) == 48);
static_assert(sizeof(GatewayClientMessageEmbedMutation) == 244);
static_assert(sizeof(GatewayClientMessageEmbedFieldMutation) == 20);
static_assert(sizeof(GatewayClientMessageStickerMutation) == 24);
static_assert(sizeof(GatewayClientMessageComponentMutation) == 72);
static_assert(sizeof(GatewayClientMessageCallMutation) == 16);

GatewayClientDispatchRouteResult route_decode_error(const GatewayEventDecodeResult& decoded) noexcept {
    GatewayClientDispatchRouteResult result =
        route_error(GatewayClientDispatchRouteError::decode, decoded.error);
    result.scan_error = decoded.scan_error;
    result.read_error = decoded.read_error;
    result.error_offset = decoded.error_offset;
    return result;
}

struct ChannelPinsUpdateContext {
    u64 channel_id;
    u32 fields;
    GatewayEventDecodeError error;
};

bool channel_pins_update_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<ChannelPinsUpdateContext*>(opaque);
    const u32 field = protocol_detail::member_bit<"channel_id", "guild_id", "last_pin_timestamp">(name);
    if (!field)
        return true;
    if (context.fields & field) {
        context.error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    context.fields |= field;
    if (field == 1) {
        if (!json_value_to_snowflake(value, &context.channel_id))
            context.error = GatewayEventDecodeError::invalid_channel_id;
    } else if (field == 2 && value.kind != JsonValueKind::null_value) {
        u64 guild_id = 0;
        if (!json_value_to_snowflake(value, &guild_id))
            context.error = GatewayEventDecodeError::invalid_data;
    } else if (field == 4 && value.kind != JsonValueKind::null_value) {
        JsonString timestamp{};
        u64 timestamp_ms = 0;
        if (!json_value_as_string(value, &timestamp) || !parse_iso_timestamp(timestamp, &timestamp_ms))
            context.error = GatewayEventDecodeError::invalid_message_timestamp;
    }
    return context.error == GatewayEventDecodeError::none;
}

GatewayClientDispatchRouteResult route_channel_pins_update(JsonValue data, Slice<u32> scratch,
                                                           u32 maximum_depth,
                                                           GatewayClientEvent* event) noexcept {
    if (data.kind != JsonValueKind::object)
        return route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok()) {
        auto result =
            route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::scan_failed);
        result.scan_error = scan.error;
        result.error_offset = scan.error_offset;
        return result;
    }
    ChannelPinsUpdateContext context{};
    const auto read = json_visit_value_object(data, {scratch.data, scan.count}, maximum_depth,
                                             channel_pins_update_field, &context);
    if (!read.ok()) {
        auto result = route_error(GatewayClientDispatchRouteError::decode,
                                  context.error == GatewayEventDecodeError::none
                                      ? GatewayEventDecodeError::invalid_json
                                      : context.error);
        result.read_error = read.error;
        result.error_offset = read.error_offset;
        return result;
    }
    if ((context.fields & 1u) == 0)
        return route_error(GatewayClientDispatchRouteError::decode,
                           GatewayEventDecodeError::missing_required_field);
    event->dispatch_kind = GatewayClientDispatchKind::channel_pins_update;
    event->message.channel_id = context.channel_id;
    event->message.fields = gateway_message_field_channel_id;
    return {};
}

void route_command_index_update(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                GatewayClientEvent* event) noexcept {
    if (data.kind != JsonValueKind::object)
        return;
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok())
        return;
    struct Context {
        u64 guild_id;
        bool seen;
    } context{};
    const auto read = json_visit_value_object(
        data, {scratch.data, scan.count}, maximum_depth,
        [](void* opaque, JsonString name, JsonValue value) noexcept {
            if (!json_name_is(name, utf8_literal("guild_id")))
                return true;
            auto& fields = *static_cast<Context*>(opaque);
            if (fields.seen)
                return false;
            fields.seen = true;
            return json_value_to_snowflake(value, &fields.guild_id);
        },
        &context);
    if (read.ok() && context.seen && context.guild_id) {
        event->dispatch_kind = GatewayClientDispatchKind::guild_command_index_update;
        event->state.guild_id = context.guild_id;
    }
}

bool copy_event_name(JsonString source, GatewayClientEvent* destination) noexcept {
    const JsonStringDecodeResult size = json_string_decode_utf8(source, Slice<char>{nullptr, 0});
    if (size.error != JsonStringDecodeError::output_too_small ||
        size.bytes >= kGatewayClientEventNameCapacity) {
        return false;
    }
    const JsonStringDecodeResult decoded = json_string_decode_utf8(
        source, Slice<char>{destination->event_name, kGatewayClientEventNameCapacity - 1});
    if (!decoded.ok()) {
        return false;
    }
    destination->event_name[decoded.bytes] = 0;
    destination->event_name_bytes = static_cast<u8>(decoded.bytes);
    return true;
}

void copy_message(const GatewayMessageEvent& source, GatewayClientMessageMutation* destination) noexcept {
    destination->id = source.id;
    destination->nonce = source.nonce;
    destination->channel_id = source.channel_id;
    destination->author_id = source.author_id;
    destination->timestamp_ms = source.timestamp_ms;
    destination->edited_timestamp_ms = source.edited_timestamp_ms;
    destination->flags = source.flags;
    destination->type = source.type;
    destination->author_public_flags = source.author_public_flags;
    // The started-thread object is not carried across this boundary; a field bit
    // without its data would read as "thread: null" and clear a starter card.
    destination->fields = source.fields & ~static_cast<u32>(gateway_message_field_thread);
    destination->content_bytes = source.content.bytes;
    destination->forwarding_features = source.forwarding_features;
    destination->transient_bytes = source.transient_bytes;
    destination->mention_role_count = static_cast<u16>(source.mention_roles.count);
    destination->member_role_count = static_cast<u16>(source.member_roles.count);
    destination->mention_user_count = static_cast<u16>(source.mention_users.count);
    destination->author_name_bytes = static_cast<u8>(source.author_name.bytes);
    destination->member_nick_bytes = static_cast<u8>(source.member_nick.bytes);
    destination->author_avatar_hash_bytes = static_cast<u8>(source.author_avatar_hash.bytes);
    destination->mention_flags = static_cast<u8>(
        (source.mention_everyone ? gateway_client_message_mention_everyone : 0) |
        (source.mentions_current_user ? gateway_client_message_mention_current_user : 0) |
        (source.author_bot ? gateway_client_message_author_bot : 0) |
        (source.activity_instance_present ? gateway_client_message_activity_instance : 0) |
        (source.application_present ? gateway_client_message_application_present : 0) |
        (source.changelog_present ? gateway_client_message_changelog_present : 0) |
        (source.interaction_metadata_present ? gateway_client_message_interaction_metadata_present : 0) |
        (source.pinned ? gateway_client_message_pinned : 0));
}

void copy_reaction(const GatewayMessageReactionEvent& source,
                   GatewayClientMessageReactionEvent* destination) noexcept {
    destination->message_id = source.message_id;
    destination->channel_id = source.channel_id;
    destination->user_id = source.user_id;
    destination->emoji_id = source.emoji_id;
    destination->name_bytes = static_cast<u16>(source.name.bytes);
    destination->type = static_cast<u8>(source.type);
    destination->flags = source.animated ? 1u : 0u;
}

template <typename Value> bool append_packed(Slice<char> output, u32* cursor, const Value& value) noexcept {
    if (*cursor > output.count || sizeof(Value) > output.count - *cursor) {
        return false;
    }
    copy_bytes(output.data + *cursor, &value, static_cast<u32>(sizeof(Value)));
    *cursor += static_cast<u32>(sizeof(Value));
    return true;
}

bool pack_text_range(Slice<char> output, u32 transient_bytes, Utf8View value,
                     GatewayClientMessageTextRange* range) noexcept {
    if (transient_bytes > output.count || value.bytes > 0xFFFFu) {
        return false;
    }
    reset_value<GatewayClientMessageTextRange>(range);
    if (value.bytes == 0) {
        return true;
    }
    if (!value.data || value.data < output.data || value.data > output.data + transient_bytes ||
        value.bytes > static_cast<u32>(output.data + transient_bytes - value.data)) {
        return false;
    }
    range->offset = static_cast<u32>(value.data - output.data);
    range->bytes = static_cast<u16>(value.bytes);
    return true;
}

bool pack_embed_media(Slice<char> output, u32 transient_bytes, const GatewayMessageEmbedMedia& source,
                      GatewayClientMessageEmbedMediaMutation* destination) noexcept {
    reset_value<GatewayClientMessageEmbedMediaMutation>(destination);
    destination->width = source.width;
    destination->height = source.height;
    destination->flags = source.flags;
    destination->state = static_cast<u8>((source.has_width ? 1u : 0u) | (source.has_height ? 2u : 0u));
    return pack_text_range(output, transient_bytes, source.url, &destination->url) &&
           pack_text_range(output, transient_bytes, source.proxy_url, &destination->proxy_url) &&
           pack_text_range(output, transient_bytes, source.content_type, &destination->content_type) &&
           pack_text_range(output, transient_bytes, source.description, &destination->description);
}

bool append_poll_metadata(Slice<char> output, const GatewayMessageEvent& message, u32* cursor) noexcept {
    const auto& poll = message.poll;
    if (!gateway_message_poll_valid(poll))
        return false;
    GatewayClientMessagePollHeader header{};
    header.expiry_ms = poll.expiry_ms;
    header.fields = poll.fields;
    header.layout_type = poll.layout_type;
    header.answer_count = static_cast<u8>(poll.answers.count);
    header.count_count = static_cast<u8>(poll.answer_counts.count);
    header.flags = static_cast<u8>((poll.present ? 1 : 0) | (poll.expiry_null ? 2 : 0) |
                                   (poll.allow_multiselect ? 4 : 0) | (poll.results_known ? 8 : 0) |
                                   (poll.is_finalized ? 16 : 0));
    if (!pack_text_range(output, message.transient_bytes, poll.question, &header.question) ||
        !append_packed(output, cursor, header))
        return false;
    for (u32 index = 0; index < poll.answers.count; ++index) {
        const auto& answer = poll.answers[index];
        GatewayClientMessagePollAnswerMutation packed{};
        packed.answer_id = answer.answer_id;
        packed.emoji_id = answer.emoji_id;
        packed.flags = answer.emoji_animated ? 1 : 0;
        if (!pack_text_range(output, message.transient_bytes, answer.text, &packed.text) ||
            !pack_text_range(output, message.transient_bytes, answer.emoji_name, &packed.emoji_name) ||
            !append_packed(output, cursor, packed))
            return false;
    }
    for (u32 index = 0; index < poll.answer_counts.count; ++index) {
        const auto& count = poll.answer_counts[index];
        GatewayClientMessagePollCountMutation packed{};
        packed.answer_id = count.answer_id;
        packed.count = count.count;
        packed.flags = count.me_voted ? 1 : 0;
        if (!append_packed(output, cursor, packed))
            return false;
    }
    return true;
}

bool append_message_rich_metadata(Slice<char> output, const GatewayMessageEvent& message, u32* cursor,
                                  u32* rich_payload_bytes) noexcept {
    if (*cursor > output.count) {
        return false;
    }
    *rich_payload_bytes = 0;
    if ((message.fields & kGatewayMessageRichFieldMask) == 0 && message.rich_truncations == 0) {
        return message.reactions.count == 0 && message.attachments.count == 0 &&
               message.embeds.count == 0 && message.embed_fields.count == 0 &&
               message.stickers.count == 0 && message.components.count == 0;
    }
    if (message.reactions.count > kGatewayMessageReactionMaximum ||
        message.attachments.count > kGatewayMessageAttachmentMaximum ||
        message.embeds.count > kGatewayMessageEmbedMaximum ||
        message.embed_fields.count > kGatewayMessageEmbedFieldMaximum ||
        message.stickers.count > kGatewayMessageStickerMaximum ||
        message.components.count > kGatewayMessageComponentMaximum) {
        return false;
    }
    const u32 begin = *cursor;
    GatewayClientMessageRichHeader header{};
    header.reference_message_id = message.reference.message_id;
    header.reference_channel_id = message.reference.channel_id;
    header.reference_guild_id = message.reference.guild_id;
    header.reference_author_id = message.reference.author_id;
    header.truncations = message.rich_truncations;
    header.reaction_count = static_cast<u16>(message.reactions.count);
    header.attachment_count = static_cast<u16>(message.attachments.count);
    header.embed_count = static_cast<u16>(message.embeds.count);
    header.embed_field_count = static_cast<u16>(message.embed_fields.count);
    header.sticker_count = static_cast<u16>(message.stickers.count);
    header.component_count = static_cast<u16>(message.components.count);
    header.reference_state = static_cast<u8>(message.reference.state);
    header.reference_type = message.reference.type;
    header.reference_content_flags = message.reference.content_flags;
    header.reference_quote_flags = message.reference.quote_flags;
    header.reference_message_type = message.reference.message_type;
    header.interaction_id = message.interaction.id;
    header.interaction_user_id = message.interaction.user_id;
    header.interaction_type = message.interaction.type;
    if (!pack_text_range(output, message.transient_bytes, message.reference.author_name,
                         &header.reference_author_name) ||
        !pack_text_range(output, message.transient_bytes, message.reference.author_avatar_hash,
                         &header.reference_author_avatar_hash) ||
        !pack_text_range(output, message.transient_bytes, message.reference.content,
                         &header.reference_content) ||
        !pack_text_range(output, message.transient_bytes, message.interaction.user_name,
                         &header.interaction_user_name) ||
        !pack_text_range(output, message.transient_bytes, message.interaction.user_avatar_hash,
                         &header.interaction_user_avatar_hash) ||
        !pack_text_range(output, message.transient_bytes, message.interaction.command_name,
                         &header.interaction_command_name) ||
        !append_packed(output, cursor, header)) {
        return false;
    }
    for (u32 index = 0; index < message.reactions.count; ++index) {
        const GatewayMessageReaction& source = message.reactions[index];
        GatewayClientMessageReactionMutation packed{};
        packed.emoji_id = source.emoji_id;
        packed.count = source.count;
        packed.normal_count = source.normal_count;
        packed.burst_count = source.burst_count;
        packed.flags = static_cast<u8>((source.me ? 1u : 0u) | (source.me_burst ? 2u : 0u) |
                                       (source.animated ? 4u : 0u));
        if (!pack_text_range(output, message.transient_bytes, source.name, &packed.name) ||
            !append_packed(output, cursor, packed)) {
            return false;
        }
    }
    for (u32 index = 0; index < message.attachments.count; ++index) {
        const GatewayMessageAttachment& source = message.attachments[index];
        GatewayClientMessageAttachmentMutation packed{};
        packed.id = source.id;
        packed.size = source.size;
        packed.width = source.width;
        packed.height = source.height;
        packed.flags = source.flags;
        packed.duration_ms = source.duration_ms;
        packed.state = static_cast<u8>((source.has_width ? 1u : 0u) | (source.has_height ? 2u : 0u) |
                                       (source.ephemeral ? 4u : 0u) | (source.has_duration ? 8u : 0u));
        if (!gateway_message_waveform_valid(source.waveform) ||
            (!source.has_duration && source.duration_ms) ||
            !pack_text_range(output, message.transient_bytes, source.waveform, &packed.waveform) ||
            !pack_text_range(output, message.transient_bytes, source.filename, &packed.filename) ||
            !pack_text_range(output, message.transient_bytes, source.title, &packed.title) ||
            !pack_text_range(output, message.transient_bytes, source.description, &packed.description) ||
            !pack_text_range(output, message.transient_bytes, source.content_type, &packed.content_type) ||
            !pack_text_range(output, message.transient_bytes, source.url, &packed.url) ||
            !append_packed(output, cursor, packed)) {
            return false;
        }
    }
    for (u32 index = 0; index < message.embeds.count; ++index) {
        const GatewayMessageEmbed& source = message.embeds[index];
        GatewayClientMessageEmbedMutation packed{};
        packed.color = source.color;
        packed.field_begin = source.field_begin;
        packed.field_count = source.field_count;
        packed.type = static_cast<u8>(source.type);
        packed.flags = source.has_color ? 1u : 0u;
        packed.reserved[0] = source.flags;
        packed.timestamp_low = static_cast<u32>(source.timestamp_ms);
        packed.timestamp_high = static_cast<u32>(source.timestamp_ms >> 32u);
        if (!pack_text_range(output, message.transient_bytes, source.title, &packed.title) ||
            !pack_text_range(output, message.transient_bytes, source.description, &packed.description) ||
            !pack_text_range(output, message.transient_bytes, source.url, &packed.url) ||
            !pack_text_range(output, message.transient_bytes, source.author_name, &packed.author_name) ||
            !pack_text_range(output, message.transient_bytes, source.footer_text, &packed.footer_text) ||
            !pack_text_range(output, message.transient_bytes, source.author_url, &packed.author_url) ||
            !pack_text_range(output, message.transient_bytes, source.author_icon, &packed.author_icon) ||
            !pack_text_range(output, message.transient_bytes, source.footer_icon, &packed.footer_icon) ||
            !pack_text_range(output, message.transient_bytes, source.provider_name, &packed.provider_name) ||
            !pack_text_range(output, message.transient_bytes, source.provider_url, &packed.provider_url) ||
            !pack_embed_media(output, message.transient_bytes, source.image, &packed.image) ||
            !pack_embed_media(output, message.transient_bytes, source.thumbnail, &packed.thumbnail) ||
            !pack_embed_media(output, message.transient_bytes, source.video, &packed.video) ||
            !append_packed(output, cursor, packed)) {
            return false;
        }
    }
    for (u32 index = 0; index < message.embed_fields.count; ++index) {
        const GatewayMessageEmbedField& source = message.embed_fields[index];
        GatewayClientMessageEmbedFieldMutation packed{};
        packed.embed_index = source.embed_index;
        packed.flags = source.inline_field ? 1u : 0u;
        if (!pack_text_range(output, message.transient_bytes, source.name, &packed.name) ||
            !pack_text_range(output, message.transient_bytes, source.value, &packed.value) ||
            !append_packed(output, cursor, packed)) {
            return false;
        }
    }
    for (u32 index = 0; index < message.stickers.count; ++index) {
        const GatewayMessageSticker& source = message.stickers[index];
        GatewayClientMessageStickerMutation packed{};
        packed.id = source.id;
        packed.format_type = source.format_type;
        if (!pack_text_range(output, message.transient_bytes, source.name, &packed.name) ||
            !append_packed(output, cursor, packed)) {
            return false;
        }
    }
    for (u32 index = 0; index < message.components.count; ++index) {
        const GatewayMessageComponent& source = message.components[index];
        GatewayClientMessageComponentMutation packed{};
        packed.id = source.id;
        packed.parent = source.parent;
        packed.order = source.order;
        packed.type = source.type;
        packed.style = source.style;
        packed.accent_color = source.accent_color;
        packed.flags = static_cast<u8>((source.disabled ? 1u : 0u) | (source.has_accent_color ? 2u : 0u));
        packed.emoji_id = source.emoji_id;
        packed.min_values = source.min_values;
        packed.max_values = source.max_values;
        packed.component_flags = source.flags;
        if ((source.flags & ~kGatewayMessageComponentKnownFlags) != 0 ||
            !pack_text_range(output, message.transient_bytes, source.label, &packed.label) ||
            !pack_text_range(output, message.transient_bytes, source.content, &packed.content) ||
            !pack_text_range(output, message.transient_bytes, source.custom_id, &packed.custom_id) ||
            !pack_text_range(output, message.transient_bytes, source.url, &packed.url) ||
            !pack_text_range(output, message.transient_bytes, source.emoji_name, &packed.emoji_name) ||
            !append_packed(output, cursor, packed)) {
            return false;
        }
    }
    if (message.fields & gateway_message_field_call) {
        GatewayClientMessageCallMutation call{};
        call.ended_timestamp_ms = message.call.ended_timestamp_ms;
        call.flags = static_cast<u8>((message.call.present ? 1u : 0u) | (message.call.ended ? 2u : 0u) |
                                     (message.call.participated ? 4u : 0u));
        if (!append_packed(output, cursor, call))
            return false;
    }
    if ((message.fields & gateway_message_field_poll) && !append_poll_metadata(output, message, cursor))
        return false;
    if (message.fields & gateway_message_field_message_snapshots) {
        GatewayClientMessageSnapshotHeader snapshot{};
        const auto& source = message.snapshot;
        const u32 at = *cursor;
        if (source.present) {
            snapshot.present = 1;
            snapshot.timestamp_ms = source.timestamp_ms;
            snapshot.edited_timestamp_ms = source.edited_timestamp_ms;
            snapshot.flags = source.flags;
            snapshot.fields = source.fields;
            snapshot.type = source.type;
            if (source.mention_users.count > kGatewayMessageMentionUserMaximum ||
                !pack_text_range(output, message.transient_bytes, source.content, &snapshot.content))
                return false;
            snapshot.mention_user_count = static_cast<u16>(source.mention_users.count);
        }
        if (!append_packed(output, cursor, snapshot))
            return false;
        if (source.present) {
            for (u32 index = 0; index < source.mention_users.count; ++index) {
                const auto& user = source.mention_users[index];
                GatewayClientNamedUserMutation packed{};
                GatewayClientMessageTextRange range{};
                if (!user.id || !pack_text_range(output, message.transient_bytes, user.name, &range))
                    return false;
                packed.id = user.id;
                packed.name_offset = range.offset;
                packed.name_bytes = range.bytes;
                if (!append_packed(output, cursor, packed))
                    return false;
            }
            GatewayMessageEvent body{};
            body.fields = source.fields;
            body.transient_bytes = message.transient_bytes;
            body.attachments = source.attachments;
            body.embeds = source.embeds;
            body.embed_fields = source.embed_fields;
            body.stickers = source.stickers;
            body.components = source.components;
            body.rich_truncations = source.rich_truncations;
            if ((body.fields & gateway_message_field_message_snapshots) ||
                !append_message_rich_metadata(output, body, cursor, &snapshot.rich_payload_bytes))
                return false;
            copy_bytes(output.data + at, &snapshot, sizeof(snapshot));
        }
    }
    *rich_payload_bytes = *cursor - begin;
    return true;
}

bool append_message_metadata(Slice<char> output, const GatewayMessageEvent& message,
                             ConstSlice<u64> role_ids, GatewayClientMessageMutation* mutation,
                             ConstSlice<u8>* payload) noexcept {
    const u64 role_bytes = static_cast<u64>(role_ids.count) * sizeof(u64);
    const u64 member_role_bytes = static_cast<u64>(message.member_roles.count) * sizeof(u64);
    const u64 user_bytes =
        static_cast<u64>(message.mention_users.count) * sizeof(GatewayClientNamedUserMutation);
    if (!mutation || !payload || message.content.bytes > message.transient_bytes ||
        message.transient_bytes > output.count || role_bytes > output.count - message.transient_bytes ||
        member_role_bytes > output.count - message.transient_bytes - role_bytes ||
        user_bytes > output.count - message.transient_bytes - role_bytes - member_role_bytes) {
        return false;
    }
    u32 cursor = message.transient_bytes;
    for (u32 index = 0; index < role_ids.count; ++index) {
        byte_store_le64(reinterpret_cast<u8*>(output.data + cursor + index * sizeof(u64)), role_ids[index]);
    }
    cursor += static_cast<u32>(role_bytes);
    for (u32 index = 0; index < message.member_roles.count; ++index) {
        byte_store_le64(reinterpret_cast<u8*>(output.data + cursor + index * sizeof(u64)),
                        message.member_roles[index]);
    }
    cursor += static_cast<u32>(member_role_bytes);
    for (u32 index = 0; index < message.mention_users.count; ++index) {
        const GatewayMessageNamedUser& user = message.mention_users[index];
        if (user.id == 0 || !user.name.data || user.name.bytes > kGatewayMessageUserNameMaximumBytes ||
            user.name.data < output.data || user.name.data > output.data + message.transient_bytes ||
            user.name.bytes > static_cast<u32>(output.data + message.transient_bytes - user.name.data)) {
            return false;
        }
        GatewayClientNamedUserMutation packed{};
        packed.id = user.id;
        packed.name_offset = static_cast<u32>(user.name.data - output.data);
        packed.name_bytes = static_cast<u16>(user.name.bytes);
        copy_bytes(output.data + cursor, &packed, static_cast<u32>(sizeof(packed)));
        cursor += sizeof(packed);
    }
    if (!append_message_rich_metadata(output, message, &cursor, &mutation->rich_payload_bytes)) {
        return false;
    }
    *payload = ConstSlice<u8>{reinterpret_cast<const u8*>(output.data), cursor};
    return true;
}

bool byte_ranges_overlap(const void* first, u32 first_bytes, const void* second,
                         u32 second_bytes) noexcept {
    if (!first || !second || first_bytes == 0 || second_bytes == 0) {
        return false;
    }
    const usize first_begin = reinterpret_cast<usize>(first);
    const usize second_begin = reinterpret_cast<usize>(second);
    const usize first_end = first_begin + first_bytes;
    const usize second_end = second_begin + second_bytes;
    if (first_end < first_begin || second_end < second_begin) {
        return true;
    }
    return first_begin < second_end && second_begin < first_end;
}

struct NotificationFields {
    JsonValue entries, proto, settings, threads, member, added, removed, members, schedule, rules, channels;
    u64 id, guild, user, version;
    u32 seen, flags, type;
    bool partial, version_known, bad, unavailable, default_mentions, request, spam, nsfw;
    u8 nsfw_level, owner_content_level;
    u32 known_gates, gates;
};
bool notification_fields(void* opaque, JsonString name, JsonValue v) noexcept {
    auto& f = *static_cast<NotificationFields*>(opaque);
    const u32 bit = protocol_detail::member_bit<"id", "guild_id", "user_id", "entries", "partial", "version", "proto",
        "settings", "threads", "member", "added_members", "removed_member_ids", "flags", "default_message_notifications",
        "lurking", "joined_at", "application_id", "is_message_request", "unavailable", "members", "type", "is_spam",
        "restricted_schedule", "rules", "channels", "nsfw", "nsfw_level", "owner_configured_content_level">(name);
    if (!bit)
        return true;
    if (f.seen & bit) {
        f.bad = true;
        return false;
    }
    f.seen |= bit;
    bool ok = true;
    u64 number = 0;
    switch (bit) {
    case 1:
        ok = json_value_to_snowflake(v, &f.id) && f.id;
        break;
    case 2:
        ok = v.kind == JsonValueKind::null_value ? (f.guild = 0, true)
                                                 : json_value_to_snowflake(v, &f.guild);
        break;
    case 4:
        ok = json_value_to_snowflake(v, &f.user);
        break;
    case 8:
        f.entries = v;
        break;
    case 16:
        ok = json_value_to_bool(v, &f.partial);
        break;
    case 32:
        ok = json_value_to_u64(v, &f.version);
        f.version_known = ok;
        break;
    case 64:
        f.proto = v;
        break;
    case 128:
        f.settings = v;
        break;
    case 256:
        f.threads = v;
        break;
    case 512:
        f.member = v;
        break;
    case 1024:
        f.added = v;
        break;
    case 2048:
        f.removed = v;
        break;
    case 4096:
        ok = json_value_to_u64(v, &number);
        f.flags = static_cast<u32>(number);
        break;
    case 8192:
        ok = json_value_to_u64(v, &number) && number <= 1;
        f.default_mentions = number == 1;
        break;
    case 16384: {
        bool yes = false;
        ok = json_value_to_bool(v, &yes);
        f.known_gates |= notification_gate_lurking;
        if (yes)
            f.gates |= notification_gate_lurking;
        break;
    }
    case 32768:
        if (v.kind == JsonValueKind::string)
            f.known_gates |= notification_gate_lurking;
        break;
    case 65536:
        f.known_gates |= notification_gate_managed;
        if (v.kind != JsonValueKind::null_value)
            f.gates |= notification_gate_managed;
        break;
    case 131072:
        ok = json_value_to_bool(v, &f.request);
        break;
    case 262144:
        ok = json_value_to_bool(v, &f.unavailable);
        break;
    default:
        break;
    }
    if (bit == 524288)
        f.members = v;
    if (bit == 1048576) {
        ok = json_value_to_u64(v, &number) && number <= ~u32{0};
        f.type = static_cast<u32>(number);
    }
    if (bit == 2097152)
        ok = json_value_to_bool(v, &f.spam);
    if (bit == 4194304)
        f.schedule = v;
    if (bit == 8388608)
        f.rules = v;
    if (bit == 16777216)
        f.channels = v;
    if (bit == 33554432)
        ok = v.kind == JsonValueKind::null_value || (json_value_to_bool(v, &f.nsfw));
    if (bit == 67108864) {
        ok = v.kind == JsonValueKind::null_value || (json_value_to_u64(v, &number) && number <= 3);
        f.nsfw_level = static_cast<u8>(number);
    }
    if (bit == 134217728) {
        ok = v.kind == JsonValueKind::null_value || (json_value_to_u64(v, &number) && number <= 3);
        f.owner_content_level = static_cast<u8>(number);
    }
    f.bad |= !ok;
    return ok;
}
bool notification_document(JsonValue source, Slice<u32> scratch, JsonIndexedDocument* doc,
                           NotificationFields* fields) noexcept {
    if (source.kind != JsonValueKind::object)
        return false;
    const auto scan = json_scan_structurals_avx1(source.source, scratch);
    if (!scan.ok())
        return false;
    *doc = {source.source, {scratch.data, scan.count}};
    return json_visit_value_object(source, doc->structurals, 64, notification_fields, fields).ok() &&
           !fields->bad;
}
struct RestrictionRules {
    const JsonIndexedDocument* doc;
    bool inactive = true;
};
struct RestrictionRule {
    bool seen = false, enabled = false;
};
bool restriction_enabled(void* opaque, JsonString name, JsonValue v) noexcept {
    if (!json_name_is(name, utf8_literal("enabled")))
        return true;
    auto& r = *static_cast<RestrictionRule*>(opaque);
    if (r.seen)
        return false;
    r.seen = true;
    return json_value_to_bool(v, &r.enabled);
}
bool restriction_rule(void* opaque, u32, JsonValue v) noexcept {
    auto& r = *static_cast<RestrictionRules*>(opaque);
    RestrictionRule rule{};
    if (v.kind != JsonValueKind::object ||
        !json_visit_indexed_object(*r.doc, v, 64, restriction_enabled, &rule).ok())
        return false;
    if (!rule.seen || rule.enabled)
        r.inactive = false;
    return true;
}
bool restriction_inactive(const JsonIndexedDocument& doc, const NotificationFields& f, bool full) noexcept {
    if (!(f.seen & 4194304))
        return full;
    if (f.schedule.kind == JsonValueKind::null_value)
        return true;
    if (f.schedule.kind != JsonValueKind::object)
        return false;
    NotificationFields schedule{};
    if (!json_visit_indexed_object(doc, f.schedule, 64, notification_fields, &schedule).ok() ||
        schedule.rules.kind != JsonValueKind::array)
        return false;
    RestrictionRules rules{&doc, true};
    return json_visit_indexed_array(doc, schedule.rules, 64, restriction_rule, &rules).ok() &&
           rules.inactive;
}
void notification_incomplete(GatewayClientEvent* e, ConstSlice<u8>* payload, u64 guild) noexcept {
    e->dispatch_kind = GatewayClientDispatchKind::notification_incomplete;
    e->notification = {guild, 0, 0, 0, gateway_notification_incomplete, 0, 0};
    e->dispatch_payload_bytes = 0;
    reset_value(payload);
}
void notification_global_incomplete(GatewayClientEvent* e, ConstSlice<u8>* payload) noexcept {
    e->dispatch_kind = GatewayClientDispatchKind::notification_global;
    e->notification = {0, 0, 0, 0, gateway_notification_incomplete, 0, 0};
    e->dispatch_payload_bytes = 0;
    reset_value(payload);
}
bool notification_entry(JsonValue source, Slice<u32> scratch, Slice<char> storage, GatewayClientEvent* e,
                        ConstSlice<u8>* payload) noexcept {
    JsonIndexedDocument doc{};
    NotificationFields f{};
    if (!notification_document(source, scratch, &doc, &f)) {
        notification_incomplete(e, payload, f.guild);
        return true;
    }
    const usize address = reinterpret_cast<usize>(storage.data);
    const u32 padding = static_cast<u32>((8 - (address & 7)) & 7);
    if (padding > storage.count || sizeof(MessageNotificationGuildSetting) > storage.count - padding) {
        notification_incomplete(e, payload, f.guild);
        return true;
    }
    char* start = storage.data + padding;
    const u32 available = storage.count - padding;
    const u32 capacity = (available - static_cast<u32>(sizeof(MessageNotificationGuildSetting))) /
                         static_cast<u32>(sizeof(MessageNotificationOverride));
    auto* overrides =
        reinterpret_cast<MessageNotificationOverride*>(start + sizeof(MessageNotificationGuildSetting));
    MessageNotificationGuildSetting guild{};
    NotificationSettingsPacket packet{};
    const auto err = decode_notification_settings(doc, {doc.input, 0, JsonValueKind::object},
                                                  {&guild, 1}, {overrides, capacity}, &packet);
    if (err != NotificationSettingsDecodeError::none) {
        notification_incomplete(e, payload, f.guild);
        return true;
    }
    copy_bytes(start, &guild, sizeof(guild));
    const u32 bytes =
        static_cast<u32>(sizeof(guild)) + packet.override_count * static_cast<u32>(sizeof(*overrides));
    e->dispatch_kind = GatewayClientDispatchKind::notification_settings_entry;
    e->notification = {guild.guild_id,
                       0,
                       packet.version,
                       packet.override_count,
                       packet.version_known ? gateway_notification_version_known : 0u,
                       0,
                       0};
    e->dispatch_payload_bytes = bytes;
    *payload = {reinterpret_cast<const u8*>(start), bytes};
    return true;
}
bool notification_proto(JsonValue value, Slice<char> storage, MessageNotificationGlobal* g) noexcept {
    JsonString text{};
    if (!json_value_as_string(value, &text))
        return false;
    Utf8View encoded = text.encoded;
    if (text.has_escapes) {
        const auto decoded = json_string_decode_utf8(text, storage);
        if (!decoded.ok())
            return false;
        encoded = {storage.data, decoded.bytes};
    }
    return decode_notification_global_base64(encoded, g) == NotificationSettingsDecodeError::none;
}
bool notification_route(const GatewayEnvelope& env, Slice<u32> scratch, Slice<char> storage,
                        GatewayClientEvent* e, ConstSlice<u8>* payload, u32 kind) noexcept {
    if (kind == 0)
        return notification_entry(env.data, scratch, storage, e, payload);
    const bool proto_update = kind == 1;
    JsonIndexedDocument doc{};
    NotificationFields f{};
    if (!notification_document(env.data, scratch, &doc, &f)) {
        notification_global_incomplete(e, payload);
        return true;
    }
    MessageNotificationGlobal global{};
    u32 flags = proto_update && f.partial ? gateway_notification_partial : 0u;
    u32 account_flags = 0;
    if (proto_update) {
        NotificationFields nested{};
        if (f.settings.kind != JsonValueKind::object ||
            !json_visit_indexed_object(doc, f.settings, 64, notification_fields, &nested).ok()) {
            notification_global_incomplete(e, payload);
            return true;
        }
        if ((nested.seen & 1048576) && nested.type != 1)
            return true;
        if (!(nested.seen & 1048576) || !notification_proto(nested.proto, storage, &global)) {
            notification_global_incomplete(e, payload);
            return true;
        }
    } else {
        if (!(f.seen & 4096)) {
            notification_global_incomplete(e, payload);
            return true;
        }
        flags = gateway_notification_partial | gateway_notification_account_flags_known;
        account_flags = f.flags;
    }
    if (storage.count < sizeof(global)) {
        notification_global_incomplete(e, payload);
        return true;
    }
    copy_bytes(storage.data, &global, sizeof(global));
    e->dispatch_kind = GatewayClientDispatchKind::notification_global;
    e->notification = {0, 0, 0, 0, flags, account_flags, 0};
    e->dispatch_payload_bytes = sizeof(global);
    *payload = {reinterpret_cast<const u8*>(storage.data), sizeof(global)};
    return true;
}

struct NotificationSide {
    const JsonIndexedDocument* doc;
    Slice<char> storage;
    GatewayClientNotificationSidecar header;
    u64 self;
    u32 cursor;
    bool bad;
};
bool notification_channel_meta(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<NotificationSide*>(opaque);
    NotificationFields f{};
    if (!json_visit_indexed_object(*s.doc, value, 64, notification_fields, &f).ok()) {
        s.bad = true;
        return false;
    }
    if (f.nsfw && !(f.seen & 1)) {
        s.bad = true;
        return false;
    }
    if (!(f.seen & 1048576) && f.nsfw) {
        s.bad = true;
        return false;
    }
    if (!f.nsfw && (!(f.seen & 1048576) || f.type != 18))
        return true;
    GatewayClientNotificationThreadRecord row{};
    row.thread.guild_id = s.header.guild_id;
    row.thread.thread_id = f.id;
    row.flags = gateway_notification_channel_metadata;
    row.known_gates = notification_gate_age_access;
    if (f.nsfw)
        row.gates |= notification_gate_age_access;
    if (f.type == 18) {
        row.known_gates |= notification_gate_managed | notification_gate_request;
        row.gates |= f.gates & notification_gate_managed;
    }
    if (!f.id || sizeof(row) > s.storage.count - s.cursor) {
        s.bad = true;
        return false;
    }
    copy_bytes(s.storage.data + s.cursor, &row, sizeof(row));
    s.cursor += sizeof(row);
    ++s.header.thread_count;
    return true;
}
bool append_notification_thread(NotificationSide& s, JsonValue value, u64 fallback_thread,
                                bool remove = false) noexcept {
    GatewayClientNotificationThreadRecord row{};
    row.flags = remove ? gateway_notification_thread_remove : 0u;
    if (remove) {
        row.thread = {s.header.guild_id, fallback_thread, {}};
    } else if (decode_notification_thread(*s.doc, value, s.header.guild_id, &row.thread, fallback_thread) !=
               NotificationSettingsDecodeError::none) {
        s.bad = true;
        return false;
    }
    if (!remove) {
        NotificationFields member{};
        if (!json_visit_indexed_object(*s.doc, value, 64, notification_fields, &member).ok() ||
            row.thread.guild_id != s.header.guild_id || !(row.thread.setting.fields & notification_flags)) {
            s.bad = true;
            return false;
        }
        if ((member.seen & 4) && member.user != s.self)
            return true;
        if (!(row.thread.setting.fields & notification_muted)) {
            row.thread.setting.fields |= notification_muted;
            row.thread.setting.muted = false;
        }
        if (!(row.thread.setting.fields & notification_mute_end)) {
            row.thread.setting.fields |= notification_mute_end;
            row.thread.setting.mute_end_ms = 0;
        }
    }
    if (sizeof(row) > s.storage.count - s.cursor) {
        s.bad = true;
        return false;
    }
    copy_bytes(s.storage.data + s.cursor, &row, sizeof(row));
    s.cursor += sizeof(row);
    ++s.header.thread_count;
    return true;
}
bool notification_joined(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<NotificationSide*>(opaque);
    NotificationFields f{};
    if (!json_visit_indexed_object(*s.doc, value, 64, notification_fields, &f).ok()) {
        s.bad = true;
        return false;
    }
    if (!notification_channel_meta(opaque, 0, value))
        return false;
    if (!f.member.source.data || f.member.kind == JsonValueKind::null_value)
        return true;
    return append_notification_thread(s, f.member, f.id);
}
bool notification_added(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<NotificationSide*>(opaque);
    NotificationFields f{};
    if (!json_visit_indexed_object(*s.doc, value, 64, notification_fields, &f).ok()) {
        s.bad = true;
        return false;
    }
    if (!(f.seen & 4)) {
        s.bad = true;
        return false;
    }
    if (f.user != s.self)
        return true;
    return append_notification_thread(s, value, s.header.channel_id);
}
bool notification_removed(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<NotificationSide*>(opaque);
    u64 user = 0;
    if (!json_value_to_snowflake(value, &user)) {
        s.bad = true;
        return false;
    }
    return user != s.self || append_notification_thread(s, {}, s.header.channel_id, true);
}
bool notification_sync(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<NotificationSide*>(opaque);
    NotificationFields f{};
    if (!json_visit_indexed_object(*s.doc, value, 64, notification_fields, &f).ok()) {
        s.bad = true;
        return false;
    }
    if ((f.seen & 4) && f.user != s.self)
        return true;
    return append_notification_thread(s, value, f.id);
}
} // namespace

void gateway_client_append_notification_sidecar(const GatewayEnvelope& env, Slice<u32> scratch,
                                                Slice<char> storage, u64 self, GatewayClientEvent* e,
                                                ConstSlice<u8>* payload) noexcept {
    const u32 named = protocol_detail::skim_member_index<
        "GUILD_CREATE", "READY_GUILD", "GUILD_UPDATE", "THREAD_MEMBER_UPDATE", "THREAD_CREATE", "THREAD_UPDATE",
        "THREAD_MEMBERS_UPDATE", "THREAD_LIST_SYNC", "CHANNEL_CREATE", "CHANNEL_UPDATE", "USER_UPDATE">(env.event_name);
    if (named >= 11)
        return;
    const bool guild = named <= 2;
    const bool self_thread = named == 3;
    const bool thread = named >= 3 && named <= 5;
    const bool delta = named == 6;
    const bool sync = named == 7;
    const bool channel = thread || named == 8 || named == 9;
    const bool user = named == 10;
    JsonIndexedDocument doc{};
    NotificationFields f{};
    bool valid = notification_document(env.data, scratch, &doc, &f);
    if (user && (f.id != self || !(f.seen & 4194304)))
        return;
    const u32 base = payload->count;
    const auto* original = payload->data;
    // Existing payload bytes are copied unchanged. The appended descriptors are
    // read with memcpy accessors, so no alignment padding leaks into its body.
    if (base > storage.count || sizeof(GatewayClientNotificationSidecar) > storage.count - base) {
        e->notification_payload_bytes = kGatewayNotificationMetadataIncomplete;
        return;
    }
    if (base && original != reinterpret_cast<const u8*>(storage.data))
        copy_bytes(storage.data, original, base);
    NotificationSide side{};
    side.doc = &doc;
    side.storage = storage;
    side.self = self;
    side.cursor = base + sizeof(side.header);
    side.header.guild_id = guild ? f.id : f.guild;
    side.header.channel_id = guild || user ? 0 : f.id;
    side.header.known_gates = f.known_gates;
    side.header.gates = f.gates;
    if (user && f.id == self && (f.seen & 4194304)) {
        if (restriction_inactive(doc, f, false))
            side.header.known_gates |= notification_gate_restricted_hours;
        else
            side.header.flags |= gateway_notification_incomplete;
    }
    const bool full_channel = channel && (named == 4 || named == 8);
    const bool full_guild = guild && named <= 1;
    const bool sensitive_guild =
        f.nsfw_level == 1 || f.nsfw_level == 3 || f.owner_content_level == 1 || f.owner_content_level == 3;
    if (guild && sensitive_guild) {
        side.header.known_gates |= notification_gate_age_access;
        side.header.gates |= notification_gate_age_access;
    }
    if (channel && (full_channel || (f.seen & 33554432))) {
        side.header.known_gates |= notification_gate_age_access;
        if (f.nsfw)
            side.header.gates |= notification_gate_age_access;
        else
            side.header.gates &= ~notification_gate_age_access;
    }
    if (channel && (f.seen & 1048576)) {
        if ((f.type != 3 && f.type != 18) || (full_channel && !(f.seen & 65536))) {
            side.header.known_gates |= notification_gate_managed;
            side.header.gates &= ~notification_gate_managed;
        }
        if (f.type != 1 && f.type != 3) {
            side.header.known_gates |= notification_gate_request;
            side.header.gates &= ~notification_gate_request;
        }
    }
    if (channel && (full_channel || ((f.seen & 131072) && !f.request) || ((f.seen & 2097152) && f.spam) ||
                    ((f.seen & (131072 | 2097152)) == (131072 | 2097152)))) {
        side.header.known_gates |= notification_gate_request;
        if (f.request && !f.spam)
            side.header.gates |= notification_gate_request;
        else
            side.header.gates &= ~notification_gate_request;
    }
    if (guild && (f.seen & 8192)) {
        side.header.flags |= gateway_notification_guild_default_known;
        if (f.default_mentions)
            side.header.flags |= gateway_notification_guild_default_mentions;
    }
    if (guild && f.threads.kind == JsonValueKind::array)
        valid = valid && json_visit_indexed_array(doc, f.threads, 64, notification_joined, &side).ok();
    if (guild && (f.seen & 16777216)) {
        valid = valid && f.channels.kind == JsonValueKind::array &&
                json_visit_indexed_array(doc, f.channels, 64, notification_channel_meta, &side).ok();
        if (valid && !side.bad) {
            side.header.flags |= gateway_notification_channel_age_baseline_complete;
            if (!sensitive_guild &&
                (full_guild || ((f.seen & (67108864 | 134217728)) == (67108864 | 134217728))))
                side.header.known_gates |= notification_gate_age_access;
        }
    }
    if (thread && (f.seen & 512) && f.member.kind == JsonValueKind::object)
        valid = valid && append_notification_thread(side, f.member, f.id);
    if (self_thread)
        valid = valid && append_notification_thread(side, env.data, f.id);
    if (delta) {
        if (f.added.kind == JsonValueKind::array)
            valid = valid && json_visit_indexed_array(doc, f.added, 64, notification_added, &side).ok();
        if (f.removed.kind == JsonValueKind::array)
            valid = valid && json_visit_indexed_array(doc, f.removed, 64, notification_removed, &side).ok();
    }
    if (sync && f.members.kind == JsonValueKind::array)
        valid = valid && json_visit_indexed_array(doc, f.members, 64, notification_sync, &side).ok();
    if (!valid || side.bad) {
        side.header.flags = (side.header.flags & ~gateway_notification_channel_age_baseline_complete) |
                            gateway_notification_incomplete;
        side.header.known_gates = side.header.gates = 0;
        side.header.thread_count = 0;
        side.cursor = base + sizeof(side.header);
    }
    copy_bytes(storage.data + base, &side.header, sizeof(side.header));
    e->notification_payload_bytes = side.cursor - base;
    e->dispatch_payload_bytes = side.cursor;
    *payload = {reinterpret_cast<const u8*>(storage.data), side.cursor};
}

namespace {
using BatchPublish = bool (*)(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept;
struct BatchBinding {
    const GatewayEnvelope* envelope;
    u64 now_ms;
    BatchPublish publish;
    void* context;
    u32 remaining_prefix;
};
bool batch_rejected(const GatewayEnvelope& envelope, BatchPublish publish, Utf8View name) noexcept {
    return !publish || envelope.opcode != 0 || !envelope.has_sequence || !envelope.has_event_name ||
           !json_string_equals_ascii(envelope.event_name, name);
}
// A resumed batch skips the records the cancelled attempt already published.
BatchBinding batch_binding(const GatewayEnvelope& envelope, u64 now_ms, BatchPublish publish, void* context,
                           const GatewayReactionBatchCursor* cursor) noexcept {
    return {&envelope, now_ms, publish, context,
            cursor && cursor->active && cursor->sequence == envelope.sequence ? cursor->published : 0};
}
bool batch_skips_record(BatchBinding& binding) noexcept {
    if (!binding.remaining_prefix)
        return false;
    --binding.remaining_prefix;
    return true;
}
GatewayReactionBatchVisitResult settle_batch(GatewayReactionBatchVisitResult result, const BatchBinding& binding,
                                             GatewayReactionBatchCursor* cursor) noexcept {
    if (result.ok() && binding.remaining_prefix)
        result.decoded.error = GatewayEventDecodeError::invalid_data;
    if (cursor) {
        if (result.ok())
            reset_value(cursor);
        else if (result.cancelled)
            *cursor = {binding.envelope->sequence, result.visited, result.visited != 0};
    }
    return result;
}
} // namespace

GatewayReactionBatchVisitResult gateway_client_publish_reaction_batch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> name_scratch, u64 now_ms, BatchPublish publish, void* context,
    GatewayReactionBatchCursor* cursor) noexcept {
    if (batch_rejected(envelope, publish, utf8_literal("MESSAGE_REACTION_ADD_MANY")))
        return {{0, GatewayEventDecodeError::invalid_data}, 0, false};
    BatchBinding binding = batch_binding(envelope, now_ms, publish, context, cursor);
    const auto result = gateway_visit_message_reaction_add_many(
        envelope.data, structural_scratch, maximum_depth, name_scratch, &binding,
        [](void* opaque, const GatewayMessageReactionEvent& reaction) noexcept {
            auto& binding = *static_cast<BatchBinding*>(opaque);
            if (batch_skips_record(binding))
                return true;
            GatewayClientEvent event =
                gateway_dispatch_event(GatewayClientDispatchKind::message_reaction_add,
                                       binding.envelope->sequence, binding.now_ms);
            event.has_sequence = binding.envelope->has_sequence;
            copy_reaction(reaction, &event.reaction);
            event.dispatch_payload_bytes = reaction.name.bytes;
            return binding.publish(binding.context, event,
                                   {reinterpret_cast<const u8*>(reaction.name.data), reaction.name.bytes});
        });
    return settle_batch(result, binding, cursor);
}

GatewayReactionBatchVisitResult gateway_client_publish_poll_vote_batch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth, u64 now_ms,
    BatchPublish publish, void* context, GatewayReactionBatchCursor* cursor) noexcept {
    if (batch_rejected(envelope, publish, utf8_literal("MESSAGE_POLL_VOTE_ADD_MANY")))
        return {{0, GatewayEventDecodeError::invalid_data}, 0, false};
    BatchBinding binding = batch_binding(envelope, now_ms, publish, context, cursor);
    const auto result = gateway_visit_message_poll_vote_add_many(
        envelope.data, structural_scratch, maximum_depth, &binding,
        [](void* opaque, const GatewayMessagePollVoteEvent& vote) noexcept {
            auto& binding = *static_cast<BatchBinding*>(opaque);
            if (batch_skips_record(binding))
                return true;
            GatewayClientEvent event = gateway_dispatch_event(GatewayClientDispatchKind::message_poll_vote_add,
                                                              binding.envelope->sequence, binding.now_ms);
            event.has_sequence = binding.envelope->has_sequence;
            event.poll_vote = vote;
            return binding.publish(binding.context, event, {});
        });
    return settle_batch(result, binding, cursor);
}

namespace {
// The index of the dispatch's frame: the envelope's own while it still holds it (every route before the
// by-name decoders filters by event name before it scans), else one scan of the data for an envelope built
// by hand.
GatewayClientDispatchRouteResult frame_index(const GatewayEnvelope& envelope, Slice<u32> structural_scratch,
                                             JsonIndexedDocument* frame) noexcept {
    if (envelope.document.structurals.data && envelope.document.structurals.data == structural_scratch.data) {
        *frame = envelope.document;
        return route_error(GatewayClientDispatchRouteError::none);
    }
    const JsonScanResult scan = json_scan_structurals_avx1(envelope.data.source, structural_scratch);
    if (!scan.ok())
        return route_decode_error(GatewayEventDecodeResult{scan.error_offset, GatewayEventDecodeError::scan_failed,
                                                           scan.error});
    *frame = JsonIndexedDocument{envelope.data.source, {structural_scratch.data, scan.count}};
    return route_error(GatewayClientDispatchRouteError::none);
}
} // namespace

namespace {
// Out of line: the activity set (3.5 KB) would otherwise cost every dispatch route a stack probe. Appends
// the rich-presence record behind the appearance bytes already written; false when it does not fit.
__declspec(noinline) bool append_presence_activities(const JsonIndexedDocument& document, JsonValue presence,
                                                     Slice<char> content_scratch, u64 user_id,
                                                     u32* extra) noexcept {
    UserActivitySet activities{};
    bool has_activities = false;
    if (user_activities_decode(document, presence, user_id, &activities, &has_activities) && has_activities &&
        activities.count) {
        const u32 tail = user_activities_write(
            activities, {reinterpret_cast<u8*>(content_scratch.data) + *extra, content_scratch.count - *extra});
        if (!tail)
            return false;
        *extra += tail;
    }
    return true;
}
} // namespace

namespace {
// Decodes a MESSAGE_CREATE/UPDATE with the arrays of `scratch`.
__declspec(noinline) GatewayClientDispatchRouteResult route_message_create_update(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> content_scratch, Slice<u64> bulk_ids, u64 self_user_id, bool create, GatewayClientEvent* event,
    ConstSlice<u8>* payload, GatewayMessageDecodeScratch* scratch) noexcept {
    GatewayMessageEvent message{};
    const GatewayMessageRichScratch rich_scratch = scratch->rich.view(true);
    // The envelope's index is still intact here (every earlier route filters by name before it scans), so a
    // message is decoded from it instead of being scanned again.
    JsonIndexedDocument frame;
    const GatewayClientDispatchRouteResult indexed = frame_index(envelope, structural_scratch, &frame);
    if (!indexed.ok())
        return indexed;
    const Slice<u64> member_roles{scratch->member_roles, kGatewayMessageMemberRoleMaximumIds};
    const Slice<GatewayMessageNamedUser> mention_users{scratch->mention_users, kGatewayMessageMentionUserMaximum};
    const GatewayEventDecodeResult decoded =
        create ? gateway_decode_message_create(frame, envelope.data, maximum_depth, content_scratch, bulk_ids,
                                               member_roles, mention_users, rich_scratch, self_user_id, &message)
               : gateway_decode_message_update(frame, envelope.data, maximum_depth, content_scratch, bulk_ids,
                                               member_roles, mention_users, rich_scratch, self_user_id, &message);
    if (!decoded.ok())
        return route_decode_error(decoded);
    event->dispatch_kind =
        create ? GatewayClientDispatchKind::message_create : GatewayClientDispatchKind::message_update;
    copy_message(message, &event->message);
    if (!append_message_metadata(content_scratch, message, message.mention_roles, &event->message, payload))
        return route_error(GatewayClientDispatchRouteError::payload_capacity);
    return route_error(GatewayClientDispatchRouteError::none);
}
} // namespace

GatewayClientDispatchRouteResult gateway_client_route_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> content_scratch, Slice<u64> bulk_ids, u64 self_user_id, GatewayClientEvent* event,
    ConstSlice<u8>* payload, GatewayMessageDecodeScratch* message_scratch) noexcept {
    if (!event || !payload || envelope.opcode != 0 || !envelope.has_event_name ||
        !envelope.data.source.data || envelope.data.source.bytes == 0 || !structural_scratch.data ||
        structural_scratch.count == 0 || maximum_depth == 0 ||
        (!content_scratch.data && content_scratch.count != 0) || (!bulk_ids.data && bulk_ids.count != 0) ||
        byte_ranges_overlap(envelope.data.source.data, envelope.data.source.bytes, content_scratch.data,
                            content_scratch.count)) {
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    }
    reset_value<GatewayClientEvent>(event);
    *payload = ConstSlice<u8>{nullptr, 0};
    event->kind = GatewayClientEventKind::dispatch;
    event->sequence = envelope.sequence;
    event->has_sequence = envelope.has_sequence;
    event->dispatch_kind = GatewayClientDispatchKind::unknown;
    // One lookup for every event that is routed by name before the state, thread, administration and
    // member list routes: the cursor, notification and acknowledgement events, the verbatim interaction
    // kinds (positions 7-26), and the stage, call and user events.
    const u32 early = protocol_detail::wide_member_index<
        "PASSIVE_UPDATE_V2", "CHANNEL_UNREAD_UPDATE", "MESSAGE_ACK", "CHANNEL_PINS_UPDATE",
        "GUILD_APPLICATION_COMMAND_INDEX_UPDATE", "USER_REQUIRED_ACTION_UPDATE",
        "APPLICATION_COMMAND_AUTOCOMPLETE_RESPONSE",
        "INTERACTION_CREATE", "INTERACTION_SUCCESS", "INTERACTION_FAILURE", "INTERACTION_MODAL_CREATE",
        "USER_NOTE_UPDATE", "GUILD_SCHEDULED_EVENT_CREATE", "GUILD_SCHEDULED_EVENT_UPDATE",
        "GUILD_SCHEDULED_EVENT_DELETE", "GUILD_SCHEDULED_EVENT_USER_ADD", "GUILD_SCHEDULED_EVENT_USER_REMOVE",
        "VOICE_CHANNEL_EFFECT_SEND", "GUILD_SOUNDBOARD_SOUND_CREATE", "GUILD_SOUNDBOARD_SOUND_UPDATE",
        "GUILD_SOUNDBOARD_SOUND_DELETE", "GUILD_SOUNDBOARD_SOUNDS_UPDATE", "SOUNDBOARD_SOUNDS",
        "GUILD_BULK_BAN_UPDATE", "CHANNEL_INFO", "VOICE_CHANNEL_START_TIME_UPDATE",
        "VOICE_CHANNEL_STATUS_UPDATE",
        "STATE_UPDATE", "CHANNEL_RECIPIENT_ADD", "CHANNEL_RECIPIENT_REMOVE", "STAGE_INSTANCE_CREATE",
        "STAGE_INSTANCE_UPDATE", "STAGE_INSTANCE_DELETE", "CALL_CREATE", "CALL_UPDATE", "CALL_DELETE",
        "READY_USER", "USER_UPDATE", "USER_GUILD_SETTINGS_UPDATE", "USER_SETTINGS_PROTO_UPDATE",
        "NOTIFICATION_SETTINGS_UPDATE">(envelope.event_name);
    if (early <= 1)
        return gateway_client_route_channel_cursors(envelope, structural_scratch, maximum_depth,
                                                     content_scratch, event, payload);
    if (early >= 38 && early <= 40 &&
        notification_route(envelope, structural_scratch, content_scratch, event, payload, early - 38))
        return {};
    if (early == 2)
        return gateway_client_route_message_ack(envelope.data, structural_scratch, maximum_depth, event,
                                                payload);
    if (early == 3)
        return route_channel_pins_update(envelope.data, structural_scratch, maximum_depth, event);
    if (early == 4) {
        // A cache hint is ancillary; malformed data never fails the session.
        route_command_index_update(envelope.data, structural_scratch, maximum_depth, event);
        return {};
    }
    if (early == 5) {
        if (!account_requirement_update(envelope.data, structural_scratch, &event->required_action))
            return route_error(GatewayClientDispatchRouteError::decode,
                               GatewayEventDecodeError::invalid_data);
        event->dispatch_kind = GatewayClientDispatchKind::account_required_action;
        return {};
    }
    if (early == 6) {
        // Suggestions are ancillary: malformed/oversized responses must not
        // tear down the Gateway or invalidate channel history.
        if (envelope.data.kind == JsonValueKind::object &&
            envelope.data.source.bytes <= content_scratch.count) {
            copy_bytes(content_scratch.data, envelope.data.source.data, envelope.data.source.bytes);
            event->dispatch_kind = GatewayClientDispatchKind::command_autocomplete;
            event->dispatch_payload_bytes = envelope.data.source.bytes;
            *payload = {reinterpret_cast<const u8*>(content_scratch.data), envelope.data.source.bytes};
        }
        return {};
    }
    if (early >= 7 && early <= 26) {
        // Interaction lifecycle events are small and ancillary like
        // autocomplete: a malformed one never fails the session.
        static constexpr GatewayClientDispatchKind kinds[]{
            GatewayClientDispatchKind::interaction_create,
            GatewayClientDispatchKind::interaction_success,
            GatewayClientDispatchKind::interaction_failure,
            GatewayClientDispatchKind::interaction_modal_create,
            GatewayClientDispatchKind::user_note_update,
            // Scheduled events ride the same verbatim path (guild_community.hpp).
            GatewayClientDispatchKind::guild_scheduled_event_create,
            GatewayClientDispatchKind::guild_scheduled_event_update,
            GatewayClientDispatchKind::guild_scheduled_event_delete,
            GatewayClientDispatchKind::guild_scheduled_event_user_add,
            GatewayClientDispatchKind::guild_scheduled_event_user_remove,
            // Soundboard (soundboard.hpp): effects in voice channels and sound lists.
            GatewayClientDispatchKind::voice_channel_effect_send,
            GatewayClientDispatchKind::guild_soundboard_sound_create,
            GatewayClientDispatchKind::guild_soundboard_sound_update,
            GatewayClientDispatchKind::guild_soundboard_sound_delete,
            GatewayClientDispatchKind::guild_soundboard_sounds_update,
            GatewayClientDispatchKind::soundboard_sounds,
            GatewayClientDispatchKind::guild_bulk_ban_update,
            // Voice channel statuses and start times (voice_channel_info.hpp).
            GatewayClientDispatchKind::channel_info,
            GatewayClientDispatchKind::voice_channel_start_time_update,
            GatewayClientDispatchKind::voice_channel_status_update};
        if (envelope.data.kind == JsonValueKind::object &&
            envelope.data.source.bytes <= content_scratch.count) {
            copy_bytes(content_scratch.data, envelope.data.source.data, envelope.data.source.bytes);
            event->dispatch_kind = kinds[early - 7];
            event->dispatch_payload_bytes = envelope.data.source.bytes;
            *payload = {reinterpret_cast<const u8*>(content_scratch.data), envelope.data.source.bytes};
        }
        return {};
    }
    if (early == 27) {
        if (!upload_policy_state(envelope.data, self_user_id, structural_scratch,
                                 &event->state.upload_policy, &event->state.upload_policy_known))
            return route_error(GatewayClientDispatchRouteError::decode,
                               GatewayEventDecodeError::invalid_data);
        event->dispatch_kind = GatewayClientDispatchKind::user_snapshot;
        return {};
    }
    bool relationship_handled = false;
    const auto relationship = gateway_route_relationship(envelope, structural_scratch, content_scratch,
                                                         event, payload, &relationship_handled);
    if (relationship_handled)
        return relationship;
    if (early == 28 || early == 29)
        return gateway_route_recipient(envelope, structural_scratch, maximum_depth, content_scratch, event,
                                       payload);
    if (early >= 30 && early <= 32)
        return gateway_client_route_stage_instance(envelope, structural_scratch, maximum_depth, content_scratch, event,
                                                   payload);
    if (early >= 33 && early <= 35)
        return gateway_route_call(envelope, structural_scratch, maximum_depth, self_user_id, event, payload);

    if (early == 36 || early == 37) {
        if (!copy_event_name(envelope.event_name, event))
            return route_error(GatewayClientDispatchRouteError::event_name_capacity);
        GatewayPayloadWriter writer{content_scratch, 0};
        const auto result = gateway_append_user(envelope.data, structural_scratch, maximum_depth, &writer);
        if (!result.ok())
            return result;
        event->dispatch_kind = GatewayClientDispatchKind::user_snapshot;
        event->state.record_count = 1;
        event->dispatch_payload_bytes = writer.used;
        *payload = {reinterpret_cast<const u8*>(content_scratch.data), writer.used};
        gateway_client_append_notification_sidecar(envelope, structural_scratch, content_scratch,
                                                   self_user_id, event, payload);
        return {};
    }

    bool state_handled = false;
    const GatewayClientDispatchRouteResult state =
        gateway_client_route_state_dispatch(envelope, structural_scratch, maximum_depth, content_scratch,
                                            self_user_id, event, payload, &state_handled);
    if (state_handled) {
        if (state.ok()) {
            event->dispatch_payload_bytes = payload->count;
            gateway_client_append_notification_sidecar(envelope, structural_scratch, content_scratch,
                                                       self_user_id, event, payload);
        }
        return state;
    }

    bool thread_handled = false;
    const auto thread_result = gateway_client_route_thread_member_dispatch(
        envelope, structural_scratch, maximum_depth, content_scratch, self_user_id, event, payload,
        &thread_handled);
    if (thread_handled) {
        if (thread_result.ok()) {
            event->dispatch_payload_bytes = payload->count;
            gateway_client_append_notification_sidecar(envelope, structural_scratch, content_scratch,
                                                       self_user_id, event, payload);
        }
        return thread_result;
    }
    bool admin_handled = false;
    const auto admin = gateway_client_route_guild_admin_dispatch(envelope, structural_scratch, maximum_depth,
                                                                 content_scratch, event, payload, &admin_handled);
    if (admin_handled)
        return admin;
    bool member_list_handled = false;
    const GatewayClientDispatchRouteResult member_list = gateway_client_route_member_list_dispatch(
        envelope, structural_scratch, maximum_depth, content_scratch, event, payload, &member_list_handled);
    if (member_list_handled) {
        if (member_list.ok()) {
            event->dispatch_payload_bytes = payload->count;
        }
        return member_list;
    }

    GatewayEventDecodeResult decoded{};
    const u32 late = protocol_detail::wide_member_index<
        "PRESENCE_UPDATE", "TYPING_START", "MESSAGE_CREATE", "MESSAGE_UPDATE", "MESSAGE_POLL_VOTE_ADD",
        "MESSAGE_POLL_VOTE_REMOVE", "MESSAGE_REACTION_ADD", "MESSAGE_REACTION_REMOVE",
        "MESSAGE_REACTION_REMOVE_ALL", "MESSAGE_REACTION_REMOVE_EMOJI", "MESSAGE_DELETE",
        "MESSAGE_DELETE_BULK">(envelope.event_name);
    JsonIndexedDocument frame;
    if (late <= 1) {
        const GatewayClientDispatchRouteResult indexed = frame_index(envelope, structural_scratch, &frame);
        if (!indexed.ok())
            return indexed;
    }
    if (late == 0) {
        UserAppearance appearance{};
        decoded = gateway_client_decode_presence(frame, envelope.data, maximum_depth, &event->presence,
                                                 &appearance);
        if (!decoded.ok())
            return route_decode_error(decoded);
        u32 extra = user_appearance_write(
            appearance, {reinterpret_cast<u8*>(content_scratch.data), content_scratch.count});
        if (appearance.fields && !extra)
            return route_error(GatewayClientDispatchRouteError::payload_capacity);
        // Rich presence rides behind the appearance record (user_activity.hpp);
        // an appearance with activities but no trailer means none are rich.
        if (extra && !append_presence_activities(frame, envelope.data, content_scratch, event->presence.user_id,
                                                 &extra))
            return route_error(GatewayClientDispatchRouteError::payload_capacity);
        *payload = {reinterpret_cast<const u8*>(content_scratch.data), extra};
        event->dispatch_kind = GatewayClientDispatchKind::presence_update;
    } else if (late == 1) {
        GatewayTypingStartEvent typing{};
        decoded = gateway_decode_typing_start(frame, envelope.data, maximum_depth, &typing);
        if (!decoded.ok()) {
            return route_decode_error(decoded);
        }
        event->dispatch_kind = GatewayClientDispatchKind::typing_start;
        event->typing = GatewayClientTypingStartEvent{typing.channel_id, typing.guild_id, typing.user_id,
                                                      typing.timestamp_seconds};
    } else if (late == 2 || late == 3) {
        // Messages decode into the caller's scratch; without one the route cannot decode them.
        const GatewayClientDispatchRouteResult routed_message =
            message_scratch
                ? route_message_create_update(envelope, structural_scratch, maximum_depth, content_scratch,
                                              bulk_ids, self_user_id, late == 2, event, payload, message_scratch)
                : route_error(GatewayClientDispatchRouteError::invalid_input);
        if (!routed_message.ok())
            return routed_message;
    } else if (late == 4 || late == 5) {
        const bool add = late == 4;
        GatewayMessagePollVoteEvent vote{};
        decoded =
            gateway_decode_message_poll_vote(envelope.data, structural_scratch, maximum_depth, add, &vote);
        if (!decoded.ok())
            return route_decode_error(decoded);
        event->dispatch_kind = add ? GatewayClientDispatchKind::message_poll_vote_add
                                   : GatewayClientDispatchKind::message_poll_vote_remove;
        event->poll_vote = vote;
    } else if (late >= 6 && late <= 9) {
        const bool add = late == 6;
        const bool remove = late == 7;
        const bool remove_all = late == 8;
        const GatewayMessageReactionEventKind kind = add      ? GatewayMessageReactionEventKind::add
                                                     : remove ? GatewayMessageReactionEventKind::remove
                                                     : remove_all
                                                         ? GatewayMessageReactionEventKind::remove_all
                                                         : GatewayMessageReactionEventKind::remove_emoji;
        GatewayMessageReactionEvent reaction{};
        decoded = gateway_decode_message_reaction(envelope.data, structural_scratch, maximum_depth,
                                                  content_scratch, kind, &reaction);
        if (!decoded.ok()) {
            return route_decode_error(decoded);
        }
        event->dispatch_kind = add          ? GatewayClientDispatchKind::message_reaction_add
                               : remove     ? GatewayClientDispatchKind::message_reaction_remove
                               : remove_all ? GatewayClientDispatchKind::message_reaction_remove_all
                                            : GatewayClientDispatchKind::message_reaction_remove_emoji;
        copy_reaction(reaction, &event->reaction);
        *payload = ConstSlice<u8>{reinterpret_cast<const u8*>(content_scratch.data), reaction.name.bytes};
    } else if (late == 10) {
        GatewayMessageDeleteEvent message{};
        decoded = gateway_decode_message_delete(envelope.data, structural_scratch, maximum_depth, &message);
        if (!decoded.ok()) {
            return route_decode_error(decoded);
        }
        event->dispatch_kind = GatewayClientDispatchKind::message_delete;
        event->message.id = message.id;
        event->message.channel_id = message.channel_id;
        event->message.fields = gateway_message_field_id | gateway_message_field_channel_id;
    } else if (late == 11) {
        GatewayMessageBulkDeleteEvent message{};
        decoded = gateway_decode_message_bulk_delete(envelope.data, structural_scratch, maximum_depth,
                                                     bulk_ids, &message);
        if (!decoded.ok()) {
            return route_decode_error(decoded);
        }
        event->dispatch_kind = GatewayClientDispatchKind::message_bulk_delete;
        event->message.channel_id = message.channel_id;
        event->message.fields = gateway_message_field_channel_id;
        event->message.id_count = message.id_count;
        *payload = ConstSlice<u8>{reinterpret_cast<const u8*>(bulk_ids.data),
                                  message.id_count * static_cast<u32>(sizeof(u64))};
    } else if (!copy_event_name(envelope.event_name, event)) {
        return route_error(GatewayClientDispatchRouteError::event_name_capacity);
    }
    event->dispatch_payload_bytes = payload->count;
    gateway_client_append_notification_sidecar(envelope, structural_scratch, content_scratch,
                                               self_user_id, event, payload);
    return route_error(GatewayClientDispatchRouteError::none);
}

void gateway_client_route_failure_event(const GatewayEnvelope& envelope,
                                        const GatewayClientDispatchRouteResult& routed, u64 now_ms,
                                        GatewayClientEvent* event) noexcept {
    if (!event)
        return;
    event->kind = GatewayClientEventKind::dispatch;
    event->sequence = envelope.sequence;
    event->has_sequence = envelope.has_sequence;
    event->timestamp_ms = now_ms;
    event->failure_stage = GatewayClientFailureStage::dispatch_route;
    event->event_decode_error = routed.event_error;
    event->json_scan_error = routed.scan_error;
    event->json_read_error = routed.read_error;
    event->route_error_offset = routed.error_offset;
    event->route_detail = routed.detail;
    const u32 failed_event = protocol_detail::member_index<
        "RELATIONSHIP_ADD", "RELATIONSHIP_UPDATE", "RELATIONSHIP_REMOVE", "READY_SUPPLEMENTAL",
        "CHANNEL_RECIPIENT_ADD", "CHANNEL_RECIPIENT_REMOVE", "MESSAGE_CREATE", "MESSAGE_UPDATE", "MESSAGE_DELETE",
        "MESSAGE_DELETE_BULK", "MESSAGE_REACTION_ADD_MANY", "MESSAGE_REACTION_ADD", "MESSAGE_REACTION_REMOVE",
        "MESSAGE_REACTION_REMOVE_ALL", "MESSAGE_REACTION_REMOVE_EMOJI", "MESSAGE_POLL_VOTE_ADD",
        "MESSAGE_POLL_VOTE_REMOVE", "MESSAGE_POLL_VOTE_ADD_MANY">(envelope.event_name);
    event->dispatch_kind = failed_event < 3    ? GatewayClientDispatchKind::relationship_state_incomplete
                           : failed_event < 6  ? GatewayClientDispatchKind::private_state_incomplete
                           : failed_event < 18 ? GatewayClientDispatchKind::message_catchup_required
                                               : GatewayClientDispatchKind::unknown;
    event->event_name_bytes = 0;
    // The other kinds use the union; only the unknown marker has room for a name.
    if (event->dispatch_kind == GatewayClientDispatchKind::unknown)
        copy_event_name(envelope.event_name, event);
}

namespace {
struct NotificationSnapshot {
    Slice<u32> objects;
    Slice<char> storage;
    bool (*publish)(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept;
    void* context;
    u64 sequence, now;
    bool incomplete, interrupted;
    const JsonIndexedDocument* doc;
    JsonValue entries;
};
struct NotificationPrior {
    const JsonIndexedDocument* doc;
    u32 before;
    u64 guild;
    bool duplicate;
};
bool notification_prior(void* opaque, u32 index, JsonValue value) noexcept {
    auto& p = *static_cast<NotificationPrior*>(opaque);
    if (index >= p.before)
        return true;
    NotificationFields f{};
    if (!json_visit_indexed_object(*p.doc, value, 64, notification_fields, &f).ok())
        return false;
    if ((f.seen & 2) && f.guild == p.guild)
        p.duplicate = true;
    return true;
}
bool snapshot_entry(void* opaque, u32 index, JsonValue value) noexcept {
    auto& s = *static_cast<NotificationSnapshot*>(opaque);
    auto e = gateway_dispatch_event(GatewayClientDispatchKind::notification_settings_entry, s.sequence,
                                    s.now);
    ConstSlice<u8> payload{};
    notification_entry(value, s.objects, s.storage, &e, &payload);
    if (e.dispatch_kind == GatewayClientDispatchKind::notification_settings_entry) {
        NotificationPrior prior{s.doc, index, e.notification.guild_id, false};
        if (!json_visit_indexed_array(*s.doc, s.entries, 64, notification_prior, &prior).ok() ||
            prior.duplicate)
            notification_incomplete(&e, &payload, e.notification.guild_id);
    }
    if (e.dispatch_kind == GatewayClientDispatchKind::notification_incomplete)
        s.incomplete = true;
    if (!s.publish(s.context, e, payload)) {
        s.interrupted = true;
        return false;
    }
    return true;
}
} // namespace
GatewayClientDispatchRouteResult gateway_client_publish_notification_snapshot(
    const GatewayReady& ready, Slice<u32> arrays, Slice<u32> objects, Slice<char> storage, u64 sequence,
    u64 now, bool (*publish)(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept,
    void* context) noexcept {
    if (!publish || !arrays.data || !arrays.count || !objects.data || !objects.count || !storage.data)
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    NotificationSnapshot s{
        objects, storage, publish, context, sequence, now, ready.notification_policy_invalid,
        false,   nullptr, {}};
    NotificationFields f{};
    JsonIndexedDocument doc{};
    const bool valid = ready.user_guild_settings.source.data &&
                       notification_document(ready.user_guild_settings, arrays, &doc, &f) &&
                       f.entries.kind == JsonValueKind::array;
    s.doc = &doc;
    s.entries = f.entries;
    s.incomplete |= !valid;
    auto begin = gateway_dispatch_event(GatewayClientDispatchKind::notification_snapshot_begin, s.sequence,
                                        s.now);
    begin.notification = {0,
                          0,
                          f.version,
                          0,
                          (f.partial ? gateway_notification_partial : 0u) |
                              (f.version_known ? gateway_notification_version_known : 0u) |
                              (s.incomplete ? gateway_notification_incomplete : 0u),
                          0,
                          0};
    if (!publish(context, begin, {}))
        return route_error(GatewayClientDispatchRouteError::payload_capacity);
    if (valid && !json_visit_indexed_array(doc, f.entries, 64, snapshot_entry, &s).ok()) {
        if (s.interrupted)
            return route_error(GatewayClientDispatchRouteError::payload_capacity);
        s.incomplete = true;
    }
    auto global = gateway_dispatch_event(GatewayClientDispatchKind::notification_global, s.sequence, s.now);
    MessageNotificationGlobal g{};
    NotificationFields account{};
    JsonIndexedDocument account_doc{};
    const bool account_valid =
        ready.notification_settings.source.data &&
        notification_document(ready.notification_settings, objects, &account_doc, &account) &&
        (account.seen & 4096);
    const bool proto_valid =
        ready.user_settings_proto.source.data && notification_proto(ready.user_settings_proto, storage, &g);
    global.notification.flags = (account_valid ? gateway_notification_account_flags_known : 0u) |
                                (!proto_valid ? gateway_notification_incomplete : 0u);
    global.notification.account_flags = account.flags;
    NotificationFields user{};
    JsonIndexedDocument user_doc{};
    if (notification_document(ready.user, objects, &user_doc, &user) &&
        restriction_inactive(user_doc, user, true))
        global.notification.flags |= gateway_notification_restricted_known;
    if (storage.count < sizeof(g))
        return route_error(GatewayClientDispatchRouteError::payload_capacity);
    copy_bytes(storage.data, &g, sizeof(g));
    global.dispatch_payload_bytes = sizeof(g);
    if (!publish(context, global, {reinterpret_cast<const u8*>(storage.data), sizeof(g)}))
        return route_error(GatewayClientDispatchRouteError::payload_capacity);
    s.incomplete |= !proto_valid || !account_valid;
    if (s.incomplete) {
        auto bad = gateway_dispatch_event(GatewayClientDispatchKind::notification_incomplete, s.sequence,
                                          s.now);
        bad.notification.flags = gateway_notification_incomplete;
        if (!publish(context, bad, {}))
            return route_error(GatewayClientDispatchRouteError::payload_capacity);
    }
    auto complete = gateway_dispatch_event(GatewayClientDispatchKind::notification_snapshot_complete,
                                           s.sequence, s.now);
    complete.notification = begin.notification;
    if (s.incomplete)
        complete.notification.flags |= gateway_notification_incomplete;
    if (!publish(context, complete, {}))
        return route_error(GatewayClientDispatchRouteError::payload_capacity);
    return {};
}

} // namespace maicord
