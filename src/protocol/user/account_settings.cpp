#include "maicord/protocol/user/account_settings.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "maicord/protocol/http/rest_route.hpp"
#include "maicord/protocol/user/settings_proto.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/utf.hpp"
#include "../encoding/json_helpers.hpp"
#include "../encoding/json_field_lists.hpp"
#include "../encoding/json_write.hpp"

namespace maicord {
namespace {
using protocol_detail::member_index;
using Op = AccountSettingsOperation;
using W = AccountSettingsWriteError;

bool hash_valid(ConstSlice<u8> hash) noexcept {
    if (!hash.count || hash.count > kAccountSettingsHashBytes)
        return false;
    for (u32 i = 0; i < hash.count; ++i) {
        const u8 c = hash[i];
        if (!(ascii_is_alnum(c) || c == '+' ||
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
Utf8View text(const ConstSlice<u8>& payload) noexcept {
    return {reinterpret_cast<const char*>(payload.data), payload.count};
}

// Copies a JSON string, truncating at a code point boundary.
u32 copy_text(const JsonValue& value, char* output, u32 capacity) noexcept {
    return protocol_detail::copy_text_truncated<1024>(value, output, capacity);
}
template <typename T> u8 copy_small(const JsonValue& value, T (&output)) noexcept {
    return static_cast<u8>(copy_text(value, output, sizeof(output)));
}
bool scan(const Utf8View& body, const Slice<u32>& structurals, JsonIndexedDocument* document) noexcept {
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
    switch (member_index<"os", "platform", "location">(name)) {
    case 0: s.os_bytes = copy_small(value, s.os); break;
    case 1: s.platform_bytes = copy_small(value, s.platform); break;
    case 2: s.location_bytes = copy_small(value, s.location); break;
    default: break;
    }
    return true;
}
bool session_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<SessionContext*>(opaque);
    auto& s = *c.current;
    switch (member_index<"id_hash", "approx_last_used_time", "client_info">(name)) {
    case 0: s.id_hash_bytes = copy_small(value, s.id_hash); break;
    case 1: json_value_to_timestamp(value, &s.last_used_ms); break;
    case 2:
        if (value.kind == JsonValueKind::object)
            return json_visit_indexed_object(c.document, value, 4, client_info_member, &s).ok();
        break;
    default: break;
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
    switch (member_index<"id", "name">(name)) {
    case 0: return json_value_to_snowflake(value, &app.application_id);
    case 1: app.name_bytes = copy_small(value, app.name); break;
    default: break;
    }
    return true;
}
bool token_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<TokenContext*>(opaque);
    auto& app = *c.current;
    switch (member_index<"id", "scopes", "application">(name)) {
    case 0: return json_value_to_snowflake(value, &app.token_id);
    case 1:
        if (value.kind == JsonValueKind::array)
            return json_visit_indexed_array(c.document, value, 4, scope_item, &app).ok();
        break;
    case 2:
        if (value.kind == JsonValueKind::object)
            return json_visit_indexed_object(c.document, value, 6, application_member, &app).ok();
        break;
    default: break;
    }
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
    JsonIndexedDocument document;
    AccountSelfUser user;
};
bool authenticator_item(void* opaque, u32, JsonValue value) noexcept {
    u64 type = 0;
    if (json_value_to_u64(value, &type) && type >= 1 && type <= 3)
        *static_cast<u8*>(opaque) = static_cast<u8>(*static_cast<u8*>(opaque) | (1u << type));
    return true;
}
bool self_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<SelfContext*>(opaque);
    auto& u = c.user;
    const bool string = value.kind == JsonValueKind::string;
    u64 flags = 0;
    switch (member_index<"username", "global_name", "email", "phone", "verified", "mfa_enabled", "flags",
                         "authenticator_types">(name)) {
    case 0: u.username_bytes = string ? copy_small(value, u.username) : 0; break;
    case 1: u.global_name_bytes = string ? copy_small(value, u.global_name) : 0; break;
    case 2: u.email_bytes = string ? copy_small(value, u.email) : 0; break;
    case 3: u.phone_bytes = string ? copy_small(value, u.phone) : 0; break;
    case 4: json_value_to_bool(value, &u.verified); break;
    case 5: json_value_to_bool(value, &u.mfa_enabled); break;
    // MFA_SMS (1 << 4): SMS is a backup method, also where authenticator_types is absent.
    case 6:
        if (json_value_to_u64(value, &flags) && (flags & 16))
            u.authenticators = static_cast<u8>(u.authenticators | kAccountAuthenticatorSms);
        break;
    case 7:
        if (value.kind == JsonValueKind::array)
            return json_visit_indexed_array(c.document, value, 4, authenticator_item, &u.authenticators).ok();
        break;
    default: break;
    }
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
    switch (member_index<"bio", "pronouns">(name)) {
    case 0: p.bio_bytes = static_cast<u16>(copy_text(value, p.bio, sizeof(p.bio))); break;
    case 1: p.pronouns_bytes = static_cast<u16>(copy_text(value, p.pronouns, sizeof(p.pronouns))); break;
    default: break;
    }
    return true;
}
bool profile_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<ProfileContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("user_profile")) && value.kind == JsonValueKind::object)
        return json_visit_indexed_object(c.document, value, 6, profile_text_member, &c).ok();
    return true;
}
bool code_member(void* opaque, JsonString name, JsonValue value) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("code")))
        json_value_to_u64(value, static_cast<u64*>(opaque));
    return true;
}
// The text operations (username … harvest_create), in operation order: the method, the number of texts, the ones that
// may be empty (bits) and what else the body or path holds. Their lines in kTextLines, in the same order: the path
// under /api/v9, then a space and the JSON keys of the texts ('|' separated) unless the texts only name the path.
enum class Extra : u8 { none, phone_reason, regenerate, key_id, connection, consent, harvest, create_key };
struct TextOperation {
    RestMethod method;
    u8 texts, optional;
    Extra extra;
};
constexpr RestMethod get = RestMethod::get, post = RestMethod::post, patch = RestMethod::patch, put = RestMethod::put,
                     remove = RestMethod::delete_method;
