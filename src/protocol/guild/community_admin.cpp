#include "maicord/protocol/guild/community_admin.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "../encoding/json_helpers.hpp"
#include "../encoding/json_field_lists.hpp"

namespace maicord {
namespace {
using protocol_detail::JsonBody;
using protocol_detail::member_index;
using protocol_detail::name_text_valid;
using protocol_detail::view_valid;
constexpr u32 kDepth = 32;

// The indexed document of a reply; template replies carry whole servers.
bool scan(Utf8View body, Slice<u32> scratch, JsonIndexedDocument* document) noexcept {
    if (!body.data || !body.bytes || body.bytes > kGuildTemplateMaximumResponseBytes || !scratch.data ||
        !scratch.count)
        return false;
    const auto scanned = json_scan_structurals_avx1(body, scratch);
    if (!scanned.ok())
        return false;
    *document = {body, {scratch.data, scanned.count}, 0};
    return true;
}

// Single-line text of 1..maximum code points that is not blank.
bool line_valid(Utf8View text, u32 maximum) noexcept {
    return name_text_valid(text, maximum * 4, maximum, true);
}
// Optional free text of at most maximum code points.
bool text_within(Utf8View text, u32 maximum) noexcept {
    if (!text.bytes)
        return !text.data || view_valid(text);
    const auto checked = utf8_validate(text);
    return view_valid(text) && checked.valid && checked.codepoints <= maximum;
}

const char* field_type_name(GuildVerificationFieldType type) noexcept {
    switch (type) {
    case GuildVerificationFieldType::terms: return "\"TERMS\"";
    case GuildVerificationFieldType::text_input: return "\"TEXT_INPUT\"";
    case GuildVerificationFieldType::paragraph: return "\"PARAGRAPH\"";
    case GuildVerificationFieldType::multiple_choice: return "\"MULTIPLE_CHOICE\"";
    default: return nullptr;
    }
}

template <typename Text> bool copy_string(JsonValue value, char* output, u32 capacity, Text* bytes) noexcept {
    const u32 copied = protocol_detail::copy_text_truncated<4096>(value, output, capacity);
    *bytes = static_cast<Text>(copied);
    return value.kind == JsonValueKind::string;
}

// Templates

// The snapshot's channels as read, for the preview's category grouping.
constexpr u32 kSnapshotChannels = 96;
struct SnapshotChannel {
    u64 id, parent;
    u16 name_offset;
    u8 name_bytes, type;
};
struct SnapshotPass {
    const JsonIndexedDocument* document;
    GuildTemplate* output;
    SnapshotChannel channels[kSnapshotChannels];
    char names[4096];
    u32 channel_count, names_used;
    SnapshotChannel row;
};
// One preview line, whole or not at all.
void preview_line(GuildTemplate& t, Utf8View prefix, Utf8View text, Utf8View suffix) noexcept {
    const u32 bytes = prefix.bytes + text.bytes + suffix.bytes + 1;
    if (bytes > sizeof(t.preview) - t.preview_bytes)
        return;
    char* at = t.preview + t.preview_bytes;
    copy_bytes(at, prefix.data, prefix.bytes);
    copy_bytes(at + prefix.bytes, text.data, text.bytes);
    copy_bytes(at + prefix.bytes + text.bytes, suffix.data, suffix.bytes);
    at[bytes - 1] = '\n';
    t.preview_bytes = static_cast<u16>(t.preview_bytes + bytes);
}
bool snapshot_channel_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<SnapshotPass*>(opaque);
    u64 number = 0;
    switch (member_index<"id", "type", "name", "parent_id">(name)) {
    case 0: json_value_to_u64_or_string(value, &p.row.id); break;
    case 1:
        if (json_value_to_u64(value, &number) && number < 256)
            p.row.type = static_cast<u8>(number);
        break;
    case 2: {
        u8 bytes = 0;
        const u32 room = sizeof(p.names) - p.names_used;
        copy_string(value, p.names + p.names_used, room < 200 ? room : 200, &bytes);
        p.row.name_offset = static_cast<u16>(p.names_used);
        p.row.name_bytes = bytes;
        p.names_used += bytes;
        break;
    }
    case 3: json_value_to_u64_or_string(value, &p.row.parent); break;
    default: break;
    }
    return true;
}
bool snapshot_channel(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<SnapshotPass*>(opaque);
    p.row = {};
    if (value.kind == JsonValueKind::object &&
        json_visit_indexed_object(*p.document, value, kDepth, snapshot_channel_member, &p).ok() &&
        p.channel_count < kSnapshotChannels)
        p.channels[p.channel_count++] = p.row;
    return true;
}
bool snapshot_role(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<SnapshotPass*>(opaque);
    if (value.kind != JsonValueKind::object)
        return true;
    struct Role {
        char name[200];
        u8 bytes;
    } role{};
    json_visit_indexed_object(*p.document, value, kDepth,
                              +[](void* context, JsonString key, JsonValue field) noexcept {
                                  auto& r = *static_cast<Role*>(context);
                                  if (protocol_detail::name_is(key, "name"))
                                      copy_string(field, r.name, sizeof(r.name), &r.bytes);
                                  return true;
                              },
                              &role);
    ++p.output->role_count;
    const bool at = role.bytes && role.name[0] == '@';
    preview_line(*p.output, at ? Utf8View{} : utf8_literal("@"), {role.name, role.bytes}, {});
    return true;
}
bool snapshot_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<SnapshotPass*>(opaque);
    if (value.kind == JsonValueKind::array && protocol_detail::name_is(name, "channels"))
        json_visit_indexed_array(*p.document, value, kDepth, snapshot_channel, &p);
    return true;
}
bool snapshot_roles(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<SnapshotPass*>(opaque);
    if (value.kind == JsonValueKind::array && protocol_detail::name_is(name, "roles"))
        json_visit_indexed_array(*p.document, value, kDepth, snapshot_role, &p);
    return true;
}
// The preview: uncategorized channels, each category with its channels, then the roles.
void decode_snapshot(const JsonIndexedDocument& document, JsonValue snapshot, GuildTemplate& t) noexcept {
    static SnapshotPass p;
    reset_value(&p);
    p.document = &document;
    p.output = &t;
    json_visit_indexed_object(document, snapshot, kDepth, snapshot_member, &p);
    t.channel_count = static_cast<u16>(p.channel_count);
    const auto channel = [&](const SnapshotChannel& c, bool nested) noexcept {
        const bool voice = c.type == 2 || c.type == 13;
        const auto indent = nested ? utf8_literal("    ") : Utf8View{};
        char prefix[8];
        copy_bytes(prefix, indent.data, indent.bytes);
        const u32 prefix_bytes = indent.bytes + (voice ? 0 : 2);
        if (!voice)
            copy_bytes(prefix + indent.bytes, "# ", 2);
        preview_line(t, {prefix, prefix_bytes}, {p.names + c.name_offset, c.name_bytes},
                     voice ? utf8_literal(" (voice)") : Utf8View{});
    };
    for (u32 i = 0; i < p.channel_count; ++i)
        if (!p.channels[i].parent && p.channels[i].type != 4)
            channel(p.channels[i], false);
    for (u32 i = 0; i < p.channel_count; ++i) {
        if (p.channels[i].type != 4)
            continue;
        preview_line(t, {}, {p.names + p.channels[i].name_offset, p.channels[i].name_bytes}, {});
        for (u32 j = 0; j < p.channel_count; ++j)
            if (p.channels[j].parent == p.channels[i].id && p.channels[j].type != 4)
                channel(p.channels[j], true);
    }
    json_visit_indexed_object(document, snapshot, kDepth, snapshot_roles, &p);
}

