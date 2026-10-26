#include "maicord/protocol/message_search.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/message_card_excerpt.hpp"

#include "maicord/protocol/writer.hpp"
#include "maicord/protocol/json_ids.hpp"

namespace maicord {
namespace {

bool valid_ids(const u64* ids, u8 count) noexcept {
    if (count > kMessageSearchFilterIds) {
        return false;
    }
    for (u8 index = 0; index < count; ++index) {
        if (!ids[index]) {
            return false;
        }
        for (u8 earlier = 0; earlier < index; ++earlier) {
            if (ids[index] == ids[earlier]) {
                return false;
            }
        }
    }
    return true;
}

bool append_parameter(ProtocolWriter* writer, const char* name) noexcept {
    return protocol_writer_append_byte(writer, '&') && protocol_writer_append_c_string(writer, name) &&
           protocol_writer_append_byte(writer, '=');
}

bool append_ids(ProtocolWriter* writer, const char* name, const u64* ids, u8 count) noexcept {
    for (u8 index = 0; index < count; ++index) {
        if (!append_parameter(writer, name) || !protocol_writer_append_u64(writer, ids[index])) {
            return false;
        }
    }
    return true;
}

struct DecodeContext {
    JsonIndexedDocument document;
    const MessageSearchQuery* query;
    Slice<u32> message_structurals;
    Slice<char> text;
    Slice<MessageSearchGroup> groups;
    Slice<MessageSearchMessage> messages;
    MessageSearchDecodeResult result;
    u32 maximum_depth;
    u32 fields;
    u32 current_group;
    JsonValue message_groups, tabs;
    const MessageSearchRichScratch* rich_scratch;
    MessageSearchRichSink sink;
};

template <typename T> bool disjoint(Slice<T> a, Slice<T> b) noexcept {
    const usize start_a = reinterpret_cast<usize>(a.data), start_b = reinterpret_cast<usize>(b.data);
    return start_a <= start_b ? start_b - start_a >= static_cast<usize>(a.count) * sizeof(T)
                              : start_a - start_b >= static_cast<usize>(b.count) * sizeof(T);
}

bool reject(DecodeContext* context, MessageSearchDecodeError error, u32 offset) noexcept {
    context->result.error = error;
    context->result.error_offset = offset;
    return false;
}

struct HitContext {
    u64 guild_id;
    bool hit;
    bool seen;
    bool valid;
    bool guild_seen;
    JsonValue snapshots{};
    bool snapshots_seen{};
};

bool visit_hit(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* hit = static_cast<HitContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("message_snapshots"))) {
        if (hit->snapshots_seen) {
            hit->valid = false;
            return false;
        }
        hit->snapshots_seen = true;
        hit->snapshots = value;
    } else if (json_string_equals_ascii(name, utf8_literal("guild_id"))) {
        if (hit->guild_seen || (value.kind != JsonValueKind::null_value &&
                                !json_value_to_snowflake(value, &hit->guild_id))) {
            hit->valid = false;
            return false;
        }
        hit->guild_seen = true;
    } else if (json_string_equals_ascii(name, utf8_literal("hit"))) {
        if (hit->seen || !json_value_to_bool(value, &hit->hit)) {
            hit->valid = false;
            return false;
        }
        hit->seen = true;
    }
    return true;
}

