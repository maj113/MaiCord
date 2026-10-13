#include "maicord/protocol/guild_moderation.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/writer.hpp"

#include <Windows.h>

namespace maicord {
namespace {

using Op = GuildModerationOperation;
constexpr u32 kDepth = 32;
// 2015-01-01T00:00:00Z, the snowflake epoch.
constexpr u64 kSnowflakeEpochMs = 1'420'070'400'000ull;
constexpr u32 kSearchPieces = 64;

bool view_valid(Utf8View text) noexcept {
    return byte_range_valid({reinterpret_cast<const u8*>(text.data), text.bytes});
}

// String.prototype.trim's ASCII part, which the search inputs apply.
bool space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
Utf8View trim(Utf8View text) noexcept {
    u32 start = 0, end = text.bytes;
    while (start < end && space(text.data[start]))
        ++start;
    while (end > start && space(text.data[end - 1]))
        --end;
    return {text.data + start, end - start};
}

u32 utf16_units(Utf8View text) noexcept {
    u32 units = 0;
    for (u32 i = 0; i < text.bytes; ++i) {
        const u8 byte = static_cast<u8>(text.data[i]);
        if ((byte & 0xC0) != 0x80)
            units += byte >= 0xF0 ? 2 : 1;
    }
    return units;
}

bool text_valid(Utf8View text, u32 maximum_bytes) noexcept {
    if (!view_valid(text) || text.bytes > maximum_bytes || !utf8_validate(text).valid)
        return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if (!text.data[i])
            return false;
    return true;
}

// The pieces of a search text: split on commas and trimmed, in order.
struct Pieces {
    Utf8View texts[kSearchPieces];
    Utf8View ids[kSearchPieces];
    u32 text_count, id_count;
    bool overflow;
};
Pieces split(Utf8View text) noexcept {
    Pieces pieces{};
    u32 start = 0;
    for (u32 i = 0; i <= text.bytes; ++i) {
        if (i < text.bytes && text.data[i] != ',')
            continue;
        const Utf8View piece = trim({text.data + start, i - start});
        const bool id = guild_member_search_snowflake(piece);
        u32& count = id ? pieces.id_count : pieces.text_count;
        if (count == kSearchPieces)
            pieces.overflow = true;
        else
            (id ? pieces.ids : pieces.texts)[count++] = piece;
        start = i + 1;
    }
    return pieces;
}

bool bulk_ids_valid(ConstSlice<u64> ids) noexcept {
    if (!ids.data || !ids.count || ids.count > kGuildBulkBanMaximumUsers)
        return false;
    for (u32 i = 0; i < ids.count; ++i)
        if (!ids[i])
            return false;
    return true;
}

bool search_valid(const GuildMemberSearchQuery& q) noexcept {
    if (!text_valid(q.text, kGuildMemberSearchTextBytes) || !text_valid(q.invite_code, kGuildMemberSearchInviteCodeBytes) ||
        (!q.role_ids.data && q.role_ids.count) || q.role_ids.count > kGuildMemberSearchRoleMaximum ||
        !q.limit || q.limit > kGuildMemberSearchLimitMaximum || q.sort > kGuildMemberSearchSortMaximum ||
        (q.signals & ~guild_member_signal_all) || (q.join_source_type_set && q.join_source_type > kGuildJoinSourceTypeMaximum) ||
        (!q.join_source_type_set && q.join_source_type) || (q.after.guild_joined_at_ms && q.before.guild_joined_at_ms) ||
        (q.signals && !q.now_ms) ||
        ((q.signals & guild_member_signal_unusual_dm) && q.now_ms < kGuildMemberSignalDmWindowMs))
        return false;
    for (u32 i = 0; i < q.role_ids.count; ++i)
        if (!q.role_ids[i])
            return false;
    if (split(trim(q.text)).overflow)
        return false;
    for (u32 i = 0; i < q.invite_code.bytes; ++i)
        if (q.invite_code.data[i] < 0x21 || q.invite_code.data[i] > 0x7E)
            return false;
    return true;
}

bool reason_valid(const GuildModerationRequest& r) noexcept {
    if (!view_valid(r.reason))
        return false;
    if (!r.reason.data)
        return true;
    if (r.operation != Op::bulk_ban)
        return false;
    if (!r.reason.bytes)
        return true;
    u8 scratch[kRestAuditReasonMaximumEncodedBytes];
    const bool ok = rest_encode_audit_reason(r.reason, {scratch, sizeof(scratch)}).ok;
    SecureZeroMemory(scratch, sizeof(scratch));
    return ok;
}

struct Body {
    ProtocolWriter writer;
    bool first = true;
    bool ok = true;

