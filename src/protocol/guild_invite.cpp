#include "maicord/protocol/guild_invite.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/rest_route.hpp"
#include "maicord/core/json_scan.hpp"
#include "json_helpers.hpp"
#include "json_field_lists.hpp"

namespace maicord {
namespace {
struct Members {
    JsonIndexedDocument document;
    GuildInvite* invite;
    JsonValue guild, channel;
    u64 error_code;
    bool guild_seen, channel_seen, captcha, error_code_seen;
};
// Copies a JSON string, truncating at a code point boundary.
u8 copy_text(JsonValue value, char* output, u32 capacity) noexcept {
    return static_cast<u8>(protocol_detail::copy_text_truncated<512>(value, output, capacity));
}
bool guild_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& m = *static_cast<Members*>(opaque);
    switch (protocol_detail::member_index<"id", "name", "icon">(name)) {
    case 0: return json_value_to_snowflake(value, &m.invite->guild_id);
    case 1: m.invite->name_bytes = copy_text(value, m.invite->name, kGuildInviteNameBytes); break;
    case 2:
        m.invite->icon_bytes = value.kind == JsonValueKind::string
                                   ? copy_text(value, m.invite->icon, kGuildInviteIconBytes)
                                   : 0;
        break;
    default: break;
    }
    return true;
}
bool channel_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& m = *static_cast<Members*>(opaque);
    u64 type = 0;
    switch (protocol_detail::member_index<"id", "name", "type">(name)) {
    case 0: return json_value_to_snowflake(value, &m.invite->channel_id);
    case 1:
        m.invite->channel_name_bytes = value.kind == JsonValueKind::string
                                           ? copy_text(value, m.invite->channel_name, kGuildInviteNameBytes)
                                           : 0;
        break;
    case 2:
        if (json_value_to_u64(value, &type))
            m.invite->channel_type = static_cast<u8>(minimum<u64>(type, 255));
        break;
    default: break;
    }
    return true;
}
bool root_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& m = *static_cast<Members*>(opaque);
    u64 number = 0;
    switch (protocol_detail::member_index<"code", "guild", "channel", "approximate_member_count",
                                          "approximate_presence_count", "captcha_key">(name)) {
    case 0:
        // Invites carry a string code; API errors carry a numeric one.
        if (value.kind == JsonValueKind::string)
            m.invite->code_bytes = copy_text(value, m.invite->code, kGuildInviteCodeMaximumBytes);
        else if (json_value_to_u64(value, &m.error_code))
            m.error_code_seen = true;
        break;
    case 1:
        if (value.kind == JsonValueKind::object) {
            m.guild = value;
            m.guild_seen = true;
        }
        break;
    case 2:
        if (value.kind == JsonValueKind::object) {
            m.channel = value;
            m.channel_seen = true;
        }
        break;
    case 3:
        if (json_value_to_u64(value, &number)) {
            m.invite->member_count = static_cast<u32>(minimum<u64>(number, ~u32{0}));
            m.invite->counts_known = true;
        }
        break;
    case 4:
        if (json_value_to_u64(value, &number))
            m.invite->presence_count = static_cast<u32>(minimum<u64>(number, ~u32{0}));
        break;
    case 5: m.captcha = true; break;
    default: break;
    }
    return true;
}
} // namespace

bool guild_invite_code_valid(const char* code, u32 bytes) noexcept {
    if (!code || !bytes || bytes > kGuildInviteCodeMaximumBytes)
        return false;
    for (u32 i = 0; i < bytes; ++i) {
        const char c = code[i];
        if (!(ascii_is_alnum(c) || c == '-'))
            return false;
    }
    return true;
}

bool guild_invite_request_valid(const GuildInviteRequest& request) noexcept {
    for (const u8 reserved : request.reserved)
        if (reserved)
            return false;
    for (u32 i = request.code_bytes; i < kGuildInviteCodeMaximumBytes; ++i)
        if (request.code[i])
            return false;
    return request.operation <= GuildInviteOperation::accept &&
           guild_invite_code_valid(request.code, request.code_bytes);
}

