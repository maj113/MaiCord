#include "maicord/protocol/messages/search.hpp"
#include "maicord/protocol/gateway/message_events.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "../encoding/json_field_lists.hpp"
#include "../encoding/json_helpers.hpp"

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
    return protocol_write(writer, '&', name, '=');
}

bool append_ids(ProtocolWriter* writer, const char* name, const u64* ids, u8 count) noexcept {
    for (u8 index = 0; index < count; ++index) {
        if (!append_parameter(writer, name) || !protocol_write(writer, ids[index])) {
            return false;
        }
    }
    return true;
}

struct DecodeContext {
    JsonIndexedDocument document;
    const MessageSearchQuery* query;
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
};

bool visit_hit(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* hit = static_cast<HitContext*>(opaque);
    switch (protocol_detail::member_index<"guild_id", "hit">(name)) {
    case 0:
        if (hit->guild_seen || (value.kind != JsonValueKind::null_value &&
                                !json_value_to_snowflake(value, &hit->guild_id))) {
            hit->valid = false;
            return false;
        }
        hit->guild_seen = true;
        break;
    case 1:
        if (hit->seen || !json_value_to_bool(value, &hit->hit)) {
            hit->valid = false;
            return false;
        }
        hit->seen = true;
        break;
    default: break;
    }
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
    const auto& scratch = *context->rich_scratch;
    const auto decoded =
        gateway_decode_message_create(context->document, value, context->maximum_depth, context->text,
                                      scratch.mention_roles, scratch.member_roles, scratch.mention_users,
                                      scratch.rich, 0, &message);
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
    item.guild_id = hit.guild_id;
    item.hit = hit.hit;
    item.guild_known = hit.guild_seen;
    // The sink consumes the reusable named-mention and media pools before the next message is decoded.
    if (!context->sink.accept(context->sink.context, page.message_count, item))
        return reject(context, MessageSearchDecodeError::sink_rejected, value.source_offset);
    clear_borrowed_views(item);
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
    const u32 field = protocol_detail::member_bit<"messages", "total_results", "doing_deep_historical_index",
                                                  "documents_indexed", "tabs">(name);
    if (!field) {
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
    const u32 field = protocol_detail::member_bit<"messages", "total_results">(name);
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
    const u32 index = protocol_detail::member_index<"messages", "media", "files", "links", "pins">(name);
    static_assert(kMessageSearchTabCount == 5);
    if (index < kMessageSearchTabCount) {
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

constexpr char kTabNames[] = "messages\0media\0files\0links\0pins\0";

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
             !protocol_write(writer, percent_text({term.value, term.bytes}))))
            return false;
    u32 bit = 0;
    for (NameList list{kMessageSearchAuthorTypeNames}; list; list.next(), ++bit)
        for (u32 excluded = 0; excluded < 2; ++excluded)
            if (((excluded ? query.author_types_excluded : query.author_types) >> bit) & 1u)
                if (!append_parameter(writer, "author_type") || (excluded && !protocol_write(writer, '-')) ||
                    !protocol_write(writer, list.name()))
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
    if (query.scope == MessageSearchScope::private_channels)
        return {0, MessageSearchWriteError::invalid_input};
    ProtocolWriter writer{output, 0};
    bool written =
        protocol_write(&writer, "/api/v9/", query.scope == MessageSearchScope::guild ? "guilds/" : "channels/",
                       query.scope_id, "/messages/search?offset=", query.offset) &&
        append_parameter(&writer, "limit") && protocol_write(&writer, query.limit) &&
        append_parameter(&writer, "sort_by") &&
        protocol_write(&writer, query.sort == MessageSearchSort::relevance ? "relevance" : "timestamp") &&
        append_parameter(&writer, "sort_order") &&
        protocol_write(&writer, query.sort == MessageSearchSort::oldest ? "asc" : "desc");
    if (query.content_bytes) {
        written = written && append_parameter(&writer, "content") &&
                  protocol_write(&writer, percent_text({query.content, query.content_bytes}));
    }
    written = written && append_ids(&writer, "author_id", query.author_ids, query.author_count) &&
              append_ids(&writer, "mentions", query.mention_ids, query.mention_count) &&
              append_ids(&writer, "channel_id", query.channel_ids, query.channel_count);
    const u16 has = effective_has(query, query.tab);
    u32 bit = 0;
    for (NameList list{kMessageSearchHasNames}; list; list.next(), ++bit)
        for (u32 excluded = 0; excluded < 2; ++excluded)
            if (((excluded ? query.has_excluded : has) >> bit) & 1u)
                written = written && append_parameter(&writer, "has") &&
                          (!excluded || protocol_write(&writer, '-')) && protocol_write(&writer, list.name());
    written = written && append_terms(&writer, query);
    if (query.min_id) {
        written = written && append_parameter(&writer, "min_id") && protocol_write(&writer, query.min_id);
    }
    if (query.max_id) {
        written = written && append_parameter(&writer, "max_id") && protocol_write(&writer, query.max_id);
    }
    const auto pinned = effective_pinned(query, query.tab);
    if (pinned != MessageSearchPinned::any) {
        written = written && append_parameter(&writer, "pinned") &&
                  protocol_write(&writer, pinned == MessageSearchPinned::pinned ? "true" : "false");
    }
    written = written && append_parameter(&writer, "include_nsfw") &&
              protocol_write(&writer, query.include_nsfw ? "true" : "false");
    return written ? MessageSearchWriteResult{writer.used, MessageSearchWriteError::none}
                   : MessageSearchWriteResult{0, MessageSearchWriteError::output_capacity};
}