    void raw(const char* text) noexcept {
        ok = ok && protocol_writer_append_c_string(&writer, text);
    }
    void byte(u8 value) noexcept {
        ok = ok && protocol_writer_append_byte(&writer, value);
    }
    void number(u64 value) noexcept {
        ok = ok && protocol_writer_append_u64(&writer, value);
    }
    void string(Utf8View value) noexcept {
        ok = ok && protocol_writer_append_json_string(&writer, value);
    }
    void snowflake(u64 value) noexcept {
        byte('"');
        number(value);
        byte('"');
    }
    void key(const char* name) noexcept {
        if (!first)
            byte(',');
        first = false;
        byte('"');
        raw(name);
        raw("\":");
    }
    void begin() noexcept {
        byte('{');
        first = true;
    }
    void end() noexcept {
        byte('}');
        first = false;
    }
    void strings(ConstSlice<Utf8View> values) noexcept {
        byte('[');
        for (u32 i = 0; i < values.count; ++i) {
            if (i)
                byte(',');
            string(values[i]);
        }
        byte(']');
    }
    void range(u64 gte, u64 lte) noexcept {
        key("range");
        begin();
        key("gte");
        number(gte);
        if (lte) {
            key("lte");
            number(lte);
        }
        end();
    }
    void snowflake_at(u64 ms) noexcept {
        char digits[24];
        const u32 bytes = guild_member_search_snowflake_at(ms, {digits, sizeof(digits)});
        ok = ok && bytes;
        string({digits, bytes});
    }
    void cursor(const char* name, const GuildMemberSearchCursor& c) noexcept {
        key(name);
        begin();
        key("guild_joined_at");
        number(c.guild_joined_at_ms);
        key("user_id");
        snowflake(c.user_id);
        end();
    }
};

void write_search(Body& b, const GuildMemberSearchQuery& q) noexcept {
    b.begin();
    b.key("or_query");
    b.begin();
    if (q.signals) {
        b.key("safety_signals");
        b.begin();
        if (q.signals & guild_member_signal_unusual_dm) {
            b.key("unusual_dm_activity_until");
            b.begin();
            b.range(q.now_ms - kGuildMemberSignalDmWindowMs, 0);
            b.end();
        }
        if (q.signals & guild_member_signal_timed_out) {
            b.key("communication_disabled_until");
            b.begin();
            b.range(q.now_ms, 0);
            b.end();
        }
        if (q.signals & guild_member_signal_unusual_account) {
            b.key("unusual_account_activity");
            b.raw("true");
        }
        if (q.signals & guild_member_signal_quarantined) {
            b.key("automod_quarantined_username");
            b.raw("true");
        }
        b.end();
    }
    b.end();
    b.key("and_query");
    b.begin();
    const Utf8View text = trim(q.text);
    Pieces pieces{};
    if (utf16_units(text) > 1)
        pieces = split(text);
    if (pieces.text_count) {
        b.key("usernames");
        b.begin();
        b.key("or_query");
        b.strings({pieces.texts, pieces.text_count});
        b.end();
    }
    // The account age range joins user_id: after the search IDs when there
    // are any, else as its own key after guild_joined_at.
    const bool created = q.created_after_ms != 0;
    const auto created_range = [&]() noexcept {
        b.key("range");
        b.begin();
        b.key("gte");
        b.snowflake_at(q.created_after_ms);
        if (q.created_before_ms) {
            b.key("lte");
            b.snowflake_at(q.created_before_ms);
        }
        b.end();
    };
    if (pieces.id_count) {
        b.key("user_id");
        b.begin();
        b.key("or_query");
        b.strings({pieces.ids, pieces.id_count});
        if (created)
            created_range();
        b.end();
    }
    if (q.role_ids.count) {
        b.key("role_ids");
        b.begin();
        b.key("and_query");
        b.byte('[');
        for (u32 i = 0; i < q.role_ids.count; ++i) {
            if (i)
                b.byte(',');
            b.snowflake(q.role_ids[i]);
        }
        b.byte(']');
        b.end();
    }
    if (q.joined_after_ms) {
        b.key("guild_joined_at");
        b.begin();
        b.range(q.joined_after_ms, q.joined_before_ms);
        b.end();
    }
    if (created && !pieces.id_count) {
        b.key("user_id");
        b.begin();
        created_range();
        b.end();
    }
    if (q.invite_code.bytes) {
        b.key("source_invite_code");
        b.begin();
        b.key("or_query");
        b.strings({&q.invite_code, 1});
        b.end();
    }
    if (q.join_source_type_set) {
        b.key("join_source_type");
        b.begin();
        b.key("or_query");
        b.byte('[');
        b.number(q.join_source_type);
        b.byte(']');
        b.end();
    }
    b.end();
    if (q.sort) {
        b.key("sort");
        b.number(q.sort);
    }
    b.key("limit");
    b.number(q.limit);
    if (q.after.guild_joined_at_ms)
        b.cursor("after", q.after);
    else if (q.before.guild_joined_at_ms)
        b.cursor("before", q.before);
    b.end();
}

// Decoders --------------------------------------------------------------------

struct Scan {
    JsonIndexedDocument document;
    GuildModerationError error;
};
Scan scan(Utf8View body, Slice<u32> scratch) noexcept {
    if (!view_valid(body) || !body.bytes || !scratch.data || !scratch.count || !utf8_validate(body).valid)
        return {{}, GuildModerationError::input};
    const auto scanned = json_scan_structurals_avx1(body, scratch);
    if (!scanned.ok())
        return {{}, scanned.error == JsonScanError::output_overflow ? GuildModerationError::capacity
                                                                   : GuildModerationError::json};
    return {{body, {scratch.data, scanned.count}}, GuildModerationError::none};
}

template <typename Size> bool decode_text(JsonValue value, char* output, u32 capacity, Size* bytes) noexcept {
    *bytes = 0;
    if (value.kind == JsonValueKind::null_value)
        return true;
    JsonString encoded{};
    if (!json_value_as_string(value, &encoded))
        return false;
    char decoded[512];
    const auto result = json_string_decode_utf8(encoded, {decoded, sizeof(decoded)});
    if (!result.ok())
        return false;
    // Display text past the bound is cut at a code point.
    *bytes = static_cast<Size>(utf8_copy_truncated(output, capacity, {decoded, result.bytes}));
    return true;
}

bool nullable_snowflake(JsonValue value, u64* output) noexcept {
    *output = 0;
    return value.kind == JsonValueKind::null_value || json_value_to_snowflake(value, output);
}

bool timestamp(JsonValue value, u64* ms) noexcept {
    *ms = 0;
    JsonString text{};
    return value.kind == JsonValueKind::null_value ||
           (json_value_as_string(value, &text) && parse_iso_timestamp(text, ms));
}

bool nullable_u64(JsonValue value, u64* output) noexcept {
    *output = 0;
    return value.kind == JsonValueKind::null_value || json_value_to_u64(value, output);
}

struct MemberParse {
    const JsonIndexedDocument* document;
    GuildSearchMember* member;
    bool user_seen;
};

bool visit_user(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<MemberParse*>(opaque);
    auto& m = *p.member;
    if (json_string_equals_ascii(name, utf8_literal("id")))
        return json_value_to_snowflake(value, &m.user_id);
    if (json_string_equals_ascii(name, utf8_literal("username")))
        return decode_text(value, m.username, kGuildModerationNameBytes, &m.username_bytes);
    if (json_string_equals_ascii(name, utf8_literal("global_name")))
        return decode_text(value, m.global_name, kGuildModerationNameBytes, &m.global_name_bytes);
    if (json_string_equals_ascii(name, utf8_literal("avatar")))
        return decode_text(value, m.avatar, kGuildModerationHashBytes, &m.avatar_bytes);
    if (json_string_equals_ascii(name, utf8_literal("bot")))
        return value.kind == JsonValueKind::null_value || json_value_to_bool(value, &m.bot);
    if (json_string_equals_ascii(name, utf8_literal("public_flags")) ||
        json_string_equals_ascii(name, utf8_literal("flags"))) {
        u64 flags = 0;
        if (!nullable_u64(value, &flags))
            return false;
        m.user_flags |= flags;
        return true;
    }
    if (json_string_equals_ascii(name, utf8_literal("discriminator"))) {
        char digits[8];
        u8 bytes = 0;
        if (!decode_text(value, digits, sizeof(digits), &bytes))
            return false;
        u32 parsed = 0;
        for (u8 i = 0; i < bytes && i < 4; ++i)
            if (digits[i] >= '0' && digits[i] <= '9')
                parsed = parsed * 10 + static_cast<u32>(digits[i] - '0');
        m.discriminator = static_cast<u16>(parsed);
    }
    return true;
}

bool visit_role(void* opaque, u32, JsonValue value) noexcept {
    auto& m = *static_cast<GuildSearchMember*>(opaque);
    u64 id = 0;
    if (!json_value_to_snowflake(value, &id))
        return false;
    if (m.role_count < kGuildModerationMemberRoles)
        m.role_ids[m.role_count] = id;
    if (m.role_count < 0xFFFF)
        ++m.role_count;
    return true;
}

bool visit_member_object(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<MemberParse*>(opaque);
    auto& m = *p.member;
    if (json_string_equals_ascii(name, utf8_literal("user"))) {
        p.user_seen = value.kind == JsonValueKind::object;
        return p.user_seen && json_visit_indexed_object(*p.document, value, kDepth, visit_user, &p).ok();
    }
    if (json_string_equals_ascii(name, utf8_literal("nick")))
        return decode_text(value, m.nick, kGuildModerationNameBytes, &m.nick_bytes);
    if (json_string_equals_ascii(name, utf8_literal("roles")))
        return value.kind == JsonValueKind::array &&
               json_visit_indexed_array(*p.document, value, kDepth, visit_role, &m).ok();
    if (json_string_equals_ascii(name, utf8_literal("joined_at")))
        return timestamp(value, &m.joined_at_ms);
    if (json_string_equals_ascii(name, utf8_literal("communication_disabled_until")))
        return timestamp(value, &m.communication_disabled_until_ms);
    if (json_string_equals_ascii(name, utf8_literal("unusual_dm_activity_until")))
        return timestamp(value, &m.unusual_dm_activity_until_ms);
    if (json_string_equals_ascii(name, utf8_literal("pending")))
        return value.kind == JsonValueKind::null_value || json_value_to_bool(value, &m.pending);
    if (json_string_equals_ascii(name, utf8_literal("flags"))) {
        u64 flags = 0;
        if (!nullable_u64(value, &flags) || flags > 0xFFFFFFFFull)
            return false;
        m.member_flags = static_cast<u32>(flags);
    }
    return true;
}

struct ResultParse {
    const JsonIndexedDocument* document;
    GuildSearchMember member;
    bool member_seen, type_known;
    GuildModerationError error;
};

bool visit_result(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<ResultParse*>(opaque);
    auto& m = p.member;
    if (json_string_equals_ascii(name, utf8_literal("member"))) {
        if (value.kind != JsonValueKind::object)
            return false;
        MemberParse member{p.document, &m, false};
        p.member_seen = json_visit_indexed_object(*p.document, value, kDepth, visit_member_object, &member).ok() &&
                        member.user_seen && m.user_id;
        return p.member_seen;
    }
    if (json_string_equals_ascii(name, utf8_literal("source_invite_code")))
        return decode_text(value, m.invite_code, kGuildMemberSearchInviteCodeBytes, &m.invite_code_bytes);
    if (json_string_equals_ascii(name, utf8_literal("join_source_type"))) {
        u64 type = 0;
        if (!nullable_u64(value, &type) || type > 255)
            return false;
        m.join_source_type = static_cast<u8>(type);
        return true;
    }
    if (json_string_equals_ascii(name, utf8_literal("join_source_application_id")))
        return nullable_snowflake(value, &m.join_source_application_id);
    if (json_string_equals_ascii(name, utf8_literal("join_source_channel_id")))
        return nullable_snowflake(value, &m.join_source_channel_id);
    if (json_string_equals_ascii(name, utf8_literal("inviter_id")))
        return nullable_snowflake(value, &m.inviter_id);
    return true;
}

struct PageParse {
    const JsonIndexedDocument* document;
    GuildMemberSearchPage* page;
    u32 count;
    bool members_seen;
    GuildModerationError error;
};

bool visit_result_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<PageParse*>(opaque);
    if (p.count >= p.page->members.count) {
        p.error = GuildModerationError::capacity;
        return false;
    }
    if (value.kind != JsonValueKind::object) {
        p.error = GuildModerationError::field;
        return false;
    }
    ResultParse result{p.document, {}, false, false, GuildModerationError::none};
    if (!json_visit_indexed_object(*p.document, value, kDepth, visit_result, &result).ok() || !result.member_seen) {
        p.error = GuildModerationError::field;
        return false;
    }
    // A code with no type is an invite, as the official client reads it.
    if (!result.member.join_source_type && result.member.invite_code_bytes)
        result.member.join_source_type = 5;
    for (u32 i = 0; i < p.count; ++i)
        if (p.page->members[i].user_id == result.member.user_id) {
            p.error = GuildModerationError::duplicate;
            return false;
        }
    p.page->members[p.count++] = result.member;
    return true;
}

bool visit_page(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<PageParse*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("members"))) {
        if (p.members_seen || value.kind != JsonValueKind::array) {
            p.error = p.members_seen ? GuildModerationError::duplicate : GuildModerationError::field;
            return false;
        }
        p.members_seen = true;
        return json_visit_indexed_array(*p.document, value, kDepth, visit_result_item, &p).ok();
    }
    if (json_string_equals_ascii(name, utf8_literal("total_result_count"))) {
        u64 total = 0;
        if (value.kind == JsonValueKind::null_value)
            return true;
        if (!json_value_to_u64(value, &total) || total > 0xFFFFFFFFull) {
            p.error = GuildModerationError::field;
            return false;
        }
        p.page->total_result_count = static_cast<u32>(total);
        p.page->total_known = true;
    }
    return true;
}