struct SnapshotCounts {
    JsonIndexedDocument document{};
    u32 maximum_depth{}, fields{};
    u16 counts[4]{};
    bool valid{true};
};
bool count_media(void* opaque, u32 index, JsonValue value) noexcept {
    auto* count = static_cast<u16*>(opaque);
    if (value.kind != JsonValueKind::object || index >= 65535)
        return false;
    *count = static_cast<u16>(index + 1);
    return true;
}
bool visit_snapshot_media(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<SnapshotCounts*>(opaque);
    const Utf8View names[]{utf8_literal("attachments"), utf8_literal("embeds"),
                           utf8_literal("sticker_items"), utf8_literal("components")};
    for (u32 i = 0; i < 4; ++i) {
        if (!json_string_equals_ascii(name, names[i]))
            continue;
        const u32 field = 1u << i;
        if ((context.fields & field) || value.kind != JsonValueKind::array) {
            context.valid = false;
            return false;
        }
        context.fields |= field;
        const auto read = json_visit_indexed_array(context.document, value, context.maximum_depth,
                                                   count_media, &context.counts[i]);
        context.valid &= read.ok();
        return context.valid;
    }
    return true;
}
bool snapshot_excerpt(DecodeContext& context, const HitContext& hit,
                      MessageSearchMessage& item) noexcept {
    if (!hit.snapshots_seen)
        return true;
    JsonValue body{};
    bool present = false;
    const auto extracted = gateway_extract_message_snapshot(hit.snapshots, context.message_structurals,
                                                            context.maximum_depth, &body, &present);
    if (!extracted.ok()) {
        context.result.message_error = extracted.error;
        context.result.scan_error = extracted.scan_error;
        context.result.read_error = extracted.read_error;
        return reject(&context, MessageSearchDecodeError::message_decode, extracted.error_offset);
    }
    item.message.fields |= gateway_message_field_message_snapshots;
    if (!present)
        return true;
    // The scoped core validates all child scalars/named mentions without an
    // invented author. Only the bounded quote is copied into the result bank.
    char temporary[32 * 1024];
    GatewayMessageNamedUser mentions[kGatewayMessageMentionUserMaximum];
    GatewayMessageEvent child;
    const auto decoded = gateway_decode_message_snapshot_core(
        body, context.message_structurals, context.maximum_depth, {temporary, sizeof(temporary)},
        {mentions, kGatewayMessageMentionUserMaximum}, &child);
    if (!decoded.ok()) {
        context.result.message_error = decoded.error;
        context.result.scan_error = decoded.scan_error;
        context.result.read_error = decoded.read_error;
        return reject(&context, MessageSearchDecodeError::message_decode, decoded.error_offset);
    }
    const auto scan = json_scan_structurals_avx1(body.source, context.message_structurals);
    if (!scan.ok())
        return reject(&context, MessageSearchDecodeError::scan_failed, scan.error_offset);
    SnapshotCounts media{{body.source, {context.message_structurals.data, scan.count}},
                         context.maximum_depth};
    const auto read = json_visit_value_object(body, media.document.structurals, context.maximum_depth,
                                             visit_snapshot_media, &media);
    if (!read.ok() || !media.valid)
        return reject(&context, MessageSearchDecodeError::invalid_field, body.source_offset);
    const u32 used = context.result.page.text_bytes + item.message.transient_bytes;
    if (used >= context.text.count || !context.text.data) {
        context.result.message_error = GatewayEventDecodeError::message_content_capacity;
        return reject(&context, MessageSearchDecodeError::message_decode, body.source_offset);
    }
    const auto card =
        message_card_excerpt({item.message.content, child.content, true, true, media.counts[0],
                              media.counts[1], media.counts[2], media.counts[3]},
                             {context.text.data + used, context.text.count - used});
    if (!card.bytes)
        return reject(&context, MessageSearchDecodeError::message_decode, body.source_offset);
    item.card_excerpt = {context.text.data + used, card.bytes};
    item.forwarded = true;
    item.excerpt_truncated = card.truncated;
    item.snapshot_attachments = media.counts[0];
    item.snapshot_embeds = media.counts[1];
    item.snapshot_stickers = media.counts[2];
    item.snapshot_components = media.counts[3];
    auto& snapshot = item.message.snapshot;
    snapshot.present = true;
    snapshot.content = {item.card_excerpt.data + card.quoted_offset, card.quoted_bytes};
    snapshot.timestamp_ms = child.timestamp_ms;
    snapshot.edited_timestamp_ms = child.edited_timestamp_ms;
    snapshot.flags = child.flags;
    snapshot.fields = child.fields;
    snapshot.type = child.type;
    // No temporary mentions or media views escape; their counts live above.
    item.message.transient_bytes += card.bytes;
    return true;
}

void clear_borrowed_views(MessageSearchMessage& item) noexcept {
    auto& message = item.message;
    reset_value(&message.content);
    reset_value(&message.author_name);
    reset_value(&message.author_avatar_hash);
    reset_value(&message.mention_roles);
    reset_value(&message.member_roles);
    reset_value(&message.mention_users);
    reset_value(&message.reference.author_name);
    reset_value(&message.reference.author_avatar_hash);
    reset_value(&message.reference.content);
    reset_value(&message.interaction.user_name);
    reset_value(&message.interaction.user_avatar_hash);
    reset_value(&message.interaction.command_name);
    reset_value(&message.reactions);
    reset_value(&message.attachments);
    reset_value(&message.embeds);
    reset_value(&message.embed_fields);
    reset_value(&message.stickers);
    reset_value(&message.components);
    reset_value(&message.poll);
    message.fields &= ~gateway_message_field_poll; // scalar summaries are not explicit poll removals
    auto& child = message.snapshot;
    reset_value(&child.content);
    reset_value(&child.attachments);
    reset_value(&child.embeds);
    reset_value(&child.embed_fields);
    reset_value(&child.stickers);
    reset_value(&child.components);
    reset_value(&child.mention_users);
    reset_value(&item.card_excerpt);
}

