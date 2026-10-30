#include "maicord/protocol/message_history.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {

struct PageContext {
    JsonIndexedDocument document;
    Slice<char> content;
    Slice<GatewayMessageEvent> messages;
    Slice<GatewayMessageNamedUser> mention_users;
    MessageHistoryRichCursor rich;
    MessageHistoryPage page;
    u32 transient_bytes;
    u32 mention_user_count;
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

template <typename T> Slice<T> remaining(Slice<T> slice, u32 used) noexcept {
    return {slice.data ? slice.data + used : nullptr, slice.count - used};
}

template <typename T> bool has_storage(Slice<T> slice) noexcept {
    return slice.data || !slice.count;
}

template <typename T> bool is_present(Slice<T> slice) noexcept {
    return slice.data;
}

bool visit_page_message(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<PageContext*>(opaque);
    if (index >= kMessageHistoryMaximumMessages || index >= context->messages.count) {
        context->error = MessageHistoryDecodeError::message_capacity;
        context->error_offset = value.source_offset;
        return false;
    }
    GatewayMessageEvent message;
    const Slice<char> content = remaining(context->content, context->transient_bytes);
    const Slice<GatewayMessageNamedUser> mentions = remaining(context->mention_users, context->mention_user_count);
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds];
    GatewayEventDecodeResult decoded =
        context->capture_rich
            ? gateway_decode_message_create(
                  context->document, value, context->maximum_depth, content,
                  Slice<u64>{mention_roles, kGatewayMessageMentionRoleMaximumIds},
                  context->rich.remaining_member_roles(), mentions, context->rich.remaining(), 0, &message)
            : gateway_decode_message_create(
                  context->document, value, context->maximum_depth, content,
                  Slice<u64>{mention_roles, kGatewayMessageMentionRoleMaximumIds}, mentions, 0, &message);
    if (!decoded.ok() && context->capture_rich) {
        // Rich decoding (cards, embeds, threads, polls, ...) is presentation
        // detail: an unexpected nested shape degrades only that message to its
        // core text. Core failures still fail the page. A failed attempt commits
        // nothing; pool cursors advance only after a successful message.
        GatewayMessageEvent core;
        const GatewayEventDecodeResult fallback = gateway_decode_message_create(
            context->document, value, context->maximum_depth, content,
            Slice<u64>{mention_roles, kGatewayMessageMentionRoleMaximumIds}, mentions, 0, &core);
        if (fallback.ok()) {
            core.fields &= ~kMessageHistoryRichFieldMask;
            core.forwarding_features &= static_cast<u8>(~gateway_message_forwarding_poll);
            reset_value(&core.poll);
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
    reset_value(&message.mention_roles);
    copy_value(&context->messages[index], message);
    context->page.oldest_id = message.id;
    context->page.count = index + 1;
    context->page.content_bytes += message.content.bytes;
    context->transient_bytes += message.transient_bytes;
    context->mention_user_count += message.mention_users.count;
    context->rich.advance(message);
    return true;
}

} // namespace

bool MessageHistoryRichCursor::valid() const noexcept {
    return has_storage(scratch.reactions) && has_storage(scratch.attachments) && has_storage(scratch.embeds) &&
           has_storage(scratch.embed_fields) && has_storage(scratch.stickers) && has_storage(scratch.components) &&
           has_storage(scratch.member_roles) && has_storage(scratch.snapshot.attachments) &&
           has_storage(scratch.snapshot.embeds) && has_storage(scratch.snapshot.embed_fields) &&
           has_storage(scratch.snapshot.stickers) && has_storage(scratch.snapshot.components) &&
           has_storage(scratch.snapshot.mention_users) && has_storage(scratch.poll.answers) &&
           has_storage(scratch.poll.answer_counts);
}

bool MessageHistoryRichCursor::present() const noexcept {
    return is_present(scratch.reactions) || is_present(scratch.attachments) || is_present(scratch.embeds) ||
           is_present(scratch.embed_fields) || is_present(scratch.stickers) || is_present(scratch.components) ||
           is_present(scratch.member_roles) || is_present(scratch.snapshot.attachments) ||
           is_present(scratch.snapshot.embeds) || is_present(scratch.snapshot.embed_fields) ||
           is_present(scratch.snapshot.stickers) || is_present(scratch.snapshot.components) ||
           is_present(scratch.snapshot.mention_users) || is_present(scratch.poll.answers) ||
           is_present(scratch.poll.answer_counts);
}

Slice<u64> MessageHistoryRichCursor::remaining_member_roles() const noexcept {
    return ::maicord::remaining(scratch.member_roles, member_roles);
}

GatewayMessageRichScratch MessageHistoryRichCursor::remaining() const noexcept {
    auto answers = ::maicord::remaining(scratch.poll.answers, poll_answers);
    auto counts = ::maicord::remaining(scratch.poll.answer_counts, poll_counts);
    answers.count = minimum<u32>(kGatewayMessagePollMaximumAnswers, answers.count);
    counts.count = minimum<u32>(kGatewayMessagePollMaximumAnswers, counts.count);
    const auto& child = scratch.snapshot;
    return {::maicord::remaining(scratch.reactions, reactions),
            ::maicord::remaining(scratch.attachments, attachments),
            ::maicord::remaining(scratch.embeds, embeds),
            ::maicord::remaining(scratch.embed_fields, embed_fields),
            ::maicord::remaining(scratch.stickers, stickers),
            ::maicord::remaining(scratch.components, components),
            {::maicord::remaining(child.attachments, snapshot_attachments),
             ::maicord::remaining(child.embeds, snapshot_embeds),
             ::maicord::remaining(child.embed_fields, snapshot_embed_fields),
             ::maicord::remaining(child.stickers, snapshot_stickers),
             ::maicord::remaining(child.components, snapshot_components),
             ::maicord::remaining(child.mention_users, snapshot_mentions)},
            {answers, counts}};
}

void MessageHistoryRichCursor::advance(const GatewayMessageEvent& message) noexcept {
    member_roles += message.member_roles.count;
    reactions += message.reactions.count;
    attachments += message.attachments.count;
    embeds += message.embeds.count;
    embed_fields += message.embed_fields.count;
    stickers += message.stickers.count;
    components += message.components.count;
    poll_answers += message.poll.answers.count;
    poll_counts += message.poll.answer_counts.count;
    snapshot_attachments += message.snapshot.attachments.count;
    snapshot_embeds += message.snapshot.embeds.count;
    snapshot_embed_fields += message.snapshot.embed_fields.count;
    snapshot_stickers += message.snapshot.stickers.count;
    snapshot_components += message.snapshot.components.count;
    snapshot_mentions += message.snapshot.mention_users.count;
}

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
    Utf8View body, Slice<u32> page_structural_scratch, u32 maximum_depth, Slice<char> content_output,
    Slice<GatewayMessageEvent> messages) noexcept {
    return decode_message_history_page(
        body, page_structural_scratch, maximum_depth, content_output, messages,
        Slice<GatewayMessageNamedUser>{}, MessageHistoryRichScratch{});
}