GuildInviteWriteResult guild_invite_write_request(const GuildInviteRequest& request, Slice<u8> target,
                                                  Slice<u8> body, Utf8View session_id) noexcept {
    GuildInviteWriteResult result{};
    if (!guild_invite_request_valid(request) || !target.data) {
        result.error = GuildInviteWriteError::invalid_input;
        return result;
    }
    const bool accept = request.operation == GuildInviteOperation::accept;
    RouteWriter route{};
    const bool written =
        route_writer_initialize(&route, target) &&
        route_append(&route, utf8_literal("/api/v10/invites/")) &&
        route_append(&route, {request.code, request.code_bytes}) &&
        (accept || route_append(&route, utf8_literal("?with_counts=true&with_expiration=true")));
    if (!written || (accept && (!body.data || body.count < 2))) {
        result.error = GuildInviteWriteError::output_capacity;
        return result;
    }
    for (u32 i = 0; i < session_id.bytes; ++i)
        if (!ascii_is_alnum(session_id.data[i])) {
            result.error = GuildInviteWriteError::invalid_input;
            return result;
        }
    if (accept) {
        ProtocolWriter json{body, 0};
        const bool encoded = session_id.bytes
                                 ? protocol_writer_append_literal(&json, "{\"session_id\":\"") &&
                                       protocol_writer_append_bytes(
                                           &json, {reinterpret_cast<const u8*>(session_id.data), session_id.bytes}) &&
                                       protocol_writer_append_literal(&json, "\"}")
                                 : protocol_writer_append_literal(&json, "{}");
        if (!encoded) {
            result.error = GuildInviteWriteError::output_capacity;
            return result;
        }
        result.body_bytes = json.used;
    }
    result.target_bytes = route.bytes;
    result.method = accept ? RestMethod::post : RestMethod::get;
    result.route_id = accept ? kGuildInviteAcceptRouteId : kGuildInviteResolveRouteId;
    return result;
}

u32 guild_invite_write_context_header(const GuildInviteRequest& request, Slice<u8> output) noexcept {
    if (!guild_invite_request_valid(request) || request.operation != GuildInviteOperation::accept)
        return 0;
    u8 json_bytes[192];
    ProtocolWriter json{{json_bytes, sizeof(json_bytes)}, 0};
    bool ok = protocol_writer_append_literal(&json, "{\"location\":\"Desktop Invite Modal\"");
    if (request.location_guild_id)
        ok = ok && protocol_writer_append_literal(&json, ",\"location_guild_id\":\"") &&
             protocol_writer_append_u64(&json, request.location_guild_id) &&
             protocol_writer_append_byte(&json, '"');
    if (request.location_channel_id)
        ok = ok && protocol_writer_append_literal(&json, ",\"location_channel_id\":\"") &&
             protocol_writer_append_u64(&json, request.location_channel_id) &&
             protocol_writer_append_literal(&json, "\",\"location_channel_type\":") &&
             protocol_writer_append_u64(&json, request.location_channel_type);
    ok = ok && protocol_writer_append_byte(&json, '}');
    u8 encoded[base64_encoded_size(sizeof(json_bytes))];
    const u32 encoded_bytes = base64_encoded_size(json.used);
    if (!ok || !base64_encode({json_bytes, json.used}, {reinterpret_cast<char*>(encoded), encoded_bytes}).ok())
        return 0;
    const u32 written =
        rest_append_extra_header(output, 0, utf8_literal("X-Context-Properties"), {encoded, encoded_bytes});
    return written == ~0u ? 0 : written;
}

GuildInviteOutcome guild_invite_decode_response(u16 http_status, Utf8View body, Slice<u32> scratch,
                                                GuildInvite* output) noexcept {
    if (!output)
        return GuildInviteOutcome::malformed;
    if (http_status == 429)
        return GuildInviteOutcome::rate_limited;
    GuildInvite invite{};
    Members members{};
    members.invite = &invite;
    bool parsed = false;
    if (body.data && body.bytes && body.bytes <= kGuildInviteMaximumResponseBytes && scratch.data && scratch.count) {
        const auto scan = json_scan_structurals_avx1(body, scratch);
        if (scan.ok()) {
            members.document = {body, {scratch.data, scan.count}};
            parsed = json_visit_root_object(body, members.document.structurals, 16, root_member, &members).ok() &&
                     (!members.guild_seen ||
                      json_visit_indexed_object(members.document, members.guild, 16, guild_member, &members).ok()) &&
                     (!members.channel_seen ||
                      json_visit_indexed_object(members.document, members.channel, 16, channel_member, &members).ok());
        }
    }
    if (members.captcha)
        return GuildInviteOutcome::captcha;
    if (http_status == 404 || (members.error_code_seen && members.error_code == 10006))
        return GuildInviteOutcome::unknown;
    if (members.error_code_seen && members.error_code == 40007)
        return GuildInviteOutcome::banned;
    if (http_status < 200 || http_status >= 300)
        return GuildInviteOutcome::unavailable;
    if (!parsed || !invite.guild_id)
        return GuildInviteOutcome::malformed;
    copy_value(output, invite);
    return GuildInviteOutcome::resolved;
}

} // namespace maicord
