#include "maicord/protocol/account_requirement.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/protocol/user_auth.hpp"
namespace maicord {
namespace {
struct Update {
    AccountRequirement value;
    bool seen;
};
bool action_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& update = *static_cast<Update*>(context);
    if (!json_string_equals_ascii(name, utf8_literal("required_action")))
        return true;
    if (update.seen)
        return false;
    update.seen = true;
    update.value = decode_account_requirement(value);
    return true;
}
bool error_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& result = *static_cast<ApiRejection*>(context);
    if (json_string_equals_ascii(name, utf8_literal("code"))) {
        u64 code = 0;
        if (result.has_code || !json_value_to_u64(value, &code) || code > 0xffffffffull)
            return false;
        result.code = static_cast<u32>(code);
        result.has_code = true;
    } else if (json_string_equals_ascii(name, utf8_literal("captcha_key")) ||
               json_string_equals_ascii(name, utf8_literal("captcha_sitekey")) ||
               json_string_equals_ascii(name, utf8_literal("captcha_rqtoken"))) {
        result.captcha |= value.kind != JsonValueKind::null_value;
    }
    return true;
}
bool nonempty_string(JsonValue value, u32 maximum) noexcept {
    JsonString text{};
    if (!json_value_as_string(value, &text))
        return false;
    const auto measured = json_string_decode_utf8(text, {});
    return measured.error == JsonStringDecodeError::output_too_small && measured.bytes &&
           measured.bytes <= maximum;
}
struct MfaMethod {
    u8 fields;
};
bool mfa_method_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& method = *static_cast<MfaMethod*>(context);
    const u8 field = json_string_equals_ascii(name, utf8_literal("type"))                   ? 1
                     : json_string_equals_ascii(name, utf8_literal("challenge"))            ? 2
                     : json_string_equals_ascii(name, utf8_literal("backup_codes_allowed")) ? 4
                                                                                            : 0;
    if (!field)
        return true;
    if (method.fields & field)
        return false;
    method.fields |= field;
    return field == 1   ? nonempty_string(value, 64)
           : field == 2 ? nonempty_string(value, kUserAuthMaximumWebauthnChallengeBytes)
                        : value.kind == JsonValueKind::boolean;
}
bool mfa_method(void* context, u32, JsonValue value) noexcept {
    if (value.kind != JsonValueKind::object)
        return false;
    const auto& document = *static_cast<const JsonIndexedDocument*>(context);
    MfaMethod method{};
    return json_visit_indexed_object(document, value, 16, mfa_method_member, &method).ok() &&
           (method.fields & 1);
}
struct MfaRequest {
    const JsonIndexedDocument* document;
    u8 fields;
};
bool mfa_request_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& request = *static_cast<MfaRequest*>(context);
    const u8 field = json_string_equals_ascii(name, utf8_literal("ticket"))    ? 1
                     : json_string_equals_ascii(name, utf8_literal("methods")) ? 2
                                                                               : 0;
    if (!field)
        return true;
    if (request.fields & field)
        return false;
    request.fields |= field;
    if (field == 1)
        return nonempty_string(value, kUserAuthMaximumTicketBytes);
    if (value.kind != JsonValueKind::array)
        return false;
    const auto methods = json_visit_indexed_array(*request.document, value, 16, mfa_method,
                                                  const_cast<JsonIndexedDocument*>(request.document));
    return methods.ok() && methods.member_count;
}
struct MfaRejection {
    ApiRejection rejection;
    const JsonIndexedDocument* document;
    bool mfa;
};
bool mfa_rejection_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& result = *static_cast<MfaRejection*>(context);
    if (!json_string_equals_ascii(name, utf8_literal("mfa")))
        return error_member(&result.rejection, name, value);
    if (result.mfa || value.kind != JsonValueKind::object)
        return false;
    result.mfa = true;
    MfaRequest request{result.document};
    return json_visit_indexed_object(*result.document, value, 16, mfa_request_member, &request).ok() &&
           request.fields == 3;
}
} // namespace
AccountRequirement decode_account_requirement(JsonValue value) noexcept {
    if (value.kind == JsonValueKind::null_value)
        return AccountRequirement::none;
    JsonString text{};
    if (!json_value_as_string(value, &text))
        return AccountRequirement::other;
    static constexpr Utf8View names[] = {utf8_literal("REQUIRE_CAPTCHA"),
                                  utf8_literal("REQUIRE_VERIFIED_EMAIL"),
                                  utf8_literal("REQUIRE_VERIFIED_PHONE"),
                                  utf8_literal("REQUIRE_REVERIFIED_EMAIL"),
                                  utf8_literal("REQUIRE_REVERIFIED_PHONE"),
                                  utf8_literal("REQUIRE_VERIFIED_EMAIL_OR_VERIFIED_PHONE"),
                                  utf8_literal("REQUIRE_REVERIFIED_EMAIL_OR_VERIFIED_PHONE"),
                                  utf8_literal("REQUIRE_VERIFIED_EMAIL_OR_REVERIFIED_PHONE"),
                                  utf8_literal("REQUIRE_REVERIFIED_EMAIL_OR_REVERIFIED_PHONE"),
                                  utf8_literal("AGREEMENTS"),
                                  utf8_literal("REQUIRE_SAFETY_FLOWS")};
    for (u32 n = 0; n < sizeof(names) / sizeof(names[0]); ++n)
        if (json_string_equals_ascii(text, names[n]))
            return static_cast<AccountRequirement>(n + 1);
    return AccountRequirement::other;
}
bool account_requirement_update(JsonValue data, Slice<u32> scratch, AccountRequirement* output) noexcept {
    if (data.kind != JsonValueKind::object || data.source.bytes > 16384)
        return false;
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok())
        return false;
    Update update{};
    if (!json_visit_value_object(data, {scratch.data, scan.count}, 16, action_member, &update).ok() ||
        !update.seen)
        return false;
    *output = update.value;
    return true;
}
Utf8View account_requirement_notice(AccountRequirement value) noexcept {
    switch (value) {
    case AccountRequirement::none:
        return {};
    case AccountRequirement::captcha:
        return utf8_literal("Complete Discord's verification before sending.");
    case AccountRequirement::verify_email:
    case AccountRequirement::reverify_email:
        return utf8_literal("Verify your email in Discord before sending.");
    case AccountRequirement::verify_phone:
    case AccountRequirement::reverify_phone:
        return utf8_literal("Verify your phone in Discord before sending.");
    case AccountRequirement::agreements:
        return utf8_literal("Review Discord's required agreements before sending.");
    case AccountRequirement::safety_flows:
        return utf8_literal("Resolve the account restriction in Discord before sending.");
    default:
        return utf8_literal("Verify your account in Discord before sending.");
    }
}
bool api_rejection_decode(Utf8View body, Slice<u32> scratch, ApiRejection* output) noexcept {
    reset_value(output);
    if (!body.bytes || body.bytes > 16384)
        return false;
    const auto scan = json_scan_structurals_avx1(body, scratch);
    if (!scan.ok())
        return false;
    ApiRejection parsed{};
    if (!json_visit_root_object(body, {scratch.data, scan.count}, 16, error_member, &parsed).ok())
        return false;
    *output = parsed;
    return true;
}
bool api_rejection_is_mfa_challenge(Utf8View body, Slice<u32> scratch) noexcept {
    if (!body.bytes || body.bytes > 16384)
        return false;
    const auto scan = json_scan_structurals_avx1(body, scratch);
    if (!scan.ok())
        return false;
    const JsonIndexedDocument document{body, {scratch.data, scan.count}};
    MfaRejection parsed{{}, &document};
    return json_visit_root_object(body, document.structurals, 16, mfa_rejection_member, &parsed).ok() &&
           parsed.mfa && parsed.rejection.has_code && parsed.rejection.code == 60003 && !parsed.rejection.captcha;
}
Utf8View api_rejection_notice(ApiRejection value) noexcept {
    if (value.captcha)
        return utf8_literal("Discord requires verification before this action.");
    if (!value.has_code)
        return {};
    switch (value.code) {
    case 40002:
        return utf8_literal("Discord requires account verification before this action.");
    case 40004:
        return utf8_literal("Discord has temporarily disabled sending messages.");
    case 20026:
        return utf8_literal("Discord restricted this action. Check your account in Discord.");
    case 50013:
        return utf8_literal("Discord denied permission for this action.");
    default:
        return {};
    }
}
} // namespace maicord