struct RetryParse {
    u64 ms;
};
// Seconds, possibly fractional, to whole milliseconds.
bool visit_retry(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<RetryParse*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("retry_after")) || value.kind != JsonValueKind::number)
        return true;
    u64 whole = 0, fraction = 0, scale = 1000;
    bool dot = false;
    for (u32 i = 0; i < value.source.bytes; ++i) {
        const char c = value.source.data[i];
        if (c == '.' && !dot)
            dot = true;
        else if (c >= '0' && c <= '9') {
            if (!dot && whole < 1'000'000)
                whole = whole * 10 + static_cast<u64>(c - '0');
            else if (dot && scale > 1) {
                scale /= 10;
                fraction += static_cast<u64>(c - '0') * scale;
            }
        } else
            return true;
    }
    p.ms = whole * 1000 + fraction;
    return true;
}

struct BulkBanParse {
    const JsonIndexedDocument* document;
    GuildBulkBanUpdate value;
    bool guild_seen;
};

bool count_item(void* opaque, u32, JsonValue value) noexcept {
    u64 id = 0;
    if (!json_value_to_snowflake(value, &id))
        return false;
    ++*static_cast<u32*>(opaque);
    return true;
}

bool visit_bulk_ban_body(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<BulkBanParse*>(opaque);
    const bool banned = json_string_equals_ascii(name, utf8_literal("banned_users"));
    if (!banned && !json_string_equals_ascii(name, utf8_literal("failed_users")))
        return true;
    if (value.kind == JsonValueKind::null_value)
        return true;
    u32& count = banned ? p.value.banned_count : p.value.failed_count;
    count = 0;
    return value.kind == JsonValueKind::array &&
           json_visit_indexed_array(*p.document, value, kDepth, count_item, &count).ok();
}