constexpr TextOperation kTextOperations[]{
    {patch, 2, 0, Extra::none},         {patch, 2, 0, Extra::none},
    {put, 0, 0, Extra::none},           {post, 1, 0, Extra::none},
    {patch, 3, 2, Extra::none},         {post, 1, 0, Extra::phone_reason},
    {post, 2, 0, Extra::none},          {post, 2, 0, Extra::phone_reason},
    {remove, 1, 0, Extra::phone_reason}, {post, 2, 0, Extra::none},
    {post, 0, 0, Extra::none},          {post, 0, 0, Extra::none},
    {post, 1, 0, Extra::none},          {post, 1, 0, Extra::none},
    {post, 2, 0, Extra::regenerate},    {get, 0, 0, Extra::none},
    {post, 3, 0, Extra::create_key},    {patch, 1, 0, Extra::key_id},
    {remove, 0, 0, Extra::key_id},      {post, 1, 0, Extra::none},
    {post, 1, 0, Extra::none},          {post, 3, 0, Extra::none},
    {post, 1, 0, Extra::none},          {get, 0, 0, Extra::none},
    {patch, 2, 0, Extra::connection},   {remove, 2, 0, Extra::connection},
    {get, 0, 0, Extra::none},           {post, 0, 0, Extra::consent},
    {get, 0, 0, Extra::none},           {post, 0, 0, Extra::harvest},
};
constexpr char kTextLines[] = "/users/@me username|password\0"
                              "/users/@me password|new_password\0"
                              "/users/@me/email\0"
                              "/users/@me/email/verify-code code\0"
                              "/users/@me email|email_token|password\0"
                              "/users/@me/phone phone\0"
                              "/phone-verifications/verify phone|code\0"
                              "/users/@me/phone phone_token|password\0"
                              "/users/@me/phone password\0"
                              "/users/@me/mfa/totp/enable code|secret\0"
                              "/users/@me/mfa/totp/disable\0"
                              "/users/@me/mfa/sms/enable\0"
                              "/users/@me/mfa/sms/disable password\0"
                              "/auth/verify/view-backup-codes-challenge password\0"
                              "/users/@me/mfa/codes-verification key|nonce\0"
                              "/users/@me/mfa/webauthn/credentials\0"
                              "/users/@me/mfa/webauthn/credentials name|ticket|credential\0"
                              "/users/@me/mfa/webauthn/credentials/ name\0"
                              "/users/@me/mfa/webauthn/credentials/\0"
                              "/users/@me/disable password\0"
                              "/users/@me/delete password\0"
                              "/mfa/finish ticket|mfa_type|data\0"
                              "/auth/mfa/sms/send ticket\0"
                              "/users/@me/connections\0"
                              "/users/@me/connections/\0"
                              "/users/@me/connections/\0"
                              "/users/@me/consent\0"
                              "/users/@me/consent\0"
                              "/users/@me/harvest\0"
                              "/users/@me/harvest\0";
