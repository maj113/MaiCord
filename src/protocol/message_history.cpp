#include "maicord/protocol/message_history.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {

struct PageContext {
    Slice<u32> message_structurals;
    Slice<char> content;
    Slice<GatewayMessageEvent> messages;
    Slice<GatewayMessageNamedUser> mention_users;
    MessageHistoryRichScratch rich;
    MessageHistoryPage page;
    u32 transient_bytes;
    u32 mention_user_count;
    u32 member_role_count;
    u32 reaction_count;
    u32 attachment_count;
    u32 embed_count;
    u32 embed_field_count;
    u32 sticker_count;
    u32 component_count;
    u32 poll_answer_count, poll_count_count;
    u32 snapshot_attachment_count, snapshot_embed_count, snapshot_embed_field_count;
    u32 snapshot_sticker_count, snapshot_component_count, snapshot_mention_count;
    u32 maximum_depth;
    u32 error_offset;
    u32 degraded;
    MessageHistoryDecodeError error;
    GatewayEventDecodeError message_error;
    JsonScanError scan_error;
    JsonReadError read_error;
    bool capture_rich;
    // Recent mentions span channels; the server orders them newest first.
    bool mixed_channels;
    char schema_field[32];
};

MessageHistoryDecodeResult decode_failure(
    MessageHistoryDecodeError error, u32 offset = 0,
    GatewayEventDecodeError message_error = GatewayEventDecodeError::none,
    JsonScanError scan_error = JsonScanError::none,
    JsonReadError read_error = JsonReadError::none) noexcept {
    return MessageHistoryDecodeResult{{}, offset, error, message_error, scan_error, read_error};
}