bool visit_bulk_ban(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<BulkBanParse*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("guild_id"))) {
        p.guild_seen = json_value_to_snowflake(value, &p.value.guild_id);
        return p.guild_seen;
    }
    if (json_string_equals_ascii(name, utf8_literal("bulk_ban")))
        return value.kind == JsonValueKind::null_value ||
               (value.kind == JsonValueKind::object &&
                json_visit_indexed_object(*p.document, value, kDepth, visit_bulk_ban_body, &p).ok());
    return true;
}

} // namespace

RestMethod guild_moderation_method(GuildModerationOperation op) noexcept {
    return op == Op::ban_search ? RestMethod::get : RestMethod::post;
}

bool guild_moderation_request_valid(const GuildModerationRequest& r) noexcept {
    for (const u8 reserved : r.reserved)
        if (reserved)
            return false;
    if (r.operation >= Op::count || !r.guild_id || !reason_valid(r) || !view_valid(r.query) ||
        (!r.user_ids.data && r.user_ids.count))
        return false;
    switch (r.operation) {
    case Op::members_search:
        return r.search && search_valid(*r.search) && !r.user_ids.count && !r.query.bytes &&
               !r.delete_message_seconds;
    case Op::bulk_ban:
        return !r.search && bulk_ids_valid(r.user_ids) && !r.query.bytes &&
               r.delete_message_seconds <= kGuildModerationBanDeleteMaximumSeconds;
    case Op::ban_search: {
        if (r.search || r.user_ids.count || r.delete_message_seconds ||
            !text_valid(r.query, kGuildMemberSearchTextBytes) || !trim(r.query).bytes)
            return false;
        const auto pieces = split(trim(r.query));
        return !pieces.overflow && (pieces.id_count || trim(pieces.texts[0]).bytes);
    }
    default: return false;
    }
}