constexpr u32 kFirstTextOperation = static_cast<u32>(Op::username);
constexpr u32 kTextOperationCount = sizeof(kTextOperations) / sizeof(kTextOperations[0]);
static_assert(kTextOperationCount == kAccountSettingsOperationCount - kFirstTextOperation &&
              name_list_count(kTextLines) == kTextOperationCount);
// harvest_create's option bits, in Userdoccers' harvest backend order.
constexpr char kHarvestBackends[] = "Account\0Analytics\0Activities\0Ads\0Messages\0Servers\0Zendesk\0";
constexpr u8 kHarvestAll = (1u << name_list_count(kHarvestBackends)) - 1;

const TextOperation* text_operation(Op operation) noexcept {
    const u32 index = static_cast<u32>(operation) - kFirstTextOperation;
    return index < kTextOperationCount ? &kTextOperations[index] : nullptr;
}
// A text operation's path under /api/v9.
Utf8View text_path(Op operation) noexcept {
    const Utf8View line = name_list_at(kTextLines, static_cast<u32>(operation) - kFirstTextOperation);
    u32 bytes = 0;
    while (bytes < line.bytes && line.data[bytes] != ' ')
        ++bytes;
    return {line.data, bytes};
}
// Its keys: '|' separated, ending at NUL; empty when its texts only name the path.
const char* text_keys(Op operation) noexcept {
    const Utf8View path = text_path(operation);
    return path.data + path.bytes + (path.data[path.bytes] == ' ');
}
// Comma-separated session hashes, 1 to 64 of them.
bool hashes_valid(ConstSlice<u8> hashes) noexcept {
    u32 start = 0, count = 0;
    for (u32 i = 0; i <= hashes.count; ++i) {
        if (i < hashes.count && hashes[i] != ',')
            continue;
        if (!hash_valid({hashes.data + start, i - start}) || ++count > 64)
            return false;
        start = i + 1;
    }
    return hashes.count != 0;
}
bool mfa_valid(const Utf8View& token) noexcept {
    if (token.bytes > kAccountSettingsMfaTokenBytes || (!token.data && token.bytes))
        return false;
    for (u32 i = 0; i < token.bytes; ++i)
        if (static_cast<u8>(token.data[i]) <= 0x20 || static_cast<u8>(token.data[i]) >= 0x7F)
            return false;
    return true;
}
bool text_request_valid(const AccountSettingsRequest& r, const TextOperation& spec) noexcept {
    const u32 count = r.texts.count;
    if (r.payload.count || r.required_data_version || r.field != AccountProfileField::pronouns ||
        (!r.texts.data && count) || (count != spec.texts && !(spec.extra == Extra::create_key && !count)))
        return false;
    for (u32 i = 0; i < count; ++i) {
        const Utf8View text = r.texts[i];
        if (text.bytes > kAccountSettingsTextBytes || (!text.data && text.bytes) || !utf8_validate(text).valid ||
            (!text.bytes && !(spec.optional & (1u << i))))
            return false;
    }
    switch (spec.extra) {
    case Extra::regenerate:
        return !r.id && r.option <= 1;
    case Extra::key_id:
        return r.id && !r.option;
    case Extra::connection:
        return r.operation == Op::connection_delete
                   ? !r.id && !r.option
                   : r.id <= 1 && r.option <= static_cast<u8>(AccountConnectionField::metadata_visibility);
    case Extra::consent:
        return r.id <= 1 && r.option <= static_cast<u8>(AccountConsent::personalization);
    case Extra::harvest:
        return !r.id && r.option && !(r.option & ~kHarvestAll);
    default:
        return !r.id && !r.option;
    }
}