struct TemplatePass {
    const JsonIndexedDocument* document;
    GuildTemplate* output;
    bool code, name;
};
bool template_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<TemplatePass*>(opaque);
    auto& t = *p.output;
    u64 number = 0;
    switch (member_index<"code", "name", "description", "usage_count", "created_at", "updated_at", "is_dirty",
                         "serialized_source_guild">(name)) {
    case 0: {
        JsonString code{};
        const auto decoded = json_value_as_string(value, &code)
                                 ? json_string_decode_utf8(code, {t.code, sizeof(t.code)})
                                 : JsonStringDecodeResult{};
        p.code = decoded.ok() && guild_template_code_valid({t.code, decoded.bytes});
        t.code_bytes = p.code ? static_cast<u8>(decoded.bytes) : 0;
        break;
    }
    case 1: p.name = copy_string(value, t.name, sizeof(t.name), &t.name_bytes); break;
    case 2: copy_string(value, t.description, sizeof(t.description), &t.description_bytes); break;
    case 3:
        if (json_value_to_u64(value, &number))
            t.usage_count = number > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(number);
        break;
    case 4: json_value_to_timestamp(value, &t.created_at_ms); break;
    case 5: json_value_to_timestamp(value, &t.updated_at_ms); break;
    case 6: json_value_to_bool(value, &t.dirty); break;
    case 7:
        if (value.kind == JsonValueKind::object)
            decode_snapshot(*p.document, value, t);
        break;
    default: break;
    }
    return true;
}
// One template object: the root of `document` when `object` is null.
bool decode_template(const JsonIndexedDocument& document, const JsonValue* object, GuildTemplate* output) noexcept {
    static GuildTemplate decoded;
    reset_value(&decoded);
    TemplatePass pass{&document, &decoded, false, false};
    const auto read = object ? json_visit_indexed_object(document, *object, kDepth, template_member, &pass)
                             : json_visit_root_object(document.input, document.structurals, kDepth,
                                                      template_member, &pass);
    if (!read.ok() || !pass.code || !pass.name)
        return false;
    copy_value(output, decoded);
    return true;
}