RestRouteKey guild_moderation_route(const GuildModerationRequest& r) noexcept {
    return {kGuildModerationRouteBase + static_cast<u64>(r.operation), r.guild_id,
            guild_moderation_method(r.operation), RestMajorParameter::guild};
}

GuildModerationResult guild_moderation_write_target(const GuildModerationRequest& r, Slice<u8> output) noexcept {
    if (!guild_moderation_request_valid(r) || !byte_range_valid({output.data, output.count}))
        return {0, 0, GuildModerationError::input};
    u8 target[kGuildModerationMaximumTargetBytes];
    ProtocolWriter w{{target, sizeof(target)}, 0};
    bool ok = protocol_writer_append_literal(&w, "/api/v10/guilds/") && protocol_writer_append_u64(&w, r.guild_id);
    switch (r.operation) {
    case Op::members_search: ok = ok && protocol_writer_append_literal(&w, "/members-search"); break;
    case Op::bulk_ban: ok = ok && protocol_writer_append_literal(&w, "/bulk-ban/v2"); break;
    case Op::ban_search: {
        // limit, the IDs (one key each), then the first text piece.
        ok = ok && protocol_writer_append_literal(&w, "/bans/search?limit=") &&
             protocol_writer_append_u64(&w, kGuildBanSearchLimit);
        const auto pieces = split(trim(r.query));
        for (u32 i = 0; ok && i < pieces.id_count; ++i)
            ok = protocol_writer_append_literal(&w, "&user_ids=") &&
                 protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(pieces.ids[i].data), pieces.ids[i].bytes);
        const Utf8View text = pieces.text_count ? trim(pieces.texts[0]) : Utf8View{};
        if (ok && text.bytes) {
            ok = protocol_writer_append_literal(&w, "&query=");
            const auto encoded =
                ok ? rest_encode_audit_reason(text, {target + w.used, sizeof(target) - w.used}) : RestAuditReasonResult{};
            ok = ok && encoded.ok;
            w.used += ok ? encoded.bytes : 0;
        }
        break;
    }
    default: ok = false; break;
    }
    if (!ok)
        return {0, 0, GuildModerationError::capacity};
    if (output.count < w.used)
        return {w.used, 0, GuildModerationError::capacity};
    copy_bytes(output.data, target, w.used);
    return {w.used, 0, GuildModerationError::none};
}