// The body of a text operation: its texts under their keys, then its other members.
bool write_text_body(GuardedWriter& w, const AccountSettingsRequest& r, const TextOperation& spec) noexcept {
    w.put_byte('{');
    bool first = true;
    const auto member = [&](const char* key) noexcept {
        w.put_text(first ? "\"" : ",\"");
        w.put_text(key);
        w.put_text("\":");
        first = false;
    };
    const char* key = text_keys(r.operation);
    for (u32 i = 0; *key && i < r.texts.count; ++i) {
        u32 length = 0;
        while (key[length] && key[length] != '|')
            ++length;
        if (r.texts[i].bytes) {
            w.put_text(first ? "\"" : ",\"");
            w.put_bytes({key, length});
            w.put_text("\":");
            first = false;
            w.put_json(r.texts[i]);
        }
        key += length + (key[length] == '|');
    }
    switch (spec.extra) {
    case Extra::phone_reason:
        member("change_phone_reason");
        w.put_text("\"user_settings_update\"");
        break;
    case Extra::regenerate:
        member("regenerate");
        w.put_text(json_bool(r.option != 0));
        break;
    case Extra::connection: {
        const auto field = static_cast<AccountConnectionField>(r.option);
        member(field == AccountConnectionField::visibility      ? "visibility"
               : field == AccountConnectionField::show_activity ? "show_activity"
                                                                : "metadata_visibility");
        if (field == AccountConnectionField::show_activity)
            w.put_text(json_bool(r.id != 0));
        else
            w.put_number(r.id);
        break;
    }
    case Extra::consent: {
        const char* type = r.option ? "\"personalization\"" : "\"usage_statistics\"";
        w.put_text("\"grant\":[");
        if (r.id)
            w.put_text(type);
        w.put_text("],\"revoke\":[");
        if (!r.id)
            w.put_text(type);
        w.put_byte(']');
        break;
    }
    case Extra::harvest: {
        member("backends");
        w.put_byte('[');
        u32 bit = 1, written = 0;
        for (NameList list{kHarvestBackends}; list; list.next(), bit <<= 1)
            if (r.option & bit) {
                w.put_text(written++ ? ",\"" : "\"");
                w.put_text(list.at);
                w.put_byte('"');
            }
        w.put_byte(']');
        break;
    }
    default:
        break;
    }
    w.put_byte('}');
    return w.ok;
}
// Whether a text operation sends a body: its texts have keys, or it has members of its own.
bool text_body(const AccountSettingsRequest& r, const TextOperation& spec) noexcept {
    return (*text_keys(r.operation) && r.texts.count) || spec.extra == Extra::phone_reason ||
           spec.extra == Extra::regenerate || spec.extra == Extra::consent || spec.extra == Extra::harvest ||
           spec.extra == Extra::create_key || r.operation == Op::connection_patch;
}
} // namespace

bool account_settings_primary(AccountSettingsOperation operation) noexcept {
    return operation == Op::settings_fetch || operation == Op::settings_patch;
}

bool account_settings_request_valid(const AccountSettingsRequest& r) noexcept {
    if (!byte_range_valid(r.payload) || !mfa_valid(r.mfa) ||
        (r.required_data_version_present && r.operation != Op::settings_patch) ||
        static_cast<u8>(r.field) > static_cast<u8>(AccountProfileField::bio))
        return false;
    if (const auto* spec = text_operation(r.operation))
        return text_request_valid(r, *spec);
    if (r.texts.count || r.option)
        return false;
    switch (r.operation) {
    case Op::settings_patch:
        return user_settings_proto_patch_valid(r.payload) && !r.id;
    case Op::session_logout:
        return hashes_valid(r.payload) && !r.id && !r.required_data_version;
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
    default:
        return false;
    }
}

