#include "maicord/protocol/account_settings.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/rest_route.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/user_settings_proto.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "json_write.hpp"

namespace maicord {
namespace {
using Op = AccountSettingsOperation;
using W = AccountSettingsWriteError;

bool hash_valid(ConstSlice<u8> hash) noexcept {
    if (!hash.count || hash.count > kAccountSettingsHashBytes)
        return false;
    for (u32 i = 0; i < hash.count; ++i) {
        const u8 c = hash[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
              c == '/' || c == '=' || c == '-' || c == '_'))
            return false;
    }
    return true;
}
u32 text_limit(const AccountSettingsRequest& r) noexcept {
    return r.operation == Op::display_name                                     ? kAccountSettingsDisplayNameBytes
           : r.field == AccountProfileField::pronouns ? kAccountSettingsPronounsBytes
                                                                               : kAccountSettingsBioBytes;
}
Utf8View text(ConstSlice<u8> payload) noexcept {
    return {reinterpret_cast<const char*>(payload.data), payload.count};
}

// Copies a JSON string, truncating at a code point boundary.
u32 copy_text(JsonValue value, char* output, u32 capacity) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string))
        return 0;
    char decoded[1024];
    const auto result = json_string_decode_utf8(string, {decoded, sizeof(decoded)});
    if (!result.ok())
        return 0;
    return utf8_copy_truncated(output, capacity, {decoded, result.bytes});
}
template <typename T> u8 copy_small(JsonValue value, T (&output)) noexcept {
    return static_cast<u8>(copy_text(value, output, sizeof(output)));
}
bool scan(Utf8View body, Slice<u32> structurals, JsonIndexedDocument* document) noexcept {
    if (!body.data || !body.bytes || !structurals.data)
        return false;
    const auto scanned = json_scan_structurals_avx1(body, structurals);
    if (!scanned.ok())
        return false;
    *document = {body, {structurals.data, scanned.count}};
    return true;
}

struct SessionContext {
    JsonIndexedDocument document;
    Slice<AccountSession> output;
    AccountSession* current;
    u32 count;
    JsonValue sessions;
    bool sessions_seen;
};
bool client_info_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& s = *static_cast<AccountSession*>(opaque);
    if (value.kind != JsonValueKind::string)
        return true;
    if (json_string_equals_ascii(name, utf8_literal("os")))
        s.os_bytes = copy_small(value, s.os);
    else if (json_string_equals_ascii(name, utf8_literal("platform")))
        s.platform_bytes = copy_small(value, s.platform);
    else if (json_string_equals_ascii(name, utf8_literal("location")))
        s.location_bytes = copy_small(value, s.location);
    return true;
}
bool session_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<SessionContext*>(opaque);
    auto& s = *c.current;
    if (json_string_equals_ascii(name, utf8_literal("id_hash"))) {
        s.id_hash_bytes = copy_small(value, s.id_hash);
    } else if (json_string_equals_ascii(name, utf8_literal("approx_last_used_time"))) {
        JsonString string{};
        if (json_value_as_string(value, &string))
            parse_iso_timestamp(string, &s.last_used_ms);
    } else if (json_string_equals_ascii(name, utf8_literal("client_info")) && value.kind == JsonValueKind::object) {
        return json_visit_indexed_object(c.document, value, 4, client_info_member, &s).ok();
    }
    return true;
}
bool session_item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<SessionContext*>(opaque);
    if (value.kind != JsonValueKind::object)
        return false;
    if (c.count >= c.output.count)
        return true;
    AccountSession session{};
    c.current = &session;
    if (!json_visit_indexed_object(c.document, value, 6, session_member, &c).ok())
        return false;
    if (hash_valid({reinterpret_cast<const u8*>(session.id_hash), session.id_hash_bytes}))
        c.output[c.count++] = session;
    return true;
}
bool sessions_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<SessionContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("user_sessions"))) {
        if (value.kind != JsonValueKind::array || c.sessions_seen)
            return false;
        c.sessions = value;
        c.sessions_seen = true;
    }
    return true;
}