GuildModerationResult guild_moderation_write_body(const GuildModerationRequest& r, Slice<u8> output) noexcept {
    if (!guild_moderation_request_valid(r) || !byte_range_valid({output.data, output.count}))
        return {0, 0, GuildModerationError::input};
    if (r.operation == Op::ban_search)
        return {0, 0, GuildModerationError::none};
    if (!output.data || !output.count)
        return {0, 0, GuildModerationError::capacity};
    Body b{{output, 0}};
    if (r.operation == Op::members_search)
        write_search(b, *r.search);
    else {
        b.begin();
        b.key("user_ids");
        b.byte('[');
        for (u32 i = 0; i < r.user_ids.count; ++i) {
            if (i)
                b.byte(',');
            b.snowflake(r.user_ids[i]);
        }
        b.byte(']');
        b.key("delete_message_seconds");
        b.number(r.delete_message_seconds);
        b.end();
    }
    if (!b.ok) {
        SecureZeroMemory(output.data, b.writer.used);
        return {0, 0, GuildModerationError::capacity};
    }
    return {b.writer.used, 0, GuildModerationError::none};
}

GuildModerationResult guild_moderation_write_reason(const GuildModerationRequest& r, Slice<u8> output) noexcept {
    if (!guild_moderation_request_valid(r) || !byte_range_valid({output.data, output.count}))
        return {0, 0, GuildModerationError::input};
    if (!r.reason.bytes)
        return {0, 0, GuildModerationError::none};
    const auto encoded = rest_encode_audit_reason(r.reason, output);
    return {encoded.bytes, 0, encoded.ok ? GuildModerationError::none : GuildModerationError::capacity};
}