bool visit_message(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<DecodeContext*>(opaque);
    auto& page = context->result.page;
    auto& group = context->groups[context->current_group];
    if (value.kind != JsonValueKind::object) {
        return reject(context, MessageSearchDecodeError::invalid_group, value.source_offset);
    }
    if (index >= kMessageSearchMaximumContext || page.message_count >= context->messages.count ||
        page.message_count >= kMessageSearchMaximumMessages) {
        return reject(context, MessageSearchDecodeError::message_capacity, value.source_offset);
    }
    HitContext hit{};
    hit.valid = true;
    const auto hit_read =
        json_visit_indexed_object(context->document, value, context->maximum_depth, visit_hit, &hit);
    if (!hit_read.ok() || !hit.valid) {
        return reject(context, MessageSearchDecodeError::invalid_field, value.source_offset);
    }
    if (hit.guild_seen && (context->query->scope == MessageSearchScope::guild
                               ? hit.guild_id != context->query->scope_id
                               : hit.guild_id != 0)) {
        return reject(context, MessageSearchDecodeError::guild_mismatch, value.source_offset);
    }
    // Decoded in the result slot; it only counts once the message is kept.
    MessageSearchMessage& item = context->messages[page.message_count];
    GatewayMessageEvent& message = item.message;
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds];
    GatewayMessageNamedUser mention_users[kGatewayMessageMentionUserMaximum];
    const auto decoded =
        context->rich_scratch
            ? gateway_decode_message_create(
                  value, context->message_structurals, context->maximum_depth, context->text,
                  context->rich_scratch->mention_roles, context->rich_scratch->member_roles,
                  context->rich_scratch->mention_users, context->rich_scratch->rich, 0, &message)
            : gateway_decode_message_create(
                  value, context->message_structurals, context->maximum_depth,
                  {context->text.data ? context->text.data + page.text_bytes : nullptr,
                   context->text.count - page.text_bytes},
                  {mention_roles, kGatewayMessageMentionRoleMaximumIds},
                  {mention_users, kGatewayMessageMentionUserMaximum}, 0, &message);
    if (!decoded.ok()) {
        context->result.message_error = decoded.error;
        context->result.scan_error = decoded.scan_error;
        context->result.read_error = decoded.read_error;
        return reject(context, MessageSearchDecodeError::message_decode, value.source_offset);
    }
    if ((context->query->scope == MessageSearchScope::channel &&
         message.channel_id != context->query->scope_id) ||
        (index && message.channel_id != context->messages[group.first_message].message.channel_id)) {
        return reject(context, MessageSearchDecodeError::channel_mismatch, value.source_offset);
    }
    if (context->query->channel_count) {
        bool allowed = false;
        for (u8 channel = 0; channel < context->query->channel_count; ++channel) {
            allowed |= message.channel_id == context->query->channel_ids[channel];
        }
        if (!allowed) {
            return reject(context, MessageSearchDecodeError::channel_mismatch, value.source_offset);
        }
    }
    for (u32 earlier = group.first_message; earlier < page.message_count; ++earlier) {
        if (context->messages[earlier].message.id == message.id) {
            return reject(context, MessageSearchDecodeError::invalid_group, value.source_offset);
        }
    }
    // Without a rich sink the excerpt keeps no mention views; a sink consumes the
    // reusable named-mention and media pools before the next message is decoded.
    if (!context->rich_scratch) {
        reset_value(&message.mention_roles);
        reset_value(&message.mention_users);
    }
    item.guild_id = hit.guild_id;
    item.hit = hit.hit;
    item.guild_known = hit.guild_seen;
    item.snapshot_attachments = item.snapshot_embeds = item.snapshot_stickers = item.snapshot_components = 0;
    item.forwarded = item.excerpt_truncated = false;
    // Ordinary cards borrow the already-owned message text. Only a forwarded
    // quote needs an additional bounded copy in the same arena.
    item.card_excerpt = message.content;
    if (context->rich_scratch) {
        item.forwarded = message.snapshot.present;
        item.snapshot_attachments = static_cast<u16>(message.snapshot.attachments.count);
        item.snapshot_embeds = static_cast<u16>(message.snapshot.embeds.count);
        item.snapshot_stickers = static_cast<u16>(message.snapshot.stickers.count);
        item.snapshot_components = static_cast<u16>(message.snapshot.components.count);
        if (!context->sink.accept(context->sink.context, page.message_count, item))
            return reject(context, MessageSearchDecodeError::sink_rejected, value.source_offset);
        clear_borrowed_views(item);
    } else if (!snapshot_excerpt(*context, hit, item))
        return false;
    ++page.message_count;
    page.text_bytes += item.message.transient_bytes;
    ++group.message_count;
    group.hit_count += hit.hit ? 1 : 0;
    return true;
}

bool visit_group(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<DecodeContext*>(opaque);
    if (value.kind != JsonValueKind::array) {
        return reject(context, MessageSearchDecodeError::invalid_group, value.source_offset);
    }
    if (index >= context->query->limit || index >= context->groups.count ||
        index >= kMessageSearchMaximumGroups) {
        return reject(context, MessageSearchDecodeError::group_capacity, value.source_offset);
    }
    context->current_group = index;
    auto& group = context->groups[index];
    group = {context->result.page.message_count, 0, 0};
    const auto read =
        json_visit_indexed_array(context->document, value, context->maximum_depth, visit_message, context);
    if (!read.ok()) {
        context->result.read_error = read.error;
        return false;
    }
    if (!group.message_count || !group.hit_count) {
        return reject(context, MessageSearchDecodeError::invalid_group, value.source_offset);
    }
    ++context->result.page.group_count;
    return true;
}