bool visit_page_message(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<PageContext*>(opaque);
    if (index >= kMessageHistoryMaximumMessages || index >= context->messages.count) {
        context->error = MessageHistoryDecodeError::message_capacity;
        context->error_offset = value.source_offset;
        return false;
    }
    GatewayMessageEvent message{};
    const u32 content_remaining = context->content.count - context->transient_bytes;
    char* const content_begin =
        context->content.data ? context->content.data + context->transient_bytes : nullptr;
    GatewayMessageNamedUser* const mention_begin =
        context->mention_users.data ? context->mention_users.data + context->mention_user_count : nullptr;
    const u32 mention_remaining = context->mention_users.count - context->mention_user_count;
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds]{};
    u64* const member_role_begin = context->rich.member_roles.data
                                       ? context->rich.member_roles.data + context->member_role_count
                                       : nullptr;
    const u32 member_role_remaining = context->rich.member_roles.count - context->member_role_count;
    const GatewayMessageRichScratch rich{
        Slice<GatewayMessageReaction>{
            context->rich.reactions.data ? context->rich.reactions.data + context->reaction_count : nullptr,
            context->rich.reactions.count - context->reaction_count},
        Slice<GatewayMessageAttachment>{context->rich.attachments.data
                                            ? context->rich.attachments.data + context->attachment_count
                                            : nullptr,
                                        context->rich.attachments.count - context->attachment_count},
        Slice<GatewayMessageEmbed>{
            context->rich.embeds.data ? context->rich.embeds.data + context->embed_count : nullptr,
            context->rich.embeds.count - context->embed_count},
        Slice<GatewayMessageEmbedField>{context->rich.embed_fields.data
                                            ? context->rich.embed_fields.data + context->embed_field_count
                                            : nullptr,
                                        context->rich.embed_fields.count - context->embed_field_count},
        Slice<GatewayMessageSticker>{
            context->rich.stickers.data ? context->rich.stickers.data + context->sticker_count : nullptr,
            context->rich.stickers.count - context->sticker_count},
        Slice<GatewayMessageComponent>{context->rich.components.data
                                           ? context->rich.components.data + context->component_count
                                           : nullptr,
                                       context->rich.components.count - context->component_count},
        GatewayMessageSnapshotScratch{
            {context->rich.snapshot.attachments.data
                 ? context->rich.snapshot.attachments.data + context->snapshot_attachment_count
                 : nullptr,
             context->rich.snapshot.attachments.count - context->snapshot_attachment_count},
            {context->rich.snapshot.embeds.data
                 ? context->rich.snapshot.embeds.data + context->snapshot_embed_count
                 : nullptr,
             context->rich.snapshot.embeds.count - context->snapshot_embed_count},
            {context->rich.snapshot.embed_fields.data
                 ? context->rich.snapshot.embed_fields.data + context->snapshot_embed_field_count
                 : nullptr,
             context->rich.snapshot.embed_fields.count - context->snapshot_embed_field_count},
            {context->rich.snapshot.stickers.data
                 ? context->rich.snapshot.stickers.data + context->snapshot_sticker_count
                 : nullptr,
             context->rich.snapshot.stickers.count - context->snapshot_sticker_count},
            {context->rich.snapshot.components.data
                 ? context->rich.snapshot.components.data + context->snapshot_component_count
                 : nullptr,
             context->rich.snapshot.components.count - context->snapshot_component_count},
            {context->rich.snapshot.mention_users.data
                 ? context->rich.snapshot.mention_users.data + context->snapshot_mention_count
                 : nullptr,
             context->rich.snapshot.mention_users.count - context->snapshot_mention_count}},
        GatewayMessagePollScratch{
            {context->rich.poll.answers.data ? context->rich.poll.answers.data + context->poll_answer_count
                                             : nullptr,
             minimum<u32>(kGatewayMessagePollMaximumAnswers,
                          context->rich.poll.answers.count - context->poll_answer_count)},
            {context->rich.poll.answer_counts.data
                 ? context->rich.poll.answer_counts.data + context->poll_count_count
                 : nullptr,
             minimum<u32>(kGatewayMessagePollMaximumAnswers,
                          context->rich.poll.answer_counts.count - context->poll_count_count)}}};
    GatewayEventDecodeResult decoded =
        context->capture_rich
            ? gateway_decode_message_create(
                  value, context->message_structurals, context->maximum_depth,
                  Slice<char>{content_begin, content_remaining},
                  Slice<u64>{mention_roles, kGatewayMessageMentionRoleMaximumIds},
                  Slice<u64>{member_role_begin, member_role_remaining},
                  Slice<GatewayMessageNamedUser>{mention_begin, mention_remaining}, rich, 0, &message)
            : (context->mention_users.data
                   ? gateway_decode_message_create(
                         value, context->message_structurals, context->maximum_depth,
                         Slice<char>{content_begin, content_remaining},
                         Slice<u64>{mention_roles, kGatewayMessageMentionRoleMaximumIds},
                         Slice<GatewayMessageNamedUser>{mention_begin, mention_remaining}, 0, &message)
                   : gateway_decode_message_create(
                         value, context->message_structurals, context->maximum_depth,
                         Slice<char>{content_begin, content_remaining}, &message));
    if (!decoded.ok() && context->capture_rich) {
        // Rich decoding (cards, embeds, threads, polls, ...) is presentation
        // detail: an unexpected nested shape degrades only that message to its
        // core text. Core failures still fail the page. A failed attempt commits
        // nothing; pool cursors advance only after a successful message.
        GatewayMessageEvent core{};
        const GatewayEventDecodeResult fallback =
            context->mention_users.data
                ? gateway_decode_message_create(
                      value, context->message_structurals, context->maximum_depth,
                      Slice<char>{content_begin, content_remaining},
                      Slice<u64>{mention_roles, kGatewayMessageMentionRoleMaximumIds},
                      Slice<GatewayMessageNamedUser>{mention_begin, mention_remaining}, 0, &core)
                : gateway_decode_message_create(value, context->message_structurals, context->maximum_depth,
                                                Slice<char>{content_begin, content_remaining}, &core);
        if (fallback.ok()) {
            core.fields &= ~kMessageHistoryRichFieldMask;
            core.forwarding_features &= static_cast<u8>(~gateway_message_forwarding_poll);
            core.poll = {};
            message = core;
            decoded = fallback;
            ++context->degraded;
        }
    }
    if (!decoded.ok()) {
        context->error = MessageHistoryDecodeError::message_decode;
        context->error_offset = decoded.error_offset < value.source.bytes
                                    ? value.source_offset + decoded.error_offset
                                    : decoded.error_offset;
        context->message_error = decoded.error;
        context->scan_error = decoded.scan_error;
        context->read_error = decoded.read_error;
        copy_bytes(context->schema_field, decoded.schema_field, sizeof(decoded.schema_field));
        return false;
    }
    if (index != 0 && !context->mixed_channels && message.id >= context->page.oldest_id) {
        context->error = MessageHistoryDecodeError::invalid_order;
        context->error_offset = value.source_offset;
        return false;
    }
    if (index == 0) {
        context->page.channel_id = message.channel_id;
        context->page.newest_id = message.id;
    } else if (!context->mixed_channels && message.channel_id != context->page.channel_id) {
        context->error = MessageHistoryDecodeError::channel_mismatch;
        context->error_offset = value.source_offset;
        return false;
    }
    // Role IDs were validated during decoding; this temporary role array
    // cannot escape the per-message visitor as a borrowed page view.
    message.mention_roles = {};
    context->messages[index] = message;
    context->page.oldest_id = message.id;
    context->page.count = index + 1;
    context->page.content_bytes += message.content.bytes;
    context->transient_bytes += message.transient_bytes;
    context->mention_user_count += message.mention_users.count;
    context->member_role_count += message.member_roles.count;
    context->reaction_count += message.reactions.count;
    context->attachment_count += message.attachments.count;
    context->embed_count += message.embeds.count;
    context->embed_field_count += message.embed_fields.count;
    context->sticker_count += message.stickers.count;
    context->component_count += message.components.count;
    context->poll_answer_count += message.poll.answers.count;
    context->poll_count_count += message.poll.answer_counts.count;
    context->snapshot_attachment_count += message.snapshot.attachments.count;
    context->snapshot_embed_count += message.snapshot.embeds.count;
    context->snapshot_embed_field_count += message.snapshot.embed_fields.count;
    context->snapshot_sticker_count += message.snapshot.stickers.count;
    context->snapshot_component_count += message.snapshot.components.count;
    context->snapshot_mention_count += message.snapshot.mention_users.count;
    return true;
}

} // namespace