struct TokenContext {
    JsonIndexedDocument document;
    AccountAuthorizedApp* current;
};
bool scope_item(void* opaque, u32, JsonValue value) noexcept {
    auto& app = *static_cast<AccountAuthorizedApp*>(opaque);
    char scope[48];
    const u32 bytes = copy_text(value, scope, sizeof(scope));
    const u32 separator = app.scopes_bytes ? 2 : 0;
    if (!bytes || app.scopes_bytes + separator + bytes > sizeof(app.scopes))
        return true;
    if (separator) {
        app.scopes[app.scopes_bytes++] = ',';
        app.scopes[app.scopes_bytes++] = ' ';
    }
    copy_bytes(app.scopes + app.scopes_bytes, scope, bytes);
    app.scopes_bytes = static_cast<u8>(app.scopes_bytes + bytes);
    return true;
}
bool application_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& app = *static_cast<AccountAuthorizedApp*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("id")))
        return json_value_to_snowflake(value, &app.application_id);
    if (json_string_equals_ascii(name, utf8_literal("name")))
        app.name_bytes = copy_small(value, app.name);
    return true;
}
bool token_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<TokenContext*>(opaque);
    auto& app = *c.current;
    if (json_string_equals_ascii(name, utf8_literal("id")))
        return json_value_to_snowflake(value, &app.token_id);
    if (json_string_equals_ascii(name, utf8_literal("scopes")) && value.kind == JsonValueKind::array)
        return json_visit_indexed_array(c.document, value, 4, scope_item, &app).ok();
    if (json_string_equals_ascii(name, utf8_literal("application")) && value.kind == JsonValueKind::object)
        return json_visit_indexed_object(c.document, value, 6, application_member, &app).ok();
    return true;
}
struct TokenList {
    TokenContext context;
    Slice<AccountAuthorizedApp> output;
    u32 count;
};
bool token_item(void* opaque, u32, JsonValue value) noexcept {
    auto& list = *static_cast<TokenList*>(opaque);
    if (value.kind != JsonValueKind::object)
        return false;
    if (list.count >= list.output.count)
        return true;
    AccountAuthorizedApp app{};
    list.context.current = &app;
    if (!json_visit_indexed_object(list.context.document, value, 8, token_member, &list.context).ok())
        return false;
    if (app.token_id)
        list.output[list.count++] = app;
    return true;
}

struct SelfContext {
    AccountSelfUser* user;
};
bool self_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& u = *static_cast<SelfContext*>(opaque)->user;
    const bool string = value.kind == JsonValueKind::string;
    if (json_string_equals_ascii(name, utf8_literal("username")))
        u.username_bytes = string ? copy_small(value, u.username) : 0;
    else if (json_string_equals_ascii(name, utf8_literal("global_name")))
        u.global_name_bytes = string ? copy_small(value, u.global_name) : 0;
    else if (json_string_equals_ascii(name, utf8_literal("email"))) {
        u.email_bytes = string ? copy_small(value, u.email) : 0;
        u.email_known = true;
    } else if (json_string_equals_ascii(name, utf8_literal("phone"))) {
        u.phone_bytes = string ? copy_small(value, u.phone) : 0;
        u.phone_known = true;
    } else if (json_string_equals_ascii(name, utf8_literal("verified")))
        json_value_to_bool(value, &u.verified);
    else if (json_string_equals_ascii(name, utf8_literal("mfa_enabled")))
        json_value_to_bool(value, &u.mfa_enabled);
    return true;
}
struct ProfileContext {
    JsonIndexedDocument document;
    AccountProfileText* profile;
};
bool profile_text_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<ProfileContext*>(opaque)->profile;
    if (value.kind != JsonValueKind::string)
        return true;
    if (json_string_equals_ascii(name, utf8_literal("bio")))
        p.bio_bytes = static_cast<u16>(copy_text(value, p.bio, sizeof(p.bio)));
    else if (json_string_equals_ascii(name, utf8_literal("pronouns")))
        p.pronouns_bytes = static_cast<u16>(copy_text(value, p.pronouns, sizeof(p.pronouns)));
    return true;
}
bool profile_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<ProfileContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("user_profile")) && value.kind == JsonValueKind::object)
        return json_visit_indexed_object(c.document, value, 6, profile_text_member, &c).ok();
    return true;
}
struct CodeContext {
    u64 code;
};
bool code_member(void* opaque, JsonString name, JsonValue value) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("code")))
        json_value_to_u64(value, &static_cast<CodeContext*>(opaque)->code);
    return true;
}
} // namespace

bool account_settings_primary(AccountSettingsOperation operation) noexcept {
    return operation == Op::settings_fetch || operation == Op::settings_patch;
}