bool visit_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<DecodeContext*>(opaque);
    u32 field = 0;
    if (json_string_equals_ascii(name, utf8_literal("messages"))) {
        field = 1;
    } else if (json_string_equals_ascii(name, utf8_literal("total_results"))) {
        field = 2;
    } else if (json_string_equals_ascii(name, utf8_literal("doing_deep_historical_index"))) {
        field = 4;
    } else if (json_string_equals_ascii(name, utf8_literal("documents_indexed"))) {
        field = 8;
    } else if (json_string_equals_ascii(name, utf8_literal("tabs"))) {
        field = 16;
    } else {
        return true;
    }
    if (context->fields & field) {
        return reject(context, MessageSearchDecodeError::duplicate_field, value.source_offset);
    }
    context->fields |= field;
    bool valid = false;
    switch (field) {
    case 1:
        valid = value.kind == JsonValueKind::array;
        context->message_groups = value;
        break;
    case 2:
        valid = json_value_to_u64(value, &context->result.page.total_results);
        break;
    case 4:
        valid = json_value_to_bool(value, &context->result.page.historical_indexing);
        break;
    case 8:
        valid = json_value_to_u64(value, &context->result.page.documents_indexed);
        break;
    case 16:
        valid = value.kind == JsonValueKind::object;
        context->tabs = value;
        break;
    }
    return valid || reject(context, MessageSearchDecodeError::invalid_field, value.source_offset);
}

struct TabContext {
    DecodeContext* decode;
    JsonValue messages;
    u64 total;
    u32 fields;
};
bool visit_tab_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* tab = static_cast<TabContext*>(opaque);
    const u32 field = json_string_equals_ascii(name, utf8_literal("messages"))        ? 1u
                      : json_string_equals_ascii(name, utf8_literal("total_results")) ? 2u
                                                                                      : 0u;
    if (!field)
        return true;
    if (tab->fields & field)
        return reject(tab->decode, MessageSearchDecodeError::duplicate_field, value.source_offset);
    tab->fields |= field;
    const bool valid =
        field == 1 ? value.kind == JsonValueKind::array : json_value_to_u64(value, &tab->total);
    tab->messages = field == 1 ? value : tab->messages;
    return valid ||
           reject(tab->decode, MessageSearchDecodeError::invalid_field, value.source_offset);
}
bool visit_tab(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<DecodeContext*>(opaque);
    static constexpr Utf8View names[] = {utf8_literal("messages"), utf8_literal("media"), utf8_literal("files"),
                                  utf8_literal("links"), utf8_literal("pins")};
    for (u32 index = 0; index < kMessageSearchTabCount; ++index) {
        if (!json_string_equals_ascii(name, names[index]))
            continue;
        auto& page = context->result.page;
        if (page.tab_known & (1u << index))
            return reject(context, MessageSearchDecodeError::duplicate_field, value.source_offset);
        if (value.kind != JsonValueKind::object)
            return reject(context, MessageSearchDecodeError::invalid_field, value.source_offset);
        TabContext tab{context};
        const auto read = json_visit_indexed_object(context->document, value, context->maximum_depth,
                                                    visit_tab_field, &tab);
        if (!read.ok())
            return false;
        if (tab.fields != 3)
            return reject(context, MessageSearchDecodeError::missing_field, value.source_offset);
        page.tab_known |= static_cast<u8>(1u << index);
        page.tab_totals[index] = tab.total;
        if (index == static_cast<u8>(context->query->tab)) {
            page.total_results = tab.total;
            context->message_groups = tab.messages;
            context->fields |= 3;
        }
        return true;
    }
    return true;
}

constexpr const char* kHasNames[] = {"link",  "embed",   "file", "video",   "image",
                                     "sound", "sticker", "poll", "snapshot"};
constexpr const char* kAuthorTypeNames[] = {"user", "bot", "webhook"};
constexpr const char* kTabNames[] = {"messages", "media", "files", "links", "pins"};