// Discovery

struct RequirementsPass {
    GuildDiscoveryRequirements* output;
    bool sufficient;
};
bool requirements_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<RequirementsPass*>(opaque);
    auto& r = *p.output;
    u64 number = 0;
    switch (member_index<"sufficient", "sufficient_without_grace_period", "safe_environment", "size", "age",
                         "healthy", "health_score_pending", "protected", "minimum_size", "minimum_age",
                         "nsfw_properties", "grace_period_end_date">(name)) {
    case 0: p.sufficient = json_value_to_bool(value, &r.sufficient); break;
    case 1: json_value_to_bool(value, &r.sufficient_without_grace_period); break;
    case 2:
        r.checklist = true;
        json_value_to_bool(value, &r.safe_environment);
        break;
    case 3: json_value_to_bool(value, &r.size); break;
    case 4: json_value_to_bool(value, &r.age); break;
    case 5: json_value_to_bool(value, &r.healthy); break;
    case 6: json_value_to_bool(value, &r.health_score_pending); break;
    case 7: json_value_to_bool(value, &r.protected_moderation); break;
    case 8:
        if (json_value_to_u64(value, &number) && number <= 0xFFFFFFFFull)
            r.minimum_size = static_cast<u32>(number);
        break;
    case 9:
        if (json_value_to_u64(value, &number) && number <= 0xFFFFFFFFull)
            r.minimum_age_days = static_cast<u32>(number);
        break;
    case 10:
        // An object naming what was found; an empty one means nothing was.
        if (value.kind == JsonValueKind::object)
            for (u32 i = 0; i < value.source.bytes && !r.bad_content; ++i)
                r.bad_content = value.source.data[i] == ':';
        break;
    case 11: json_value_to_timestamp(value, &r.grace_period_end_ms); break;
    default: break;
    }
    return true;
}