bool account_settings_request_valid(const AccountSettingsRequest& r) noexcept {
    if (!byte_range_valid(r.payload) || r.reserved[0] || r.reserved[1] ||
        static_cast<u8>(r.field) > static_cast<u8>(AccountProfileField::bio))
        return false;
    switch (r.operation) {
    case Op::settings_patch:
        return user_settings_proto_patch_valid(r.payload) && !r.id;
    case Op::session_logout:
        return hash_valid(r.payload) && !r.id && !r.required_data_version;
    case Op::token_revoke:
    case Op::profile:
        return r.id && !r.payload.count && !r.required_data_version;
    case Op::profile_patch:
    case Op::display_name:
        return !r.id && !r.required_data_version && r.payload.count <= text_limit(r) &&
               utf8_validate(text(r.payload)).valid;
    case Op::settings_fetch:
    case Op::self_user:
    case Op::sessions:
    case Op::tokens:
        return !r.id && !r.payload.count && !r.required_data_version;
    }
    return false;
}

AccountSettingsWriteResult account_settings_write_request(const AccountSettingsRequest& r, Slice<u8> target,
                                                          Slice<u8> body) noexcept {
    AccountSettingsWriteResult result{0, 0, 0, RestMethod::get, W::invalid_input};
    if (!account_settings_request_valid(r) || !target.data || !byte_range_valid({body.data, body.count}))
        return result;
    RouteWriter route{};
    if (!route_writer_initialize(&route, target))
        return result;
    using namespace protocol_detail;
    Utf8View path{};
    u32 at = 0;
    const auto fail_capacity = [&]() noexcept {
        result.error = W::output_capacity;
        return result;
    };
    switch (r.operation) {
    case Op::settings_fetch:
    case Op::settings_patch:
        path = utf8_literal("/api/v10/users/@me/settings-proto/1");
        result.route_id = kAccountSettingsRouteId;
        result.method = r.operation == Op::settings_fetch ? RestMethod::get : RestMethod::patch;
        if (r.operation == Op::settings_patch) {
            at = user_settings_proto_write_patch_json(r.payload, r.required_data_version, body);
            if (!at)
                return fail_capacity();
        }
        break;
    case Op::self_user:
        path = utf8_literal("/api/v10/users/@me");
        result.route_id = kAccountSelfRouteId;
        break;
    case Op::sessions:
    case Op::session_logout: {
        path = r.operation == Op::sessions ? utf8_literal("/api/v10/auth/sessions")
                                           : utf8_literal("/api/v10/auth/sessions/logout");
        result.route_id = kAccountSessionsRouteId;
        if (r.operation == Op::session_logout) {
            result.method = RestMethod::post;
            constexpr char head[] = "{\"session_id_hashes\":[\"";
            constexpr char tail[] = "\"]}";
            if (body.count < sizeof(head) - 1 + r.payload.count + sizeof(tail) - 1)
                return fail_capacity();
            write_literal(body.data, &at, head);
            copy_bytes(body.data + at, r.payload.data, r.payload.count);
            at += r.payload.count;
            write_literal(body.data, &at, tail);
        }
        break;
    }
    case Op::tokens:
    case Op::token_revoke:
        result.route_id = kAccountTokensRouteId;
        if (r.operation == Op::token_revoke) {
            result.method = RestMethod::delete_method;
            if (!route_append(&route, utf8_literal("/api/v10/oauth2/tokens/")) ||
                !route_append_u64(&route, r.id))
                return fail_capacity();
        } else
            path = utf8_literal("/api/v10/oauth2/tokens");
        break;
    case Op::profile:
        result.route_id = kAccountProfileRouteId;
        if (!route_append(&route, utf8_literal("/api/v10/users/")) ||
            !route_append_u64(&route, r.id) || !route_append(&route, utf8_literal("/profile")))
            return fail_capacity();
        break;
    case Op::profile_patch:
    case Op::display_name: {
        const bool name = r.operation == Op::display_name;
        path = name ? utf8_literal("/api/v10/users/@me") : utf8_literal("/api/v10/users/@me/profile");
        result.route_id = name ? kAccountSelfRouteId : kAccountProfileRouteId;
        result.method = RestMethod::patch;
        const Utf8View key = name ? utf8_literal("global_name")
                             : r.field == AccountProfileField::pronouns ? utf8_literal("pronouns")
                                                                        : utf8_literal("bio");
        u32 encoded = 0;
        // An empty display name resets to the username; profile text clears to "".
        const bool null_value = name && !r.payload.count;
        if (!measure_json_string(text(r.payload), &encoded))
            return result;
        const u32 value_bytes = null_value ? 4 : encoded;
        if (body.count < key.bytes + value_bytes + 5)
            return fail_capacity();
        body.data[at++] = '{';
        body.data[at++] = '"';
        copy_bytes(body.data + at, key.data, key.bytes);
        at += key.bytes;
        body.data[at++] = '"';
        body.data[at++] = ':';
        if (null_value)
            write_literal(body.data, &at, "null");
        else {
            write_json_string(body.data + at, text(r.payload));
            at += encoded;
        }
        body.data[at++] = '}';
        break;
    }
    }
    if (path.bytes && !route_append(&route, path))
        return fail_capacity();
    if (!route.valid)
        return fail_capacity();
    result.target_bytes = route.bytes;
    result.body_bytes = at;
    result.error = W::none;
    return result;
}