AccountSettingsWriteResult account_settings_write_request(const AccountSettingsRequest& r, Slice<u8> target,
                                                          Slice<u8> body) noexcept {
    AccountSettingsWriteResult result{0, 0, 0, RestMethod::get, W::invalid_input};
    if (!account_settings_request_valid(r) || !byte_range_valid({body.data, body.count}))
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
    if (const auto* spec = text_operation(r.operation)) {
        result.route_id = kAccountActionRouteId + static_cast<u64>(r.operation);
        result.method = spec->method;
        if (!route_write(&route, "/api/v9", text_path(r.operation)))
            return fail_capacity();
        if (spec->extra == Extra::key_id && !route_write(&route, r.id))
            return fail_capacity();
        if (spec->extra == Extra::connection &&
            !route_write(&route, path_component(r.texts[0]), '/', path_component(r.texts[1])))
            return fail_capacity();
        if (text_body(r, *spec)) {
            GuardedWriter w{body};
            if (!write_text_body(w, r, *spec))
                return fail_capacity();
            at = w.finish();
        }
    } else
        switch (r.operation) {
        case Op::settings_fetch:
        case Op::settings_patch:
            path = utf8_literal("/api/v9/users/@me/settings-proto/1");
            result.route_id = kAccountSettingsRouteId;
            result.method = r.operation == Op::settings_fetch ? RestMethod::get : RestMethod::patch;
            if (r.operation == Op::settings_patch) {
                at = user_settings_proto_write_patch_json(r.payload, r.required_data_version, body,
                                                          r.required_data_version_present);
                if (!at)
                    return fail_capacity();
            }
            break;
        case Op::self_user:
            path = utf8_literal("/api/v9/users/@me");
            result.route_id = kAccountSelfRouteId;
            break;
        case Op::sessions:
        case Op::session_logout: {
            path = r.operation == Op::sessions ? utf8_literal("/api/v9/auth/sessions")
                                               : utf8_literal("/api/v9/auth/sessions/logout");
            result.route_id = kAccountSessionsRouteId;
            if (r.operation == Op::session_logout) {
                result.method = RestMethod::post;
                // Each comma becomes "," between quoted hashes.
                GuardedWriter w{body};
                w.put_text("{\"session_id_hashes\":[\"");
                for (u32 i = 0; i < r.payload.count; ++i)
                    if (r.payload[i] == ',')
                        w.put_text("\",\"");
                    else
                        w.put_byte(static_cast<char>(r.payload[i]));
                w.put_text("\"]}");
                at = w.finish();
                if (!at)
                    return fail_capacity();
            }
            break;
        }
        case Op::tokens:
        case Op::token_revoke:
            result.route_id = kAccountTokensRouteId;
            if (r.operation == Op::token_revoke) {
                result.method = RestMethod::delete_method;
                if (!route_write(&route, "/api/v9/oauth2/tokens/", r.id))
                    return fail_capacity();
            } else
                path = utf8_literal("/api/v9/oauth2/tokens");
            break;
        case Op::profile:
            result.route_id = kAccountProfileRouteId;
            if (!route_write(&route, "/api/v9/users/", r.id, "/profile"))
                return fail_capacity();
            break;
        case Op::profile_patch:
        case Op::display_name: {
            const bool name = r.operation == Op::display_name;
            path = name ? utf8_literal("/api/v9/users/@me") : utf8_literal("/api/v9/users/@me/profile");
            result.route_id = name ? kAccountSelfRouteId : kAccountProfileRouteId;
            result.method = RestMethod::patch;
            const Utf8View key = name ? utf8_literal("global_name")
                                 : r.field == AccountProfileField::pronouns ? utf8_literal("pronouns")
                                                                            : utf8_literal("bio");
            // An empty display name resets to the username; profile text clears to "".
            const bool null_value = name && !r.payload.count;
            ProtocolWriter w{body, 0};
            if (!protocol_write(&w, "{\"", key, "\":", or_null(!null_value, json_text(text(r.payload))), '}'))
                return fail_capacity();
            at = w.used;
            break;
        }
        default:
            return result;
        }
    if (path.bytes && !route_write(&route, path))
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
    SelfContext c{};
    if (!scan(body, structurals, &c.document) ||
        !json_visit_root_object(body, c.document.structurals, 8, self_member, &c).ok() || !c.user.username_bytes)
        return false;
    copy_value(user, c.user);
    return true;
}

bool account_settings_decode_profile(Utf8View body, Slice<u32> structurals, AccountProfileText* profile) noexcept {
    ProfileContext c{};
    if (!scan(body, structurals, &c.document))
        return false;
    AccountProfileText decoded{};
    c.profile = &decoded;
    if (!json_visit_root_object(body, c.document.structurals, 8, profile_root, &c).ok())
        return false;
    copy_value(profile, decoded);
    return true;
}

u64 account_settings_error_code(Utf8View body, Slice<u32> structurals) noexcept {
    JsonIndexedDocument document{};
    u64 code = 0;
    if (!scan(body, structurals, &document) ||
        !json_visit_root_object(body, document.structurals, 8, code_member, &code).ok())
        return 0;
    return code;
}