struct MetadataPass {
    const JsonIndexedDocument* document;
    GuildDiscoveryMetadata* output;
    bool keywords;
};
bool keyword_element(void* opaque, u32, JsonValue value) noexcept {
    auto& m = *static_cast<GuildDiscoveryMetadata*>(opaque);
    if (m.keyword_count >= kGuildDiscoveryKeywordMaximum)
        return true;
    u8 bytes = 0;
    if (copy_string(value, m.keywords[m.keyword_count], kGuildDiscoveryKeywordBytes, &bytes) && bytes)
        m.keyword_bytes[m.keyword_count++] = bytes;
    return true;
}
bool metadata_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<MetadataPass*>(opaque);
    auto& m = *p.output;
    u64 number = 0;
    switch (member_index<"primary_category_id", "keywords", "emoji_discoverability_enabled", "category_ids">(name)) {
    case 0:
        if (json_value_to_u64(value, &number) && number <= 0xFFFFFFFFull)
            m.primary_category_id = static_cast<u32>(number);
        break;
    case 1:
        if (value.kind == JsonValueKind::array)
            p.keywords = json_visit_indexed_array(*p.document, value, kDepth, keyword_element, &m).ok();
        else
            p.keywords = value.kind == JsonValueKind::null_value;
        break;
    case 2: json_value_to_bool(value, &m.emoji_discoverability_enabled); break;
    case 3:
        if (value.kind == JsonValueKind::array)
            json_visit_indexed_array(*p.document, value, kDepth,
                                     +[](void* context, u32, JsonValue id) noexcept {
                                         auto& meta = *static_cast<GuildDiscoveryMetadata*>(context);
                                         u64 n = 0;
                                         if (meta.subcategory_count < kGuildDiscoverySubcategoryMaximum &&
                                             json_value_to_u64(id, &n) && n <= 0xFFFFFFFFull)
                                             meta.subcategories[meta.subcategory_count++] = static_cast<u32>(n);
                                         return true;
                                     },
                                     &m);
        break;
    default: break;
    }
    return true;
}

// The read keys the PATCH repeats, in the official order around the edited ones.
constexpr u32 kEchoCount = 6;
struct EchoPass {
    JsonValue values[kEchoCount];
    bool seen[kEchoCount];
};
bool echo_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<EchoPass*>(opaque);
    const u32 index = member_index<"partner_actioned_timestamp", "partner_application_timestamp", "is_published",
                                   "reasons_to_join", "social_links", "about">(name);
    if (index < kEchoCount && !p.seen[index]) {
        p.values[index] = value;
        p.seen[index] = true;
    }
    return true;
}

struct CategoryPass {
    const JsonIndexedDocument* document;
    Slice<GuildDiscoveryCategory> output;
    u32 count;
    GuildDiscoveryCategory row;
    bool id;
};
bool category_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<CategoryPass*>(opaque);
    u64 number = 0;
    switch (member_index<"id", "name", "is_primary">(name)) {
    case 0:
        p.id = json_value_to_u64(value, &number) && number <= 0xFFFFFFFFull;
        p.row.id = static_cast<u32>(number);
        break;
    case 1: copy_string(value, p.row.name, sizeof(p.row.name), &p.row.name_bytes); break;
    case 2: json_value_to_bool(value, &p.row.primary); break;
    default: break;
    }
    return true;
}
bool category_element(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<CategoryPass*>(opaque);
    reset_value(&p.row);
    p.id = false;
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(*p.document, value, kDepth, category_member, &p).ok())
        return false;
    if (p.id && p.row.name_bytes && p.count < p.output.count)
        copy_value(&p.output[p.count++], p.row);
    return true;
}

} // namespace

// Membership screening

bool guild_verification_fields_valid(ConstSlice<GuildVerificationFieldWrite> fields) noexcept {
    if (fields.count > kGuildVerificationEditFieldMaximum || (fields.count && !fields.data))
        return false;
    u32 terms = 0;
    for (u32 i = 0; i < fields.count; ++i) {
        const auto& f = fields[i];
        if (!field_type_name(f.type) || !line_valid(f.label, kGuildVerificationLabelCodepoints) ||
            !text_within(f.description, kGuildVerificationLabelCodepoints) ||
            !text_within(f.placeholder, kGuildVerificationLabelCodepoints) ||
            (f.values.count && !f.values.data))
            return false;
        u32 maximum_values = 0, codepoints = kGuildVerificationLabelCodepoints;
        if (f.type == GuildVerificationFieldType::terms) {
            ++terms;
            maximum_values = kGuildVerificationRuleMaximum;
        } else if (f.type == GuildVerificationFieldType::multiple_choice) {
            maximum_values = kGuildVerificationChoiceMaximum;
            codepoints = kGuildVerificationChoiceCodepoints;
        }
        if (maximum_values ? !f.values.count || f.values.count > maximum_values : f.values.count != 0)
            return false;
        for (u32 v = 0; v < f.values.count; ++v)
            if (!line_valid(f.values[v], codepoints))
                return false;
    }
    return terms <= 1;
}