MessageHistoryDecodeResult decode_message_history_page(
    Utf8View body, Slice<u32> page_structural_scratch, u32 maximum_depth, Slice<char> content_output,
    Slice<GatewayMessageEvent> messages, Slice<GatewayMessageNamedUser> mention_users) noexcept {
    return decode_message_history_page(body, page_structural_scratch, maximum_depth, content_output, messages,
                                       mention_users, MessageHistoryRichScratch{});
}


namespace {
MessageHistoryDecodeResult decode_page(
    Utf8View body, Slice<u32> page_structural_scratch, u32 maximum_depth, Slice<char> content_output,
    Slice<GatewayMessageEvent> messages, Slice<GatewayMessageNamedUser> mention_users,
    const MessageHistoryRichScratch& rich_scratch, bool mixed_channels) noexcept {
    const MessageHistoryRichCursor rich{rich_scratch};
    if (!body.data || body.bytes == 0 || !page_structural_scratch.data ||
        page_structural_scratch.count == 0 || maximum_depth == 0 ||
        (!content_output.data && content_output.count != 0) || (!messages.data && messages.count != 0) ||
        (!mention_users.data && mention_users.count != 0) || !rich.valid()) {
        return decode_failure(MessageHistoryDecodeError::invalid_input);
    }
    const JsonScanResult scan = json_scan_structurals_avx1(body, page_structural_scratch);
    if (!scan.ok()) {
        return decode_failure(MessageHistoryDecodeError::scan_failed, scan.error_offset,
                              GatewayEventDecodeError::none, scan.error);
    }
    PageContext context{};
    context.document = {body, {page_structural_scratch.data, scan.count}};
    context.content = content_output;
    context.messages = messages;
    context.mention_users = mention_users;
    context.rich = rich;
    context.capture_rich = rich.present();
    context.maximum_depth = maximum_depth;
    context.mixed_channels = mixed_channels;
    context.error = MessageHistoryDecodeError::none;
    const JsonReadResult read = json_visit_root_array(body, context.document.structurals, maximum_depth,
                                                      visit_page_message, &context);
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
    Utf8View body, Slice<u32> page_structural_scratch, u32 maximum_depth, Slice<char> content_output,
    Slice<GatewayMessageEvent> messages, Slice<GatewayMessageNamedUser> mention_users,
    const MessageHistoryRichScratch& rich_scratch) noexcept {
    return decode_page(body, page_structural_scratch, maximum_depth, content_output, messages, mention_users,
                       rich_scratch, false);
}

MessageHistoryDecodeResult decode_message_mentions_page(
    Utf8View body, Slice<u32> page_structural_scratch, u32 maximum_depth, Slice<char> content_output,
    Slice<GatewayMessageEvent> messages, Slice<GatewayMessageNamedUser> mention_users) noexcept {
    return decode_page(body, page_structural_scratch, maximum_depth, content_output, messages, mention_users,
                       MessageHistoryRichScratch{}, true);
}

} // namespace maicord
