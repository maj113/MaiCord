#include "maicord/app/gateway/client_dispatch.hpp"
#include "maicord/app/gateway/client_event.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

#include "dispatch_common.hpp"
#include "message_mailbox.hpp"

namespace maicord {
namespace {

using TextRange = GatewayClientMessageTextRange;

// The descriptor block of one mailbox payload being read: ranges lie in data[0, text_limit), records in data[at, end).
// Every Reader is built with text_limit <= count and at <= end <= count (message_payload_offsets proves them, a
// nested block only narrows end) and a read advances at only within end, so reads never re-check either bound.
struct Reader {
    const u8* data;
    u32 count;
    u32 text_limit;
    u32 end;
    u32 at;
};

__declspec(noinline) bool read_bytes(Reader& reader, void* value, u32 bytes) noexcept {
    if (bytes > reader.end - reader.at) {
        return false;
    }
    copy_bytes(value, reader.data + reader.at, bytes);
    reader.at += bytes;
    return true;
}

template <typename Value> bool read_packed(Reader& reader, Value* value) noexcept {
    return read_bytes(reader, value, static_cast<u32>(sizeof(Value)));
}

// `count` adjacent ranges as `count` adjacent views of the transient text (message_mailbox.hpp). An empty range
// must have offset 0; any other lies inside the text and is UTF-8.
__declspec(noinline) bool unpack_texts(const Reader& reader, const TextRange* ranges, Utf8View* views,
                                       u32 count) noexcept {
    const char* const text = reinterpret_cast<const char*>(reader.data);
    for (u32 index = 0; index < count; ++index) {
        const TextRange range = ranges[index];
        if (range.reserved != 0) {
            return false;
        }
        views[index] = Utf8View{};
        if (range.bytes == 0) {
            if (range.offset != 0) {
                return false;
            }
            continue;
        }
        if (range.offset > reader.text_limit || range.bytes > reader.text_limit - range.offset) {
            return false;
        }
        views[index] = Utf8View{text + range.offset, range.bytes};
        if (!utf8_validate(views[index]).valid) {
            return false;
        }
    }
    return true;
}

// The destination is zero (a record reset, or a zeroed event) and receives its four texts.
__declspec(noinline) bool unpack_embed_media(const Reader& reader,
                                             const GatewayClientMessageEmbedMediaMutation& source,
                                             GatewayMessageEmbedMedia* destination) noexcept {
    if (!bytes_zero(source.reserved, sizeof(source.reserved)) || (source.state & ~3u) != 0) {
        return false;
    }
    destination->width = source.width;
    destination->height = source.height;
    destination->flags = source.flags;
    destination->has_width = (source.state & 1u) != 0;
    destination->has_height = (source.state & 2u) != 0;
    return unpack_texts(reader, &source.url, &destination->url, 4);
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

// The six counts of a rich header, in the order of the descriptor arrays behind it: their largest values, the
// message field that licenses each, and the bytes of one record.
constexpr u8 kRichCountMaximum[6] = {kGatewayMessageReactionMaximum, kGatewayMessageAttachmentMaximum,
                                     kGatewayMessageEmbedMaximum, kGatewayMessageEmbedFieldMaximum,
                                     kGatewayMessageStickerMaximum, kGatewayMessageComponentMaximum};
constexpr u32 kRichCountField[6] = {gateway_message_field_reactions,      gateway_message_field_attachments,
                                    gateway_message_field_embeds,         gateway_message_field_embeds,
                                    gateway_message_field_sticker_items, gateway_message_field_components};
constexpr u8 kRichRecordBytes[6] = {sizeof(GatewayClientMessageReactionMutation),
                                    sizeof(GatewayClientMessageAttachmentMutation),
                                    sizeof(GatewayClientMessageEmbedMutation),
                                    sizeof(GatewayClientMessageEmbedFieldMutation),
                                    sizeof(GatewayClientMessageStickerMutation),
                                    sizeof(GatewayClientMessageComponentMutation)};

void rich_counts(const GatewayClientMessageRichHeader& header, u16 (&counts)[6]) noexcept {
    copy_bytes(counts, &header.reaction_count, sizeof(counts));
}

// The bytes of the descriptor arrays behind a header whose counts are within their maxima.
u32 rich_descriptor_bytes(const GatewayClientMessageRichHeader& header) noexcept {
    u16 counts[6];
    rich_counts(header, counts);
    u32 bytes = 0;
    for (u32 index = 0; index < 6; ++index) {
        bytes += counts[index] * kRichRecordBytes[index];
    }
    return bytes;
}

bool rich_header_valid(const GatewayClientMessageRichHeader& header, u32 rich_fields) noexcept {
    u16 counts[6];
    rich_counts(header, counts);
    for (u32 index = 0; index < 6; ++index) {
        if (counts[index] > kRichCountMaximum[index] ||
            (counts[index] != 0 && (rich_fields & kRichCountField[index]) == 0)) {
            return false;
        }
    }
    return bytes_zero(header.reserved, sizeof(header.reserved)) &&
           header.reference_state <= static_cast<u8>(GatewayMessageReferenceState::available) &&
           ((rich_fields & gateway_message_field_interaction) != 0 ||
            (header.interaction_id == 0 && header.interaction_user_id == 0 && header.interaction_type == 0 &&
             header.interaction_user_name.bytes == 0 && header.interaction_user_avatar_hash.bytes == 0 &&
             header.interaction_command_name.bytes == 0)) &&
           ((rich_fields & (gateway_message_field_message_reference | gateway_message_field_referenced_message)) != 0 ||
            (header.reference_state == 0 && header.reference_message_id == 0 && header.reference_channel_id == 0 &&
             header.reference_guild_id == 0 && header.reference_author_id == 0 &&
             header.reference_author_name.bytes == 0 && header.reference_author_avatar_hash.bytes == 0 &&
             header.reference_content.bytes == 0 && header.reference_content_flags == 0 &&
             header.reference_message_type == 0 && header.reference_quote_flags == 0));
}

bool poll_span_overlap(const void* left, u64 left_bytes, const void* right, u64 right_bytes) noexcept {
    if (!left_bytes || !right_bytes)
        return false;
    const auto a = reinterpret_cast<usize>(left), b = reinterpret_cast<usize>(right);
    return !left || !right || left_bytes > ~static_cast<usize>(0) - a ||
           right_bytes > ~static_cast<usize>(0) - b || (a < b + right_bytes && b < a + left_bytes);
}

// Reads a poll block at reader.at. With scratch and output it fills them (neither may overlap the other or the
// payload); without, it only validates.
bool read_poll_metadata(Reader& reader, const GatewayMessagePollScratch* scratch = nullptr,
                        GatewayMessagePoll* output = nullptr) noexcept {
    if (scratch) {
        struct Span {
            const void* data;
            u64 bytes;
        };
        const Span spans[4] = {
            {scratch->answers.data, static_cast<u64>(scratch->answers.count) * sizeof(GatewayMessagePollAnswer)},
            {scratch->answer_counts.data,
             static_cast<u64>(scratch->answer_counts.count) * sizeof(GatewayMessagePollAnswerCount)},
            {output, sizeof(*output)},
            {reader.data, reader.count}};
        for (u32 left = 0; left < 3; ++left)
            for (u32 right = left + 1; right < 4; ++right)
                if (poll_span_overlap(spans[left].data, spans[left].bytes, spans[right].data, spans[right].bytes))
                    return false;
    }
    GatewayClientMessagePollHeader header;
    if (!read_packed(reader, &header) || !bytes_zero(header.reserved, sizeof(header.reserved)) ||
        (header.flags & ~31u) || header.answer_count > kGatewayMessagePollMaximumAnswers ||
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
    if (!unpack_texts(reader, &header.question, &candidate.question, 1))
        return false;
    GatewayMessagePollAnswer answers[kGatewayMessagePollMaximumAnswers]{};
    GatewayMessagePollAnswerCount counts[kGatewayMessagePollMaximumAnswers]{};
    for (u32 index = 0; index < header.answer_count; ++index) {
        GatewayClientMessagePollAnswerMutation packed;
        if (!read_packed(reader, &packed) || !bytes_zero(packed.reserved, sizeof(packed.reserved)) ||
            (packed.flags & ~1u))
            return false;
        auto& answer = answers[index];
        answer.answer_id = packed.answer_id;
        answer.emoji_id = packed.emoji_id;
        answer.emoji_animated = (packed.flags & 1) != 0;
        if (!unpack_texts(reader, &packed.text, &answer.text, 1) ||
            !unpack_texts(reader, &packed.emoji_name, &answer.emoji_name, 1))
            return false;
    }
    for (u32 index = 0; index < header.count_count; ++index) {
        GatewayClientMessagePollCountMutation packed;
        if (!read_packed(reader, &packed) || !bytes_zero(packed.reserved, sizeof(packed.reserved)) ||
            (packed.flags & ~1u))
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
    return true;
}

// What a snapshot header says about itself: its message fields, and that an absent snapshot carries nothing.
bool snapshot_header_valid(const GatewayClientMessageSnapshotHeader& snapshot) noexcept {
    constexpr u32 allowed = gateway_message_field_content | gateway_message_field_timestamp |
                            gateway_message_field_edited_timestamp | gateway_message_field_flags |
                            gateway_message_field_type | gateway_message_field_mentions |
                            gateway_message_field_mention_roles | gateway_message_field_mention_everyone |
                            gateway_message_field_attachments | gateway_message_field_embeds |
                            gateway_message_field_sticker_items | gateway_message_field_components;
    return snapshot.present <= 1 && bytes_zero(snapshot.reserved, sizeof(snapshot.reserved)) &&
           (snapshot.fields & ~allowed) == 0 && snapshot.mention_user_count <= kGatewayMessageMentionUserMaximum &&
           (snapshot.present || !(snapshot.fields || snapshot.content.bytes || snapshot.timestamp_ms ||
                                  snapshot.edited_timestamp_ms || snapshot.flags || snapshot.type ||
                                  snapshot.mention_user_count || snapshot.rich_payload_bytes));
}

bool snapshot_user_valid(const GatewayClientNamedUserMutation& user) noexcept {
    return user.id && !user.reserved && user.name_bytes <= kGatewayMessageUserNameMaximumBytes;
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
          gateway_message_forwarding_activity | kGatewayClientMessageTts))
        return false;
    message->forwarding_features = static_cast<u8>(event.message.forwarding_features);
    message->tts = (event.message.forwarding_features & kGatewayClientMessageTts) != 0;
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

// Reads the rich header at reader.at and steps over the descriptor arrays behind it and the call and poll blocks
// that `fields` announce, validating the poll block. The header must be valid for `fields`.
bool skip_rich_descriptors(Reader& reader, const GatewayClientMessageRichHeader& header, u32 fields) noexcept {
    const u32 bytes = rich_descriptor_bytes(header);
    if (bytes > reader.end - reader.at)
        return false;
    reader.at += bytes;
    if (fields & gateway_message_field_call) {
        if (sizeof(GatewayClientMessageCallMutation) > reader.end - reader.at)
            return false;
        reader.at += static_cast<u32>(sizeof(GatewayClientMessageCallMutation));
    }
    return !(fields & gateway_message_field_poll) || read_poll_metadata(reader);
}

bool gateway_client_event_message_interaction_user(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                                   u64* user_id) noexcept {
    GatewayMessageEvent message;
    if (!gateway_client_event_as_message(event, payload, &message))
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
    Reader reader{payload.data, payload.count, text_limit, payload.count, cursor};
    GatewayClientMessageRichHeader header;
    // Descriptor lengths are checked without decoding unrelated arrays. Only
    // validated scalar/header text can affect the activity notification gate.
    if (!read_packed(reader, &header) || !rich_header_valid(header, rich_fields) ||
        !skip_rich_descriptors(reader, header, rich_fields) || reader.at != reader.end)
        return false;
    Utf8View text[3];
    if (!unpack_texts(reader, &header.reference_author_name, text, 3) ||
        !unpack_texts(reader, &header.interaction_user_name, text, 3))
        return false;
    *user_id = header.interaction_user_id;
    return true;
}

static bool unpack_message_rich_metadata(Reader& reader, u32 rich_fields,
                                         const GatewayMessageRichScratch& rich_scratch,
                                         GatewayMessageEvent* message) noexcept {
    if ((!rich_scratch.reactions.data && rich_scratch.reactions.count != 0) ||
        (!rich_scratch.attachments.data && rich_scratch.attachments.count != 0) ||
        (!rich_scratch.embeds.data && rich_scratch.embeds.count != 0) ||
        (!rich_scratch.embed_fields.data && rich_scratch.embed_fields.count != 0) ||
        (!rich_scratch.stickers.data && rich_scratch.stickers.count != 0) ||
        (!rich_scratch.components.data && rich_scratch.components.count != 0)) {
        return false;
    }
    GatewayClientMessageRichHeader header;
    if (!read_packed(reader, &header) || !rich_header_valid(header, rich_fields) ||
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
    message->reference.quote_flags = header.reference_quote_flags;
    message->reference.message_type = header.reference_message_type;
    message->interaction.id = header.interaction_id;
    message->interaction.user_id = header.interaction_user_id;
    message->interaction.type = header.interaction_type;
    message->rich_truncations = header.truncations;
    if (!unpack_texts(reader, &header.reference_author_name, &message->reference.author_name, 3) ||
        !unpack_texts(reader, &header.interaction_user_name, &message->interaction.user_name, 3)) {
        return false;
    }

    for (u32 index = 0; index < header.reaction_count; ++index) {
        GatewayClientMessageReactionMutation packed;
        if (!read_packed(reader, &packed) || !bytes_zero(packed.reserved, sizeof(packed.reserved)) ||
            (packed.flags & ~7u) != 0 || packed.count == 0 || packed.normal_count > packed.count ||
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
        if (!unpack_texts(reader, &packed.name, &destination.name, 1) ||
            (destination.emoji_id == 0 && destination.name.bytes == 0)) {
            return false;
        }
    }
    for (u32 index = 0; index < header.attachment_count; ++index) {
        GatewayClientMessageAttachmentMutation packed;
        if (!read_packed(reader, &packed) || !bytes_zero(packed.reserved, sizeof(packed.reserved)) ||
            packed.audio_reserved != 0 || packed.id == 0 || (packed.state & ~15u) != 0) {
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
        if (!unpack_texts(reader, &packed.filename, &destination.filename, 5) ||
            !unpack_texts(reader, &packed.waveform, &destination.waveform, 1) ||
            !gateway_message_waveform_valid(destination.waveform) ||
            (!destination.has_duration && destination.duration_ms)) {
            return false;
        }
    }
    for (u32 index = 0; index < header.embed_count; ++index) {
        GatewayClientMessageEmbedMutation packed;
        if (!read_packed(reader, &packed) || packed.reserved[1] != 0 || (packed.flags & ~1u) != 0 ||
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
        if (!unpack_texts(reader, &packed.title, &destination.title, 10) ||
            !unpack_embed_media(reader, packed.image, &destination.image) ||
            !unpack_embed_media(reader, packed.thumbnail, &destination.thumbnail) ||
            !unpack_embed_media(reader, packed.video, &destination.video)) {
            return false;
        }
    }
    for (u32 index = 0; index < header.embed_field_count; ++index) {
        GatewayClientMessageEmbedFieldMutation packed;
        if (!read_packed(reader, &packed) || packed.reserved != 0 || (packed.flags & ~1u) != 0 ||
            packed.embed_index >= header.embed_count) {
            return false;
        }
        GatewayMessageEmbedField& destination = rich_scratch.embed_fields[index];
        reset_value<GatewayMessageEmbedField>(&destination);
        destination.embed_index = packed.embed_index;
        destination.inline_field = (packed.flags & 1u) != 0;
        if (!unpack_texts(reader, &packed.name, &destination.name, 2)) {
            return false;
        }
    }
    for (u32 index = 0; index < header.sticker_count; ++index) {
        GatewayClientMessageStickerMutation packed;
        if (!read_packed(reader, &packed) || !bytes_zero(packed.reserved, sizeof(packed.reserved)) ||
            packed.id == 0 || packed.format_type == 0 || packed.format_type > 4) {
            return false;
        }
        GatewayMessageSticker& destination = rich_scratch.stickers[index];
        reset_value<GatewayMessageSticker>(&destination);
        destination.id = packed.id;
        destination.format_type = packed.format_type;
        if (!unpack_texts(reader, &packed.name, &destination.name, 1)) {
            return false;
        }
    }
    for (u32 index = 0; index < header.component_count; ++index) {
        GatewayClientMessageComponentMutation packed;
        if (!read_packed(reader, &packed) || packed.reserved != 0 || (packed.flags & ~3u) != 0 ||
            (packed.component_flags & ~kGatewayMessageComponentKnownFlags) != 0 || packed.type == 0 ||
            ((packed.flags & 2u) != 0 && packed.accent_color > 0xFFFFFFu) ||
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
        if (!unpack_texts(reader, &packed.label, &destination.label, 5)) {
            return false;
        }
    }
    if (rich_fields & gateway_message_field_call) {
        GatewayClientMessageCallMutation call;
        if (!read_packed(reader, &call) || (call.flags & ~7u) != 0 ||
            !bytes_zero(call.reserved, sizeof(call.reserved)))
            return false;
        message->call = {call.ended_timestamp_ms, (call.flags & 1u) != 0, (call.flags & 2u) != 0,
                         (call.flags & 4u) != 0};
    }
    if ((rich_fields & gateway_message_field_poll) &&
        !read_poll_metadata(reader, &rich_scratch.poll, &message->poll))
        return false;
    if (rich_fields & gateway_message_field_message_snapshots) {
        GatewayClientMessageSnapshotHeader snapshot;
        if (!read_packed(reader, &snapshot) || !snapshot_header_valid(snapshot) ||
            snapshot.mention_user_count > rich_scratch.snapshot.mention_users.count)
            return false;
        GatewayMessageEvent body{};
        if (!unpack_texts(reader, &snapshot.content, &body.content, 1))
            return false;
        for (u32 index = 0; index < snapshot.mention_user_count; ++index) {
            GatewayClientNamedUserMutation user;
            if (!read_packed(reader, &user) || !snapshot_user_valid(user) ||
                !rich_scratch.snapshot.mention_users.data)
                return false;
            auto& target = rich_scratch.snapshot.mention_users[index];
            target.id = user.id;
            const TextRange name{user.name_offset, user.name_bytes, 0};
            if (!unpack_texts(reader, &name, &target.name, 1))
                return false;
            for (u32 earlier = 0; earlier < index; ++earlier)
                if (rich_scratch.snapshot.mention_users[earlier].id == user.id)
                    return false;
        }
        const u64 child_end = static_cast<u64>(reader.at) + snapshot.rich_payload_bytes;
        if (child_end > reader.end)
            return false;
        const GatewayMessageRichScratch child{{},
                                              rich_scratch.snapshot.attachments,
                                              rich_scratch.snapshot.embeds,
                                              rich_scratch.snapshot.embed_fields,
                                              rich_scratch.snapshot.stickers,
                                              rich_scratch.snapshot.components,
                                              {}};
        if (snapshot.rich_payload_bytes) {
            Reader nested = reader;
            nested.end = static_cast<u32>(child_end);
            if (!unpack_message_rich_metadata(nested, snapshot.fields & kGatewayMessageRichFieldMask, child, &body))
                return false;
        } else if (snapshot.fields & kGatewayMessageRichFieldMask)
            return false;
        reader.at = static_cast<u32>(child_end);
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
    if (reader.at != reader.end) {
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
    Reader reader{payload.data, payload.count, role_offset, payload.count, cursor};
    return unpack_message_rich_metadata(reader, fields, scratch, message);
}

static bool snapshot_user_offsets(const GatewayClientEvent& event, ConstSlice<u8> payload, u32* users,
                                  u32* count, u32* text_limit) noexcept {
    GatewayMessageEvent message;
    if (!gateway_client_event_as_message(event, payload, &message))
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
    Reader reader{payload.data, payload.count, role_offset, payload.count, cursor};
    GatewayClientMessageRichHeader outer;
    if (!read_packed(reader, &outer) ||
        !rich_header_valid(outer, event.message.fields & kGatewayMessageRichFieldMask) ||
        !skip_rich_descriptors(reader, outer, event.message.fields))
        return false;
    GatewayClientMessageSnapshotHeader snapshot;
    Utf8View content;
    if (!read_packed(reader, &snapshot) || !snapshot_header_valid(snapshot) ||
        !unpack_texts(reader, &snapshot.content, &content, 1))
        return false;
    const u32 begin = reader.at;
    for (u32 index = 0; index < snapshot.mention_user_count; ++index) {
        GatewayClientNamedUserMutation user;
        Utf8View name;
        if (!read_packed(reader, &user) || !snapshot_user_valid(user))
            return false;
        const TextRange name_range{user.name_offset, user.name_bytes, 0};
        if (!unpack_texts(reader, &name_range, &name, 1))
            return false;
        for (u32 previous = 0; previous < index; ++previous)
            if (byte_load_le64(payload.data + begin + previous * sizeof(user)) == user.id)
                return false;
    }
    if (static_cast<u64>(reader.at) + snapshot.rich_payload_bytes != payload.count)
        return false;
    if (snapshot.rich_payload_bytes) {
        GatewayClientMessageRichHeader child;
        if (!read_packed(reader, &child) ||
            !rich_header_valid(child, snapshot.fields & kGatewayMessageRichFieldMask) ||
            static_cast<u64>(reader.at) + rich_descriptor_bytes(child) != payload.count)
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
    Reader reader{payload.data, payload.count, text_limit, payload.count, offset};
    for (u32 index = 0; index < count; ++index) {
        GatewayClientNamedUserMutation user;
        Utf8View name;
        if (!read_packed(reader, &user))
            return false;
        const TextRange range{user.name_offset, user.name_bytes, 0};
        if (!unpack_texts(reader, &range, &name, 1) || !visit(context, user.id, name))
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
    if (!message_event_offsets(event, payload))
        return false;
    *name = Utf8View{reinterpret_cast<const char*>(payload.data) + event.message.content_bytes,
                     event.message.author_name_bytes};
    return utf8_validate(*name).valid;
}

bool gateway_client_event_message_author_avatar_hash(const GatewayClientEvent& event,
                                                     ConstSlice<u8> payload,
                                                     Utf8View* avatar_hash) noexcept {
    if (!message_event_offsets(event, payload))
        return false;
    *avatar_hash = Utf8View{reinterpret_cast<const char*>(payload.data) + event.message.content_bytes +
                                event.message.author_name_bytes,
                            event.message.author_avatar_hash_bytes};
    return utf8_validate(*avatar_hash).valid;
}

bool gateway_client_event_message_member_nick(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                              Utf8View* nick) noexcept {
    if (!message_event_offsets(event, payload))
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