u32 guild_verification_write_update(ConstSlice<GuildVerificationFieldWrite> fields, bool enabled,
                                    GuildVerificationBulkAction bulk_action, Slice<u8> output) noexcept {
    if (!output.data || !guild_verification_fields_valid(fields) ||
        bulk_action > GuildVerificationBulkAction::rejected)
        return 0;
    JsonBody b(output);
    b.begin_object();
    b.begin_array("form_fields");
    for (u32 i = 0; i < fields.count; ++i) {
        const auto& f = fields[i];
        b.next();
        b.begin_object();
        b.member("field_type", field_type_name(f.type));
        b.member("label", json_text(f.label));
        if (f.values.count) {
            b.begin_array(f.type == GuildVerificationFieldType::terms ? "values" : "choices");
            for (u32 v = 0; v < f.values.count; ++v)
                b.element(json_text(f.values[v]));
            b.end_array();
        }
        b.member("required", json_bool(f.required));
        b.member_if(f.description.bytes != 0, "description", json_text(f.description));
        b.member_if(f.placeholder.bytes != 0, "placeholder", json_text(f.placeholder));
        b.end_object();
    }
    b.end_array();
    b.member("enabled", json_bool(enabled));
    b.member_if(bulk_action != GuildVerificationBulkAction::none, "bulk_action",
                bulk_action == GuildVerificationBulkAction::approved ? "\"APPROVED\"" : "\"REJECTED\"");
    b.end_object();
    return b.finish();
}

// Server templates

bool guild_template_code_valid(Utf8View code) noexcept {
    if (!code.data || !code.bytes || code.bytes > kGuildTemplateCodeBytes)
        return false;
    for (u32 i = 0; i < code.bytes; ++i)
        if (!ascii_is_alnum(code.data[i]))
            return false;
    return true;
}

bool guild_template_title_valid(Utf8View name) noexcept {
    if (!text_within(name, kGuildTemplateNameCodepoints))
        return false;
    u32 first = 0, last = name.bytes;
    while (first < last && ascii_is_space(name.data[first]))
        ++first;
    while (last > first && ascii_is_space(name.data[last - 1]))
        --last;
    return utf8_validate({name.data + first, last - first}).codepoints >= 2;
}

bool guild_template_description_valid(Utf8View description) noexcept {
    return text_within(description, kGuildTemplateDescriptionCodepoints);
}

u32 guild_template_write(Utf8View name, Utf8View description, Slice<u8> output) noexcept {
    if (!output.data || !guild_template_title_valid(name) || !guild_template_description_valid(description))
        return 0;
    ProtocolWriter w{output, 0};
    return protocol_write(&w, "{\"name\":", json_text(name), ",\"description\":", json_text(description), "}")
               ? w.used
               : 0;
}

bool guild_template_decode(Utf8View body, Slice<u32> scratch, GuildTemplate* output) noexcept {
    JsonIndexedDocument document{};
    if (!output || !scan(body, scratch, &document))
        return false;
    return decode_template(document, nullptr, output);
}

u32 guild_template_decode_list(Utf8View body, Slice<u32> scratch, GuildTemplate* first) noexcept {
    JsonIndexedDocument document{};
    if (!first || !scan(body, scratch, &document))
        return ~0u;
    struct Pass {
        const JsonIndexedDocument* document;
        GuildTemplate* first;
        u32 count;
    } pass{&document, first, 0};
    const bool read = json_visit_root_array(body, document.structurals, kDepth,
                                            +[](void* opaque, u32, JsonValue value) noexcept {
                                                auto& p = *static_cast<Pass*>(opaque);
                                                if (!p.count && (value.kind != JsonValueKind::object ||
                                                                 !decode_template(*p.document, &value, p.first)))
                                                    return false;
                                                ++p.count;
                                                return true;
                                            },
                                            &pass)
                          .ok();
    return read ? pass.count : ~0u;
}

u32 guild_template_write_link(Utf8View code, Slice<char> output) noexcept {
    if (!output.data || !guild_template_code_valid(code))
        return 0;
    ProtocolWriter w{{reinterpret_cast<u8*>(output.data), output.count}, 0};
    return protocol_write(&w, "https://discord.new/", code) ? w.used : 0;
}

// Server Discovery

