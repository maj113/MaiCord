#include "maicord/app/gateway_client.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/gateway_message_events.hpp"

#include "gateway_dispatch_common.hpp"

namespace maicord {
namespace {

template <typename Value>
bool read_packed(const ConstSlice<u8>& payload, u32 limit, u32* cursor, Value* value) noexcept {
    if (!cursor || !value || limit > payload.count || *cursor > limit || sizeof(Value) > limit - *cursor) {
        return false;
    }
    copy_bytes(value, payload.data + *cursor, static_cast<u32>(sizeof(Value)));
    *cursor += static_cast<u32>(sizeof(Value));
    return true;
}

bool unpack_text_range(const ConstSlice<u8>& payload_view, u32 transient_bytes, GatewayClientMessageTextRange range,
                       Utf8View* value) noexcept {
    const ConstSlice<u8> payload = payload_view; // *value is written below
    if (range.reserved != 0 || transient_bytes > payload.count) {
        return false;
    }
    reset_value<Utf8View>(value);
    if (range.bytes == 0) {
        return range.offset == 0;
    }
    if (range.offset > transient_bytes || range.bytes > transient_bytes - range.offset) {
        return false;
    }
    *value = Utf8View{reinterpret_cast<const char*>(payload.data) + range.offset, range.bytes};
    return utf8_validate(*value).valid;
}

bool reserved_zero(const u8* bytes, u32 count) noexcept {
    if (!bytes && count != 0) {
        return false;
    }
    for (u32 index = 0; index < count; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

bool unpack_embed_media(ConstSlice<u8> payload, u32 text_limit,
                        const GatewayClientMessageEmbedMediaMutation& source,
                        GatewayMessageEmbedMedia* destination) noexcept {
    if (!reserved_zero(source.reserved, sizeof(source.reserved)) ||
        (source.state & ~3u) != 0) {
        return false;
    }
    reset_value<GatewayMessageEmbedMedia>(destination);
    destination->width = source.width;
    destination->height = source.height;
    destination->flags = source.flags;
    destination->has_width = (source.state & 1u) != 0;
    destination->has_height = (source.state & 2u) != 0;
    return unpack_text_range(payload, text_limit, source.url, &destination->url) &&
           unpack_text_range(payload, text_limit, source.proxy_url, &destination->proxy_url) &&
           unpack_text_range(payload, text_limit, source.content_type, &destination->content_type) &&
           unpack_text_range(payload, text_limit, source.description, &destination->description);
}

bool message_payload_offsets(const GatewayClientEvent& event, const ConstSlice<u8>& payload, u32* role_offset,
                             u32* user_offset, u32* rich_offset) noexcept {
    const u32 transient_bytes = event.message.transient_bytes == 0 && event.message.content_bytes != 0 &&
                                        event.message.author_name_bytes == 0 &&
                                        event.message.author_avatar_hash_bytes == 0 &&
                                        event.message.member_nick_bytes == 0 &&
                                        event.message.mention_user_count == 0
                                    ? event.message.content_bytes
                                    : event.message.transient_bytes;
    if (!role_offset || !user_offset || !rich_offset || event.message.content_bytes > transient_bytes ||
        transient_bytes > payload.count ||
        event.message.author_name_bytes > transient_bytes - event.message.content_bytes ||
        event.message.author_avatar_hash_bytes >
            transient_bytes - event.message.content_bytes - event.message.author_name_bytes ||
        event.message.member_nick_bytes > transient_bytes - event.message.content_bytes -
                                              event.message.author_name_bytes -
                                              event.message.author_avatar_hash_bytes) {
        return false;
    }
    const u64 roles_end = static_cast<u64>(transient_bytes) +
                          static_cast<u64>(event.message.mention_role_count) * sizeof(u64);
    const u64 member_roles_end =
        roles_end + static_cast<u64>(event.message.member_role_count) * sizeof(u64);
    const u64 users_end = member_roles_end + static_cast<u64>(event.message.mention_user_count) *
                                                 sizeof(GatewayClientNamedUserMutation);
    const u64 rich_end = users_end + event.message.rich_payload_bytes;
    if (rich_end != payload.count) {
        return false;
    }
    *role_offset = transient_bytes;
    *user_offset = static_cast<u32>(member_roles_end);
    *rich_offset = static_cast<u32>(users_end);
    return true;
}

// A message create/update event whose descriptor matches its payload; reports the roles and users
// offsets.
bool message_event_offsets(const GatewayClientEvent& event, const ConstSlice<u8>& payload, u32* role_offset = nullptr,
                           u32* user_offset = nullptr) noexcept {
    u32 roles = 0, users = 0, rich = 0;
    if (event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::message_create &&
         event.dispatch_kind != GatewayClientDispatchKind::message_update) ||
        event.dispatch_payload_bytes != payload.count || (!payload.data && payload.count != 0) ||
        !message_payload_offsets(event, payload, &roles, &users, &rich))
        return false;
    if (role_offset)
        *role_offset = roles;
    if (user_offset)
        *user_offset = users;
    return true;
}

bool rich_header_valid(const GatewayClientMessageRichHeader& header, u32 rich_fields) noexcept {
    return !(
        !reserved_zero(header.reserved, sizeof(header.reserved)) ||
        header.reference_state > static_cast<u8>(GatewayMessageReferenceState::available) ||
        header.reaction_count > kGatewayMessageReactionMaximum ||
        header.attachment_count > kGatewayMessageAttachmentMaximum ||
        header.embed_count > kGatewayMessageEmbedMaximum ||
        header.embed_field_count > kGatewayMessageEmbedFieldMaximum ||
        header.sticker_count > kGatewayMessageStickerMaximum ||
        header.component_count > kGatewayMessageComponentMaximum ||
        ((rich_fields & gateway_message_field_interaction) == 0 &&
         (header.interaction_id != 0 || header.interaction_user_id != 0 || header.interaction_type != 0 ||
          header.interaction_user_name.bytes != 0 || header.interaction_user_avatar_hash.bytes != 0 ||
          header.interaction_command_name.bytes != 0)) ||
        ((rich_fields &
          (gateway_message_field_message_reference | gateway_message_field_referenced_message)) == 0 &&
         (header.reference_state != 0 || header.reference_message_id != 0 ||
          header.reference_channel_id != 0 || header.reference_guild_id != 0 ||
          header.reference_author_id != 0 || header.reference_author_name.bytes != 0 ||
          header.reference_author_avatar_hash.bytes != 0 || header.reference_content.bytes != 0 ||
          header.reference_content_flags != 0 || header.reference_message_type != 0)) ||
        ((rich_fields & gateway_message_field_reactions) == 0 && header.reaction_count != 0) ||
        ((rich_fields & gateway_message_field_attachments) == 0 && header.attachment_count != 0) ||
        ((rich_fields & gateway_message_field_embeds) == 0 &&
         (header.embed_count != 0 || header.embed_field_count != 0)) ||
        ((rich_fields & gateway_message_field_sticker_items) == 0 && header.sticker_count != 0) ||
        ((rich_fields & gateway_message_field_components) == 0 && header.component_count != 0));
}

bool poll_span_overlap(const void* left, u64 left_bytes, const void* right, u64 right_bytes) noexcept {
    if (!left_bytes || !right_bytes)
        return false;
    const auto a = reinterpret_cast<usize>(left), b = reinterpret_cast<usize>(right);
    return !left || !right || left_bytes > ~static_cast<usize>(0) - a ||
           right_bytes > ~static_cast<usize>(0) - b || (a < b + right_bytes && b < a + left_bytes);
}

bool read_poll_metadata(ConstSlice<u8> payload, u32 text_limit, u32 rich_end, u32* cursor,
                        const GatewayMessagePollScratch* scratch = nullptr,
                        GatewayMessagePoll* output = nullptr) noexcept {
    if (!cursor || (!!scratch != !!output))
        return false;
    if (scratch) {
        const u64 answer_bytes =
            static_cast<u64>(scratch->answers.count) * sizeof(GatewayMessagePollAnswer);
        const u64 count_bytes =
            static_cast<u64>(scratch->answer_counts.count) * sizeof(GatewayMessagePollAnswerCount);
        if (poll_span_overlap(scratch->answers.data, answer_bytes, payload.data, payload.count) ||
            poll_span_overlap(scratch->answer_counts.data, count_bytes, payload.data, payload.count) ||
            poll_span_overlap(output, sizeof(*output), payload.data, payload.count) ||
            poll_span_overlap(scratch->answers.data, answer_bytes, scratch->answer_counts.data,
                              count_bytes) ||
            poll_span_overlap(output, sizeof(*output), scratch->answers.data, answer_bytes) ||
            poll_span_overlap(output, sizeof(*output), scratch->answer_counts.data, count_bytes))
            return false;
    }
    u32 position = *cursor;
    GatewayClientMessagePollHeader header{};
    if (!read_packed(payload, rich_end, &position, &header) ||
        !reserved_zero(header.reserved, sizeof(header.reserved)) || (header.flags & ~31u) ||
        header.answer_count > kGatewayMessagePollMaximumAnswers ||
        header.count_count > kGatewayMessagePollMaximumAnswers)
        return false;
    GatewayMessagePoll candidate{};
    candidate.present = (header.flags & 1) != 0;
    candidate.expiry_null = (header.flags & 2) != 0;
    candidate.allow_multiselect = (header.flags & 4) != 0;
    candidate.results_known = (header.flags & 8) != 0;
    candidate.is_finalized = (header.flags & 16) != 0;
    candidate.expiry_ms = header.expiry_ms;
    candidate.fields = header.fields;
    candidate.layout_type = header.layout_type;
    if (!unpack_text_range(payload, text_limit, header.question, &candidate.question))
        return false;
    GatewayMessagePollAnswer answers[kGatewayMessagePollMaximumAnswers]{};
    GatewayMessagePollAnswerCount counts[kGatewayMessagePollMaximumAnswers]{};
    for (u32 index = 0; index < header.answer_count; ++index) {
        GatewayClientMessagePollAnswerMutation packed{};
        if (!read_packed(payload, rich_end, &position, &packed) ||
            !reserved_zero(packed.reserved, sizeof(packed.reserved)) || (packed.flags & ~1u))
            return false;
        auto& answer = answers[index];
        answer.answer_id = packed.answer_id;
        answer.emoji_id = packed.emoji_id;
        answer.emoji_animated = (packed.flags & 1) != 0;
        if (!unpack_text_range(payload, text_limit, packed.text, &answer.text) ||
            !unpack_text_range(payload, text_limit, packed.emoji_name, &answer.emoji_name))
            return false;
    }
    for (u32 index = 0; index < header.count_count; ++index) {
        GatewayClientMessagePollCountMutation packed{};
        if (!read_packed(payload, rich_end, &position, &packed) ||
            !reserved_zero(packed.reserved, sizeof(packed.reserved)) || (packed.flags & ~1u))
            return false;
        counts[index] = {packed.answer_id, packed.count, (packed.flags & 1) != 0};
    }
    candidate.answers = {answers, header.answer_count};
    candidate.answer_counts = {counts, header.count_count};
    if (!gateway_message_poll_valid(candidate))
        return false;
    if (scratch) {
        if (header.answer_count > scratch->answers.count ||
            header.count_count > scratch->answer_counts.count ||
            (header.answer_count && !scratch->answers.data) ||
            (header.count_count && !scratch->answer_counts.data))
            return false;
        for (u32 index = 0; index < header.answer_count; ++index)
            scratch->answers[index] = answers[index];
        for (u32 index = 0; index < header.count_count; ++index)
            scratch->answer_counts[index] = counts[index];
        candidate.answers = {header.answer_count ? scratch->answers.data : nullptr, header.answer_count};
        candidate.answer_counts = {header.count_count ? scratch->answer_counts.data : nullptr,
                                   header.count_count};
        *output = candidate;
    }
    *cursor = position;
    return true;
}

} // namespace

bool gateway_client_event_as_message_poll_vote(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                               GatewayMessagePollVoteEvent* output) noexcept {
    if (!output || event.kind != GatewayClientEventKind::dispatch || event.dispatch_payload_bytes ||
        payload.count ||
        (event.dispatch_kind != GatewayClientDispatchKind::message_poll_vote_add &&
         event.dispatch_kind != GatewayClientDispatchKind::message_poll_vote_remove))
        return false;
    const auto& vote = event.poll_vote;
    if (!vote.channel_id || !vote.message_id || !vote.user_id || !vote.answer_id ||
        (vote.has_guild ? !vote.guild_id : vote.guild_id != 0) ||
        vote.add != (event.dispatch_kind == GatewayClientDispatchKind::message_poll_vote_add))
        return false;
    *output = vote;
    return true;
}

bool gateway_client_event_as_message(const GatewayClientEvent& event, const ConstSlice<u8>& payload_view,
                                     GatewayMessageEvent* message) noexcept {
    const ConstSlice<u8> payload = payload_view; // *message is written throughout
    if (!message || event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::message_create &&
         event.dispatch_kind != GatewayClientDispatchKind::message_update) ||
        event.dispatch_payload_bytes != payload.count || (!payload.data && payload.count != 0) ||
        event.message.content_bytes > kGatewayMessageContentMaximumBytes ||
        event.message.mention_role_count > kGatewayMessageMentionRoleMaximumIds ||
        event.message.member_role_count > kGatewayMessageMemberRoleMaximumIds ||
        event.message.mention_user_count > kGatewayMessageMentionUserMaximum ||
        event.message.author_name_bytes > kGatewayMessageUserNameMaximumBytes ||
        event.message.member_nick_bytes > kGatewayMessageUserNameMaximumBytes ||
        event.message.author_avatar_hash_bytes > kGatewayMessageAvatarHashMaximumBytes) {
        return false;
    }
    const bool has_content = (event.message.fields & gateway_message_field_content) != 0;
    const bool has_roles = (event.message.fields & gateway_message_field_mention_roles) != 0;
    const bool has_member = (event.message.fields & gateway_message_field_member) != 0;
    u32 role_offset = 0;
    u32 user_offset = 0;
    u32 rich_offset = 0;
    if ((!has_content && event.message.content_bytes != 0) ||
        (!has_roles && event.message.mention_role_count != 0) ||
        (!has_member && (event.message.member_role_count != 0 || event.message.member_nick_bytes != 0)) ||
        !message_payload_offsets(event, payload, &role_offset, &user_offset, &rich_offset)) {
        return false;
    }
    reset_value<GatewayMessageEvent>(message);
    message->id = event.message.id;
    message->nonce = event.message.nonce;
    message->channel_id = event.message.channel_id;
    message->author_id = event.message.author_id;
    message->timestamp_ms = event.message.timestamp_ms;
    message->edited_timestamp_ms = event.message.edited_timestamp_ms;
    message->content = Utf8View{reinterpret_cast<const char*>(payload.data), event.message.content_bytes};
    message->author_name =
        Utf8View{reinterpret_cast<const char*>(payload.data) + event.message.content_bytes,
                 event.message.author_name_bytes};
    message->author_avatar_hash =
        Utf8View{reinterpret_cast<const char*>(payload.data) + event.message.content_bytes +
                     event.message.author_name_bytes,
                 event.message.author_avatar_hash_bytes};
    message->member_nick =
        Utf8View{reinterpret_cast<const char*>(payload.data) + event.message.content_bytes +
                     event.message.author_name_bytes + event.message.author_avatar_hash_bytes,
                 event.message.member_nick_bytes};
    message->transient_bytes = role_offset;
    message->flags = event.message.flags;
    message->type = event.message.type;
    message->author_public_flags = event.message.author_public_flags;
    message->author_bot = (event.message.mention_flags & gateway_client_message_author_bot) != 0;
    message->activity_instance_present =
        (event.message.mention_flags & gateway_client_message_activity_instance) != 0;
    message->application_present =
        (event.message.mention_flags & gateway_client_message_application_present) != 0;
    message->changelog_present =
        (event.message.mention_flags & gateway_client_message_changelog_present) != 0;
    message->interaction_metadata_present =
        (event.message.mention_flags & gateway_client_message_interaction_metadata_present) != 0;
    message->fields = event.message.fields;
    if (event.message.forwarding_features &
        ~(gateway_message_forwarding_poll | gateway_message_forwarding_call |
          gateway_message_forwarding_activity))
        return false;
    message->forwarding_features = static_cast<u8>(event.message.forwarding_features);
    message->mention_everyone =
        (event.message.mention_flags & gateway_client_message_mention_everyone) != 0;
    message->mentions_current_user =
        (event.message.mention_flags & gateway_client_message_mention_current_user) != 0;
    message->pinned = (event.message.mention_flags & gateway_client_message_pinned) != 0;
    message->poll.present = (event.message.fields & gateway_message_field_poll) &&
                            (event.message.forwarding_features & gateway_message_forwarding_poll);
    return utf8_validate(message->content).valid && utf8_validate(message->author_name).valid &&
           utf8_validate(message->author_avatar_hash).valid;
}

bool gateway_client_event_message_interaction_user(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                                   u64* user_id) noexcept {
    GatewayMessageEvent message{};
    if (!user_id || !gateway_client_event_as_message(event, payload, &message))
        return false;
    const u32 rich_fields = event.message.fields & kGatewayMessageRichFieldMask;
    if (!event.message.rich_payload_bytes) {
        if (rich_fields)
            return false;
        *user_id = 0;
        return true;
    }
    if (!rich_fields)
        return false;
    u32 text_limit{}, user_offset{}, cursor{};
    if (!message_payload_offsets(event, payload, &text_limit, &user_offset, &cursor))
        return false;
    GatewayClientMessageRichHeader header{};
    if (!read_packed(payload, payload.count, &cursor, &header) || !rich_header_valid(header, rich_fields))
        return false;
    // Descriptor lengths are checked without decoding unrelated arrays. Only
    // validated scalar/header text can affect the activity notification gate.
    const u64 end =
        static_cast<u64>(cursor) +
        static_cast<u64>(header.reaction_count) * sizeof(GatewayClientMessageReactionMutation) +
        static_cast<u64>(header.attachment_count) * sizeof(GatewayClientMessageAttachmentMutation) +
        static_cast<u64>(header.embed_count) * sizeof(GatewayClientMessageEmbedMutation) +
        static_cast<u64>(header.embed_field_count) * sizeof(GatewayClientMessageEmbedFieldMutation) +
        static_cast<u64>(header.sticker_count) * sizeof(GatewayClientMessageStickerMutation) +
        static_cast<u64>(header.component_count) * sizeof(GatewayClientMessageComponentMutation);
    if (end > payload.count)
        return false;
    cursor = static_cast<u32>(end);
    if ((rich_fields & gateway_message_field_call) && sizeof(GatewayClientMessageCallMutation) > payload.count - cursor)
        return false;
    if (rich_fields & gateway_message_field_call)
        cursor += static_cast<u32>(sizeof(GatewayClientMessageCallMutation));
    if ((rich_fields & gateway_message_field_poll) &&
        !read_poll_metadata(payload, text_limit, payload.count, &cursor))
        return false;
    if (cursor != payload.count)
        return false;
    Utf8View text{};
    if (!unpack_text_range(payload, text_limit, header.reference_author_name, &text) ||
        !unpack_text_range(payload, text_limit, header.reference_author_avatar_hash, &text) ||
        !unpack_text_range(payload, text_limit, header.reference_content, &text) ||
        !unpack_text_range(payload, text_limit, header.interaction_user_name, &text) ||
        !unpack_text_range(payload, text_limit, header.interaction_user_avatar_hash, &text) ||
        !unpack_text_range(payload, text_limit, header.interaction_command_name, &text))
        return false;
    *user_id = header.interaction_user_id;
    return true;
}

static bool unpack_message_rich_metadata(ConstSlice<u8> payload, u32 role_offset, u32 cursor, u32 rich_end,
                                         u32 rich_fields, const GatewayMessageRichScratch& rich_scratch,
                                         GatewayMessageEvent* message) noexcept {
    if ((!rich_scratch.reactions.data && rich_scratch.reactions.count != 0) ||
        (!rich_scratch.attachments.data && rich_scratch.attachments.count != 0) ||
        (!rich_scratch.embeds.data && rich_scratch.embeds.count != 0) ||
        (!rich_scratch.embed_fields.data && rich_scratch.embed_fields.count != 0) ||
        (!rich_scratch.stickers.data && rich_scratch.stickers.count != 0) ||
        (!rich_scratch.components.data && rich_scratch.components.count != 0)) {
        return false;
    }
    GatewayClientMessageRichHeader header{};
    if (!read_packed(payload, rich_end, &cursor, &header) || !rich_header_valid(header, rich_fields) ||
        header.reaction_count > rich_scratch.reactions.count ||
        header.attachment_count > rich_scratch.attachments.count ||
        header.embed_count > rich_scratch.embeds.count ||
        header.embed_field_count > rich_scratch.embed_fields.count ||
        header.sticker_count > rich_scratch.stickers.count ||
        header.component_count > rich_scratch.components.count) {
        return false;
    }
    message->reference.message_id = header.reference_message_id;
    message->reference.channel_id = header.reference_channel_id;
    message->reference.guild_id = header.reference_guild_id;
    message->reference.author_id = header.reference_author_id;
    message->reference.state = static_cast<GatewayMessageReferenceState>(header.reference_state);
    message->reference.type = header.reference_type;
    message->reference.content_flags = header.reference_content_flags;
    message->reference.message_type = header.reference_message_type;
    message->interaction.id = header.interaction_id;
    message->interaction.user_id = header.interaction_user_id;
    message->interaction.type = header.interaction_type;
    message->rich_truncations = header.truncations;
    if (!unpack_text_range(payload, role_offset, header.reference_author_name,
                           &message->reference.author_name) ||
        !unpack_text_range(payload, role_offset, header.reference_author_avatar_hash,
                           &message->reference.author_avatar_hash) ||
        !unpack_text_range(payload, role_offset, header.reference_content, &message->reference.content) ||
        !unpack_text_range(payload, role_offset, header.interaction_user_name,
                           &message->interaction.user_name) ||
        !unpack_text_range(payload, role_offset, header.interaction_user_avatar_hash,
                           &message->interaction.user_avatar_hash) ||
        !unpack_text_range(payload, role_offset, header.interaction_command_name,
                           &message->interaction.command_name)) {
        return false;
    }

    for (u32 index = 0; index < header.reaction_count; ++index) {
        GatewayClientMessageReactionMutation packed{};
        if (!read_packed(payload, rich_end, &cursor, &packed) ||
            !reserved_zero(packed.reserved, sizeof(packed.reserved)) || (packed.flags & ~7u) != 0 ||
            packed.count == 0 || packed.normal_count > packed.count ||
            packed.burst_count > packed.count - packed.normal_count) {
            return false;
        }
        GatewayMessageReaction& destination = rich_scratch.reactions[index];
        reset_value<GatewayMessageReaction>(&destination);
        destination.emoji_id = packed.emoji_id;
        destination.count = packed.count;
        destination.normal_count = packed.normal_count;
        destination.burst_count = packed.burst_count;
        destination.me = (packed.flags & 1u) != 0;
        destination.me_burst = (packed.flags & 2u) != 0;
        destination.animated = (packed.flags & 4u) != 0;
        if (!unpack_text_range(payload, role_offset, packed.name, &destination.name) ||
            (destination.emoji_id == 0 && destination.name.bytes == 0)) {
            return false;
        }
    }
    for (u32 index = 0; index < header.attachment_count; ++index) {
        GatewayClientMessageAttachmentMutation packed{};
        if (!read_packed(payload, rich_end, &cursor, &packed) ||
            !reserved_zero(packed.reserved, sizeof(packed.reserved)) || packed.audio_reserved != 0 ||
            packed.id == 0 || (packed.state & ~15u) != 0) {
            return false;
        }
        GatewayMessageAttachment& destination = rich_scratch.attachments[index];
        reset_value<GatewayMessageAttachment>(&destination);
        destination.id = packed.id;
        destination.size = packed.size;
        destination.width = packed.width;
        destination.height = packed.height;
        destination.flags = packed.flags;
        destination.duration_ms = packed.duration_ms;
        destination.has_duration = (packed.state & 8u) != 0;
        destination.has_width = (packed.state & 1u) != 0;
        destination.has_height = (packed.state & 2u) != 0;
        destination.ephemeral = (packed.state & 4u) != 0;
        if (!unpack_text_range(payload, role_offset, packed.filename, &destination.filename) ||
            !unpack_text_range(payload, role_offset, packed.title, &destination.title) ||
            !unpack_text_range(payload, role_offset, packed.description, &destination.description) ||
            !unpack_text_range(payload, role_offset, packed.content_type, &destination.content_type) ||
            !unpack_text_range(payload, role_offset, packed.url, &destination.url) ||
            !unpack_text_range(payload, role_offset, packed.waveform, &destination.waveform) ||
            !gateway_message_waveform_valid(destination.waveform) ||
            (!destination.has_duration && destination.duration_ms)) {
            return false;
        }
    }
    for (u32 index = 0; index < header.embed_count; ++index) {
        GatewayClientMessageEmbedMutation packed{};
        if (!read_packed(payload, rich_end, &cursor, &packed) ||
            packed.reserved[1] != 0 || (packed.flags & ~1u) != 0 ||
            packed.type > static_cast<u8>(GatewayMessageEmbedType::components) ||
            packed.field_begin > header.embed_field_count ||
            packed.field_count > header.embed_field_count - packed.field_begin) {
            return false;
        }
        GatewayMessageEmbed& destination = rich_scratch.embeds[index];
        reset_value<GatewayMessageEmbed>(&destination);
        destination.color = packed.color;
        destination.field_begin = packed.field_begin;
        destination.field_count = packed.field_count;
        destination.type = static_cast<GatewayMessageEmbedType>(packed.type);
        destination.has_color = (packed.flags & 1u) != 0;
        destination.flags = packed.reserved[0];
        destination.timestamp_ms = (static_cast<u64>(packed.timestamp_high) << 32u) | packed.timestamp_low;
        if (!unpack_text_range(payload, role_offset, packed.title, &destination.title) ||
            !unpack_text_range(payload, role_offset, packed.description, &destination.description) ||
            !unpack_text_range(payload, role_offset, packed.url, &destination.url) ||
            !unpack_text_range(payload, role_offset, packed.author_name, &destination.author_name) ||
            !unpack_text_range(payload, role_offset, packed.footer_text, &destination.footer_text) ||
            !unpack_text_range(payload, role_offset, packed.author_url, &destination.author_url) ||
            !unpack_text_range(payload, role_offset, packed.author_icon, &destination.author_icon) ||
            !unpack_text_range(payload, role_offset, packed.footer_icon, &destination.footer_icon) ||
            !unpack_text_range(payload, role_offset, packed.provider_name, &destination.provider_name) ||
            !unpack_text_range(payload, role_offset, packed.provider_url, &destination.provider_url) ||
            !unpack_embed_media(payload, role_offset, packed.image, &destination.image) ||
            !unpack_embed_media(payload, role_offset, packed.thumbnail, &destination.thumbnail) ||
            !unpack_embed_media(payload, role_offset, packed.video, &destination.video)) {
            return false;
        }
    }
    for (u32 index = 0; index < header.embed_field_count; ++index) {
        GatewayClientMessageEmbedFieldMutation packed{};
        if (!read_packed(payload, rich_end, &cursor, &packed) || packed.reserved != 0 ||
            (packed.flags & ~1u) != 0 || packed.embed_index >= header.embed_count) {
            return false;
        }
        GatewayMessageEmbedField& destination = rich_scratch.embed_fields[index];
        reset_value<GatewayMessageEmbedField>(&destination);
        destination.embed_index = packed.embed_index;
        destination.inline_field = (packed.flags & 1u) != 0;
        if (!unpack_text_range(payload, role_offset, packed.name, &destination.name) ||
            !unpack_text_range(payload, role_offset, packed.value, &destination.value)) {
            return false;
        }
    }
    for (u32 index = 0; index < header.sticker_count; ++index) {
        GatewayClientMessageStickerMutation packed{};
        if (!read_packed(payload, rich_end, &cursor, &packed) ||
            !reserved_zero(packed.reserved, sizeof(packed.reserved)) || packed.id == 0 ||
            packed.format_type == 0 || packed.format_type > 4) {
            return false;
        }
        GatewayMessageSticker& destination = rich_scratch.stickers[index];
        reset_value<GatewayMessageSticker>(&destination);
        destination.id = packed.id;
        destination.format_type = packed.format_type;
        if (!unpack_text_range(payload, role_offset, packed.name, &destination.name)) {
            return false;
        }
    }
    for (u32 index = 0; index < header.component_count; ++index) {
        GatewayClientMessageComponentMutation packed{};
        if (!read_packed(payload, rich_end, &cursor, &packed) || packed.reserved != 0 ||
            (packed.flags & ~3u) != 0 || (packed.component_flags & ~kGatewayMessageComponentKnownFlags) != 0 ||
            packed.type == 0 || ((packed.flags & 2u) != 0 && packed.accent_color > 0xFFFFFFu) ||
            (packed.parent != 0xFFFFu && packed.parent >= index) || packed.min_values > packed.max_values) {
            return false;
        }
        GatewayMessageComponent& destination = rich_scratch.components[index];
        reset_value<GatewayMessageComponent>(&destination);
        destination.id = packed.id;
        destination.parent = packed.parent;
        destination.order = packed.order;
        destination.type = packed.type;
        destination.style = packed.style;
        destination.disabled = (packed.flags & 1u) != 0;
        destination.accent_color = packed.accent_color;
        destination.has_accent_color = (packed.flags & 2u) != 0;
        destination.emoji_id = packed.emoji_id;
        destination.min_values = packed.min_values;
        destination.max_values = packed.max_values;
        destination.flags = packed.component_flags;
        if (!unpack_text_range(payload, role_offset, packed.label, &destination.label) ||
            !unpack_text_range(payload, role_offset, packed.content, &destination.content) ||
            !unpack_text_range(payload, role_offset, packed.custom_id, &destination.custom_id) ||
            !unpack_text_range(payload, role_offset, packed.url, &destination.url) ||
            !unpack_text_range(payload, role_offset, packed.emoji_name, &destination.emoji_name)) {
            return false;
        }
    }
    if (rich_fields & gateway_message_field_call) {
        GatewayClientMessageCallMutation call{};
        if (!read_packed(payload, rich_end, &cursor, &call) || (call.flags & ~7u) != 0 ||
            !reserved_zero(call.reserved, sizeof(call.reserved)))
            return false;
        message->call = {call.ended_timestamp_ms, (call.flags & 1u) != 0, (call.flags & 2u) != 0,
                         (call.flags & 4u) != 0};
    }
    if ((rich_fields & gateway_message_field_poll) &&
        !read_poll_metadata(payload, role_offset, rich_end, &cursor, &rich_scratch.poll, &message->poll))
        return false;
    if (rich_fields & gateway_message_field_message_snapshots) {
        GatewayClientMessageSnapshotHeader snapshot{};
        if (!read_packed(payload, rich_end, &cursor, &snapshot) || snapshot.present > 1 ||
            !reserved_zero(snapshot.reserved, sizeof(snapshot.reserved)) ||
            snapshot.mention_user_count > kGatewayMessageMentionUserMaximum ||
            snapshot.mention_user_count > rich_scratch.snapshot.mention_users.count)
            return false;
        constexpr u32 allowed = gateway_message_field_content | gateway_message_field_timestamp |
                                gateway_message_field_edited_timestamp | gateway_message_field_flags |
                                gateway_message_field_type | gateway_message_field_mentions |
                                gateway_message_field_mention_roles |
                                gateway_message_field_mention_everyone | gateway_message_field_attachments |
                                gateway_message_field_embeds | gateway_message_field_sticker_items |
                                gateway_message_field_components;
        if ((snapshot.fields & ~allowed) ||
            (!snapshot.present &&
             (snapshot.content.bytes || snapshot.timestamp_ms || snapshot.edited_timestamp_ms ||
              snapshot.flags || snapshot.fields || snapshot.rich_payload_bytes || snapshot.type ||
              snapshot.mention_user_count)))
            return false;
        GatewayMessageEvent body{};
        if (!unpack_text_range(payload, role_offset, snapshot.content, &body.content))
            return false;
        for (u32 index = 0; index < snapshot.mention_user_count; ++index) {
            GatewayClientNamedUserMutation user{};
            if (!read_packed(payload, rich_end, &cursor, &user) || !user.id || user.reserved ||
                user.name_bytes > kGatewayMessageUserNameMaximumBytes ||
                !rich_scratch.snapshot.mention_users.data)
                return false;
            auto& target = rich_scratch.snapshot.mention_users[index];
            target.id = user.id;
            if (!unpack_text_range(payload, role_offset, {user.name_offset, user.name_bytes, 0},
                                   &target.name))
                return false;
            for (u32 earlier = 0; earlier < index; ++earlier)
                if (rich_scratch.snapshot.mention_users[earlier].id == user.id)
                    return false;
        }
        const u64 child_end = static_cast<u64>(cursor) + snapshot.rich_payload_bytes;
        if (child_end > rich_end)
            return false;
        const GatewayMessageRichScratch child{{},
                                              rich_scratch.snapshot.attachments,
                                              rich_scratch.snapshot.embeds,
                                              rich_scratch.snapshot.embed_fields,
                                              rich_scratch.snapshot.stickers,
                                              rich_scratch.snapshot.components,
                                              {}};
        if (snapshot.rich_payload_bytes) {
            if (!unpack_message_rich_metadata(payload, role_offset, cursor, static_cast<u32>(child_end),
                                              snapshot.fields & kGatewayMessageRichFieldMask, child, &body))
                return false;
        } else if (snapshot.fields & kGatewayMessageRichFieldMask)
            return false;
        cursor = static_cast<u32>(child_end);
        message->snapshot = {body.content,
                             snapshot.timestamp_ms,
                             snapshot.edited_timestamp_ms,
                             body.attachments,
                             body.embeds,
                             body.embed_fields,
                             body.stickers,
                             body.components,
                             {rich_scratch.snapshot.mention_users.data, snapshot.mention_user_count},
                             snapshot.flags,
                             snapshot.fields,
                             body.rich_truncations,
                             snapshot.type,
                             snapshot.present != 0};
    }
    if (cursor != rich_end) {
        return false;
    }
    message->reactions =
        ConstSlice<GatewayMessageReaction>{rich_scratch.reactions.data, header.reaction_count};
    message->attachments =
        ConstSlice<GatewayMessageAttachment>{rich_scratch.attachments.data, header.attachment_count};
    message->embeds = ConstSlice<GatewayMessageEmbed>{rich_scratch.embeds.data, header.embed_count};
    message->embed_fields =
        ConstSlice<GatewayMessageEmbedField>{rich_scratch.embed_fields.data, header.embed_field_count};
    message->stickers = ConstSlice<GatewayMessageSticker>{rich_scratch.stickers.data, header.sticker_count};
    message->components =
        ConstSlice<GatewayMessageComponent>{rich_scratch.components.data, header.component_count};
    return true;
}

bool gateway_client_event_as_message(const GatewayClientEvent& event, const ConstSlice<u8>& payload,
                                     const GatewayMessageRichScratch& scratch,
                                     GatewayMessageEvent* message) noexcept {
    if (!message || !gateway_client_event_as_message(event, payload, message))
        return false;
    const u32 fields = event.message.fields & kGatewayMessageRichFieldMask;
    if (!event.message.rich_payload_bytes)
        return fields == 0;
    u32 role_offset = 0, user_offset = 0, cursor = 0;
    if (!fields || !message_payload_offsets(event, payload, &role_offset, &user_offset, &cursor) ||
        static_cast<u64>(cursor) + event.message.rich_payload_bytes != payload.count)
        return false;
    return unpack_message_rich_metadata(payload, role_offset, cursor, payload.count, fields, scratch,
                                        message);
}

static bool snapshot_user_offsets(const GatewayClientEvent& event, ConstSlice<u8> payload, u32* users,
                                  u32* count, u32* text_limit) noexcept {
    GatewayMessageEvent message{};
    if (!users || !count || !text_limit || !gateway_client_event_as_message(event, payload, &message))
        return false;
    *count = 0;
    *users = 0;
    *text_limit = event.message.transient_bytes;
    if (!(event.message.fields & gateway_message_field_message_snapshots))
        return true;
    u32 role_offset = 0, user_offset = 0, cursor = 0;
    if (!message_payload_offsets(event, payload, &role_offset, &user_offset, &cursor) ||
        static_cast<u64>(cursor) + event.message.rich_payload_bytes != payload.count)
        return false;
    GatewayClientMessageRichHeader outer{};
    if (!read_packed(payload, payload.count, &cursor, &outer) ||
        !rich_header_valid(outer, event.message.fields & kGatewayMessageRichFieldMask))
        return false;
    const u64 descriptors =
        static_cast<u64>(outer.reaction_count) * sizeof(GatewayClientMessageReactionMutation) +
        static_cast<u64>(outer.attachment_count) * sizeof(GatewayClientMessageAttachmentMutation) +
        static_cast<u64>(outer.embed_count) * sizeof(GatewayClientMessageEmbedMutation) +
        static_cast<u64>(outer.embed_field_count) * sizeof(GatewayClientMessageEmbedFieldMutation) +
        static_cast<u64>(outer.sticker_count) * sizeof(GatewayClientMessageStickerMutation) +
        static_cast<u64>(outer.component_count) * sizeof(GatewayClientMessageComponentMutation);
    const u64 call_bytes =
        (event.message.fields & gateway_message_field_call) ? sizeof(GatewayClientMessageCallMutation) : 0u;
    if (descriptors + call_bytes > payload.count - cursor)
        return false;
    cursor += static_cast<u32>(descriptors + call_bytes);
    if ((event.message.fields & gateway_message_field_poll) &&
        !read_poll_metadata(payload, role_offset, payload.count, &cursor))
        return false;
    GatewayClientMessageSnapshotHeader snapshot{};
    Utf8View content{};
    constexpr u32 allowed = gateway_message_field_content | gateway_message_field_timestamp |
                            gateway_message_field_edited_timestamp | gateway_message_field_flags |
                            gateway_message_field_type | gateway_message_field_mentions |
                            gateway_message_field_mention_roles | gateway_message_field_mention_everyone |
                            gateway_message_field_attachments | gateway_message_field_embeds |
                            gateway_message_field_sticker_items | gateway_message_field_components;
    if (!read_packed(payload, payload.count, &cursor, &snapshot) || snapshot.present > 1 ||
        !reserved_zero(snapshot.reserved, sizeof(snapshot.reserved)) || (snapshot.fields & ~allowed) ||
        snapshot.mention_user_count > kGatewayMessageMentionUserMaximum ||
        !unpack_text_range(payload, role_offset, snapshot.content, &content) ||
        (!snapshot.present && (snapshot.fields || snapshot.content.bytes || snapshot.timestamp_ms ||
                               snapshot.edited_timestamp_ms || snapshot.flags || snapshot.type ||
                               snapshot.mention_user_count || snapshot.rich_payload_bytes)))
        return false;
    const u32 begin = cursor;
    for (u32 index = 0; index < snapshot.mention_user_count; ++index) {
        GatewayClientNamedUserMutation user{};
        Utf8View name{};
        if (!read_packed(payload, payload.count, &cursor, &user) || !user.id || user.reserved ||
            user.name_bytes > kGatewayMessageUserNameMaximumBytes ||
            !unpack_text_range(payload, role_offset, {user.name_offset, user.name_bytes, 0}, &name))
            return false;
        for (u32 previous = 0; previous < index; ++previous) {
            GatewayClientNamedUserMutation earlier{};
            u32 at = begin + previous * sizeof(earlier);
            if (!read_packed(payload, payload.count, &at, &earlier) || earlier.id == user.id)
                return false;
        }
    }
    if (static_cast<u64>(cursor) + snapshot.rich_payload_bytes != payload.count)
        return false;
    if (snapshot.rich_payload_bytes) {
        GatewayClientMessageRichHeader child{};
        if (!read_packed(payload, payload.count, &cursor, &child) ||
            !rich_header_valid(child, snapshot.fields & kGatewayMessageRichFieldMask))
            return false;
        const u64 child_bytes =
            static_cast<u64>(child.reaction_count) * sizeof(GatewayClientMessageReactionMutation) +
            static_cast<u64>(child.attachment_count) * sizeof(GatewayClientMessageAttachmentMutation) +
            static_cast<u64>(child.embed_count) * sizeof(GatewayClientMessageEmbedMutation) +
            static_cast<u64>(child.embed_field_count) * sizeof(GatewayClientMessageEmbedFieldMutation) +
            static_cast<u64>(child.sticker_count) * sizeof(GatewayClientMessageStickerMutation) +
            static_cast<u64>(child.component_count) * sizeof(GatewayClientMessageComponentMutation);
        if (static_cast<u64>(cursor) + child_bytes != payload.count)
            return false;
    } else if (snapshot.fields & kGatewayMessageRichFieldMask)
        return false;
    *users = begin;
    *count = snapshot.mention_user_count;
    *text_limit = role_offset;
    return true;
}

bool gateway_client_event_visit_message_snapshot_mention_users(
    const GatewayClientEvent& event, ConstSlice<u8> payload, void* context,
    bool (*visit)(void*, u64, Utf8View) noexcept) noexcept {
    u32 offset = 0, count = 0, text_limit = 0;
    if (!visit || !snapshot_user_offsets(event, payload, &offset, &count, &text_limit))
        return false;
    for (u32 index = 0; index < count; ++index) {
        GatewayClientNamedUserMutation user{};
        Utf8View name{};
        if (!read_packed(payload, payload.count, &offset, &user) ||
            !unpack_text_range(payload, text_limit, {user.name_offset, user.name_bytes, 0}, &name) ||
            !visit(context, user.id, name))
            return false;
    }
    return true;
}
bool gateway_client_event_message_mention_role_at(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                                  u32 index, u64* role_id) noexcept {
    u32 role_offset = 0;
    if (!role_id || index >= event.message.mention_role_count ||
        !message_event_offsets(event, payload, &role_offset)) {
        return false;
    }
    const u64 offset = static_cast<u64>(role_offset) + static_cast<u64>(index) * sizeof(u64);
    if (offset + sizeof(u64) > payload.count) {
        return false;
    }
    const u64 value = byte_load_le64(payload.data + static_cast<u32>(offset));
    *role_id = value;
    return value != 0;
}

bool gateway_client_event_message_member_role_at(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                                 u32 index, u64* role_id) noexcept {
    u32 role_offset = 0;
    u32 user_offset = 0;
    if (!role_id || index >= event.message.member_role_count ||
        !message_event_offsets(event, payload, &role_offset, &user_offset)) {
        return false;
    }
    const u64 offset = static_cast<u64>(role_offset) +
                       static_cast<u64>(event.message.mention_role_count) * sizeof(u64) +
                       static_cast<u64>(index) * sizeof(u64);
    if (offset + sizeof(u64) > user_offset) {
        return false;
    }
    const u64 value = byte_load_le64(payload.data + static_cast<u32>(offset));
    *role_id = value;
    return value != 0;
}

bool gateway_client_event_message_author_name(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                              Utf8View* name) noexcept {
    if (!name || !message_event_offsets(event, payload))
        return false;
    *name = Utf8View{reinterpret_cast<const char*>(payload.data) + event.message.content_bytes,
                     event.message.author_name_bytes};
    return utf8_validate(*name).valid;
}

bool gateway_client_event_message_author_avatar_hash(const GatewayClientEvent& event,
                                                     ConstSlice<u8> payload,
                                                     Utf8View* avatar_hash) noexcept {
    if (!avatar_hash || !message_event_offsets(event, payload))
        return false;
    *avatar_hash = Utf8View{reinterpret_cast<const char*>(payload.data) + event.message.content_bytes +
                                event.message.author_name_bytes,
                            event.message.author_avatar_hash_bytes};
    return utf8_validate(*avatar_hash).valid;
}

bool gateway_client_event_message_member_nick(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                              Utf8View* nick) noexcept {
    if (!nick || !message_event_offsets(event, payload))
        return false;
    *nick = Utf8View{reinterpret_cast<const char*>(payload.data) + event.message.content_bytes +
                         event.message.author_name_bytes + event.message.author_avatar_hash_bytes,
                     event.message.member_nick_bytes};
    return utf8_validate(*nick).valid;
}

bool gateway_client_event_message_mention_user_at(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                                  u32 index, u64* user_id, Utf8View* name) noexcept {
    u32 user_offset = 0;
    if (!user_id || !name || index >= event.message.mention_user_count ||
        !message_event_offsets(event, payload, nullptr, &user_offset)) {
        return false;
    }
    GatewayClientNamedUserMutation packed{};
    copy_bytes(&packed, payload.data + user_offset + index * static_cast<u32>(sizeof(packed)),
               sizeof(packed));
    if (packed.id == 0 || packed.name_bytes > kGatewayMessageUserNameMaximumBytes ||
        packed.name_offset < event.message.content_bytes ||
        packed.name_offset > event.message.transient_bytes ||
        packed.name_bytes > event.message.transient_bytes - packed.name_offset) {
        return false;
    }
    *user_id = packed.id;
    *name = Utf8View{reinterpret_cast<const char*>(payload.data) + packed.name_offset, packed.name_bytes};
    return utf8_validate(*name).valid;
}

bool gateway_client_event_as_message_delete(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                            GatewayMessageDeleteEvent* message) noexcept {
    if (!message || event.kind != GatewayClientEventKind::dispatch ||
        event.dispatch_kind != GatewayClientDispatchKind::message_delete ||
        event.dispatch_payload_bytes != 0 || payload.count != 0) {
        return false;
    }
    *message = GatewayMessageDeleteEvent{event.message.id, event.message.channel_id};
    return event.message.id != 0 && event.message.channel_id != 0;
}

bool gateway_client_event_as_message_bulk_delete(const GatewayClientEvent& event, const ConstSlice<u8>& payload_view,
                                                 const Slice<u64>& ids_view,
                                                 GatewayMessageBulkDeleteEvent* message) noexcept {
    const ConstSlice<u8> payload = payload_view; // *message is written throughout
    const Slice<u64> ids = ids_view;
    if (!message || event.kind != GatewayClientEventKind::dispatch ||
        event.dispatch_kind != GatewayClientDispatchKind::message_bulk_delete ||
        event.message.id_count == 0 || event.message.id_count > kGatewayBulkDeleteMaximumIds ||
        event.dispatch_payload_bytes != payload.count ||
        payload.count != event.message.id_count * static_cast<u32>(sizeof(u64)) ||
        (!payload.data && payload.count != 0) || !ids.data || ids.count < event.message.id_count) {
        return false;
    }
    for (u32 index = 0; index < event.message.id_count; ++index) {
        const u64 id = byte_load_le64(payload.data + index * sizeof(u64));
        if (id == 0) {
            return false;
        }
        ids[index] = id;
    }
    *message = GatewayMessageBulkDeleteEvent{event.message.channel_id, event.message.id_count};
    return event.message.channel_id != 0;
}

bool gateway_client_event_as_message_reaction(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                              GatewayMessageReactionEvent* reaction) noexcept {
    if (!reaction || event.kind != GatewayClientEventKind::dispatch ||
        event.dispatch_payload_bytes != payload.count || (!payload.data && payload.count != 0) ||
        event.reaction.message_id == 0 || event.reaction.channel_id == 0 ||
        event.reaction.name_bytes != payload.count ||
        event.reaction.name_bytes > kMessageReactionMaximumEmojiNameBytes ||
        event.reaction.type > static_cast<u8>(MessageReactionType::vote) ||
        (event.reaction.flags & ~1u) != 0) {
        return false;
    }
    GatewayMessageReactionEventKind kind{};
    if (event.dispatch_kind == GatewayClientDispatchKind::message_reaction_add) {
        kind = GatewayMessageReactionEventKind::add;
    } else if (event.dispatch_kind == GatewayClientDispatchKind::message_reaction_remove) {
        kind = GatewayMessageReactionEventKind::remove;
    } else if (event.dispatch_kind == GatewayClientDispatchKind::message_reaction_remove_all) {
        kind = GatewayMessageReactionEventKind::remove_all;
    } else if (event.dispatch_kind == GatewayClientDispatchKind::message_reaction_remove_emoji) {
        kind = GatewayMessageReactionEventKind::remove_emoji;
    } else {
        return false;
    }
    const bool needs_user =
        kind == GatewayMessageReactionEventKind::add || kind == GatewayMessageReactionEventKind::remove;
    const bool needs_emoji = kind != GatewayMessageReactionEventKind::remove_all;
    if ((needs_user && event.reaction.user_id == 0) ||
        (needs_emoji &&
         ((event.reaction.name_bytes == 0 && event.reaction.emoji_id == 0) ||
          !utf8_validate(Utf8View{reinterpret_cast<const char*>(payload.data), payload.count}).valid)) ||
        (!needs_emoji &&
         (event.reaction.user_id != 0 || event.reaction.emoji_id != 0 || payload.count != 0))) {
        return false;
    }
    reset_value<GatewayMessageReactionEvent>(reaction);
    reaction->message_id = event.reaction.message_id;
    reaction->channel_id = event.reaction.channel_id;
    reaction->user_id = event.reaction.user_id;
    reaction->emoji_id = event.reaction.emoji_id;
    reaction->name =
        needs_emoji ? Utf8View{reinterpret_cast<const char*>(payload.data), payload.count} : Utf8View{};
    reaction->type = static_cast<MessageReactionType>(event.reaction.type);
    reaction->kind = kind;
    reaction->animated = (event.reaction.flags & 1u) != 0;
    return true;
}

bool gateway_client_event_as_typing_start(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                          GatewayTypingStartEvent* typing) noexcept {
    if (!typing || event.kind != GatewayClientEventKind::dispatch ||
        event.dispatch_kind != GatewayClientDispatchKind::typing_start ||
        event.dispatch_payload_bytes != 0 || payload.count != 0 || event.typing.channel_id == 0 ||
        event.typing.user_id == 0 || event.typing.timestamp_seconds == 0) {
        return false;
    }
    *typing = GatewayTypingStartEvent{event.typing.channel_id, event.typing.guild_id, event.typing.user_id,
                                      event.typing.timestamp_seconds};
    return true;
}

} // namespace maicord
