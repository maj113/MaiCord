#include "message_rich_internal.hpp"

namespace maicord {
namespace message_rich_detail {

bool value_u32(const JsonValue& value, u32* output) noexcept {
    u64 parsed = 0;
    if (!json_value_to_u64(value, &parsed) || parsed > ~0u) {
        return false;
    }
    *output = static_cast<u32>(parsed);
    return true;
}

bool bounded_string(const JsonString& source, u32 maximum_bytes, const RichText& text, Utf8View* value) noexcept {
    const Slice<char> output = text.output; // *value/*used are written before output is last read
    u32* const used = text.used;
    if (*used > output.count) {
        return false;
    }
    reset_value<Utf8View>(value);
    const u32 remaining = output.count - *used;
    const JsonStringDecodeResult decoded = json_string_decode_utf8(
        source, Slice<char>{output.data ? output.data + *used : nullptr,
                            output.data ? minimum(maximum_bytes, remaining) : 0});
    if (decoded.error == JsonStringDecodeError::output_too_small) {
        if (decoded.bytes > maximum_bytes || decoded.bytes > remaining) {
            *text.truncations |= gateway_message_rich_truncated_text;
            return true;
        }
        return false;
    }
    if (!decoded.ok()) {
        return false;
    }
    if (decoded.bytes == 0) {
        return true;
    }
    *value = Utf8View{output.data + *used, decoded.bytes};
    *used += decoded.bytes;
    return true;
}

bool ascii_prefix(Utf8View value, const char* prefix, u32 bytes) noexcept {
    return value.bytes >= bytes && bytes_equal(value.data, prefix, bytes);
}

bool rich_url_valid(Utf8View value, bool attachment_allowed) noexcept {
    if (!value.data || value.bytes == 0 || value.bytes > kRichUrlMaximumBytes ||
        !utf8_validate(value).valid) {
        return false;
    }
    return ascii_prefix(value, "https://", 8) || ascii_prefix(value, "http://", 7) ||
           (attachment_allowed && ascii_prefix(value, "attachment://", 13));
}

bool visit_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<RootContext*>(opaque);
    u64 bit = 0;
    JsonValue* target = nullptr;
    JsonValueKind expected = JsonValueKind::array;
    switch (member_index<"message_reference", "referenced_message", "reactions", "attachments", "embeds",
                         "sticker_items", "components", "message_snapshots", "poll", "interaction", "thread",
                         "mentions", "media_mention">(name)) {
    case 0:
        bit = gateway_message_field_message_reference;
        target = &context->message_reference;
        expected = JsonValueKind::object;
        break;
    case 1:
        bit = gateway_message_field_referenced_message;
        target = &context->referenced_message;
        expected = value.kind;
        if (value.kind != JsonValueKind::object && value.kind != JsonValueKind::null_value) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
        break;
    case 2:
        bit = gateway_message_field_reactions;
        target = &context->reactions;
        break;
    case 3:
        bit = gateway_message_field_attachments;
        target = &context->attachments;
        break;
    case 4:
        bit = gateway_message_field_embeds;
        target = &context->embeds;
        break;
    case 5:
        bit = gateway_message_field_sticker_items;
        target = &context->stickers;
        break;
    case 6:
        bit = gateway_message_field_components;
        target = &context->components;
        break;
    case 7:
        bit = gateway_message_field_message_snapshots;
        target = &context->snapshots;
        expected = value.kind == JsonValueKind::null_value ? value.kind : JsonValueKind::array;
        break;
    case 8:
        bit = gateway_message_field_poll;
        target = &context->poll;
        expected = value.kind == JsonValueKind::null_value ? value.kind : JsonValueKind::object;
        break;
    case 9:
        bit = gateway_message_field_interaction;
        target = &context->interaction;
        expected = value.kind == JsonValueKind::null_value ? value.kind : JsonValueKind::object;
        break;
    case 10:
        bit = gateway_message_field_thread;
        target = &context->thread;
        expected = value.kind == JsonValueKind::null_value ? value.kind : JsonValueKind::object;
        break;
    case 11:
        if (value.kind == JsonValueKind::array)
            context->mentions = value;
        return true;
    case 12:
        if (value.kind == JsonValueKind::object)
            context->media_mention = value;
        return true;
    default:
        return true;
    }
    if (!accept_member(&context->seen, bit, value.kind == expected, &context->error)) {
        return false;
    }
    *target = value;
    return true;
}


#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
struct SnapshotWrapperContext {
    JsonValue message;
    GatewayEventDecodeError error;
    bool seen;
};
bool visit_snapshot_wrapper(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<SnapshotWrapperContext*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("message")))
        return true;
    if (context->seen || value.kind != JsonValueKind::object) {
        context->error = context->seen ? GatewayEventDecodeError::duplicate_field
                                       : GatewayEventDecodeError::invalid_data;
        return false;
    }
    context->seen = true;
    context->message = value;
    return true;
}
struct SnapshotArrayContext {
    JsonValue value;
    GatewayEventDecodeError error;
    bool present;
};
bool visit_snapshot_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<SnapshotArrayContext*>(opaque);
    if (index != 0 || value.kind != JsonValueKind::object) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    context->present = true;
    context->value = value;
    return true;
}
// `child` receives the snapshot body's root members (validated, none of the forbidden ones present).
GatewayEventDecodeResult extract_snapshot(JsonValue value, const JsonIndexedDocument& document,
                                          u32 maximum_depth, JsonValue* body, bool* present,
                                          RootContext* child) noexcept {
    reset_value(body);
    *present = false;
    if (value.kind == JsonValueKind::null_value)
        return decode_failure(GatewayEventDecodeError::none);
    SnapshotArrayContext array{};
    const auto read = json_visit_indexed_array(document, value, maximum_depth, visit_snapshot_item, &array);
    if (!read.ok())
        return decode_failure(array.error == GatewayEventDecodeError::none
                                  ? GatewayEventDecodeError::invalid_json
                                  : array.error,
                              read.error_offset);
    if (!array.present)
        return decode_failure(GatewayEventDecodeError::none);
    SnapshotWrapperContext wrapper{};
    auto result =
        read_object(array.value, document, maximum_depth, visit_snapshot_wrapper, &wrapper, &wrapper.error);
    if (!result.ok())
        return result;
    if (!wrapper.seen)
        return decode_failure(GatewayEventDecodeError::missing_required_field);
    result = read_object(wrapper.message, document, maximum_depth, visit_root, child, &child->error);
    if (!result.ok())
        return result;
    if (child->seen & (gateway_message_field_message_snapshots | gateway_message_field_message_reference |
                       gateway_message_field_referenced_message | gateway_message_field_reactions |
                       gateway_message_field_interaction))
        return decode_failure(GatewayEventDecodeError::invalid_data);
    *body = wrapper.message;
    *present = true;
    return decode_failure(GatewayEventDecodeError::none);
}
GatewayEventDecodeResult decode_snapshot(JsonValue value, const JsonIndexedDocument& document,
                                         u32 maximum_depth, Slice<char> text, u32* used,
                                         const GatewayMessageSnapshotScratch& scratch,
                                         GatewayMessageSnapshot* output) noexcept {
    reset_value(output);
    JsonValue body_value{};
    bool present = false;
    RootContext child{};
    auto result = extract_snapshot(value, document, maximum_depth, &body_value, &present, &child);
    if (!result.ok() || !present)
        return result;
    GatewayMessageEvent body{};
    result = gateway_decode_message_snapshot_core(
        document, body_value, maximum_depth,
        {text.data ? text.data + *used : nullptr, text.count - *used}, scratch.mention_users, &body);
    if (!result.ok())
        return result;
    *used += body.transient_bytes;
    const GatewayMessageRichScratch media{
        {}, scratch.attachments, scratch.embeds, scratch.embed_fields, scratch.stickers, scratch.components,
        {}};
    const RichText body_text{text, used, &body.rich_truncations};
    if (child.seen & gateway_message_field_attachments)
        result = decode_attachments(child.attachments, document, maximum_depth, body_text, media, &body);
    if (result.ok() && (child.seen & gateway_message_field_embeds))
        result = decode_embeds(child.embeds, document, maximum_depth, body_text, media, &body);
    if (result.ok() && (child.seen & gateway_message_field_sticker_items))
        result = decode_stickers(child.stickers, document, maximum_depth, body_text, media, &body);
    if (result.ok() && (child.seen & gateway_message_field_components))
        result = decode_components(child.components, document, maximum_depth, body_text, media, &body);
    if (!result.ok())
        return result;
    // A forwarded snapshot never carries its source's thread.
    body.fields |= static_cast<u32>(child.seen) & ~static_cast<u32>(gateway_message_field_thread);
    *output = {body.content,
               body.timestamp_ms,
               body.edited_timestamp_ms,
               body.attachments,
               body.embeds,
               body.embed_fields,
               body.stickers,
               body.components,
               body.mention_users,
               body.flags,
               body.fields,
               body.rich_truncations,
               body.type,
               true};
    return decode_failure(GatewayEventDecodeError::none);
}
#pragma clang attribute pop

} // namespace message_rich_detail
using namespace message_rich_detail;