u32 guild_join_requests_decode_total(Utf8View body, Slice<u32> scratch) noexcept {
    JsonIndexedDocument document{};
    if (!scan(body, scratch, &document))
        return ~0u;
    u64 total = ~0ull;
    const auto member = [](void* opaque, JsonString name, JsonValue value) noexcept {
        if (json_string_equals_ascii(name, utf8_literal("total")) && !json_value_to_u64(value, static_cast<u64*>(opaque)))
            return false;
        return true;
    };
    if (!json_visit_root_object(body, document.structurals, kDepth, member, &total).ok() || total >= ~0u)
        return ~0u;
    return static_cast<u32>(total);
}

bool guild_discovery_requirements_decode(Utf8View body, Slice<u32> scratch,
                                         GuildDiscoveryRequirements* output) noexcept {
    JsonIndexedDocument document{};
    if (!output || !scan(body, scratch, &document))
        return false;
    GuildDiscoveryRequirements decoded{};
    RequirementsPass pass{&decoded, false};
    if (!json_visit_root_object(body, document.structurals, kDepth, requirements_member, &pass).ok() ||
        !pass.sufficient)
        return false;
    copy_value(output, decoded);
    return true;
}

bool guild_discovery_metadata_decode(Utf8View body, Slice<u32> scratch, GuildDiscoveryMetadata* output) noexcept {
    JsonIndexedDocument document{};
    if (!output || body.bytes > kGuildDiscoveryMetadataMaximumBytes || !scan(body, scratch, &document))
        return false;
    GuildDiscoveryMetadata decoded{};
    MetadataPass pass{&document, &decoded, true};
    if (!json_visit_root_object(body, document.structurals, kDepth, metadata_member, &pass).ok() || !pass.keywords)
        return false;
    copy_value(output, decoded);
    return true;
}

u32 guild_discovery_metadata_write(Utf8View read_body, Slice<u32> scratch, u32 primary_category_id,
                                   ConstSlice<Utf8View> keywords, bool emoji_discoverability_enabled,
                                   Slice<u8> output) noexcept {
    JsonIndexedDocument document{};
    if (!output.data || keywords.count > kGuildDiscoveryKeywordMaximum || (keywords.count && !keywords.data) ||
        read_body.bytes > kGuildDiscoveryMetadataMaximumBytes || !scan(read_body, scratch, &document))
        return 0;
    for (u32 i = 0; i < keywords.count; ++i)
        if (!line_valid(keywords[i], kGuildDiscoveryKeywordCodepoints))
            return 0;
    EchoPass read{};
    if (!json_visit_root_object(read_body, document.structurals, kDepth, echo_member, &read).ok())
        return 0;
    JsonBody b(output);
    const auto echo = [&](u32 index, const char* name) noexcept {
        b.member_if(read.seen[index], name, read.values[index].source);
    };
    b.begin_object();
    b.member("primary_category_id", primary_category_id);
    b.member("emoji_discoverability_enabled", json_bool(emoji_discoverability_enabled));
    echo(0, "partner_actioned_timestamp");
    echo(1, "partner_application_timestamp");
    b.begin_array("keywords");
    for (u32 i = 0; i < keywords.count; ++i)
        b.element(json_text(keywords[i]));
    b.end_array();
    echo(2, "is_published");
    echo(3, "reasons_to_join");
    echo(4, "social_links");
    echo(5, "about");
    b.end_object();
    return b.finish();
}

u32 guild_discovery_decode_categories(Utf8View body, Slice<u32> scratch,
                                      Slice<GuildDiscoveryCategory> output) noexcept {
    JsonIndexedDocument document{};
    if (!output.data || !scan(body, scratch, &document))
        return ~0u;
    CategoryPass pass{&document, output, 0, {}, false};
    return json_visit_root_array(body, document.structurals, kDepth, category_element, &pass).ok() ? pass.count
                                                                                                   : ~0u;
}

u32 guild_visibility_write(u8 visibility, Slice<u8> output) noexcept {
    if (!output.data || (visibility != 1 && visibility != 3))
        return 0;
    ProtocolWriter w{output, 0};
    return protocol_write(&w, "{\"visibility\":", visibility, "}") ? w.used : 0;
}

} // namespace maicord
