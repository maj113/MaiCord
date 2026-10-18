#include "maicord/protocol/message_pins.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {

struct PinInstant { u64 milliseconds; u32 submillisecond_ns; };

bool cursor_instant(const MessagePinsCursor& cursor, PinInstant* instant) noexcept {
    if (cursor.bytes < 20 || cursor.bytes > kMessagePinsCursorMaximumBytes)
        return false;
    for (u32 at = 0; at < cursor.bytes; ++at) {
        const char c = cursor.timestamp[at];
        if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == ':' || c == '.' ||
              c == 'T' || c == 'Z')) return false;
    }
    *instant = {};
    if (!parse_iso_timestamp({{cursor.timestamp, cursor.bytes}, 0, false}, &instant->milliseconds))
        return false;
    if (cursor.timestamp[19] == '.') {
        u32 fraction = 0, digits = 0;
        for (u32 at = 20; at < cursor.bytes && cursor.timestamp[at] >= '0' && cursor.timestamp[at] <= '9'; ++at) {
            fraction = fraction * 10 + static_cast<u32>(cursor.timestamp[at] - '0');
            ++digits;
        }
        while (digits < 9) { fraction *= 10; ++digits; }
        instant->submillisecond_ns = fraction % 1000000u;
    }
    return true;
}

bool later(PinInstant left, PinInstant right) noexcept {
    return left.milliseconds > right.milliseconds ||
           (left.milliseconds == right.milliseconds && left.submillisecond_ns > right.submillisecond_ns);
}

template <typename T> bool valid_slice(Slice<T> slice) noexcept { return slice.data || !slice.count; }
template <typename T> Slice<T> remaining(Slice<T> slice, u32 used) noexcept {
    return {slice.data ? slice.data + used : nullptr, slice.count - used};
}

struct PageContext {
    JsonIndexedDocument document;
    MessagePinsQuery query;
    MessagePinsDecodeResult result;
    Slice<u32> message_structurals;
    Slice<char> text;
    Slice<MessagePin> items;
    Slice<GatewayMessageNamedUser> mentions;
    MessageHistoryRichScratch rich;
    PinInstant previous;
    PinInstant before;
    u32 maximum_depth;
    u32 mention_count, member_role_count, reaction_count, attachment_count, embed_count;
    u32 embed_field_count, sticker_count, component_count;
    u32 snapshot_attachment_count, snapshot_embed_count, snapshot_embed_field_count;
    u32 snapshot_sticker_count, snapshot_component_count, snapshot_mention_count;
    JsonValue item_array;
    u8 fields;
    bool capture_rich;
};

bool reject(PageContext* context, MessagePinsDecodeError error, u32 offset) noexcept {
    context->result.error = error;
    context->result.error_offset = offset;
    return false;
}

struct ItemContext {
    PageContext* page;
    MessagePinsCursor pinned_at;
    PinInstant instant;
    JsonValue message;
    u8 fields;
};

bool visit_item_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* item = static_cast<ItemContext*>(opaque);
    u8 field = 0;
    if (json_string_equals_ascii(name, utf8_literal("pinned_at"))) field = 1;
    else if (json_string_equals_ascii(name, utf8_literal("message"))) field = 2;
    else return true;
    if (item->fields & field) return reject(item->page, MessagePinsDecodeError::duplicate_field, value.source_offset);
    item->fields |= field;
    if (field == 2) {
        if (value.kind != JsonValueKind::object)
            return reject(item->page, MessagePinsDecodeError::invalid_field, value.source_offset);
        item->message = value;
        return true;
    }
    JsonString timestamp{};
    if (!json_value_as_string(value, &timestamp))
        return reject(item->page, MessagePinsDecodeError::invalid_cursor, value.source_offset);
    const auto decoded = json_string_decode_utf8(timestamp, {item->pinned_at.timestamp, kMessagePinsCursorMaximumBytes});
    if (!decoded.ok()) return reject(item->page, MessagePinsDecodeError::invalid_cursor, value.source_offset);
    item->pinned_at.bytes = static_cast<u8>(decoded.bytes);
    return cursor_instant(item->pinned_at, &item->instant) ||
           reject(item->page, MessagePinsDecodeError::invalid_cursor, value.source_offset);
}