bool printable(const char* text, u32 bytes) noexcept {
    for (u32 index = 0; index < bytes; ++index) {
        const u8 byte = static_cast<u8>(text[index]);
        if (byte < 0x20 || byte == 0x7f)
            return false;
    }
    return utf8_validate({text, bytes}).valid;
}
bool term_valid(const char* text, u32 bytes, u32 capacity, bool hostname, bool extension) noexcept {
    if (bytes > capacity || !printable(text, bytes))
        return false;
    for (u32 index = 0; index < bytes; ++index) {
        const char c = text[index];
        const bool alnum = ascii_is_alnum(c);
        if ((hostname && !alnum && c != '.' && c != '-') || (extension && !alnum))
            return false;
    }
    return true;
}
// The tab's preset applies only when the user did not choose that filter.
u16 effective_has(const MessageSearchQuery& query, MessageSearchTab tab) noexcept {
    if (query.has || query.has_excluded)
        return query.has;
    switch (tab) {
    case MessageSearchTab::media:
        return search_has_image | search_has_video;
    case MessageSearchTab::files:
        return search_has_file;
    case MessageSearchTab::links:
        return search_has_link;
    default:
        return 0;
    }
}
MessageSearchPinned effective_pinned(const MessageSearchQuery& query, MessageSearchTab tab) noexcept {
    return query.pinned == MessageSearchPinned::any && tab == MessageSearchTab::pins
               ? MessageSearchPinned::pinned
               : query.pinned;
}
bool append_terms(ProtocolWriter* writer, const MessageSearchQuery& query) noexcept {
    const struct {
        const char* name;
        const char* value;
        u8 bytes;
    } terms[] = {{"link_hostname", query.link_hostname, query.hostname_bytes},
                 {"attachment_extension", query.attachment_extension, query.extension_bytes},
                 {"attachment_filename", query.attachment_filename, query.filename_bytes}};
    for (const auto& term : terms)
        if (term.bytes &&
            (!append_parameter(writer, term.name) ||
             !protocol_writer_append_percent_encoded(writer, {term.value, term.bytes})))
            return false;
    for (u32 bit = 0; bit < 3; ++bit)
        for (u32 excluded = 0; excluded < 2; ++excluded)
            if (((excluded ? query.author_types_excluded : query.author_types) >> bit) & 1u)
                if (!append_parameter(writer, "author_type") ||
                    (excluded && !protocol_writer_append_byte(writer, '-')) ||
                    !protocol_writer_append_c_string(writer, kAuthorTypeNames[bit]))
                    return false;
    return true;
}
} // namespace

bool message_search_query_valid(const MessageSearchQuery& query) noexcept {
    if (!query.limit || query.limit > kMessageSearchMaximumGroups ||
        query.offset > kMessageSearchMaximumOffset ||
        query.content_bytes > kMessageSearchContentBytes ||
        !printable(query.content, query.content_bytes) ||
        !valid_ids(query.author_ids, query.author_count) ||
        !valid_ids(query.mention_ids, query.mention_count) ||
        !valid_ids(query.channel_ids, query.channel_count) || (query.has & ~0x1ffu) ||
        (query.has_excluded & ~0x1ffu) || (query.has & query.has_excluded) || (query.author_types & ~7u) ||
        (query.author_types_excluded & ~7u) || (query.author_types & query.author_types_excluded) ||
        !term_valid(query.link_hostname, query.hostname_bytes, kMessageSearchHostnameBytes, true,
                    false) ||
        !term_valid(query.attachment_extension, query.extension_bytes, kMessageSearchExtensionBytes,
                    false, true) ||
        !term_valid(query.attachment_filename, query.filename_bytes, kMessageSearchFilenameBytes,
                    false, false) ||
        (query.min_id && query.max_id && query.min_id >= query.max_id) ||
        query.scope > MessageSearchScope::private_channels ||
        (query.scope == MessageSearchScope::private_channels ? query.scope_id != 0
                                                                    : !query.scope_id) ||
        query.sort > MessageSearchSort::relevance ||
        query.pinned > MessageSearchPinned::unpinned || query.tab > MessageSearchTab::pins ||
        query.endpoint > MessageSearchEndpoint::tabs)
        return false;
    bool content = false;
    for (u32 index = 0; index < query.content_bytes; ++index)
        content |= query.content[index] != ' ';
    const bool filtered =
        content || query.author_count || query.mention_count || query.channel_count || query.has ||
        query.has_excluded || query.author_types || query.author_types_excluded || query.hostname_bytes ||
        query.extension_bytes || query.filename_bytes || query.min_id || query.max_id ||
        query.pinned != MessageSearchPinned::any || query.tab != MessageSearchTab::messages;
    return filtered &&
           !(query.scope == MessageSearchScope::channel &&
             (query.channel_count > 1 || (query.channel_count && query.channel_ids[0] != query.scope_id)));
}