MessageHistoryTargetResult write_message_history_target(
    u64 channel_id, const MessageHistoryQuery& query, Slice<u8> output) noexcept {
    if (channel_id == 0 || query.limit == 0 || query.limit > kMessageHistoryMaximumMessages ||
        (!output.data && output.count != 0) ||
        (query.position == MessageHistoryPosition::latest ? query.cursor != 0 : query.cursor == 0)) {
        return MessageHistoryTargetResult{0, MessageHistoryTargetError::invalid_input};
    }
    const char* cursor_name = nullptr;
    switch (query.position) {
    case MessageHistoryPosition::latest:
        break;
    case MessageHistoryPosition::before:
        cursor_name = "before";
        break;
    case MessageHistoryPosition::after:
        cursor_name = "after";
        break;
    case MessageHistoryPosition::around:
        cursor_name = "around";
        break;
    default:
        return MessageHistoryTargetResult{0, MessageHistoryTargetError::invalid_input};
    }

    ProtocolWriter writer{output, 0};
    const bool written = protocol_writer_append_literal(&writer, "/api/v10/channels/") &&
                         protocol_writer_append_u64(&writer, channel_id) &&
                         protocol_writer_append_literal(&writer, "/messages?") &&
                         (!cursor_name || (protocol_writer_append_c_string(&writer, cursor_name) &&
                                           protocol_writer_append_byte(&writer, '=') &&
                                           protocol_writer_append_u64(&writer, query.cursor) &&
                                           protocol_writer_append_byte(&writer, '&'))) &&
                         protocol_writer_append_literal(&writer, "limit=") &&
                         protocol_writer_append_u64(&writer, query.limit);
    return written
               ? MessageHistoryTargetResult{writer.used, MessageHistoryTargetError::none}
               : MessageHistoryTargetResult{0, MessageHistoryTargetError::output_capacity};
}

MessageHistoryDecodeResult decode_message_history_page(
    Utf8View body, Slice<u32> page_structural_scratch, Slice<u32> message_structural_scratch,
    u32 maximum_depth, Slice<char> content_output, Slice<GatewayMessageEvent> messages) noexcept {
    return decode_message_history_page(
        body, page_structural_scratch, message_structural_scratch, maximum_depth, content_output, messages,
        Slice<GatewayMessageNamedUser>{}, MessageHistoryRichScratch{});
}

MessageHistoryDecodeResult decode_message_history_page(
    Utf8View body, Slice<u32> page_structural_scratch, Slice<u32> message_structural_scratch,
    u32 maximum_depth, Slice<char> content_output, Slice<GatewayMessageEvent> messages,
    Slice<GatewayMessageNamedUser> mention_users) noexcept {
    return decode_message_history_page(body, page_structural_scratch, message_structural_scratch,
                                       maximum_depth, content_output, messages, mention_users,
                                       MessageHistoryRichScratch{});
}