bool visit_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<PageContext*>(opaque);
    if (index >= context->query.limit || index >= context->items.count)
        return reject(context, MessagePinsDecodeError::item_capacity, value.source_offset);
    if (value.kind != JsonValueKind::object)
        return reject(context, MessagePinsDecodeError::invalid_field, value.source_offset);
    ItemContext item{};
    item.page = context;
    const auto read = json_visit_indexed_object(context->document, value, context->maximum_depth, visit_item_field, &item);
    if (!read.ok()) {
        if (context->result.error == MessagePinsDecodeError::none)
            reject(context, MessagePinsDecodeError::invalid_json, read.error_offset);
        context->result.read_error = read.error;
        return false;
    }
    if (item.fields != 3) return reject(context, MessagePinsDecodeError::missing_field, value.source_offset);
    if ((index && later(item.instant, context->previous)) ||
        (context->query.before.bytes && !later(context->before, item.instant)))
        return reject(context, MessagePinsDecodeError::invalid_order, value.source_offset);

    GatewayMessageEvent message{};
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds]{};
    GatewayMessageNamedUser temporary_mentions[kGatewayMessageMentionUserMaximum]{};
    const GatewayMessageRichScratch rich{
        remaining(context->rich.reactions, context->reaction_count),
        remaining(context->rich.attachments, context->attachment_count),
        remaining(context->rich.embeds, context->embed_count),
        remaining(context->rich.embed_fields, context->embed_field_count),
        remaining(context->rich.stickers, context->sticker_count),
        remaining(context->rich.components, context->component_count),
        {remaining(context->rich.snapshot.attachments, context->snapshot_attachment_count),
         remaining(context->rich.snapshot.embeds, context->snapshot_embed_count),
         remaining(context->rich.snapshot.embed_fields, context->snapshot_embed_field_count),
         remaining(context->rich.snapshot.stickers, context->snapshot_sticker_count),
         remaining(context->rich.snapshot.components, context->snapshot_component_count),
         remaining(context->rich.snapshot.mention_users, context->snapshot_mention_count)}};
    const auto text = remaining(context->text, context->result.page.text_bytes);
    const auto mentions = context->mentions.data ? remaining(context->mentions, context->mention_count)
        : Slice<GatewayMessageNamedUser>{temporary_mentions, kGatewayMessageMentionUserMaximum};
    const auto decoded = context->capture_rich
        ? gateway_decode_message_create(item.message, context->message_structurals, context->maximum_depth, text,
              {mention_roles, kGatewayMessageMentionRoleMaximumIds},
              remaining(context->rich.member_roles, context->member_role_count), mentions, rich, 0, &message)
        : gateway_decode_message_create(item.message, context->message_structurals, context->maximum_depth, text,
              {mention_roles, kGatewayMessageMentionRoleMaximumIds}, mentions, 0, &message);
    if (!decoded.ok()) {
        context->result.message_error = decoded.error;
        context->result.scan_error = decoded.scan_error;
        context->result.read_error = decoded.read_error;
        copy_bytes(context->result.schema_field, decoded.schema_field, sizeof(decoded.schema_field));
        return reject(context, MessagePinsDecodeError::message_decode,
                      decoded.error_offset < item.message.source.bytes ? item.message.source_offset + decoded.error_offset
                                                                      : decoded.error_offset);
    }
    if (message.channel_id != context->query.channel_id)
        return reject(context, MessagePinsDecodeError::channel_mismatch, item.message.source_offset);
    for (u32 previous = 0; previous < index; ++previous) {
        if (context->items[previous].message.id == message.id)
            return reject(context, MessagePinsDecodeError::duplicate_message, item.message.source_offset);
    }
    message.mention_roles = {};
    if (!context->mentions.data) message.mention_users = {};
    context->items[index] = {message, item.pinned_at, item.instant.milliseconds};
    context->result.page.next_before = item.pinned_at;
    context->result.page.count = index + 1;
    context->result.page.text_bytes += message.transient_bytes;
    context->mention_count += message.mention_users.count;
    context->member_role_count += message.member_roles.count;
    context->reaction_count += message.reactions.count;
    context->attachment_count += message.attachments.count;
    context->embed_count += message.embeds.count;
    context->embed_field_count += message.embed_fields.count;
    context->sticker_count += message.stickers.count;
    context->component_count += message.components.count;
    context->snapshot_attachment_count += message.snapshot.attachments.count;
    context->snapshot_embed_count += message.snapshot.embeds.count;
    context->snapshot_embed_field_count += message.snapshot.embed_fields.count;
    context->snapshot_sticker_count += message.snapshot.stickers.count;
    context->snapshot_component_count += message.snapshot.components.count;
    context->snapshot_mention_count += message.snapshot.mention_users.count;
    context->previous = item.instant;
    return true;
}