MessageSearchWriteResult write_message_search_target(const MessageSearchQuery& query,
                                                     Slice<u8> output) noexcept {
    if (!message_search_query_valid(query) ||
        query.scope == MessageSearchScope::private_channels || (!output.data && output.count))
        return {0, MessageSearchWriteError::invalid_input};
    ProtocolWriter writer{output, 0};
    bool written =
        protocol_writer_append_literal(&writer, "/api/v10/") &&
        protocol_writer_append_c_string(
            &writer, query.scope == MessageSearchScope::guild ? "guilds/" : "channels/") &&
        protocol_writer_append_u64(&writer, query.scope_id) &&
        protocol_writer_append_literal(&writer, "/messages/search?offset=") &&
        protocol_writer_append_u64(&writer, query.offset) && append_parameter(&writer, "limit") &&
        protocol_writer_append_u64(&writer, query.limit) && append_parameter(&writer, "sort_by") &&
        protocol_writer_append_c_string(
            &writer, query.sort == MessageSearchSort::relevance ? "relevance" : "timestamp") &&
        append_parameter(&writer, "sort_order") &&
        protocol_writer_append_c_string(&writer,
                                        query.sort == MessageSearchSort::oldest ? "asc" : "desc");
    if (query.content_bytes) {
        written = written && append_parameter(&writer, "content") &&
                  protocol_writer_append_percent_encoded(&writer, {query.content, query.content_bytes});
    }
    written = written && append_ids(&writer, "author_id", query.author_ids, query.author_count) &&
              append_ids(&writer, "mentions", query.mention_ids, query.mention_count) &&
              append_ids(&writer, "channel_id", query.channel_ids, query.channel_count);
    const u16 has = effective_has(query, query.tab);
    for (u32 bit = 0; bit < 9; ++bit)
        for (u32 excluded = 0; excluded < 2; ++excluded)
            if (((excluded ? query.has_excluded : has) >> bit) & 1u)
                written = written && append_parameter(&writer, "has") &&
                          (!excluded || protocol_writer_append_byte(&writer, '-')) &&
                          protocol_writer_append_c_string(&writer, kHasNames[bit]);
    written = written && append_terms(&writer, query);
    if (query.min_id) {
        written = written && append_parameter(&writer, "min_id") &&
                  protocol_writer_append_u64(&writer, query.min_id);
    }
    if (query.max_id) {
        written = written && append_parameter(&writer, "max_id") &&
                  protocol_writer_append_u64(&writer, query.max_id);
    }
    const auto pinned = effective_pinned(query, query.tab);
    if (pinned != MessageSearchPinned::any) {
        written = written && append_parameter(&writer, "pinned") &&
                  protocol_writer_append_c_string(
                      &writer, pinned == MessageSearchPinned::pinned ? "true" : "false");
    }
    written = written && append_parameter(&writer, "include_nsfw") &&
              protocol_writer_append_c_string(&writer, query.include_nsfw ? "true" : "false");
    return written ? MessageSearchWriteResult{writer.used, MessageSearchWriteError::none}
                   : MessageSearchWriteResult{0, MessageSearchWriteError::output_capacity};
}

namespace {
bool json_key(ProtocolWriter* writer, const char* name, bool* first) noexcept {
    const bool comma = !*first;
    *first = false;
    return (!comma || protocol_writer_append_byte(writer, ',')) &&
           protocol_writer_append_byte(writer, '"') && protocol_writer_append_c_string(writer, name) &&
           protocol_writer_append_literal(writer, "\":");
}
bool json_ids(ProtocolWriter* writer, const char* name, const u64* ids, u8 count, bool* first) noexcept {
    if (!count)
        return true;
    if (!json_key(writer, name, first) || !protocol_writer_append_byte(writer, '['))
        return false;
    for (u8 index = 0; index < count; ++index)
        if ((index && !protocol_writer_append_byte(writer, ',')) ||
            !protocol_writer_append_byte(writer, '"') || !protocol_writer_append_u64(writer, ids[index]) ||
            !protocol_writer_append_byte(writer, '"'))
            return false;
    return protocol_writer_append_byte(writer, ']');
}
bool json_flags(ProtocolWriter* writer, const char* name, u32 included, u32 excluded,
                const char* const* names, u32 count, bool* first) noexcept {
    if (!included && !excluded)
        return true;
    if (!json_key(writer, name, first) || !protocol_writer_append_byte(writer, '['))
        return false;
    bool item = true;
    for (u32 bit = 0; bit < count; ++bit)
        for (u32 negated = 0; negated < 2; ++negated)
            if (((negated ? excluded : included) >> bit) & 1u) {
                if ((!item && !protocol_writer_append_byte(writer, ',')) ||
                    !protocol_writer_append_byte(writer, '"') ||
                    (negated && !protocol_writer_append_byte(writer, '-')) ||
                    !protocol_writer_append_c_string(writer, names[bit]) ||
                    !protocol_writer_append_byte(writer, '"'))
                    return false;
                item = false;
            }
    return protocol_writer_append_byte(writer, ']');
}
bool json_term(ProtocolWriter* writer, const char* name, const char* value, u8 bytes,
               bool* first) noexcept {
    return !bytes || (json_key(writer, name, first) && protocol_writer_append_byte(writer, '[') &&
                      protocol_writer_append_json_string(writer, {value, bytes}) &&
                      protocol_writer_append_byte(writer, ']'));
}
bool json_tab(ProtocolWriter* writer, const MessageSearchQuery& query, MessageSearchTab tab,
              u32 offset, u32 limit) noexcept {
    bool first = true;
    bool written = protocol_writer_append_byte(writer, '"') &&
                   protocol_writer_append_c_string(writer, kTabNames[static_cast<u8>(tab)]) &&
                   protocol_writer_append_literal(writer, "\":{") && json_key(writer, "sort_by", &first) &&
                   protocol_writer_append_c_string(writer, query.sort == MessageSearchSort::relevance
                                                               ? "\"relevance\""
                                                               : "\"timestamp\"") &&
                   json_key(writer, "sort_order", &first) &&
                   protocol_writer_append_c_string(
                       writer, query.sort == MessageSearchSort::oldest ? "\"asc\"" : "\"desc\"");
    if (query.content_bytes)
        written = written && json_key(writer, "content", &first) &&
                  protocol_writer_append_json_string(writer, {query.content, query.content_bytes});
    written =
        written && json_ids(writer, "author_id", query.author_ids, query.author_count, &first) &&
        json_flags(writer, "author_type", query.author_types, query.author_types_excluded, kAuthorTypeNames,
                   3, &first) &&
        json_ids(writer, "mentions", query.mention_ids, query.mention_count, &first) &&
        json_flags(writer, "has", effective_has(query, tab), query.has_excluded, kHasNames, 9, &first) &&
        json_term(writer, "link_hostname", query.link_hostname, query.hostname_bytes, &first) &&
        json_term(writer, "attachment_extension", query.attachment_extension, query.extension_bytes,
                  &first) &&
        json_term(writer, "attachment_filename", query.attachment_filename, query.filename_bytes, &first);
    const auto pinned = effective_pinned(query, tab);
    if (pinned != MessageSearchPinned::any)
        written = written && json_key(writer, "pinned", &first) &&
                  protocol_writer_append_c_string(
                      writer, pinned == MessageSearchPinned::pinned ? "true" : "false");
    const u64 bounds[] = {query.min_id, query.max_id};
    static constexpr const char* bound_names[] = {"min_id", "max_id"};
    for (u32 index = 0; index < 2; ++index)
        if (bounds[index])
            written = written && json_key(writer, bound_names[index], &first) &&
                      protocol_writer_append_byte(writer, '"') &&
                      protocol_writer_append_u64(writer, bounds[index]) &&
                      protocol_writer_append_byte(writer, '"');
    return written && json_key(writer, "offset", &first) && protocol_writer_append_u64(writer, offset) &&
           json_key(writer, "limit", &first) && protocol_writer_append_u64(writer, limit) &&
           protocol_writer_append_byte(writer, '}');
}
} // namespace