namespace {
// A list of objects: the root array, or the array under `field` of the root object. Each item is decoded by
// `member` into a zeroed record of `size` bytes and kept when `keep` accepts it.
struct ListDecode {
    JsonIndexedDocument document;
    u8* output;
    u32 size, capacity, count;
    JsonMemberVisitor member;
    bool (*keep)(const void*) noexcept;
    Utf8View field;
    JsonValue array;
    bool seen;
};
bool list_item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<ListDecode*>(opaque);
    if (value.kind != JsonValueKind::object)
        return false;
    if (c.count >= c.capacity)
        return true;
    alignas(8) u8 item[sizeof(AccountConnection)];
    static_assert(sizeof(AccountConnection) >= sizeof(AccountSecurityKey) &&
                  sizeof(AccountConnection) >= sizeof(AccountBackupCode));
    clear_bytes(item, c.size);
    if (!json_visit_indexed_object(c.document, value, 6, c.member, item).ok())
        return false;
    if (c.keep(item))
        copy_bytes(c.output + c.size * c.count++, item, c.size);
    return true;
}
bool list_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<ListDecode*>(opaque);
    if (json_string_equals_ascii(name, c.field)) {
        if (value.kind != JsonValueKind::array || c.seen)
            return false;
        c.array = value;
        c.seen = true;
    }
    return true;
}
u32 decode_list(Utf8View body, Slice<u32> structurals, ListDecode& c) noexcept {
    if (!scan(body, structurals, &c.document))
        return ~0u;
    const bool ok = c.field.bytes ? json_visit_root_object(body, c.document.structurals, 8, list_root, &c).ok() &&
                                        c.seen &&
                                        json_visit_indexed_array(c.document, c.array, 8, list_item, &c).ok()
                                  : json_visit_root_array(body, c.document.structurals, 8, list_item, &c).ok();
    return ok ? c.count : ~0u;
}

bool backup_code_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& code = *static_cast<AccountBackupCode*>(opaque);
    switch (member_index<"code", "consumed">(name)) {
    case 0: code.code_bytes = value.kind == JsonValueKind::string ? copy_small(value, code.code) : 0; break;
    case 1: json_value_to_bool(value, &code.consumed); break;
    default: break;
    }
    return true;
}
bool backup_code_keep(const void* item) noexcept {
    return static_cast<const AccountBackupCode*>(item)->code_bytes != 0;
}
bool security_key_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& key = *static_cast<AccountSecurityKey*>(opaque);
    switch (member_index<"id", "name">(name)) {
    case 0: return json_value_to_snowflake(value, &key.id);
    case 1: key.name_bytes = value.kind == JsonValueKind::string ? copy_small(value, key.name) : 0; break;
    default: break;
    }
    return true;
}
bool security_key_keep(const void* item) noexcept {
    return static_cast<const AccountSecurityKey*>(item)->id != 0;
}
bool connection_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& connection = *static_cast<AccountConnection*>(opaque);
    const bool string = value.kind == JsonValueKind::string;
    u64 number = 0;
    switch (member_index<"type", "id", "name", "verified", "revoked", "visibility", "show_activity",
                         "metadata_visibility">(name)) {
    case 0: connection.type_bytes = string ? copy_small(value, connection.type) : 0; break;
    case 1: connection.id_bytes = string ? copy_small(value, connection.id) : 0; break;
    case 2: connection.name_bytes = string ? copy_small(value, connection.name) : 0; break;
    case 3: json_value_to_bool(value, &connection.verified); break;
    case 4: json_value_to_bool(value, &connection.revoked); break;
    case 5: connection.visible = json_value_to_u64(value, &number) && number == 1; break;
    case 6: json_value_to_bool(value, &connection.show_activity); break;
    case 7: connection.metadata_visible = json_value_to_u64(value, &number) && number == 1; break;
    default: break;
    }
    return true;
}
// A connection's type and id name its routes; one whose id was cut short is left out.
bool connection_keep(const void* item) noexcept {
    const auto& connection = *static_cast<const AccountConnection*>(item);
    return connection.type_bytes && connection.id_bytes && connection.id_bytes < sizeof(connection.id) - 4;
}