bool visit_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<PageContext*>(opaque);
    u8 field = 0;
    if (json_string_equals_ascii(name, utf8_literal("items"))) field = 1;
    else if (json_string_equals_ascii(name, utf8_literal("has_more"))) field = 2;
    else return true;
    if (context->fields & field) return reject(context, MessagePinsDecodeError::duplicate_field, value.source_offset);
    context->fields |= field;
    if (field == 1) {
        if (value.kind != JsonValueKind::array)
            return reject(context, MessagePinsDecodeError::invalid_field, value.source_offset);
        context->item_array = value;
        return true;
    }
    return json_value_to_bool(value, &context->result.page.has_more) ||
           reject(context, MessagePinsDecodeError::invalid_field, value.source_offset);
}

} // namespace

bool message_pins_cursor_valid(const MessagePinsCursor& cursor, bool allow_empty) noexcept {
    PinInstant instant{};
    return !cursor.bytes ? allow_empty : cursor_instant(cursor, &instant);
}

MessagePinsWriteResult write_message_pins_target(const MessagePinsQuery& query,
                                                 Slice<u8> output) noexcept {
    if (!query.channel_id || !query.limit || query.limit > kMessagePinsMaximumItems ||
        !message_pins_cursor_valid(query.before) || !valid_slice(output))
        return {0, MessagePinsWriteError::invalid_input};
    ProtocolWriter writer{output, 0};
    const bool written = protocol_writer_append_literal(&writer, "/api/v10/channels/") &&
        protocol_writer_append_u64(&writer, query.channel_id) &&
        protocol_writer_append_literal(&writer, "/messages/pins?limit=") &&
        protocol_writer_append_u64(&writer, query.limit) &&
        (!query.before.bytes || (protocol_writer_append_literal(&writer, "&before=") &&
         protocol_writer_append_percent_encoded(&writer, {query.before.timestamp, query.before.bytes})));
    return written ? MessagePinsWriteResult{writer.used, MessagePinsWriteError::none}
                   : MessagePinsWriteResult{0, MessagePinsWriteError::output_capacity};
}

MessagePinsWriteResult write_message_pin_target(u64 channel_id, u64 message_id,
                                                Slice<u8> output) noexcept {
    if (!channel_id || !message_id || !valid_slice(output)) return {0, MessagePinsWriteError::invalid_input};
    ProtocolWriter writer{output, 0};
    const bool written = protocol_writer_append_literal(&writer, "/api/v10/channels/") &&
        protocol_writer_append_u64(&writer, channel_id) &&
        protocol_writer_append_literal(&writer, "/messages/pins/") &&
        protocol_writer_append_u64(&writer, message_id);
    return written ? MessagePinsWriteResult{writer.used, MessagePinsWriteError::none}
                   : MessagePinsWriteResult{0, MessagePinsWriteError::output_capacity};
}