MessageSearchTabsWriteResult write_message_search_tabs_request(
    const MessageSearchQuery& query, Slice<u8> target, Slice<u8> body) noexcept {
    if (!message_search_query_valid(query) || (!target.data && target.count) ||
        (!body.data && body.count))
        return {0, 0, MessageSearchWriteError::invalid_input};
    ProtocolWriter path{target, 0};
    bool written = protocol_writer_append_literal(&path, "/api/v10/");
    if (query.scope == MessageSearchScope::private_channels)
        written = written && protocol_writer_append_literal(&path, "users/@me");
    else
        written = written &&
                  protocol_writer_append_c_string(
                      &path, query.scope == MessageSearchScope::guild ? "guilds/" : "channels/") &&
                  protocol_writer_append_u64(&path, query.scope_id);
    written = written && protocol_writer_append_literal(&path, "/messages/search/tabs");
    ProtocolWriter json{body, 0};
    bool first = true;
    written = written && protocol_writer_append_byte(&json, '{') &&
              json_key(&json, "include_nsfw", &first) &&
              protocol_writer_append_c_string(&json, query.include_nsfw ? "true" : "false") &&
              json_ids(&json, "channel_ids", query.channel_ids, query.channel_count, &first) &&
              json_key(&json, "tabs", &first) && protocol_writer_append_byte(&json, '{') &&
              json_tab(&json, query, query.tab, query.offset, query.limit);
    if (query.tab_counts && !query.offset)
        for (u32 tab = 0; tab < kMessageSearchTabCount; ++tab)
            if (tab != static_cast<u8>(query.tab))
                written = written && protocol_writer_append_byte(&json, ',') &&
                          json_tab(&json, query, static_cast<MessageSearchTab>(tab), 0, 1);
    written = written && protocol_writer_append_byte(&json, '}') &&
              protocol_writer_append_literal(&json, ",\"track_exact_total_hits\":true}");
    return written
               ? MessageSearchTabsWriteResult{path.used, json.used,
                                              MessageSearchWriteError::none}
               : MessageSearchTabsWriteResult{0, 0, MessageSearchWriteError::output_capacity};
}