struct ConsentContext {
    JsonIndexedDocument document;
    bool* target;
    AccountConsents consents;
};
bool consent_status_member(void* opaque, JsonString name, JsonValue value) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("consented")))
        json_value_to_bool(value, static_cast<bool*>(opaque));
    return true;
}
bool consent_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<ConsentContext*>(opaque);
    bool* target = json_string_equals_ascii(name, utf8_literal("usage_statistics")) ? &c.consents.usage_statistics
                   : json_string_equals_ascii(name, utf8_literal("personalization")) ? &c.consents.personalization
                                                                                     : nullptr;
    return !target || value.kind != JsonValueKind::object ||
           json_visit_indexed_object(c.document, value, 4, consent_status_member, target).ok();
}
bool harvest_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& harvest = *static_cast<AccountHarvest*>(opaque);
    u64 status = 0;
    switch (member_index<"created_at", "status">(name)) {
    case 0: json_value_to_timestamp(value, &harvest.created_ms); break;
    case 1:
        if (json_value_to_u64(value, &status) && status <= 4)
            harvest.status = static_cast<u8>(status);
        break;
    default: break;
    }
    return true;
}

struct MfaContext {
    JsonIndexedDocument document;
    AccountMfaChallenge challenge;
    Slice<char> webauthn;
    u32 webauthn_bytes;
    bool seen;
};
bool mfa_method_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<MfaContext*>(opaque);
    JsonString text{};
    if (json_string_equals_ascii(name, utf8_literal("type")) && json_value_as_string(value, &text)) {
        const u32 type = member_index<"password", "webauthn", "totp", "sms", "backup">(text);
        if (type < 5)
            c.challenge.methods = static_cast<u8>(c.challenge.methods | (1u << type));
    } else if (json_string_equals_ascii(name, utf8_literal("challenge")) && json_value_as_string(value, &text)) {
        const auto decoded = json_string_decode_utf8(text, c.webauthn);
        c.webauthn_bytes = decoded.ok() ? decoded.bytes : 0;
    } else if (json_string_equals_ascii(name, utf8_literal("backup_codes_allowed"))) {
        bool allowed = false;
        if (json_value_to_bool(value, &allowed) && allowed)
            c.challenge.methods = static_cast<u8>(c.challenge.methods | kAccountMfaMethodBackup);
    }
    return true;
}
bool mfa_method_item(void* opaque, u32, JsonValue value) noexcept {
    return value.kind != JsonValueKind::object ||
           json_visit_indexed_object(static_cast<MfaContext*>(opaque)->document, value, 4, mfa_method_member, opaque)
               .ok();
}
bool mfa_request_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<MfaContext*>(opaque);
    JsonString text{};
    if (json_string_equals_ascii(name, utf8_literal("ticket")) && json_value_as_string(value, &text)) {
        const auto decoded = json_string_decode_utf8(text, {c.challenge.ticket, sizeof(c.challenge.ticket)});
        c.challenge.ticket_bytes = static_cast<u16>(decoded.ok() ? decoded.bytes : 0);
    } else if (json_string_equals_ascii(name, utf8_literal("methods")) && value.kind == JsonValueKind::array)
        return json_visit_indexed_array(c.document, value, 6, mfa_method_item, &c).ok();
    return true;
}
bool mfa_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<MfaContext*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("mfa")) || value.kind != JsonValueKind::object)
        return true;
    c.seen = true;
    return json_visit_indexed_object(c.document, value, 8, mfa_request_member, &c).ok();
}

struct MemberContext {
    Utf8View name;
    Slice<char> output;
    u32 bytes;
};
bool named_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<MemberContext*>(opaque);
    JsonString text{};
    if (c.bytes == ~0u && json_string_equals_ascii(name, c.name) && json_value_as_string(value, &text)) {
        const auto decoded = json_string_decode_utf8(text, c.output);
        c.bytes = decoded.ok() ? decoded.bytes : ~0u;
    }
    return true;
}