MessagePinsDecodeResult decode_message_pins_page(Utf8View body, const MessagePinsQuery& query,
    Slice<u32> page_structurals, Slice<u32> message_structurals, u32 maximum_depth,
    Slice<char> text_output, Slice<MessagePin> items, Slice<GatewayMessageNamedUser> mention_users,
    const MessageHistoryRichScratch& rich_scratch) noexcept {
    u8 target[kMessagePinsMaximumTargetBytes]{};
    if (!write_message_pins_target(query, {target, sizeof(target)}).ok() || !body.data || !body.bytes ||
        !page_structurals.data || !page_structurals.count || !message_structurals.data || !message_structurals.count ||
        !maximum_depth || !valid_slice(text_output) || !valid_slice(items) || !valid_slice(mention_users) ||
        !valid_slice(rich_scratch.reactions) || !valid_slice(rich_scratch.attachments) || !valid_slice(rich_scratch.embeds) ||
        !valid_slice(rich_scratch.embed_fields) || !valid_slice(rich_scratch.stickers) || !valid_slice(rich_scratch.components) ||
        !valid_slice(rich_scratch.member_roles) || !valid_slice(rich_scratch.snapshot.attachments) ||
        !valid_slice(rich_scratch.snapshot.embeds) || !valid_slice(rich_scratch.snapshot.embed_fields) ||
        !valid_slice(rich_scratch.snapshot.stickers) || !valid_slice(rich_scratch.snapshot.components) ||
        !valid_slice(rich_scratch.snapshot.mention_users)) return {{}, 0, MessagePinsDecodeError::invalid_input};
    const auto scan = json_scan_structurals_avx1(body, page_structurals);
    if (!scan.ok()) return {{}, scan.error_offset, MessagePinsDecodeError::scan_failed,
                            GatewayEventDecodeError::none, scan.error};
    PageContext context{};
    context.document = {body, {page_structurals.data, scan.count}};
    context.query = query;
    context.result.page.channel_id = query.channel_id;
    context.message_structurals = message_structurals;
    context.text = text_output;
    context.items = items;
    context.mentions = mention_users;
    context.rich = rich_scratch;
    context.maximum_depth = maximum_depth;
    context.capture_rich = rich_scratch.reactions.data || rich_scratch.attachments.data || rich_scratch.embeds.data ||
        rich_scratch.embed_fields.data || rich_scratch.stickers.data || rich_scratch.components.data || rich_scratch.member_roles.data ||
        rich_scratch.snapshot.attachments.data || rich_scratch.snapshot.embeds.data || rich_scratch.snapshot.embed_fields.data ||
        rich_scratch.snapshot.stickers.data || rich_scratch.snapshot.components.data || rich_scratch.snapshot.mention_users.data;
    if (query.before.bytes) cursor_instant(query.before, &context.before);
    auto read = json_visit_root_object(body, context.document.structurals, maximum_depth, visit_root, &context);
    if (read.ok() && context.fields != 3) context.result.error = MessagePinsDecodeError::missing_field;
    else if (read.ok()) {
        read = json_visit_indexed_array(context.document, context.item_array, maximum_depth, visit_item, &context);
        if (read.ok() && context.result.page.has_more && !context.result.page.count)
            context.result.error = MessagePinsDecodeError::invalid_cursor;
    }
    if (!read.ok()) {
        if (context.result.error == MessagePinsDecodeError::none) {
            context.result.error = MessagePinsDecodeError::invalid_json;
            context.result.error_offset = read.error_offset;
        }
        if (context.result.read_error == JsonReadError::none) context.result.read_error = read.error;
    }
    return context.result;
}

} // namespace maicord