u32 account_settings_decode_sessions(Utf8View body, Slice<u32> structurals, Slice<AccountSession> output) noexcept {
    SessionContext c{};
    if (!scan(body, structurals, &c.document))
        return ~0u;
    c.output = output;
    if (!json_visit_root_object(body, c.document.structurals, 8, sessions_root, &c).ok() || !c.sessions_seen ||
        !json_visit_indexed_array(c.document, c.sessions, 8, session_item, &c).ok())
        return ~0u;
    return c.count;
}

u32 account_settings_decode_tokens(Utf8View body, Slice<u32> structurals,
                                   Slice<AccountAuthorizedApp> output) noexcept {
    TokenList list{};
    if (!scan(body, structurals, &list.context.document))
        return ~0u;
    list.output = output;
    if (!json_visit_root_array(body, list.context.document.structurals, 10, token_item, &list).ok())
        return ~0u;
    return list.count;
}

bool account_settings_decode_self(Utf8View body, Slice<u32> structurals, AccountSelfUser* user) noexcept {
    JsonIndexedDocument document{};
    if (!user || !scan(body, structurals, &document))
        return false;
    AccountSelfUser decoded{};
    SelfContext c{&decoded};
    if (!json_visit_root_object(body, document.structurals, 8, self_member, &c).ok() || !decoded.username_bytes)
        return false;
    *user = decoded;
    return true;
}

bool account_settings_decode_profile(Utf8View body, Slice<u32> structurals, AccountProfileText* profile) noexcept {
    ProfileContext c{};
    if (!profile || !scan(body, structurals, &c.document))
        return false;
    AccountProfileText decoded{};
    c.profile = &decoded;
    if (!json_visit_root_object(body, c.document.structurals, 8, profile_root, &c).ok())
        return false;
    *profile = decoded;
    return true;
}

u64 account_settings_error_code(Utf8View body, Slice<u32> structurals) noexcept {
    JsonIndexedDocument document{};
    CodeContext c{};
    if (!scan(body, structurals, &document) ||
        !json_visit_root_object(body, document.structurals, 8, code_member, &c).ok())
        return 0;
    return c.code;
}

u32 account_settings_mask_email(Utf8View email, Slice<char> output) noexcept {
    u32 at_sign = email.bytes;
    for (u32 i = 0; i < email.bytes; ++i)
        if (email.data[i] == '@')
            at_sign = i;
    if (!email.bytes || at_sign == email.bytes || !at_sign || output.count < email.bytes)
        return 0;
    // Keep the first character of the local part and the whole domain.
    u32 at = 0, first = 1;
    while (first < at_sign && (static_cast<u8>(email.data[first]) & 0xC0) == 0x80)
        ++first;
    for (; at < first; ++at)
        output[at] = email.data[at];
    for (u32 i = first; i < at_sign; ++i) {
        if ((static_cast<u8>(email.data[i]) & 0xC0) != 0x80)
            output[at++] = '*';
    }
    for (u32 i = at_sign; i < email.bytes; ++i)
        output[at++] = email.data[i];
    return at;
}

u32 account_settings_mask_phone(Utf8View phone, Slice<char> output) noexcept {
    if (phone.bytes < 4 || output.count < phone.bytes)
        return 0;
    for (u32 i = 0; i + 4 < phone.bytes; ++i)
        output[i] = '*';
    for (u32 i = phone.bytes - 4; i < phone.bytes; ++i)
        output[i] = phone.data[i];
    return phone.bytes;
}

} // namespace maicord