// The first field error's message: {"errors":{"field":{"_errors":[{"message":…}]}}}, at any depth.
struct MessageContext {
    JsonIndexedDocument document;
    Slice<char> output;
    u32 bytes, depth;
};
bool message_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<MessageContext*>(opaque);
    JsonString text{};
    if (!c.bytes && json_string_equals_ascii(name, utf8_literal("message")) && json_value_as_string(value, &text)) {
        const auto decoded = json_string_decode_utf8(text, c.output);
        c.bytes = decoded.ok() ? decoded.bytes : 0;
    }
    return true;
}
bool error_item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<MessageContext*>(opaque);
    return c.bytes || value.kind != JsonValueKind::object ||
           json_visit_indexed_object(c.document, value, 4, message_member, &c).ok();
}
bool error_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<MessageContext*>(opaque);
    if (c.bytes || c.depth >= 6)
        return true;
    if (json_string_equals_ascii(name, utf8_literal("_errors")) && value.kind == JsonValueKind::array)
        return json_visit_indexed_array(c.document, value, 4, error_item, &c).ok();
    if (value.kind != JsonValueKind::object)
        return true;
    ++c.depth;
    const bool ok = json_visit_indexed_object(c.document, value, 8, error_member, &c).ok();
    --c.depth;
    return ok;
}
bool error_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<MessageContext*>(opaque);
    return !json_string_equals_ascii(name, utf8_literal("errors")) || value.kind != JsonValueKind::object ||
           json_visit_indexed_object(c.document, value, 16, error_member, &c).ok();
}
// The list of `Item` records at the root array, or under `field` of the root object.
template <typename Item>
u32 decode_items(Utf8View body, Slice<u32> structurals, Slice<Item> output, JsonMemberVisitor member,
                 bool (*keep)(const void*) noexcept, Utf8View field = {}) noexcept {
    ListDecode c{};
    c.output = reinterpret_cast<u8*>(output.data);
    c.size = sizeof(Item);
    c.capacity = output.count;
    c.member = member;
    c.keep = keep;
    c.field = field;
    return decode_list(body, structurals, c);
}
} // namespace

u32 account_settings_decode_backup_codes(Utf8View body, Slice<u32> structurals,
                                         Slice<AccountBackupCode> output) noexcept {
    return decode_items(body, structurals, output, backup_code_member, backup_code_keep, utf8_literal("backup_codes"));
}
u32 account_settings_decode_security_keys(Utf8View body, Slice<u32> structurals,
                                          Slice<AccountSecurityKey> output) noexcept {
    return decode_items(body, structurals, output, security_key_member, security_key_keep);
}
u32 account_settings_decode_connections(Utf8View body, Slice<u32> structurals,
                                        Slice<AccountConnection> output) noexcept {
    return decode_items(body, structurals, output, connection_member, connection_keep);
}

bool account_settings_decode_consents(Utf8View body, Slice<u32> structurals, AccountConsents* consents) noexcept {
    ConsentContext c{};
    if (!scan(body, structurals, &c.document) ||
        !json_visit_root_object(body, c.document.structurals, 8, consent_member, &c).ok())
        return false;
    *consents = c.consents;
    return true;
}

bool account_settings_decode_harvest(Utf8View body, Slice<u32> structurals, AccountHarvest* harvest) noexcept {
    JsonIndexedDocument document{};
    AccountHarvest decoded{};
    if (!scan(body, structurals, &document) ||
        !json_visit_root_object(body, document.structurals, 8, harvest_member, &decoded).ok() || !decoded.created_ms)
        return false;
    *harvest = decoded;
    return true;
}

bool account_settings_decode_mfa_challenge(Utf8View body, Slice<u32> structurals, AccountMfaChallenge* challenge,
                                           Slice<char> webauthn, u32* webauthn_bytes) noexcept {
    MfaContext c{};
    c.webauthn = webauthn;
    if (!scan(body, structurals, &c.document) ||
        !json_visit_root_object(body, c.document.structurals, 10, mfa_root, &c).ok() || !c.seen ||
        !c.challenge.ticket_bytes || !c.challenge.methods)
        return false;
    copy_value(challenge, c.challenge);
    if (webauthn_bytes)
        *webauthn_bytes = (c.challenge.methods & kAccountAuthenticatorWebauthn) ? c.webauthn_bytes : 0;
    return true;
}

u32 account_settings_decode_member(Utf8View body, Slice<u32> structurals, Utf8View name, Slice<char> output) noexcept {
    JsonIndexedDocument document{};
    MemberContext c{name, output, ~0u};
    if (!scan(body, structurals, &document) ||
        !json_visit_root_object(body, document.structurals, 8, named_member, &c).ok())
        return ~0u;
    return c.bytes;
}

u32 account_settings_error_message(Utf8View body, Slice<u32> structurals, Slice<char> output) noexcept {
    MessageContext c{};
    c.output = output;
    if (!scan(body, structurals, &c.document) ||
        !json_visit_root_object(body, c.document.structurals, 20, error_root, &c).ok())
        return 0;
    // No field error: the body's own message.
    if (!c.bytes)
        json_visit_root_object(body, c.document.structurals, 20, message_member, &c);
    return c.bytes;
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