namespace {
MessageHistoryDecodeResult decode_page(
    Utf8View body, Slice<u32> page_structural_scratch, Slice<u32> message_structural_scratch,
    u32 maximum_depth, Slice<char> content_output, Slice<GatewayMessageEvent> messages,
    Slice<GatewayMessageNamedUser> mention_users, const MessageHistoryRichScratch& rich_scratch,
    bool mixed_channels) noexcept {
    if (!body.data || body.bytes == 0 || !page_structural_scratch.data ||
        page_structural_scratch.count == 0 || !message_structural_scratch.data ||
        message_structural_scratch.count == 0 || maximum_depth == 0 ||
        (!content_output.data && content_output.count != 0) || (!messages.data && messages.count != 0) ||
        (!mention_users.data && mention_users.count != 0) ||
        (!rich_scratch.reactions.data && rich_scratch.reactions.count != 0) ||
        (!rich_scratch.attachments.data && rich_scratch.attachments.count != 0) ||
        (!rich_scratch.embeds.data && rich_scratch.embeds.count != 0) ||
        (!rich_scratch.embed_fields.data && rich_scratch.embed_fields.count != 0) ||
        (!rich_scratch.stickers.data && rich_scratch.stickers.count != 0) ||
        (!rich_scratch.components.data && rich_scratch.components.count != 0) ||
        (!rich_scratch.member_roles.data && rich_scratch.member_roles.count != 0) ||
        (!rich_scratch.snapshot.attachments.data && rich_scratch.snapshot.attachments.count) ||
        (!rich_scratch.snapshot.embeds.data && rich_scratch.snapshot.embeds.count) ||
        (!rich_scratch.snapshot.embed_fields.data && rich_scratch.snapshot.embed_fields.count) ||
        (!rich_scratch.snapshot.stickers.data && rich_scratch.snapshot.stickers.count) ||
        (!rich_scratch.snapshot.components.data && rich_scratch.snapshot.components.count) ||
        (!rich_scratch.snapshot.mention_users.data && rich_scratch.snapshot.mention_users.count) ||
        (!rich_scratch.poll.answers.data && rich_scratch.poll.answers.count) ||
        (!rich_scratch.poll.answer_counts.data && rich_scratch.poll.answer_counts.count)) {
        return decode_failure(MessageHistoryDecodeError::invalid_input);
    }
    const JsonScanResult scan = json_scan_structurals_avx1(body, page_structural_scratch);
    if (!scan.ok()) {
        return decode_failure(MessageHistoryDecodeError::scan_failed, scan.error_offset,
                              GatewayEventDecodeError::none, scan.error);
    }
    PageContext context{};
    context.message_structurals = message_structural_scratch;
    context.content = content_output;
    context.messages = messages;
    context.mention_users = mention_users;
    context.rich = rich_scratch;
    context.capture_rich = rich_scratch.reactions.data || rich_scratch.attachments.data ||
                           rich_scratch.embeds.data || rich_scratch.embed_fields.data ||
                           rich_scratch.stickers.data || rich_scratch.components.data ||
                           rich_scratch.member_roles.data || rich_scratch.snapshot.attachments.data ||
                           rich_scratch.snapshot.embeds.data || rich_scratch.snapshot.embed_fields.data ||
                           rich_scratch.snapshot.stickers.data || rich_scratch.snapshot.components.data ||
                           rich_scratch.snapshot.mention_users.data || rich_scratch.poll.answers.data ||
                           rich_scratch.poll.answer_counts.data;
    context.maximum_depth = maximum_depth;
    context.mixed_channels = mixed_channels;
    context.error = MessageHistoryDecodeError::none;
    const JsonReadResult read =
        json_visit_root_array(body, ConstSlice<u32>{page_structural_scratch.data, scan.count},
                              maximum_depth, visit_page_message, &context);
    if (!read.ok()) {
        if (context.error != MessageHistoryDecodeError::none) {
            auto result = decode_failure(
                context.error, context.error_offset, context.message_error, context.scan_error,
                context.read_error != JsonReadError::none ? context.read_error : read.error);
            copy_bytes(result.schema_field, context.schema_field, sizeof(result.schema_field));
            return result;
        }
        return decode_failure(MessageHistoryDecodeError::invalid_json, read.error_offset,
                              GatewayEventDecodeError::none, JsonScanError::none, read.error);
    }
    MessageHistoryDecodeResult result{context.page,
                                      0,
                                      MessageHistoryDecodeError::none,
                                      GatewayEventDecodeError::none,
                                      JsonScanError::none,
                                      JsonReadError::none};
    result.degraded_messages = context.degraded;
    return result;
}

} // namespace

MessageHistoryDecodeResult decode_message_history_page(
    Utf8View body, Slice<u32> page_structural_scratch, Slice<u32> message_structural_scratch,
    u32 maximum_depth, Slice<char> content_output, Slice<GatewayMessageEvent> messages,
    Slice<GatewayMessageNamedUser> mention_users,
    const MessageHistoryRichScratch& rich_scratch) noexcept {
    return decode_page(body, page_structural_scratch, message_structural_scratch, maximum_depth, content_output,
                       messages, mention_users, rich_scratch, false);
}

MessageHistoryDecodeResult decode_message_mentions_page(
    Utf8View body, Slice<u32> page_structural_scratch, Slice<u32> message_structural_scratch,
    u32 maximum_depth, Slice<char> content_output, Slice<GatewayMessageEvent> messages,
    Slice<GatewayMessageNamedUser> mention_users) noexcept {
    return decode_page(body, page_structural_scratch, message_structural_scratch, maximum_depth, content_output,
                       messages, mention_users, MessageHistoryRichScratch{}, true);
}

} // namespace maicord