GatewayEventDecodeResult gateway_extract_message_snapshot(const JsonIndexedDocument& document, JsonValue array,
                                                          u32 maximum_depth, JsonValue* body,
                                                          bool* present) noexcept {
    if (!body || !present || !array.source.data || !document.structurals.data || !maximum_depth ||
        (array.kind != JsonValueKind::array && array.kind != JsonValueKind::null_value))
        return decode_failure(GatewayEventDecodeError::invalid_input);
    RootContext child{};
    return extract_snapshot(array, document, maximum_depth, body, present, &child);
}

GatewayEventDecodeResult gateway_decode_message_rich_fields(const JsonIndexedDocument& document, JsonValue data,
                                                            u32 maximum_depth, Slice<char> text_output,
                                                            u32* text_used,
                                                            const GatewayMessageRichScratch& scratch,
                                                            GatewayMessageEvent* output) noexcept {
    if (!output || !text_used || !data.source.data || data.source.bytes == 0 ||
        data.kind != JsonValueKind::object || !document.structurals.data || document.structurals.count == 0 ||
        maximum_depth == 0 || (!text_output.data && text_output.count != 0) ||
        *text_used > text_output.count || (!scratch.reactions.data && scratch.reactions.count != 0) ||
        (!scratch.attachments.data && scratch.attachments.count != 0) ||
        (!scratch.embeds.data && scratch.embeds.count != 0) ||
        (!scratch.embed_fields.data && scratch.embed_fields.count != 0) ||
        (!scratch.stickers.data && scratch.stickers.count != 0) ||
        (!scratch.components.data && scratch.components.count != 0) ||
        (!scratch.snapshot.attachments.data && scratch.snapshot.attachments.count) ||
        (!scratch.snapshot.embeds.data && scratch.snapshot.embeds.count) ||
        (!scratch.snapshot.embed_fields.data && scratch.snapshot.embed_fields.count) ||
        (!scratch.snapshot.stickers.data && scratch.snapshot.stickers.count) ||
        (!scratch.snapshot.components.data && scratch.snapshot.components.count) ||
        (!scratch.snapshot.mention_users.data && scratch.snapshot.mention_users.count)) {
        return decode_failure(GatewayEventDecodeError::invalid_input);
    }
    RootContext root{};
    GatewayEventDecodeResult result =
        read_object(data, document, maximum_depth, visit_root, &root, &root.error);
    const RichText text{text_output, text_used, &output->rich_truncations};
    if (result.ok())
        result = decode_reference(root, document, maximum_depth, text, output);
    if (result.ok() && (root.seen & gateway_message_field_interaction) != 0)
        result = decode_interaction(root.interaction, document, maximum_depth, text, output);
    if (result.ok() && (root.seen & gateway_message_field_thread) != 0)
        result = decode_thread(root.thread, document, maximum_depth, text, output);
    if (!result.ok()) {
        return result;
    }
    if ((root.seen & gateway_message_field_reactions) != 0)
        result = decode_reactions(root.reactions, document, maximum_depth, text, scratch, output);
    if (result.ok() && (root.seen & gateway_message_field_attachments) != 0)
        result = decode_attachments(root.attachments, document, maximum_depth, text, scratch, output);
    if (result.ok() && (root.seen & gateway_message_field_embeds) != 0)
        result = decode_embeds(root.embeds, document, maximum_depth, text, scratch, output);
    if (result.ok() && (root.seen & gateway_message_field_sticker_items) != 0)
        result = decode_stickers(root.stickers, document, maximum_depth, text, scratch, output);
    if (result.ok() && (root.seen & gateway_message_field_components) != 0)
        result = decode_components(root.components, document, maximum_depth, text, scratch, output);
    if (result.ok() && (root.seen & gateway_message_field_poll))
        result = gateway_decode_message_poll(root.poll, document, maximum_depth, text_output, text_used,
                                             scratch.poll, &output->poll);
    if (result.ok() && (root.seen & gateway_message_field_message_snapshots))
        result = decode_snapshot(root.snapshots, document, maximum_depth, text_output, text_used,
                                 scratch.snapshot, &output->snapshot);
    if (!result.ok()) {
        return result;
    }
    output->fields |= static_cast<u32>(root.seen);
    output->transient_bytes = *text_used;
    return decode_failure(GatewayEventDecodeError::none);
}

} // namespace maicord
