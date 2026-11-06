#include "message_rich_internal.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"

namespace maicord {
namespace message_rich_detail {

bool visit_embed(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EmbedContext*>(opaque);
    switch (member_index<"title", "description", "url", "type", "color", "author", "footer", "fields", "image",
                         "thumbnail", "video", "flags", "provider", "timestamp", "components">(name)) {
    case 0:
        return accept_member(&context->seen, 1ull << 0, json_value_as_string(value, &context->title),
                             &context->error);
    case 1:
        return accept_member(&context->seen, 1ull << 1, json_value_as_string(value, &context->description),
                             &context->error);
    case 2:
        return accept_member(&context->seen, 1ull << 2, json_value_as_string(value, &context->url),
                             &context->error);
    case 3:
        return accept_member(&context->seen, 1ull << 3, json_value_as_string(value, &context->type),
                             &context->error);
    case 4:
        return accept_member(&context->seen, 1ull << 4,
                             value_u32(value, &context->color) && context->color <= 0xFFFFFFu,
                             &context->error);
    case 5:
        if (!accept_member(&context->seen, 1ull << 5, value.kind == JsonValueKind::object, &context->error))
            return false;
        context->author = value;
        break;
    case 6:
        if (!accept_member(&context->seen, 1ull << 6, value.kind == JsonValueKind::object, &context->error))
            return false;
        context->footer = value;
        break;
    case 7:
        if (!accept_member(&context->seen, 1ull << 7, value.kind == JsonValueKind::array, &context->error))
            return false;
        context->fields = value;
        break;
    case 8:
        if (!accept_member(&context->seen, 1ull << 8, value.kind == JsonValueKind::object, &context->error))
            return false;
        context->image = value;
        break;
    case 9:
        if (!accept_member(&context->seen, 1ull << 9, value.kind == JsonValueKind::object, &context->error))
            return false;
        context->thumbnail = value;
        break;
    case 10:
        if (!accept_member(&context->seen, 1ull << 10, value.kind == JsonValueKind::object,
                           &context->error))
            return false;
        context->video = value;
        break;
    case 11:
        return accept_member(&context->seen, 1ull << 11,
                             value.kind == JsonValueKind::null_value || value_u32(value, &context->flags),
                             &context->error);
    case 12:
        if (value.kind == JsonValueKind::null_value) {
            return true;
        }
        if (!accept_member(&context->seen, 1ull << 12, value.kind == JsonValueKind::object,
                           &context->error))
            return false;
        context->provider = value;
        break;
    case 13:
        if (value.kind == JsonValueKind::null_value) {
            return true;
        }
        return accept_member(&context->seen, 1ull << 13, json_value_as_string(value, &context->timestamp),
                             &context->error);
    case 14:
        if (value.kind == JsonValueKind::array && !(context->seen & (1ull << 14))) {
            context->seen |= 1ull << 14;
            context->components = value;
        }
        break;
    default: break;
    }
    return true;
}

struct EmbedMediaContext {
    JsonString url;
    JsonString proxy_url;
    JsonString content_type;
    JsonString description;
    u32 width;
    u32 height;
    u32 flags;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_embed_media(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EmbedMediaContext*>(opaque);
    const u32 index = member_index<"url", "proxy_url", "content_type", "description", "width", "height", "flags">(name);
    if (index > 6)
        return true;
    if (index == 4) {
        return accept_member(&context->seen, 1ull << 4,
                             value.kind == JsonValueKind::null_value || value_u32(value, &context->width),
                             &context->error);
    }
    if (index == 5) {
        return accept_member(&context->seen, 1ull << 5,
                             value.kind == JsonValueKind::null_value || value_u32(value, &context->height),
                             &context->error);
    }
    if (index == 6) {
        return accept_member(&context->seen, 1ull << 6, value_u32(value, &context->flags), &context->error);
    }
    JsonString* const text = index == 0   ? &context->url
                             : index == 1 ? &context->proxy_url
                             : index == 2 ? &context->content_type
                                          : &context->description;
    return accept_member(&context->seen, 1ull << index, json_value_as_string(value, text), &context->error);
}

GatewayEventDecodeResult decode_embed_media(JsonValue object, const JsonIndexedDocument& scratch,
                                            u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                            GatewayMessageEmbedMedia* destination,
                                            u32* truncations) noexcept {
    EmbedMediaContext context{};
    GatewayEventDecodeResult result =
        read_object(object, scratch, maximum_depth, visit_embed_media, &context, &context.error);
    if (!result.ok()) {
        return result;
    }
    reset_value<GatewayMessageEmbedMedia>(destination);
    destination->width = context.width;
    destination->height = context.height;
    destination->flags = context.flags;
    destination->has_width = context.width != 0;
    destination->has_height = context.height != 0;
    if (((context.seen & (1ull << 0)) != 0 &&
         !bounded_string(context.url, kRichUrlMaximumBytes, text_output, text_used, &destination->url,
                         truncations)) ||
        ((context.seen & (1ull << 1)) != 0 &&
         !bounded_string(context.proxy_url, kRichUrlMaximumBytes, text_output, text_used,
                         &destination->proxy_url, truncations)) ||
        ((context.seen & (1ull << 2)) != 0 &&
         !bounded_string(context.content_type, kRichContentTypeMaximumBytes, text_output, text_used,
                         &destination->content_type, truncations)) ||
        ((context.seen & (1ull << 3)) != 0 &&
         !bounded_string(context.description, kRichAttachmentDescriptionMaximumBytes, text_output,
                         text_used, &destination->description, truncations))) {
        return decode_failure(GatewayEventDecodeError::invalid_data, object.source_offset);
    }
    if ((destination->url.bytes != 0 && !rich_url_valid(destination->url, true)) ||
        (destination->proxy_url.bytes != 0 && !rich_url_valid(destination->proxy_url, false))) {
        return decode_failure(GatewayEventDecodeError::invalid_data, object.source_offset);
    }
    return decode_failure(GatewayEventDecodeError::none);
}

GatewayMessageEmbedType embed_type(JsonString value) noexcept {
    char decoded[16]{};
    const JsonStringDecodeResult result = json_string_decode_utf8(value, Slice<char>{decoded, 16});
    if (!result.ok()) {
        return GatewayMessageEmbedType::unknown;
    }
    const Utf8View text{decoded, result.bytes};
    if (utf8_equal(text, utf8_literal("rich"))) {
        return GatewayMessageEmbedType::rich;
    }
    if (utf8_equal(text, utf8_literal("image"))) {
        return GatewayMessageEmbedType::image;
    }
    if (utf8_equal(text, utf8_literal("video"))) {
        return GatewayMessageEmbedType::video;
    }
    if (utf8_equal(text, utf8_literal("gifv"))) {
        return GatewayMessageEmbedType::gifv;
    }
    if (utf8_equal(text, utf8_literal("article"))) {
        return GatewayMessageEmbedType::article;
    }
    if (utf8_equal(text, utf8_literal("link"))) {
        return GatewayMessageEmbedType::link;
    }
    if (utf8_equal(text, utf8_literal("poll_result"))) {
        return GatewayMessageEmbedType::poll_result;
    }
    if (utf8_equal(text, utf8_literal("components"))) {
        return GatewayMessageEmbedType::components;
    }
    return GatewayMessageEmbedType::unknown;
}

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// A components embed carries one Components V2 tree: containers (17) with an
// accent colour, sections (9) with a thumbnail accessory (11), text displays
// (10), media galleries (12) and separators (14). The embed card shows its
// text as one markdown description.
inline constexpr u32 kEmbedComponentTextMaximum = 16;
struct EmbedComponentWalk {
    const JsonIndexedDocument* document;
    u32 depth;
    JsonString texts[kEmbedComponentTextMaximum];
    bool paragraph_before[kEmbedComponentTextMaximum];
    u32 text_count;
    JsonValue thumbnail;
    JsonValue image;
    u32 accent;
    u32 nesting;
    bool accent_seen;
    bool separator_pending;
};
struct EmbedComponentNode {
    JsonString content;
    JsonValue components, accessory, items, media;
    u32 type, accent;
    bool content_seen, components_seen, accessory_seen, items_seen, media_seen, accent_seen;
};
bool visit_embed_component_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& node = *static_cast<EmbedComponentNode*>(opaque);
    u64 number = 0;
    switch (member_index<"type", "content", "components", "accessory", "items", "media", "accent_color">(name)) {
    case 0:
        node.type = json_value_to_u64(value, &number) && number < 256 ? static_cast<u32>(number) : 0;
        break;
    case 1: node.content_seen = value.kind == JsonValueKind::string && json_value_as_string(value, &node.content); break;
    case 2:
        node.components_seen = value.kind == JsonValueKind::array;
        node.components = value;
        break;
    case 3:
        node.accessory_seen = value.kind == JsonValueKind::object;
        node.accessory = value;
        break;
    case 4:
        node.items_seen = value.kind == JsonValueKind::array;
        node.items = value;
        break;
    case 5:
        node.media_seen = value.kind == JsonValueKind::object;
        node.media = value;
        break;
    case 6:
        node.accent_seen = json_value_to_u64(value, &number) && number <= 0xFFFFFFu;
        node.accent = static_cast<u32>(number);
        break;
    default: break;
    }
    return true;
}
bool visit_embed_gallery_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto& walk = *static_cast<EmbedComponentWalk*>(opaque);
    if (index != 0 || value.kind != JsonValueKind::object || walk.image.source.data)
        return true;
    EmbedComponentNode node{};
    if (json_visit_indexed_object(*walk.document, value, walk.depth, visit_embed_component_member, &node).ok() &&
        node.media_seen)
        walk.image = node.media;
    return true;
}
bool visit_embed_component_item(void* opaque, u32, JsonValue value) noexcept {
    auto& walk = *static_cast<EmbedComponentWalk*>(opaque);
    if (value.kind != JsonValueKind::object || walk.nesting > 3)
        return true;
    EmbedComponentNode node{};
    if (!json_visit_indexed_object(*walk.document, value, walk.depth, visit_embed_component_member, &node).ok())
        return true;
    if (node.type == 17 && node.accent_seen && !walk.accent_seen) {
        walk.accent = node.accent;
        walk.accent_seen = true;
    }
    if (node.type == 10 && node.content_seen && walk.text_count < kEmbedComponentTextMaximum) {
        walk.paragraph_before[walk.text_count] = walk.separator_pending;
        walk.texts[walk.text_count++] = node.content;
        walk.separator_pending = false;
    } else if (node.type == 14) {
        walk.separator_pending = walk.text_count != 0;
    } else if (node.type == 12 && node.items_seen) {
        json_visit_indexed_array(*walk.document, node.items, walk.depth, visit_embed_gallery_item, &walk);
    }
    if ((node.type == 17 || node.type == 9) && node.components_seen) {
        ++walk.nesting;
        json_visit_indexed_array(*walk.document, node.components, walk.depth, visit_embed_component_item, &walk);
        --walk.nesting;
    }
    if (node.type == 9 && node.accessory_seen && !walk.thumbnail.source.data) {
        EmbedComponentNode accessory{};
        if (json_visit_indexed_object(*walk.document, node.accessory, walk.depth, visit_embed_component_member,
                                      &accessory)
                .ok() &&
            accessory.type == 11 && accessory.media_seen)
            walk.thumbnail = accessory.media;
    }
    return true;
}
// Text displays joined by line breaks (a blank line across a separator) as
// one bounded description in the decoded text bank.
bool embed_component_description(const EmbedComponentWalk& walk, Slice<char> text_output, u32* text_used,
                                 Utf8View* description, u32* truncations) noexcept {
    reset_value(description);
    const u32 begin = *text_used;
    for (u32 i = 0; i < walk.text_count; ++i) {
        if (i) {
            const u32 breaks = walk.paragraph_before[i] ? 2u : 1u;
            if (!text_output.data || *text_used + breaks > text_output.count ||
                *text_used - begin + breaks > kRichEmbedDescriptionMaximumBytes) {
                *truncations |= gateway_message_rich_truncated_text;
                break;
            }
            for (u32 b = 0; b < breaks; ++b)
                text_output.data[(*text_used)++] = '\n';
        }
        Utf8View piece{};
        if (!bounded_string(walk.texts[i], kRichEmbedDescriptionMaximumBytes - (*text_used - begin), text_output,
                            text_used, &piece, truncations))
            return false;
    }
    if (*text_used > begin && text_output.data)
        *description = {text_output.data + begin, *text_used - begin};
    return true;
}
#pragma clang attribute pop

GatewayEventDecodeResult decode_embeds(JsonValue array, const JsonIndexedDocument& scratch,
                                       u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                       GatewayMessageRichScratch output_scratch,
                                       GatewayMessageEvent* output) noexcept {
    DecodeScratch<JsonValue, kGatewayMessageEmbedMaximum> values;
    const u32 capacity = minimum<u32>(output_scratch.embeds.count, kGatewayMessageEmbedMaximum);
    u32 count = 0;
    GatewayEventDecodeResult result =
        gather_array(array, scratch, maximum_depth, values, capacity, gateway_message_rich_truncated_embeds,
                     &output->rich_truncations, &count);
    if (!result.ok()) {
        return result;
    }
    u32 field_count = 0;
    for (u32 index = 0; index < count; ++index) {
        EmbedContext context{};
        result = read_object(values[index], scratch, maximum_depth, visit_embed, &context, &context.error);
        if (!result.ok()) {
            return result;
        }
        GatewayMessageEmbed& destination = output_scratch.embeds[index];
        reset_value<GatewayMessageEmbed>(&destination);
        destination.field_begin = static_cast<u16>(field_count);
        destination.color = context.color;
        destination.has_color = (context.seen & (1ull << 4)) != 0;
        destination.has_title = (context.seen & (1ull << 0)) != 0;
        destination.has_author = (context.seen & (1ull << 5)) != 0;
        destination.flags = static_cast<u8>(context.flags & 0xFFu);
        destination.type =
            (context.seen & (1ull << 3)) != 0 ? embed_type(context.type) : GatewayMessageEmbedType::unknown;
        if (((context.seen & (1ull << 0)) != 0 &&
             !bounded_string(context.title, kRichEmbedTitleMaximumBytes, text_output, text_used,
                             &destination.title, &output->rich_truncations)) ||
            ((context.seen & (1ull << 1)) != 0 &&
             !bounded_string(context.description, kRichEmbedDescriptionMaximumBytes, text_output, text_used,
                             &destination.description, &output->rich_truncations)) ||
            ((context.seen & (1ull << 2)) != 0 &&
             !bounded_string(context.url, kRichUrlMaximumBytes, text_output, text_used, &destination.url,
                             &output->rich_truncations))) {
            return decode_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
        }
        if (destination.url.bytes != 0 && !rich_url_valid(destination.url, true)) {
            return decode_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
        }
        if ((context.seen & (1ull << 5)) != 0) {
            EmbedPartContext author{};
            result = read_object(context.author, scratch, maximum_depth, visit_embed_part, &author,
                                 &author.error);
            if (!result.ok()) {
                return result;
            }
            if ((author.seen & 1ull) == 0 ||
                !bounded_string(author.name, kRichEmbedTitleMaximumBytes, text_output, text_used,
                                &destination.author_name, &output->rich_truncations) ||
                !embed_part_url(author, 1ull << 2, author.url, text_output, text_used, &destination.author_url,
                                &output->rich_truncations) ||
                !embed_part_url(author, 1ull << 3, author.proxy_icon_url, text_output, text_used,
                                &destination.author_icon, &output->rich_truncations)) {
                return decode_failure(GatewayEventDecodeError::invalid_data, context.author.source_offset);
            }
        }
        if ((context.seen & (1ull << 6)) != 0) {
            EmbedPartContext footer{};
            result = read_object(context.footer, scratch, maximum_depth, visit_embed_part, &footer,
                                 &footer.error);
            if (!result.ok()) {
                return result;
            }
            if ((footer.seen & (1ull << 1)) == 0 ||
                !bounded_string(footer.text, kRichEmbedFooterMaximumBytes, text_output, text_used,
                                &destination.footer_text, &output->rich_truncations) ||
                !embed_part_url(footer, 1ull << 3, footer.proxy_icon_url, text_output, text_used,
                                &destination.footer_icon, &output->rich_truncations)) {
                return decode_failure(GatewayEventDecodeError::invalid_data, context.footer.source_offset);
            }
        }
        if ((context.seen & (1ull << 12)) != 0) {
            EmbedPartContext provider{};
            result = read_object(context.provider, scratch, maximum_depth, visit_embed_part, &provider,
                                 &provider.error);
            if (!result.ok()) {
                return result;
            }
            if (((provider.seen & 1ull) != 0 &&
                 !bounded_string(provider.name, kRichEmbedTitleMaximumBytes, text_output, text_used,
                                 &destination.provider_name, &output->rich_truncations)) ||
                !embed_part_url(provider, 1ull << 2, provider.url, text_output, text_used,
                                &destination.provider_url, &output->rich_truncations)) {
                return decode_failure(GatewayEventDecodeError::invalid_data,
                                      context.provider.source_offset);
            }
        }
        if ((context.seen & (1ull << 13)) != 0)
            parse_iso_timestamp(context.timestamp, &destination.timestamp_ms);
        if ((context.seen & (1ull << 7)) != 0) {
            DecodeScratch<JsonValue, 25> field_values;
            const u32 remaining = output_scratch.embed_fields.count > field_count
                                      ? output_scratch.embed_fields.count - field_count
                                      : 0;
            const u32 field_capacity = minimum<u32>(25, remaining);
            u32 captured = 0;
            result = gather_array(context.fields, scratch, maximum_depth, field_values, field_capacity,
                                  gateway_message_rich_truncated_embed_fields, &output->rich_truncations,
                                  &captured);
            if (!result.ok()) {
                return result;
            }
            for (u32 field = 0; field < captured; ++field) {
                EmbedFieldContext parsed{};
                result = read_object(field_values[field], scratch, maximum_depth, visit_embed_field,
                                     &parsed, &parsed.error);
                if (!result.ok()) {
                    return result;
                }
                if ((parsed.seen & 3ull) != 3ull) {
                    return decode_failure(GatewayEventDecodeError::missing_required_field,
                                          field_values[field].source_offset);
                }
                GatewayMessageEmbedField& field_output = output_scratch.embed_fields[field_count];
                reset_value<GatewayMessageEmbedField>(&field_output);
                field_output.embed_index = static_cast<u16>(index);
                field_output.inline_field = parsed.inline_field;
                if (!bounded_string(parsed.name, kRichEmbedTitleMaximumBytes, text_output, text_used,
                                    &field_output.name, &output->rich_truncations) ||
                    !bounded_string(parsed.value, kRichComponentTextMaximumBytes, text_output, text_used,
                                    &field_output.value, &output->rich_truncations)) {
                    return decode_failure(GatewayEventDecodeError::invalid_data,
                                          field_values[field].source_offset);
                }
                ++field_count;
                ++destination.field_count;
            }
        }
        if ((context.seen & (1ull << 8)) != 0) {
            result = decode_embed_media(context.image, scratch, maximum_depth, text_output, text_used,
                                        &destination.image, &output->rich_truncations);
            if (!result.ok()) {
                return result;
            }
        }
        if ((context.seen & (1ull << 9)) != 0) {
            result = decode_embed_media(context.thumbnail, scratch, maximum_depth, text_output, text_used,
                                        &destination.thumbnail, &output->rich_truncations);
            if (!result.ok()) {
                return result;
            }
        }
        if ((context.seen & (1ull << 10)) != 0) {
            result = decode_embed_media(context.video, scratch, maximum_depth, text_output, text_used,
                                        &destination.video, &output->rich_truncations);
            if (!result.ok()) {
                return result;
            }
        }
        if ((context.seen & (1ull << 14)) != 0) {
            EmbedComponentWalk walk{};
            walk.document = &scratch;
            walk.depth = maximum_depth;
            json_visit_indexed_array(scratch, context.components, maximum_depth, visit_embed_component_item,
                                     &walk);
            if (!destination.description.bytes &&
                !embed_component_description(walk, text_output, text_used, &destination.description,
                                             &output->rich_truncations))
                return decode_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
            if (!destination.has_color && walk.accent_seen) {
                destination.color = walk.accent;
                destination.has_color = true;
            }
            // Malformed optional media degrades to absent, never rejects the message.
            if (walk.thumbnail.source.data && !destination.thumbnail.url.bytes &&
                !decode_embed_media(walk.thumbnail, scratch, maximum_depth, text_output, text_used,
                                    &destination.thumbnail, &output->rich_truncations)
                     .ok())
                reset_value<GatewayMessageEmbedMedia>(&destination.thumbnail);
            if (walk.image.source.data && !destination.image.url.bytes &&
                !decode_embed_media(walk.image, scratch, maximum_depth, text_output, text_used,
                                    &destination.image, &output->rich_truncations)
                     .ok())
                reset_value<GatewayMessageEmbedMedia>(&destination.image);
        }
    }
    output->embeds = ConstSlice<GatewayMessageEmbed>{output_scratch.embeds.data, count};
    output->embed_fields =
        ConstSlice<GatewayMessageEmbedField>{output_scratch.embed_fields.data, field_count};
    return decode_failure(GatewayEventDecodeError::none);
}

struct StickerContext {
    u64 id;
    JsonString name;
    u32 format_type;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_sticker(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<StickerContext*>(opaque);
    switch (member_index<"id", "name", "format_type">(name)) {
    case 0:
        return accept_member(&context->seen, 1ull << 0, json_value_to_snowflake(value, &context->id),
                             &context->error);
    case 1:
        return accept_member(&context->seen, 1ull << 1, json_value_as_string(value, &context->name),
                             &context->error);
    case 2:
        return accept_member(&context->seen, 1ull << 2,
                             value_u32(value, &context->format_type) && context->format_type >= 1 &&
                                 context->format_type <= 4,
                             &context->error);
    default: break;
    }
    return true;
}

GatewayEventDecodeResult decode_stickers(JsonValue array, const JsonIndexedDocument& scratch,
                                         u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                         GatewayMessageRichScratch output_scratch,
                                         GatewayMessageEvent* output) noexcept {
    DecodeScratch<JsonValue, kGatewayMessageStickerMaximum> values;
    const u32 capacity = minimum<u32>(output_scratch.stickers.count, kGatewayMessageStickerMaximum);
    u32 count = 0;
    GatewayEventDecodeResult result =
        gather_array(array, scratch, maximum_depth, values, capacity,
                     gateway_message_rich_truncated_stickers, &output->rich_truncations, &count);
    if (!result.ok()) {
        return result;
    }
    for (u32 index = 0; index < count; ++index) {
        StickerContext parsed{};
        result = read_object(values[index], scratch, maximum_depth, visit_sticker, &parsed, &parsed.error);
        if (!result.ok()) {
            return result;
        }
        if ((parsed.seen & 7ull) != 7ull) {
            return decode_failure(GatewayEventDecodeError::missing_required_field,
                                  values[index].source_offset);
        }
        GatewayMessageSticker& destination = output_scratch.stickers[index];
        reset_value<GatewayMessageSticker>(&destination);
        destination.id = parsed.id;
        destination.format_type = static_cast<u8>(parsed.format_type);
        if (!bounded_string(parsed.name, kGatewayMessageUserNameMaximumBytes, text_output, text_used,
                            &destination.name, &output->rich_truncations)) {
            return decode_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
        }
    }
    output->stickers = ConstSlice<GatewayMessageSticker>{output_scratch.stickers.data, count};
    return decode_failure(GatewayEventDecodeError::none);
}

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
struct UrlObjectContext {
    JsonString url;
    JsonString proxy_url;
    u64 seen;
    u32 width;
    u32 height;
    GatewayEventDecodeError error;
};

// Unfurled media items carry both the source URL and the media proxy URL;
// the proxy is fetchable through the CDN pipeline, so it is preferred. Their
// width and height (when known) size a lone gallery item.
bool visit_url_object(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<UrlObjectContext*>(opaque);
    const u32 which = member_index<"url", "proxy_url", "width", "height">(name);
    const bool null = value.kind == JsonValueKind::null_value;
    if (which >= 2 && which < 4) {
        u64 number = 0;
        if (!null && json_value_to_u64(value, &number))
            (which == 2 ? context->width : context->height) = static_cast<u32>(minimum<u64>(number, 0xFFFFu));
        return true;
    }
    const bool url = which == 0;
    if (which < 2) {
        const u64 bit = url ? 1ull : 2ull;
        if (!accept_member(&context->seen, bit,
                           null || json_value_as_string(value, url ? &context->url : &context->proxy_url),
                           &context->error))
            return false;
        if (null)
            context->seen &= ~bit;
    }
    return true;
}

struct ComponentContext {
    JsonString label;
    JsonString content;
    JsonString custom_id;
    JsonString url;
    JsonString placeholder;
    JsonString description;
    JsonString value;
    JsonString name;
    JsonValue children;
    JsonValue accessory;
    JsonValue media;
    JsonValue file;
    JsonValue emoji;
    JsonValue options;
    JsonValue items;
    u32 id;
    u32 type;
    u32 style;
    u32 accent_color;
    u32 min_values;
    u32 max_values;
    u32 spacing;
    bool disabled;
    bool has_accent_color;
    bool spoiler;
    bool divider;
    bool selected_default;
    u64 seen;
    GatewayEventDecodeError error;
};

enum : u64 {
    component_seen_type = 1ull << 0,
    component_seen_id = 1ull << 1,
    component_seen_style = 1ull << 2,
    component_seen_disabled = 1ull << 3,
    component_seen_label = 1ull << 4,
    component_seen_content = 1ull << 5,
    component_seen_custom_id = 1ull << 6,
    component_seen_url = 1ull << 7,
    component_seen_children = 1ull << 8,
    component_seen_accessory = 1ull << 9,
    component_seen_media = 1ull << 10,
    component_seen_file = 1ull << 11,
    component_seen_accent = 1ull << 12,
    component_seen_emoji = 1ull << 13,
    component_seen_placeholder = 1ull << 14,
    component_seen_description = 1ull << 15,
    component_seen_value = 1ull << 16,
    component_seen_name = 1ull << 17,
    component_seen_options = 1ull << 18,
    component_seen_items = 1ull << 19,
    component_seen_min = 1ull << 20,
    component_seen_max = 1ull << 21,
    component_seen_spacing = 1ull << 22,
    component_seen_spoiler = 1ull << 23,
    component_seen_divider = 1ull << 24,
    component_seen_default = 1ull << 25,
};

// Optional members may be null; null is then treated as absent.
bool component_text(ComponentContext* context, u64 bit, JsonValue value, JsonString* output) noexcept {
    const bool null = value.kind == JsonValueKind::null_value;
    if (!accept_member(&context->seen, bit, null || json_value_as_string(value, output), &context->error))
        return false;
    if (null)
        context->seen &= ~bit;
    return true;
}

bool component_bool(ComponentContext* context, u64 bit, JsonValue value, bool* output) noexcept {
    return accept_member(&context->seen, bit,
                         value.kind == JsonValueKind::null_value || json_value_to_bool(value, output),
                         &context->error);
}

bool component_number(ComponentContext* context, u64 bit, JsonValue value, u32* output,
                      u32 maximum_value) noexcept {
    return accept_member(&context->seen, bit,
                         value.kind == JsonValueKind::null_value ||
                             (value_u32(value, output) && *output <= maximum_value),
                         &context->error);
}

bool component_value(ComponentContext* context, u64 bit, JsonValue value, JsonValueKind kind,
                     JsonValue* output) noexcept {
    const bool null = value.kind == JsonValueKind::null_value;
    if (!accept_member(&context->seen, bit, null || value.kind == kind, &context->error))
        return false;
    if (null)
        context->seen &= ~bit;
    else
        *output = value;
    return true;
}

bool visit_component(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ComponentContext*>(opaque);
    switch (member_index<"accent_color", "type", "id", "style", "disabled", "label", "content", "custom_id", "url",
                         "placeholder", "description", "value", "name", "components", "accessory", "media", "file",
                         "emoji", "options", "items", "min_values", "max_values", "spacing", "spoiler", "divider",
                         "default">(name)) {
    case 0:
        context->has_accent_color = value.kind != JsonValueKind::null_value;
        return accept_member(&context->seen, component_seen_accent,
                             !context->has_accent_color || (value_u32(value, &context->accent_color) &&
                                                            context->accent_color <= 0xFFFFFFu),
                             &context->error);
    case 1:
        return accept_member(&context->seen, component_seen_type,
                             value_u32(value, &context->type) && context->type != 0 &&
                                 context->type < gateway_component_select_option,
                             &context->error);
    case 2: {
        // A JSON number in REST; the ETF Gateway quotes every value under an "id" key.
        u64 id = 0;
        const bool valid = json_value_to_u64_or_string(value, &id) && id <= ~0u;
        context->id = static_cast<u32>(id);
        return accept_member(&context->seen, component_seen_id, valid, &context->error);
    }
    case 3: return component_number(context, component_seen_style, value, &context->style, 0xFFFFu);
    case 4: return component_bool(context, component_seen_disabled, value, &context->disabled);
    case 5: return component_text(context, component_seen_label, value, &context->label);
    case 6: return component_text(context, component_seen_content, value, &context->content);
    case 7: return component_text(context, component_seen_custom_id, value, &context->custom_id);
    case 8: return component_text(context, component_seen_url, value, &context->url);
    case 9: return component_text(context, component_seen_placeholder, value, &context->placeholder);
    case 10: return component_text(context, component_seen_description, value, &context->description);
    case 11: return component_text(context, component_seen_value, value, &context->value);
    case 12: return component_text(context, component_seen_name, value, &context->name);
    case 13:
        return component_value(context, component_seen_children, value, JsonValueKind::array,
                               &context->children);
    case 14:
        return component_value(context, component_seen_accessory, value, JsonValueKind::object,
                               &context->accessory);
    case 15: return component_value(context, component_seen_media, value, JsonValueKind::object, &context->media);
    case 16: return component_value(context, component_seen_file, value, JsonValueKind::object, &context->file);
    case 17: return component_value(context, component_seen_emoji, value, JsonValueKind::object, &context->emoji);
    case 18:
        return component_value(context, component_seen_options, value, JsonValueKind::array,
                               &context->options);
    case 19: return component_value(context, component_seen_items, value, JsonValueKind::array, &context->items);
    case 20: return component_number(context, component_seen_min, value, &context->min_values, 25);
    case 21: return component_number(context, component_seen_max, value, &context->max_values, 25);
    case 22: return component_number(context, component_seen_spacing, value, &context->spacing, 0xFFFFu);
    case 23: return component_bool(context, component_seen_spoiler, value, &context->spoiler);
    case 24: return component_bool(context, component_seen_divider, value, &context->divider);
    case 25: return component_bool(context, component_seen_default, value, &context->selected_default);
    default: break;
    }
    return true;
}

struct ComponentEmojiContext {
    u64 id;
    JsonString name;
    bool animated;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_component_emoji(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ComponentEmojiContext*>(opaque);
    const bool null_value = value.kind == JsonValueKind::null_value;
    switch (member_index<"id", "name", "animated">(name)) {
    case 0: {
        JsonString text{};
        return accept_member(
            &context->seen, 1ull << 0,
            null_value || (json_value_as_string(value, &text) && json_string_to_u64(text, &context->id)),
            &context->error);
    }
    case 1:
        if (!accept_member(&context->seen, 1ull << 1,
                           null_value || json_value_as_string(value, &context->name), &context->error))
            return false;
        if (null_value)
            context->seen &= ~(1ull << 1);
        break;
    case 2:
        return accept_member(&context->seen, 1ull << 2,
                             null_value || json_value_to_bool(value, &context->animated), &context->error);
    default: break;
    }
    return true;
}

struct ComponentWork {
    JsonValue value;
    u16 parent;
    u16 order;
    u16 pseudo_type; // 0 for wire components; otherwise an option or gallery item
};

bool queue_component_children(JsonValue array, const JsonIndexedDocument& scratch, u32 maximum_depth,
                              u32 capacity, u16 parent, u16 pseudo_type, ComponentWork* work, u32* work_count,
                              GatewayMessageEvent* output, GatewayEventDecodeResult* result) noexcept {
    DecodeScratch<JsonValue, kGatewayMessageComponentMaximum> children;
    const u32 remaining = capacity > *work_count ? capacity - *work_count : 0;
    u32 child_count = 0;
    *result = gather_array(array, scratch, maximum_depth, children, remaining,
                           gateway_message_rich_truncated_components, &output->rich_truncations, &child_count);
    if (!result->ok())
        return false;
    for (u32 child = 0; child < child_count; ++child)
        work[(*work_count)++] = ComponentWork{children[child], parent, static_cast<u16>(child), pseudo_type};
    return true;
}

GatewayEventDecodeResult decode_components(JsonValue array, const JsonIndexedDocument& scratch,
                                           u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                           GatewayMessageRichScratch output_scratch,
                                           GatewayMessageEvent* output) noexcept {
    DecodeScratch<ComponentWork, kGatewayMessageComponentMaximum> work;
    DecodeScratch<JsonValue, kGatewayMessageComponentMaximum> root_values;
    const u32 capacity = minimum<u32>(output_scratch.components.count, kGatewayMessageComponentMaximum);
    u32 root_count = 0;
    GatewayEventDecodeResult result =
        gather_array(array, scratch, maximum_depth, root_values, capacity,
                     gateway_message_rich_truncated_components, &output->rich_truncations, &root_count);
    if (!result.ok()) {
        return result;
    }
    u32 work_count = root_count;
    for (u32 index = 0; index < root_count; ++index) {
        work[index] = ComponentWork{root_values[index], 0xFFFFu, static_cast<u16>(index), 0};
    }
    u32 output_count = 0;
    for (u32 cursor = 0; cursor < work_count && output_count < capacity; ++cursor) {
        ComponentContext parsed{};
        result = read_object(work[cursor].value, scratch, maximum_depth, visit_component, &parsed,
                             &parsed.error);
        if (!result.ok()) {
            return result;
        }
        const u16 pseudo = work[cursor].pseudo_type;
        if (pseudo) {
            if ((parsed.seen & component_seen_type) != 0)
                return decode_failure(GatewayEventDecodeError::invalid_data,
                                      work[cursor].value.source_offset);
            parsed.type = pseudo;
        } else if ((parsed.seen & component_seen_type) == 0) {
            return decode_failure(GatewayEventDecodeError::missing_required_field,
                                  work[cursor].value.source_offset);
        }
        const u16 destination_index = static_cast<u16>(output_count);
        GatewayMessageComponent& destination = output_scratch.components[output_count++];
        reset_value<GatewayMessageComponent>(&destination);
        destination.id = parsed.id;
        destination.parent = work[cursor].parent;
        destination.order = work[cursor].order;
        destination.type = static_cast<u16>(parsed.type);
        // Separator spacing: 1 small (default), 2 large.
        destination.style = static_cast<u16>(
            parsed.type == gateway_component_separator
                ? ((parsed.seen & component_seen_spacing) && parsed.spacing ? parsed.spacing : 1u)
                : parsed.style);
        destination.accent_color = parsed.accent_color;
        destination.has_accent_color = parsed.has_accent_color;
        destination.disabled = parsed.disabled;
        const bool select = gateway_component_is_select(destination.type);
        if (select) {
            destination.min_values =
                static_cast<u8>((parsed.seen & component_seen_min) ? parsed.min_values : 1u);
            destination.max_values =
                static_cast<u8>((parsed.seen & component_seen_max) ? parsed.max_values : 1u);
            if (destination.max_values == 0 || destination.min_values > destination.max_values)
                return decode_failure(GatewayEventDecodeError::invalid_data,
                                      work[cursor].value.source_offset);
        }
        // A separator draws its divider unless the payload disables it.
        const bool divider = destination.type == gateway_component_separator &&
                             !((parsed.seen & component_seen_divider) && !parsed.divider);
        destination.flags = static_cast<u8>((parsed.spoiler ? gateway_message_component_spoiler : 0u) |
                                            (divider ? gateway_message_component_divider : 0u) |
                                            (parsed.selected_default ? gateway_message_component_default : 0u));
        // Type-specific field reuse (see GatewayMessageComponent).
        JsonString label = parsed.label, content = parsed.content, custom_id = parsed.custom_id;
        bool label_seen = (parsed.seen & component_seen_label) != 0;
        bool content_seen = (parsed.seen & component_seen_content) != 0;
        bool custom_seen = (parsed.seen & component_seen_custom_id) != 0;
        if (select) {
            content = parsed.placeholder;
            content_seen = (parsed.seen & component_seen_placeholder) != 0;
        } else if (destination.type == gateway_component_select_option) {
            content = parsed.description;
            content_seen = (parsed.seen & component_seen_description) != 0;
            custom_id = parsed.value;
            custom_seen = (parsed.seen & component_seen_value) != 0;
        } else if (destination.type == gateway_component_thumbnail ||
                   destination.type == gateway_component_gallery_item) {
            content = parsed.description;
            content_seen = (parsed.seen & component_seen_description) != 0;
        } else if (destination.type == gateway_component_file && !label_seen) {
            label = parsed.name;
            label_seen = (parsed.seen & component_seen_name) != 0;
        }
        if ((label_seen && !bounded_string(label, kRichAttachmentNameMaximumBytes, text_output, text_used,
                                           &destination.label, &output->rich_truncations)) ||
            (content_seen && !bounded_string(content, kRichComponentTextMaximumBytes, text_output, text_used,
                                             &destination.content, &output->rich_truncations)) ||
            (custom_seen && !bounded_string(custom_id, kRichComponentIdMaximumBytes, text_output, text_used,
                                            &destination.custom_id, &output->rich_truncations)) ||
            ((parsed.seen & component_seen_url) != 0 &&
             !bounded_string(parsed.url, kRichUrlMaximumBytes, text_output, text_used, &destination.url,
                             &output->rich_truncations))) {
            return decode_failure(GatewayEventDecodeError::invalid_data, work[cursor].value.source_offset);
        }
        if (destination.url.bytes != 0 && !rich_url_valid(destination.url, true)) {
            return decode_failure(GatewayEventDecodeError::invalid_data, work[cursor].value.source_offset);
        }
        if (parsed.seen & component_seen_emoji) {
            ComponentEmojiContext emoji{};
            result = read_object(parsed.emoji, scratch, maximum_depth, visit_component_emoji, &emoji,
                                 &emoji.error);
            if (!result.ok())
                return result;
            destination.emoji_id = emoji.id;
            if (emoji.animated)
                destination.flags |= gateway_message_component_emoji_animated;
            if ((emoji.seen & 2u) && !bounded_string(emoji.name, kRichAttachmentNameMaximumBytes, text_output,
                                                     text_used, &destination.emoji_name,
                                                     &output->rich_truncations))
                return decode_failure(GatewayEventDecodeError::invalid_data, parsed.emoji.source_offset);
        }
        const JsonValue indirect_url = (parsed.seen & component_seen_media) != 0 ? parsed.media : parsed.file;
        if (destination.url.bytes == 0 && indirect_url.source.data) {
            UrlObjectContext url{};
            result = read_object(indirect_url, scratch, maximum_depth, visit_url_object, &url, &url.error);
            if (!result.ok()) {
                return result;
            }
            const bool proxy = (url.seen & 2ull) != 0;
            if ((url.seen & 3ull) != 0 &&
                !bounded_string(proxy ? url.proxy_url : url.url, kRichUrlMaximumBytes, text_output, text_used,
                                &destination.url, &output->rich_truncations)) {
                return decode_failure(GatewayEventDecodeError::invalid_data, indirect_url.source_offset);
            }
            // attachment:// names resolve against the message's files.
            if (destination.url.bytes != 0 && !rich_url_valid(destination.url, true))
                return decode_failure(GatewayEventDecodeError::invalid_data, indirect_url.source_offset);
            // Media items reuse accent_color for their size: width << 16 | height.
            if ((destination.type == gateway_component_thumbnail || destination.type == gateway_component_gallery_item) &&
                url.width && url.height)
                destination.accent_color = url.width << 16 | url.height;
        }
        if ((parsed.seen & component_seen_children) != 0 &&
            !queue_component_children(parsed.children, scratch, maximum_depth, capacity, destination_index, 0,
                                      work, &work_count, output, &result))
            return result;
        if ((parsed.seen & component_seen_options) != 0 &&
            destination.type == gateway_component_string_select &&
            !queue_component_children(parsed.options, scratch, maximum_depth, capacity, destination_index,
                                      gateway_component_select_option, work, &work_count, output, &result))
            return result;
        if ((parsed.seen & component_seen_items) != 0 && destination.type == gateway_component_media_gallery &&
            !queue_component_children(parsed.items, scratch, maximum_depth, capacity, destination_index,
                                      gateway_component_gallery_item, work, &work_count, output, &result))
            return result;
        if ((parsed.seen & component_seen_accessory) != 0) {
            if (work_count < capacity) {
                work[work_count++] = ComponentWork{parsed.accessory, destination_index, 0xFFFFu, 0};
            } else {
                output->rich_truncations |= gateway_message_rich_truncated_components;
            }
        }
    }
    output->components = ConstSlice<GatewayMessageComponent>{output_scratch.components.data, output_count};
    return decode_failure(GatewayEventDecodeError::none);
}
#pragma clang attribute pop
} // namespace message_rich_detail
} // namespace maicord