bool guild_moderation_reason_header(const GuildModerationRequest& r) noexcept {
    return guild_moderation_request_valid(r) && r.operation == Op::bulk_ban && r.reason.data;
}

u32 guild_member_search_limit(u32 page_size) noexcept {
    return maximum(page_size * 5, kGuildMemberSearchChunkMinimum);
}

bool guild_member_search_snowflake(Utf8View text) noexcept {
    if (text.bytes < 17 || text.bytes > 19)
        return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if (text.data[i] < '0' || text.data[i] > '9')
            return false;
    return true;
}

u32 guild_member_search_snowflake_at(u64 unix_ms, Slice<char> output) noexcept {
    if (!output.data || !output.count)
        return 0;
    const u64 value = unix_ms > kSnowflakeEpochMs ? (unix_ms - kSnowflakeEpochMs) << 22 : 0;
    char digits[20];
    u32 count = 0;
    u64 rest = value;
    do {
        digits[count++] = static_cast<char>('0' + rest % 10);
        rest /= 10;
    } while (rest && count < 20);
    if (count > output.count)
        return 0;
    for (u32 i = 0; i < count; ++i)
        output[i] = digits[count - 1 - i];
    return count;
}

GuildModerationResult guild_member_search_decode(Utf8View body, Slice<u32> scratch,
                                                 GuildMemberSearchPage* page) noexcept {
    if (!page || (!page->members.data && page->members.count))
        return {0, 0, GuildModerationError::input};
    const Scan s = scan(body, scratch);
    if (s.error != GuildModerationError::none)
        return {0, 0, s.error};
    page->member_count = page->total_result_count = 0;
    page->total_known = false;
    PageParse p{&s.document, page, 0, false, GuildModerationError::none};
    const auto read = json_visit_root_object(s.document.input, s.document.structurals, kDepth, visit_page, &p);
    if (!read.ok() || !p.members_seen)
        return {0, 0, p.error != GuildModerationError::none ? p.error
                      : read.ok()                            ? GuildModerationError::identity
                                                             : GuildModerationError::json};
    page->member_count = p.count;
    return {body.bytes, p.count, GuildModerationError::none};
}

u64 guild_member_search_retry_after_ms(Utf8View body, Slice<u32> scratch) noexcept {
    const Scan s = scan(body, scratch);
    if (s.error != GuildModerationError::none)
        return 0;
    RetryParse p{};
    json_visit_root_object(s.document.input, s.document.structurals, kDepth, visit_retry, &p);
    return p.ms;
}

bool guild_bulk_ban_update_decode(Utf8View body, Slice<u32> scratch, GuildBulkBanUpdate* output) noexcept {
    if (!output)
        return false;
    const Scan s = scan(body, scratch);
    if (s.error != GuildModerationError::none)
        return false;
    BulkBanParse p{&s.document, {}, false};
    if (!json_visit_root_object(s.document.input, s.document.structurals, kDepth, visit_bulk_ban, &p).ok() ||
        !p.guild_seen || !p.value.guild_id)
        return false;
    *output = p.value;
    return true;
}

} // namespace maicord