namespace {
using protocol_detail::JsonBody;
void json_ids(JsonBody& b, const char* name, const u64* ids, u8 count) noexcept {
    if (!count)
        return;
    b.begin_array(name);
    for (u8 index = 0; index < count; ++index)
        b.element(snowflake_text(ids[index]));
    b.end_array();
}
void json_flags(JsonBody& b, const char* name, u32 included, u32 excluded, const char* names) noexcept {
    if (!included && !excluded)
        return;
    b.begin_array(name);
    u32 bit = 0;
    for (NameList list{names}; list; list.next(), ++bit)
        for (u32 negated = 0; negated < 2; ++negated)
            if (((negated ? excluded : included) >> bit) & 1u) {
                b.next();
                b.put('"');
                if (negated)
                    b.put('-');
                b.put(list.name(), '"');
            }
    b.end_array();
}
void json_term(JsonBody& b, const char* name, const char* value, u8 bytes) noexcept {
    if (!bytes)
        return;
    b.begin_array(name);
    b.element(json_text({value, bytes}));
    b.end_array();
}
void json_tab(JsonBody& b, const MessageSearchQuery& query, MessageSearchTab tab, u32 offset, u32 limit) noexcept {
    b.begin_object(name_list_at(kTabNames, static_cast<u8>(tab)).data);
    b.member("sort_by", query.sort == MessageSearchSort::relevance ? "\"relevance\"" : "\"timestamp\"");
    b.member("sort_order", query.sort == MessageSearchSort::oldest ? "\"asc\"" : "\"desc\"");
    b.member_if(query.content_bytes, "content", json_text({query.content, query.content_bytes}));
    json_ids(b, "author_id", query.author_ids, query.author_count);
    json_flags(b, "author_type", query.author_types, query.author_types_excluded, kMessageSearchAuthorTypeNames);
    json_ids(b, "mentions", query.mention_ids, query.mention_count);
    json_flags(b, "has", effective_has(query, tab), query.has_excluded, kMessageSearchHasNames);
    json_term(b, "link_hostname", query.link_hostname, query.hostname_bytes);
    json_term(b, "attachment_extension", query.attachment_extension, query.extension_bytes);
    json_term(b, "attachment_filename", query.attachment_filename, query.filename_bytes);
    const auto pinned = effective_pinned(query, tab);
    b.member_if(pinned != MessageSearchPinned::any, "pinned", json_bool(pinned == MessageSearchPinned::pinned));
    b.member_if(query.min_id, "min_id", snowflake_text(query.min_id));
    b.member_if(query.max_id, "max_id", snowflake_text(query.max_id));
    b.member("offset", offset);
    b.member("limit", limit);
    b.end_object();
}
} // namespace

MessageSearchTabsWriteResult write_message_search_tabs_request(
    const MessageSearchQuery& query, Slice<u8> target, Slice<u8> body) noexcept {
    ProtocolWriter path{target, 0};
    bool written = protocol_write(&path, "/api/v9/");
    if (query.scope == MessageSearchScope::private_channels)
        written = written && protocol_write(&path, "users/@me");
    else
        written = written &&
                  protocol_write(&path, query.scope == MessageSearchScope::guild ? "guilds/" : "channels/",
                                 query.scope_id);
    written = written && protocol_write(&path, "/messages/search/tabs");
    JsonBody json{body};
    json.begin_object();
    json.member("include_nsfw", json_bool(query.include_nsfw));
    json_ids(json, "channel_ids", query.channel_ids, query.channel_count);
    json.begin_object("tabs");
    json_tab(json, query, query.tab, query.offset, query.limit);
    if (query.tab_counts && !query.offset)
        for (u32 tab = 0; tab < kMessageSearchTabCount; ++tab)
            if (tab != static_cast<u8>(query.tab))
                json_tab(json, query, static_cast<MessageSearchTab>(tab), 0, 1);
    json.end_object();
    json.put_text(",\"track_exact_total_hits\":true}");
    return written && json.ok
               ? MessageSearchTabsWriteResult{path.used, json.writer.used, MessageSearchWriteError::none}
               : MessageSearchTabsWriteResult{0, 0, MessageSearchWriteError::output_capacity};
}

MessageSearchDecodeResult decode_message_search_page(
    Utf8View body, const MessageSearchQuery& query, Slice<u32> page_structurals, u32 maximum_depth,
    Slice<char> text_output, Slice<MessageSearchGroup> groups, Slice<MessageSearchMessage> messages,
    const MessageSearchRichScratch& rich_scratch, MessageSearchRichSink sink) noexcept {
    // The one caller (application_search_response) passes the controller's query, which message_search_begin
    // checked and message_search_page and message_search_fallback keep valid, a body that is not empty, and its own
    // index, text and result banks.
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
    context.text = text_output;
    context.groups = groups;
    context.messages = messages;
    context.maximum_depth = maximum_depth;
    context.rich_scratch = &rich_scratch;
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

} // namespace maicord