static MessageSearchDecodeResult decode_search_page(
    Utf8View body, const MessageSearchQuery& query, Slice<u32> page_structurals,
    Slice<u32> message_structurals, u32 maximum_depth, Slice<char> text_output,
    Slice<MessageSearchGroup> groups, Slice<MessageSearchMessage> messages,
    const MessageSearchRichScratch* rich_scratch, MessageSearchRichSink sink) noexcept {
    if (!message_search_query_valid(query) || !body.data || !body.bytes || !page_structurals.data ||
        !page_structurals.count || !message_structurals.data || !message_structurals.count ||
        !maximum_depth || (!text_output.data && text_output.count) || (!groups.data && groups.count) ||
        (!messages.data && messages.count)) {
        return {{}, 0, MessageSearchDecodeError::invalid_input};
    }
    const auto scan = json_scan_structurals_avx1(body, page_structurals);
    if (!scan.ok()) {
        return {{},
                scan.error_offset,
                MessageSearchDecodeError::scan_failed,
                GatewayEventDecodeError::none,
                scan.error};
    }
    DecodeContext context{};
    context.document = {body, {page_structurals.data, scan.count}};
    context.query = &query;
    context.message_structurals = message_structurals;
    context.text = text_output;
    context.groups = groups;
    context.messages = messages;
    context.maximum_depth = maximum_depth;
    context.rich_scratch = rich_scratch;
    context.sink = sink;
    auto read =
        json_visit_root_object(body, context.document.structurals, maximum_depth, visit_root, &context);
    if (read.ok() && (context.fields & 16)) {
        // A tabs root never carries page fields beside it.
        if (context.fields & 3)
            context.result.error = MessageSearchDecodeError::duplicate_field;
        else
            read = json_visit_indexed_object(context.document, context.tabs, maximum_depth, visit_tab,
                                             &context);
    }
    if (context.result.error != MessageSearchDecodeError::none) {
    } else if (read.ok() && (context.fields & 3) != 3) {
        context.result.error = MessageSearchDecodeError::missing_field;
    } else if (read.ok()) {
        read = json_visit_indexed_array(context.document, context.message_groups, maximum_depth,
                                        visit_group, &context);
        if (read.ok() && context.result.page.group_count > context.result.page.total_results) {
            context.result.error = MessageSearchDecodeError::invalid_field;
        }
    }
    if (!read.ok() && context.result.error == MessageSearchDecodeError::none) {
        context.result.error = MessageSearchDecodeError::invalid_json;
        context.result.error_offset = read.error_offset;
        context.result.read_error = read.error;
    }
    return context.result;
}

MessageSearchDecodeResult decode_message_search_page(
    Utf8View body, const MessageSearchQuery& query, Slice<u32> page_structurals,
    Slice<u32> message_structurals, u32 maximum_depth, Slice<char> text_output,
    Slice<MessageSearchGroup> groups, Slice<MessageSearchMessage> messages) noexcept {
    return decode_search_page(body, query, page_structurals, message_structurals, maximum_depth,
                              text_output, groups, messages, nullptr, {});
}

MessageSearchDecodeResult decode_message_search_page(
    Utf8View body, const MessageSearchQuery& query, Slice<u32> page_structurals,
    Slice<u32> message_structurals, u32 maximum_depth, Slice<char> per_message_text,
    Slice<MessageSearchGroup> groups, Slice<MessageSearchMessage> messages,
    const MessageSearchRichScratch& scratch, MessageSearchRichSink sink) noexcept {
    const auto& rich = scratch.rich;
    const auto& child = rich.snapshot;
    if (!sink.accept || !per_message_text.data || !per_message_text.count || !scratch.mention_roles.data ||
        scratch.mention_roles.count < kGatewayMessageMentionRoleMaximumIds || !scratch.member_roles.data ||
        scratch.member_roles.count < kGatewayMessageMemberRoleMaximumIds || !scratch.mention_users.data ||
        scratch.mention_users.count < kGatewayMessageMentionUserMaximum || !rich.reactions.data ||
        rich.reactions.count < kGatewayMessageReactionMaximum || !rich.attachments.data ||
        rich.attachments.count < kGatewayMessageAttachmentMaximum || !rich.embeds.data ||
        rich.embeds.count < kGatewayMessageEmbedMaximum || !rich.embed_fields.data ||
        rich.embed_fields.count < kGatewayMessageEmbedFieldMaximum || !rich.stickers.data ||
        rich.stickers.count < kGatewayMessageStickerMaximum || !rich.components.data ||
        rich.components.count < kGatewayMessageComponentMaximum || !child.attachments.data ||
        child.attachments.count < kGatewayMessageAttachmentMaximum || !child.embeds.data ||
        child.embeds.count < kGatewayMessageEmbedMaximum || !child.embed_fields.data ||
        child.embed_fields.count < kGatewayMessageEmbedFieldMaximum || !child.stickers.data ||
        child.stickers.count < kGatewayMessageStickerMaximum || !child.components.data ||
        child.components.count < kGatewayMessageComponentMaximum || !child.mention_users.data ||
        child.mention_users.count < kGatewayMessageMentionUserMaximum ||
        (!rich.poll.answers.data && rich.poll.answers.count) ||
        (!rich.poll.answer_counts.data && rich.poll.answer_counts.count) ||
        !disjoint(scratch.mention_roles, scratch.member_roles) ||
        !disjoint(rich.attachments, child.attachments) || !disjoint(rich.embeds, child.embeds) ||
        !disjoint(rich.embed_fields, child.embed_fields) || !disjoint(rich.stickers, child.stickers) ||
        !disjoint(rich.components, child.components) ||
        !disjoint(scratch.mention_users, child.mention_users))
        return {{}, 0, MessageSearchDecodeError::invalid_input};
    return decode_search_page(body, query, page_structurals, message_structurals, maximum_depth,
                              per_message_text, groups, messages, &scratch, sink);
}

} // namespace maicord
