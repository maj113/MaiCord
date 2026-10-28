#include "maicord/protocol/user_auth.hpp"
#include "maicord/protocol/login_continuations.hpp"
#include "maicord/protocol/browser_sign_in.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/protocol/rest.hpp"

#include "json_field_lists.hpp"
#include "json_write.hpp"

namespace maicord {
namespace {

using protocol_detail::EncodedString;
using protocol_detail::encoded_string;
using protocol_detail::write_encoded;
using protocol_detail::write_literal;

struct DecodedField {
    JsonString value;
    u32 bytes;
    bool present;
};

struct RawField {
    JsonValue value;
    u32 bytes;
    bool present;
};

enum ResponseField : u32 {
    response_field_token = 1u << 0,
    response_field_mfa = 1u << 1,
    response_field_ticket = 1u << 2,
    response_field_sms = 1u << 3,
    response_field_webauthn = 1u << 4,
    response_field_backup = 1u << 5,
    response_field_totp = 1u << 6,
    response_field_user_id = 1u << 7,
    response_field_login_instance = 1u << 8,
    response_field_error_code = 1u << 9,
    response_field_captcha_key = 1u << 10,
    response_field_captcha_service = 1u << 11,
    response_field_captcha_sitekey = 1u << 12,
    response_field_captcha_rqdata = 1u << 13,
    response_field_captcha_rqtoken = 1u << 14,
    response_field_captcha_session = 1u << 15,
    response_field_captcha_invisible = 1u << 16,
    response_field_captcha_user_flow = 1u << 17,
    response_field_suspended_token = 1u << 18,
    response_field_recovery_method = 1u << 19,
    response_field_user = 1u << 20,
};

struct ResponseContext {
    DecodedField token;
    DecodedField ticket;
    DecodedField login_instance;
    RawField webauthn_challenge;
    DecodedField captcha_service;
    DecodedField captcha_sitekey;
    DecodedField captcha_rqdata;
    DecodedField captcha_rqtoken;
    DecodedField captcha_session;
    DecodedField captcha_user_flow;
    DecodedField suspended_token;
    u64 user_id;
    u64 error_code;
    u32 fields;
    u32 error_offset;
    UserAuthDecodeError error;
    u8 capabilities;
    bool mfa;
    bool captcha_invisible;
};

bool captcha_headers_size(const UserAuthCaptchaHeaders& headers, bool allow_empty, u32* bytes) noexcept {
    if ((!allow_empty && headers.key.bytes == 0) ||
        (headers.key.bytes == 0 && (headers.rqtoken.bytes != 0 || headers.session_id.bytes != 0)) ||
        !ascii_graphic_string(headers.key, kUserAuthMaximumCaptchaKeyBytes) ||
        !ascii_graphic_string(headers.rqtoken, kUserAuthMaximumCaptchaTokenBytes) ||
        !ascii_graphic_string(headers.session_id, kUserAuthMaximumCaptchaSessionBytes))
        return false;
    constexpr u32 key_prefix = sizeof("X-Captcha-Key: \r\n") - 1;
    constexpr u32 token_prefix = sizeof("X-Captcha-Rqtoken: \r\n") - 1;
    constexpr u32 session_prefix = sizeof("X-Captcha-Session-Id: \r\n") - 1;
    const u64 required =
        (headers.key.bytes != 0 ? static_cast<u64>(key_prefix) + headers.key.bytes : 0) +
        (headers.rqtoken.bytes != 0 ? static_cast<u64>(token_prefix) + headers.rqtoken.bytes : 0) +
        (headers.session_id.bytes != 0 ? static_cast<u64>(session_prefix) + headers.session_id.bytes : 0);
    if (required > 0xFFFFFFFFull)
        return false;
    *bytes = static_cast<u32>(required);
    return true;
}

UserAuthWriteError string_error(Utf8View value, u32 maximum_bytes) noexcept {
    if (!value.data || value.bytes == 0)
        return UserAuthWriteError::invalid_input;
    return value.bytes > maximum_bytes ? UserAuthWriteError::value_too_large : UserAuthWriteError::invalid_utf8;
}

bool mark_field(ResponseContext* context, u32 field, u32 offset) noexcept {
    if ((context->fields & field) != 0) {
        context->error = UserAuthDecodeError::duplicate_field;
        context->error_offset = offset;
        return false;
    }
    context->fields |= field;
    return true;
}

bool reject_field(ResponseContext* context, u32 offset) noexcept {
    context->error = UserAuthDecodeError::invalid_field;
    context->error_offset = offset;
    return false;
}

// A string member; an optional one also accepts null and stays absent.
bool capture_string(ResponseContext* context, u32 field, JsonString name, JsonValue value, DecodedField* output,
                    bool optional = false) noexcept {
    if (!mark_field(context, field, name.source_offset))
        return false;
    if (optional && value.kind == JsonValueKind::null_value)
        return true;
    if (!json_value_as_string(value, &output->value))
        return reject_field(context, value.source_offset);
    output->present = true;
    return true;
}

// A boolean member; an optional one also accepts null as false.
bool capture_bool(ResponseContext* context, u32 field, JsonString name, JsonValue value, bool* output,
                  bool optional = false) noexcept {
    if (!mark_field(context, field, name.source_offset))
        return false;
    if (optional && value.kind == JsonValueKind::null_value) {
        *output = false;
        return true;
    }
    return json_value_to_bool(value, output) || reject_field(context, value.source_offset);
}

bool visit_response(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ResponseContext*>(opaque);
    u8 capability = user_auth_mfa_capability_none;
    u32 field = 0;
    switch (protocol_detail::member_index<"method", "user", "token", "ticket", "login_instance_id", "captcha_service",
                                          "captcha_sitekey", "captcha_rqdata", "captcha_rqtoken", "captcha_session_id",
                                          "user_flow", "suspended_user_token", "mfa", "should_serve_invisible", "totp",
                                          "sms", "backup", "webauthn", "captcha_key", "user_id", "code">(name)) {
    case 0: return mark_field(context, response_field_recovery_method, name.source_offset);
    case 1: return mark_field(context, response_field_user, name.source_offset);
    case 2: return capture_string(context, response_field_token, name, value, &context->token);
    case 3: return capture_string(context, response_field_ticket, name, value, &context->ticket);
    case 4: return capture_string(context, response_field_login_instance, name, value, &context->login_instance, true);
    case 5: return capture_string(context, response_field_captcha_service, name, value, &context->captcha_service, true);
    case 6: return capture_string(context, response_field_captcha_sitekey, name, value, &context->captcha_sitekey, true);
    case 7: return capture_string(context, response_field_captcha_rqdata, name, value, &context->captcha_rqdata, true);
    case 8: return capture_string(context, response_field_captcha_rqtoken, name, value, &context->captcha_rqtoken, true);
    case 9: return capture_string(context, response_field_captcha_session, name, value, &context->captcha_session, true);
    case 10:
        return capture_string(context, response_field_captcha_user_flow, name, value, &context->captcha_user_flow, true);
    case 11: return capture_string(context, response_field_suspended_token, name, value, &context->suspended_token);
    case 12: return capture_bool(context, response_field_mfa, name, value, &context->mfa);
    case 13:
        return capture_bool(context, response_field_captcha_invisible, name, value, &context->captcha_invisible, true);
    case 14:
        capability = user_auth_mfa_capability_totp;
        field = response_field_totp;
        break;
    case 15:
        capability = user_auth_mfa_capability_sms;
        field = response_field_sms;
        break;
    case 16:
        capability = user_auth_mfa_capability_backup;
        field = response_field_backup;
        break;
    case 17:
        if (!mark_field(context, response_field_webauthn, name.source_offset))
            return false;
        if (value.kind == JsonValueKind::null_value)
            return true;
        if (value.kind != JsonValueKind::object && value.kind != JsonValueKind::string)
            return reject_field(context, value.source_offset);
        context->webauthn_challenge = RawField{value, 0, true};
        context->capabilities = static_cast<u8>(context->capabilities | user_auth_mfa_capability_webauthn);
        return true;
    case 18:
        if (!mark_field(context, response_field_captcha_key, name.source_offset))
            return false;
        return value.kind == JsonValueKind::array ? true : reject_field(context, value.source_offset);
    case 19: {
        if (!mark_field(context, response_field_user_id, name.source_offset))
            return false;
        JsonString string{};
        if (json_value_to_u64(value, &context->user_id))
            return true;
        return (json_value_as_string(value, &string) && json_string_to_u64(string, &context->user_id)) ||
               reject_field(context, value.source_offset);
    }
    case 20:
        if (!mark_field(context, response_field_error_code, name.source_offset) ||
            !json_value_to_u64(value, &context->error_code))
            return context->error == UserAuthDecodeError::duplicate_field ? false : reject_field(context, value.source_offset);
        return true;
    default: return true;
    }
    bool enabled = false;
    if (!capture_bool(context, field, name, value, &enabled, true))
        return false;
    if (enabled)
        context->capabilities = static_cast<u8>(context->capabilities | capability);
    return true;
}

UserAuthDecodeResult decode_failure(UserAuthDecodeError error, u32 offset = 0,
                                    JsonScanError scan_error = JsonScanError::none,
                                    JsonReadError read_error = JsonReadError::none) noexcept {
    return UserAuthDecodeResult{offset, error, scan_error, read_error};
}

// A response string must decode to between one byte and maximum_bytes.
UserAuthDecodeError measured_string(const JsonString& value, u32 maximum_bytes) noexcept {
    const auto measured = json_string_decode_utf8(value, {nullptr, 0});
    if ((measured.error != JsonStringDecodeError::none && measured.error != JsonStringDecodeError::output_too_small) ||
        !measured.bytes)
        return UserAuthDecodeError::invalid_field;
    return measured.bytes > maximum_bytes ? UserAuthDecodeError::value_too_large : UserAuthDecodeError::none;
}

UserAuthDecodeResult classify_response(u16 status_code, Utf8View body, Slice<u32> structural_scratch,
                                       u32 maximum_depth, ResponseContext* context,
                                       UserAuthResponse* output, bool recovery = false, u8 reset_mode = 0,
                                       u8 phone_mode = 0) noexcept {
    if (output)
        reset_value<UserAuthResponse>(output);
    if (!output || status_code < 200 || status_code > 599 || !body.data || body.bytes == 0 ||
        !structural_scratch.data || structural_scratch.count == 0 || maximum_depth == 0)
        return decode_failure(UserAuthDecodeError::invalid_input);

    const JsonScanResult scan = json_scan_structurals_avx1(body, structural_scratch);
    if (!scan.ok())
        return decode_failure(UserAuthDecodeError::scan_failed, scan.error_offset, scan.error);
    const JsonReadResult read = json_visit_root_object(
        body, ConstSlice<u32>{structural_scratch.data, scan.count}, maximum_depth, visit_response, context);
    if (!read.ok()) {
        if (read.error == JsonReadError::visitor_rejected && context->error != UserAuthDecodeError::none)
            return decode_failure(context->error, context->error_offset, JsonScanError::none, read.error);
        return decode_failure(UserAuthDecodeError::invalid_json, read.error_offset, JsonScanError::none, read.error);
    }

    if (phone_mode) {
        constexpr u32 auth_fields = response_field_mfa | response_field_ticket | response_field_sms |
            response_field_webauthn | response_field_backup | response_field_totp | response_field_user_id |
            response_field_login_instance | response_field_suspended_token | response_field_user;
        constexpr u32 captcha_fields = response_field_captcha_key | response_field_captcha_service |
            response_field_captcha_sitekey | response_field_captcha_rqdata | response_field_captcha_rqtoken |
            response_field_captcha_session | response_field_captcha_invisible | response_field_captcha_user_flow;
        if ((context->fields & auth_fields) ||
            (context->token.present && (phone_mode != 2 || status_code >= 300 ||
                (context->fields & captcha_fields))) ||
            (status_code < 300 && (context->fields & captcha_fields)))
            return decode_failure(UserAuthDecodeError::unexpected_response, body.bytes);
        if (phone_mode == 2 && status_code < 300 && !context->token.present)
            return decode_failure(UserAuthDecodeError::missing_required_field, body.bytes);
        if (context->token.present) {
            const auto error = measured_string(context->token.value, kUserAuthPhoneResetTokenBytes);
            if (error != UserAuthDecodeError::none)
                return decode_failure(error, body.bytes);
        }
    }
    if (reset_mode) {
        if (context->token.present && (status_code >= 300 || context->mfa || context->ticket.present ||
            (context->fields & response_field_captcha_key)))
            return decode_failure(UserAuthDecodeError::unexpected_response, body.bytes);
        const DecodedField* required[] = {&context->token, &context->ticket};
        for (const DecodedField* field : required) {
            if (!field->present)
                continue;
            const auto error = measured_string(field->value, kUserAuthMaximumTicketBytes);
            if (error != UserAuthDecodeError::none)
                return decode_failure(error, field->value.source_offset);
        }
        if (reset_mode == 2 && status_code < 300 && context->mfa)
            return decode_failure(UserAuthDecodeError::unexpected_response, body.bytes);
    }
    UserAuthResponse parsed{};
    const bool success = status_code >= 200 && status_code < 300;
    if ((context->fields & response_field_captcha_key) != 0) {
        parsed.kind = UserAuthResponseKind::captcha_required;
    } else if (phone_mode && status_code < 300) {
        parsed.kind = phone_mode == 2 ? UserAuthResponseKind::phone_verified_reset : UserAuthResponseKind::phone_code_sent;
    } else if (recovery && success) {
        if (context->token.present || context->mfa || context->ticket.present)
            return decode_failure(UserAuthDecodeError::unexpected_response, body.bytes);
        parsed.kind = UserAuthResponseKind::recovery_sent;
    } else if (success && context->token.present) {
        parsed.kind = UserAuthResponseKind::authenticated;
    } else if (success && context->mfa) {
        if (!context->ticket.present)
            return decode_failure(UserAuthDecodeError::missing_required_field, body.bytes);
        parsed.kind = UserAuthResponseKind::mfa_required;
    } else if (status_code >= 400) {
        parsed.kind = UserAuthResponseKind::rejected;
    } else {
        return decode_failure(UserAuthDecodeError::unexpected_response, body.bytes);
    }

    parsed.user_id = context->user_id;
    parsed.error_code = context->error_code;
    parsed.mfa_capabilities = context->capabilities;
    parsed.has_user_id = (context->fields & response_field_user_id) != 0;
    parsed.has_error_code = (context->fields & response_field_error_code) != 0;
    parsed.captcha_invisible = context->captcha_invisible;
    parsed.suspended = context->suspended_token.present;
    *output = parsed;
    return decode_failure(UserAuthDecodeError::none);
}

UserAuthDecodeError preflight_field(DecodedField* field, const Slice<char>& destination,
                                    bool require_nonempty) noexcept {
    if (!field->present)
        return UserAuthDecodeError::none;
    const JsonStringDecodeResult measured = json_string_decode_utf8(field->value, Slice<char>{nullptr, 0});
    if ((measured.error != JsonStringDecodeError::none && measured.error != JsonStringDecodeError::output_too_small) ||
        (require_nonempty && measured.bytes == 0))
        return UserAuthDecodeError::invalid_field;
    field->bytes = measured.bytes;
    return measured.bytes <= destination.count && (measured.bytes == 0 || destination.data)
               ? UserAuthDecodeError::none
               : UserAuthDecodeError::output_too_small;
}

void decode_field(const DecodedField& field, const Slice<char>& destination) noexcept {
    if (field.present)
        json_string_decode_utf8(field.value, destination);
}

Utf8View captcha_default_sitekey(const DecodedField& service) noexcept {
    char name[20];
    if (!service.present || service.bytes > sizeof(name) ||
        !json_string_decode_utf8(service.value, {name, sizeof(name)}).ok())
        return {};
    const Utf8View decoded{name, service.bytes};
    if (ascii_equal_ignore_case(decoded, utf8_literal("recaptcha")))
        return utf8_literal("6Lef5iQTAAAAAKeIvIY-DeexoO3gj7ryl9rLMEnn");
    if (ascii_equal_ignore_case(decoded, utf8_literal("recaptcha_enterprise")) ||
        ascii_equal_ignore_case(decoded, utf8_literal("recaptcha-enterprise")))
        return utf8_literal("6LeYqFcqAAAAAD6iZesmNgVulsO4PkpBdr6NVG6M");
    return {};
}

void copy_raw(const RawField& field, Slice<char> destination) noexcept {
    if (!field.present)
        return;
    if (field.value.kind != JsonValueKind::string) {
        copy_bytes(destination.data, field.value.source.data, field.bytes);
        return;
    }
    JsonString string{};
    if (json_value_as_string(field.value, &string))
        json_string_decode_utf8(string, destination);
}

UserAuthTransitionResult transition_failure(const UserAuthState* state, UserAuthTransitionError error) noexcept {
    return UserAuthTransitionResult{state->generation, error};
}

} // namespace

UserAuthWriteResult user_auth_write_password_request(const UserAuthPasswordRequest& request,
                                                     Slice<u8> output) noexcept {
    if (!output.data)
        return UserAuthWriteResult{0, UserAuthWriteError::invalid_input};
    EncodedString login{};
    EncodedString password{};
    EncodedString source{};
    if (!encoded_string(request.login, kUserAuthMaximumLoginBytes, &login))
        return UserAuthWriteResult{0, string_error(request.login, kUserAuthMaximumLoginBytes)};
    if (!encoded_string(request.password, kUserAuthMaximumPasswordBytes, &password))
        return UserAuthWriteResult{0, string_error(request.password, kUserAuthMaximumPasswordBytes)};
    if (!encoded_string(request.login_source, kUserAuthMaximumSourceBytes, &source))
        return UserAuthWriteResult{0, string_error(request.login_source, kUserAuthMaximumSourceBytes)};

    constexpr u32 fixed_bytes = sizeof("{\"login\":,\"password\":,\"undelete\":false,\"login_source\":}") - 1;
    const u64 required = static_cast<u64>(fixed_bytes) + login.bytes + password.bytes + source.bytes -
                         (request.undelete ? 1u : 0u);
    if (required > output.count)
        return UserAuthWriteResult{0, UserAuthWriteError::output_too_small};

    u32 offset = 0;
    write_literal(output.data, &offset, "{\"login\":");
    write_encoded(output.data, &offset, login);
    write_literal(output.data, &offset, ",\"password\":");
    write_encoded(output.data, &offset, password);
    if (request.undelete)
        write_literal(output.data, &offset, ",\"undelete\":true");
    else
        write_literal(output.data, &offset, ",\"undelete\":false");
    write_literal(output.data, &offset, ",\"login_source\":");
    write_encoded(output.data, &offset, source);
    output.data[offset++] = '}';
    return UserAuthWriteResult{offset, UserAuthWriteError::none};
}

bool user_auth_recovery_identifier_valid(Utf8View login) noexcept {
    EncodedString encoded{};
    if (!encoded_string(login, kUserAuthMaximumLoginBytes, &encoded)) return false;
    for (u32 i = 0; i + 2 < login.bytes; ++i)
        if (login.data[i] == ':' && login.data[i + 1] == '/' && login.data[i + 2] == '/') return false;
    return true;
}

UserAuthWriteResult user_auth_write_recovery_request(Utf8View login, Slice<u8> output) noexcept {
    if (!output.data)
        return {0, UserAuthWriteError::invalid_input};
    EncodedString encoded{};
    if (!encoded_string(login, kUserAuthMaximumLoginBytes, &encoded))
        return {0, string_error(login, kUserAuthMaximumLoginBytes)};
    if (!user_auth_recovery_identifier_valid(login)) return {0, UserAuthWriteError::invalid_input};
    if (static_cast<u64>(sizeof("{\"login\":}") - 1) + encoded.bytes > output.count)
        return {0, UserAuthWriteError::output_too_small};
    u32 offset = 0;
    write_literal(output.data, &offset, "{\"login\":");
    write_encoded(output.data, &offset, encoded);
    output.data[offset++] = '}';
    return {offset, UserAuthWriteError::none};
}

bool user_auth_phone_code_valid(Utf8View code) noexcept {
    if (!code.data || code.bytes != kUserAuthPhoneCodeBytes) return false;
    for (u32 i = 0; i < code.bytes; ++i) if (code.data[i] < '0' || code.data[i] > '9') return false;
    return true;
}
UserAuthWriteResult user_auth_recovery_phone_base(Slice<u8> body, u32 bytes) noexcept {
    if (!body.data || bytes < 12 || bytes > body.count || body.data[bytes - 1] != '}' ||
        !bytes_equal(body.data, "{\"login\":\"", 10)) return {0, UserAuthWriteError::invalid_input};
    copy_bytes(body.data + 2, "phone", 5);
    return {bytes, UserAuthWriteError::none};
}
UserAuthWriteResult user_auth_write_phone_verify_request(Slice<u8> body, u32 base_bytes, Utf8View code) noexcept {
    const ConstSlice<u8> target{body.data, body.count};
    const ConstSlice<u8> input{reinterpret_cast<const u8*>(code.data), code.bytes};
    if (!byte_range_valid(target) || !byte_range_valid(input) || byte_ranges_overlap(target, input) ||
        !user_auth_phone_code_valid(code) || !body.data || base_bytes < 12 || base_bytes > body.count ||
        body.data[base_bytes - 1] != '}' || !bytes_equal(body.data, "{\"phone\":\"", 10))
        return {0, UserAuthWriteError::invalid_input};
    constexpr u32 suffix_bytes = 16;
    if (body.count - base_bytes < suffix_bytes) return {0, UserAuthWriteError::output_too_small};
    u32 offset = base_bytes - 1;
    write_literal(body.data, &offset, ",\"code\":\"");
    copy_bytes(body.data + offset, code.data, code.bytes); offset += code.bytes;
    write_literal(body.data, &offset, "\"}");
    return {offset, UserAuthWriteError::none};
}

UserAuthWriteResult user_auth_write_mfa_request(const UserAuthMfaRequest& request,
                                                Slice<u8> output) noexcept {
    if (!output.data || user_auth_mfa_target(request.type).bytes == 0)
        return UserAuthWriteResult{0, UserAuthWriteError::invalid_input};
    EncodedString code{};
    EncodedString ticket{};
    EncodedString source{};
    EncodedString instance{};
    const u32 code_maximum = user_auth_mfa_data_maximum(request.type);
    if (code_maximum == 0 || !encoded_string(request.code, code_maximum, &code))
        return UserAuthWriteResult{0, code_maximum == 0 ? UserAuthWriteError::invalid_input
                                                        : string_error(request.code, code_maximum)};
    if (!encoded_string(request.ticket, kUserAuthMaximumTicketBytes, &ticket))
        return UserAuthWriteResult{0, string_error(request.ticket, kUserAuthMaximumTicketBytes)};
    if (!encoded_string(request.login_source, kUserAuthMaximumSourceBytes, &source))
        return UserAuthWriteResult{0, string_error(request.login_source, kUserAuthMaximumSourceBytes)};
    if (!encoded_string(request.login_instance_id, kUserAuthMaximumLoginInstanceBytes, &instance))
        return UserAuthWriteResult{0, string_error(request.login_instance_id, kUserAuthMaximumLoginInstanceBytes)};

    constexpr u32 fixed_bytes = sizeof("{\"code\":,\"ticket\":,\"login_source\":,\"login_instance_id\":}") - 1;
    const u64 required =
        static_cast<u64>(fixed_bytes) + code.bytes + ticket.bytes + source.bytes + instance.bytes;
    if (required > output.count)
        return UserAuthWriteResult{0, UserAuthWriteError::output_too_small};

    u32 offset = 0;
    write_literal(output.data, &offset, "{\"code\":");
    write_encoded(output.data, &offset, code);
    write_literal(output.data, &offset, ",\"ticket\":");
    write_encoded(output.data, &offset, ticket);
    write_literal(output.data, &offset, ",\"login_source\":");
    write_encoded(output.data, &offset, source);
    write_literal(output.data, &offset, ",\"login_instance_id\":");
    write_encoded(output.data, &offset, instance);
    output.data[offset++] = '}';
    return UserAuthWriteResult{offset, UserAuthWriteError::none};
}

UserAuthWriteResult user_auth_write_mfa_sms_send_request(const UserAuthMfaSmsSendRequest& request,
                                                         Slice<u8> output) noexcept {
    if (!output.data)
        return UserAuthWriteResult{0, UserAuthWriteError::invalid_input};
    EncodedString ticket{};
    if (!encoded_string(request.ticket, kUserAuthMaximumTicketBytes, &ticket))
        return UserAuthWriteResult{0, string_error(request.ticket, kUserAuthMaximumTicketBytes)};
    if (static_cast<u64>(sizeof("{\"ticket\":}") - 1) + ticket.bytes > output.count)
        return UserAuthWriteResult{0, UserAuthWriteError::output_too_small};
    u32 offset = 0;
    write_literal(output.data, &offset, "{\"ticket\":");
    write_encoded(output.data, &offset, ticket);
    output.data[offset++] = '}';
    return UserAuthWriteResult{offset, UserAuthWriteError::none};
}

UserAuthWriteResult user_auth_write_captcha_headers(const UserAuthCaptchaHeaders& headers,
                                                    Slice<u8> output) noexcept {
    u32 required = 0;
    if (!output.data || !captcha_headers_size(headers, false, &required))
        return UserAuthWriteResult{0, UserAuthWriteError::invalid_input};
    if (required > output.count)
        return UserAuthWriteResult{0, UserAuthWriteError::output_too_small};
    u32 offset = 0;
    const struct { Utf8View label, value; } lines[] = {
        {utf8_literal("X-Captcha-Key: "), headers.key},
        {utf8_literal("X-Captcha-Rqtoken: "), headers.rqtoken},
        {utf8_literal("X-Captcha-Session-Id: "), headers.session_id}};
    for (const auto& line : lines) {
        if (line.value.bytes == 0)
            continue;
        copy_bytes(output.data + offset, line.label.data, line.label.bytes);
        offset += line.label.bytes;
        copy_bytes(output.data + offset, line.value.data, line.value.bytes);
        offset += line.value.bytes;
        write_literal(output.data, &offset, "\r\n");
    }
    return UserAuthWriteResult{offset, UserAuthWriteError::none};
}

Utf8View user_auth_mfa_target(UserAuthMfaType type) noexcept {
    switch (type) {
    case UserAuthMfaType::totp: return utf8_literal("/api/v9/auth/mfa/totp");
    case UserAuthMfaType::sms: return utf8_literal("/api/v9/auth/mfa/sms");
    case UserAuthMfaType::backup: return utf8_literal("/api/v9/auth/mfa/backup");
    case UserAuthMfaType::webauthn: return utf8_literal("/api/v9/auth/mfa/webauthn");
    default: return Utf8View{};
    }
}

u32 user_auth_mfa_data_maximum(UserAuthMfaType type) noexcept {
    switch (type) {
    case UserAuthMfaType::totp:
    case UserAuthMfaType::sms:
    case UserAuthMfaType::backup:
        return kUserAuthMaximumCodeBytes;
    case UserAuthMfaType::webauthn:
        return kUserAuthMaximumWebauthnCredentialBytes;
    default:
        return 0;
    }
}

Utf8View user_auth_target(UserAuthOperation operation) noexcept {
    switch (operation) {
    case UserAuthOperation::password: return kUserAuthLoginTarget;
    case UserAuthOperation::mfa_totp: return user_auth_mfa_target(UserAuthMfaType::totp);
    case UserAuthOperation::mfa_sms: return user_auth_mfa_target(UserAuthMfaType::sms);
    case UserAuthOperation::mfa_backup: return user_auth_mfa_target(UserAuthMfaType::backup);
    case UserAuthOperation::mfa_webauthn: return user_auth_mfa_target(UserAuthMfaType::webauthn);
    case UserAuthOperation::mfa_sms_send: return kUserAuthMfaSmsSendTarget;
    case UserAuthOperation::remote_auth_ticket: return kUserAuthRemoteAuthTicketTarget;
    case UserAuthOperation::browser_handoff_exchange: return utf8_literal("/api/v9/auth/handoff/exchange");
    case UserAuthOperation::verification: return kLoginVerificationTarget;
    case UserAuthOperation::recovery: return utf8_literal("/api/v9/auth/forgot");
    case UserAuthOperation::phone_resend: return utf8_literal("/api/v9/phone-verifications/resend");
    case UserAuthOperation::phone_verify: return utf8_literal("/api/v9/phone-verifications/verify");
    case UserAuthOperation::reset_password:
    case UserAuthOperation::reset_mfa:
        return kLoginResetTarget;
    default: return Utf8View{};
    }
}

UserAuthWriteResult user_auth_write_request_head(UserAuthOperation operation, u64 content_length,
                                                 const UserAuthCaptchaHeaders& captcha,
                                                 Slice<u8> output,
                                                 const RestUserRequestHeaders* user_headers) noexcept {
    const Utf8View target = user_auth_target(operation);
    u32 captcha_bytes = 0;
    const ConstSlice<u8> destination{output.data, output.count};
    const Utf8View captcha_fields[] = {captcha.key, captcha.rqtoken, captcha.session_id};
    if (!output.data || !byte_range_valid(destination) || target.bytes == 0 || content_length == 0 ||
        !captcha_headers_size(captcha, true, &captcha_bytes) ||
        (operation == UserAuthOperation::browser_handoff_exchange &&
            (captcha.key.bytes || captcha.rqtoken.bytes || captcha.session_id.bytes)))
        return UserAuthWriteResult{0, UserAuthWriteError::invalid_input};
    for (const Utf8View field : captcha_fields) {
        const ConstSlice<u8> source{reinterpret_cast<const u8*>(field.data), field.bytes};
        if (!byte_range_valid(source) || byte_ranges_overlap(source, destination))
            return {0, UserAuthWriteError::invalid_input};
    }
    if (user_headers) {
        const ConstSlice<u8> header_source{reinterpret_cast<const u8*>(user_headers), sizeof(*user_headers)};
        if (!byte_range_valid(header_source) || byte_ranges_overlap(header_source, destination))
            return {0, UserAuthWriteError::invalid_input};
        const ConstSlice<u8> metadata_fields[]{
            user_headers->super_properties,
            {reinterpret_cast<const u8*>(user_headers->locale.data), user_headers->locale.bytes},
            {reinterpret_cast<const u8*>(user_headers->timezone.data), user_headers->timezone.bytes},
            {reinterpret_cast<const u8*>(user_headers->accept_language.data), user_headers->accept_language.bytes}};
        for (const auto field : metadata_fields)
            if (!byte_range_valid(field) || byte_ranges_overlap(field, destination))
                return {0, UserAuthWriteError::invalid_input};
    }
    if (captcha_bytes > output.count)
        return UserAuthWriteResult{0, UserAuthWriteError::output_too_small};
    const RestRequestWriteResult base = write_rest_request_head(
        RestRequestHead{ConstSlice<u8>{reinterpret_cast<const u8*>(target.data), target.bytes},
                        {},
                        content_length,
                        RestMethod::post,
                        RestAuthorization::none,
                        true,
                        false,
                        user_headers},
        Slice<u8>{output.data, output.count - captcha_bytes});
    if (!base.ok())
        return UserAuthWriteResult{0, base.error == RestRequestError::output_capacity
                                          ? UserAuthWriteError::output_too_small
                                          : UserAuthWriteError::invalid_input};
    if (captcha_bytes == 0)
        return UserAuthWriteResult{base.bytes, UserAuthWriteError::none};

    // The CAPTCHA answer joins the page's other X- headers.
    const u32 insertion = base.extra_headers_end;
    copy_bytes(output.data + insertion + captcha_bytes, output.data + insertion, base.bytes - insertion);
    const u32 added_bytes =
        user_auth_write_captcha_headers(captcha, Slice<u8>{output.data + insertion, captcha_bytes}).bytes;
    return UserAuthWriteResult{base.bytes + added_bytes, UserAuthWriteError::none};
}

UserAuthDecodeResult user_auth_classify_response(u16 status_code, Utf8View body,
                                                 Slice<u32> structural_scratch, u32 maximum_depth,
                                                 UserAuthResponse* output) noexcept {
    ResponseContext context{};
    return classify_response(status_code, body, structural_scratch, maximum_depth, &context, output);
}

static UserAuthDecodeResult decode_response_impl(u16 status_code, Utf8View body,
                                               Slice<u32> structural_scratch, u32 maximum_depth,
                                               UserAuthResponseStorage storage,
                                               UserAuthResponse* output, u8 reset_mode = 0, u8 phone_mode = 0) noexcept {
    if (!output) {
        return decode_failure(UserAuthDecodeError::invalid_input);
    }
    ResponseContext context{};
    UserAuthResponse parsed{};
    const UserAuthDecodeResult classified =
        classify_response(status_code, body, structural_scratch, maximum_depth, &context, &parsed, false, reset_mode, phone_mode);
    reset_value<UserAuthResponse>(output);
    if (!classified.ok())
        return classified;

    UserAuthDecodeError storage_error = preflight_field(&context.token, storage.token, true);
    const auto next = [&](DecodedField* field, const Slice<char>& destination, bool require_nonempty) noexcept {
        if (storage_error == UserAuthDecodeError::none)
            storage_error = preflight_field(field, destination, require_nonempty);
    };
    next(&context.ticket, storage.ticket, true);
    next(&context.login_instance, storage.login_instance_id, false);
    RawField& challenge = context.webauthn_challenge;
    if (storage_error == UserAuthDecodeError::none && challenge.present) {
        if (challenge.value.kind == JsonValueKind::string) {
            JsonString string{};
            if (!json_value_as_string(challenge.value, &string)) {
                storage_error = UserAuthDecodeError::invalid_field;
            } else {
                const JsonStringDecodeResult measured = json_string_decode_utf8(string, Slice<char>{nullptr, 0});
                if (measured.error != JsonStringDecodeError::none &&
                    measured.error != JsonStringDecodeError::output_too_small)
                    storage_error = UserAuthDecodeError::invalid_field;
                else
                    challenge.bytes = measured.bytes;
            }
        } else {
            challenge.bytes = challenge.value.source.bytes;
        }
        if (storage_error == UserAuthDecodeError::none && challenge.bytes > kUserAuthMaximumWebauthnChallengeBytes)
            storage_error = UserAuthDecodeError::value_too_large;
        else if (storage_error == UserAuthDecodeError::none &&
                 (challenge.bytes > storage.webauthn_challenge.count ||
                  (challenge.bytes != 0 && !storage.webauthn_challenge.data)))
            storage_error = UserAuthDecodeError::output_too_small;
    }
    next(&context.captcha_service, storage.captcha_service, false);
    // Only null or omitted keys use a known provider default. An explicit
    // empty string remains empty and cannot silently select another key.
    Utf8View default_sitekey{};
    if (storage_error == UserAuthDecodeError::none && parsed.kind == UserAuthResponseKind::captcha_required &&
        !context.captcha_sitekey.present)
        default_sitekey = captcha_default_sitekey(context.captcha_service);
    next(&context.captcha_sitekey, storage.captcha_sitekey, false);
    if (storage_error == UserAuthDecodeError::none && default_sitekey.bytes &&
        (default_sitekey.bytes > storage.captcha_sitekey.count || !storage.captcha_sitekey.data))
        storage_error = UserAuthDecodeError::output_too_small;
    next(&context.captcha_rqdata, storage.captcha_rqdata, false);
    next(&context.captcha_rqtoken, storage.captcha_rqtoken, false);
    next(&context.captcha_session, storage.captcha_session_id, false);
    next(&context.captcha_user_flow, storage.captcha_user_flow, false);
    next(&context.suspended_token, storage.suspended_user_token, true);
    if (storage_error != UserAuthDecodeError::none)
        return decode_failure(storage_error, body.bytes);

    decode_field(context.token, storage.token);
    decode_field(context.ticket, storage.ticket);
    decode_field(context.login_instance, storage.login_instance_id);
    copy_raw(context.webauthn_challenge, storage.webauthn_challenge);
    decode_field(context.captcha_service, storage.captcha_service);
    decode_field(context.captcha_sitekey, storage.captcha_sitekey);
    if (default_sitekey.bytes)
        copy_bytes(storage.captcha_sitekey.data, default_sitekey.data, default_sitekey.bytes);
    decode_field(context.captcha_rqdata, storage.captcha_rqdata);
    decode_field(context.captcha_rqtoken, storage.captcha_rqtoken);
    decode_field(context.captcha_session, storage.captcha_session_id);
    decode_field(context.captcha_user_flow, storage.captcha_user_flow);
    decode_field(context.suspended_token, storage.suspended_user_token);

    parsed.user_id = context.user_id;
    parsed.error_code = context.error_code;
    parsed.token_bytes = context.token.bytes;
    parsed.ticket_bytes = context.ticket.bytes;
    parsed.login_instance_id_bytes = context.login_instance.bytes;
    parsed.webauthn_challenge_bytes = context.webauthn_challenge.bytes;
    parsed.captcha_service_bytes = context.captcha_service.bytes;
    parsed.captcha_sitekey_bytes =
        default_sitekey.bytes ? default_sitekey.bytes : context.captcha_sitekey.bytes;
    parsed.captcha_rqdata_bytes = context.captcha_rqdata.bytes;
    parsed.captcha_rqtoken_bytes = context.captcha_rqtoken.bytes;
    parsed.captcha_session_id_bytes = context.captcha_session.bytes;
    parsed.captcha_user_flow_bytes = context.captcha_user_flow.bytes;
    parsed.suspended_user_token_bytes = context.suspended_token.bytes;
    parsed.mfa_capabilities = context.capabilities;
    parsed.has_user_id = (context.fields & response_field_user_id) != 0;
    parsed.has_error_code = (context.fields & response_field_error_code) != 0;
    parsed.captcha_invisible = context.captcha_invisible;
    parsed.suspended = context.suspended_token.present;
    *output = parsed;
    return decode_failure(UserAuthDecodeError::none);
}

UserAuthDecodeResult user_auth_decode_response(u16 status_code, Utf8View body,
    Slice<u32> structural_scratch, u32 maximum_depth, UserAuthResponseStorage storage,
    UserAuthResponse* output) noexcept {
    return decode_response_impl(status_code, body, structural_scratch, maximum_depth, storage, output);
}

static bool reset_ranges_valid(Utf8View body, Slice<u32> scratch, UserAuthResponseStorage storage,
    UserAuthResponse* output) noexcept {
    if (!output || scratch.count > 0xFFFFFFFFu / sizeof(u32)) return false;
    const ConstSlice<u8> input{reinterpret_cast<const u8*>(body.data), body.bytes};
    const ConstSlice<u8> index{reinterpret_cast<const u8*>(scratch.data),
        static_cast<u32>(scratch.count * sizeof(u32))};
    const ConstSlice<u8> result{reinterpret_cast<const u8*>(output), sizeof(*output)};
    const Slice<char> fields[] = {storage.token, storage.ticket, storage.login_instance_id,
        storage.webauthn_challenge, storage.captcha_service, storage.captcha_sitekey,
        storage.captcha_rqdata, storage.captcha_rqtoken, storage.captcha_session_id,
        storage.captcha_user_flow, storage.suspended_user_token};
    if (!byte_range_valid(input) || !byte_range_valid(index) || !byte_range_valid(result) ||
        byte_ranges_overlap(input, index) || byte_ranges_overlap(input, result) || byte_ranges_overlap(index, result))
        return false;
    for (u32 i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        const ConstSlice<u8> span{reinterpret_cast<const u8*>(fields[i].data), fields[i].count};
        if (!byte_range_valid(span) || byte_ranges_overlap(span, input) ||
            byte_ranges_overlap(span, index) || byte_ranges_overlap(span, result)) return false;
        for (u32 j = 0; j < i; ++j)
            if (byte_ranges_overlap(span, {reinterpret_cast<const u8*>(fields[j].data), fields[j].count})) return false;
    }
    return true;
}
UserAuthDecodeResult user_auth_classify_reset_response(u16 status_code, Utf8View body,
    Slice<u32> structural_scratch, u32 maximum_depth, UserAuthResponse* output, bool final_mfa) noexcept {
    if (!reset_ranges_valid(body, structural_scratch, {}, output))
        return decode_failure(UserAuthDecodeError::invalid_input);
    ResponseContext context{};
    return classify_response(status_code, body, structural_scratch, maximum_depth, &context, output, false,
        final_mfa ? 2 : 1);
}
UserAuthDecodeResult user_auth_decode_reset_response(u16 status_code, Utf8View body,
    Slice<u32> structural_scratch, u32 maximum_depth, UserAuthResponseStorage storage,
    UserAuthResponse* output, bool final_mfa) noexcept {
    if (!reset_ranges_valid(body, structural_scratch, storage, output))
        return decode_failure(UserAuthDecodeError::invalid_input);
    return decode_response_impl(status_code, body, structural_scratch, maximum_depth, storage, output,
        final_mfa ? 2 : 1);
}

namespace {
// Field names borrow the immutable document. Duplicate comparisons decode only
// bounded names, never credential values, and include escaped name aliases.
struct HandoffNames {
    JsonString names[64];
    u32 count;
};
bool handoff_unique(HandoffNames* names, JsonString name, ResponseContext* context) noexcept {
    char decoded[128];
    const auto result = json_string_decode_utf8(name, {decoded, sizeof(decoded)});
    if (!result.ok() || names->count == 64) {
        context->error = UserAuthDecodeError::value_too_large;
        context->error_offset = name.source_offset;
        return false;
    }
    for (u32 i = 0; i < names->count; ++i) {
        if (json_string_equals_utf8(names->names[i], {decoded, result.bytes})) {
            context->error = UserAuthDecodeError::duplicate_field;
            context->error_offset = name.source_offset;
            return false;
        }
    }
    names->names[names->count++] = name;
    return true;
}
struct HandoffResponseContext {
    ResponseContext response;
    JsonIndexedDocument document;
    HandoffNames root_names, user_names;
    u32 maximum_depth;
    bool has_nested_id;
};
bool visit_handoff_user(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<HandoffResponseContext*>(opaque);
    if (!handoff_unique(&context->user_names, name, &context->response)) return false;
    if (!json_string_equals_ascii(name, utf8_literal("id"))) return true;
    JsonString id{};
    if (!json_value_as_string(value, &id)) return reject_field(&context->response, value.source_offset);
    const auto length = json_string_decode_utf8(id, {nullptr, 0});
    if ((!length.ok() && length.error != JsonStringDecodeError::output_too_small) || !length.bytes ||
        length.bytes > 20 || !json_string_to_u64(id, &context->response.user_id) ||
        !context->response.user_id) return reject_field(&context->response, value.source_offset);
    context->has_nested_id = true;
    return true;
}
bool visit_handoff_response(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<HandoffResponseContext*>(opaque);
    if (!handoff_unique(&context->root_names, name, &context->response)) return false;
    if (!visit_response(&context->response, name, value)) return false;
    if (!json_string_equals_ascii(name, utf8_literal("user"))) return true;
    if (value.kind != JsonValueKind::object) return reject_field(&context->response, value.source_offset);
    const auto nested = json_visit_indexed_object(context->document, value, context->maximum_depth,
        visit_handoff_user, context);
    if (nested.ok()) return true;
    if (context->response.error == UserAuthDecodeError::none) {
        context->response.error = UserAuthDecodeError::invalid_json;
        context->response.error_offset = nested.error_offset;
    }
    return false;
}
struct HandoffRequestContext { JsonString key, token; u32 fields; };
bool visit_handoff_request(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<HandoffRequestContext*>(opaque);
    const u32 field = protocol_detail::member_bit<"key", "handoff_token">(name);
    if (!field || (context->fields & field)) return false;
    context->fields |= field;
    return json_value_as_string(value, field == 1 ? &context->key : &context->token);
}
}

bool user_auth_browser_handoff_request_valid(Utf8View body, Slice<u32> scratch, u32 maximum_depth) noexcept {
    if (!body.data || !body.bytes || body.bytes > kBrowserSignInMaximumBodyBytes || !scratch.data ||
        !scratch.count || scratch.count > ~0u / sizeof(u32) || !maximum_depth) return false;
    const ConstSlice<u8> input{reinterpret_cast<const u8*>(body.data), body.bytes};
    const ConstSlice<u8> index{reinterpret_cast<const u8*>(scratch.data), static_cast<u32>(scratch.count * sizeof(u32))};
    if (!byte_range_valid(input) || !byte_range_valid(index) || byte_ranges_overlap(input, index)) return false;
    const auto scan = json_scan_structurals_avx1(body, scratch);
    if (!scan.ok()) return false;
    HandoffRequestContext context{};
    const auto read = json_visit_root_object(body, {scratch.data, scan.count}, maximum_depth,
        visit_handoff_request, &context);
    if (!read.ok() || context.fields != 3 || context.key.has_escapes ||
        context.key.encoded.bytes != kBrowserSignInKeyBytes) return false;
    // Reuse the receiver's complete UUID syntax preflight without writing a URL.
    u8 probe{};
    const auto key = browser_sign_in_write_url(kBrowserSignInFirstPort, context.key.encoded, {&probe, 0});
    if (key.error != BrowserSignInError::capacity || context.key.encoded.data[14] != '4' ||
        (context.key.encoded.data[19] != '8' && context.key.encoded.data[19] != '9' &&
         context.key.encoded.data[19] != 'a' && context.key.encoded.data[19] != 'b')) return false;
    for (u32 i = 0; i < context.key.encoded.bytes; ++i)
        if (context.key.encoded.data[i] >= 'A' && context.key.encoded.data[i] <= 'F') return false;
    const auto token = json_string_decode_utf8(context.token, {nullptr, 0});
    return (token.ok() || token.error == JsonStringDecodeError::output_too_small) &&
        token.bytes && token.bytes <= kBrowserSignInMaximumTokenBytes;
}

UserAuthDecodeResult user_auth_decode_browser_handoff_response(u16 status_code, Utf8View body,
    Slice<u32> scratch, u32 maximum_depth, UserAuthResponseStorage storage, UserAuthResponse* output) noexcept {
    if (!reset_ranges_valid(body, scratch, storage, output))
        return decode_failure(UserAuthDecodeError::invalid_input);
    reset_value(output);
    if (!body.data || !body.bytes || body.bytes > kUserAuthBrowserExchangeMaximumResponseBytes ||
        !scratch.data || !scratch.count || !maximum_depth || status_code < 200 || status_code > 599)
        return decode_failure(UserAuthDecodeError::invalid_input);
    const auto scan = json_scan_structurals_avx1(body, scratch);
    if (!scan.ok()) return decode_failure(UserAuthDecodeError::scan_failed, scan.error_offset, scan.error);
    HandoffResponseContext context{};
    context.document = {body, {scratch.data, scan.count}};
    context.maximum_depth = maximum_depth;
    const auto read = json_visit_root_object(body, context.document.structurals, maximum_depth,
        visit_handoff_response, &context);
    if (!read.ok()) return decode_failure(context.response.error != UserAuthDecodeError::none ?
        context.response.error : UserAuthDecodeError::invalid_json,
        context.response.error != UserAuthDecodeError::none ? context.response.error_offset : read.error_offset,
        JsonScanError::none, read.error);
    constexpr u32 admitted = response_field_token | response_field_user | response_field_error_code;
    if (context.response.fields & ~admitted)
        return decode_failure(UserAuthDecodeError::unexpected_response, body.bytes);
    if (status_code >= 400) {
        if (context.response.fields & (response_field_token | response_field_user))
            return decode_failure(UserAuthDecodeError::unexpected_response, body.bytes);
        output->kind = UserAuthResponseKind::rejected;
        output->has_error_code = (context.response.fields & response_field_error_code) != 0;
        output->error_code = context.response.error_code;
        return decode_failure(UserAuthDecodeError::none);
    }
    if (status_code >= 300 || (context.response.fields & response_field_error_code))
        return decode_failure(UserAuthDecodeError::unexpected_response, body.bytes);
    if (!context.has_nested_id || !context.response.token.present)
        return decode_failure(UserAuthDecodeError::missing_required_field, body.bytes);
    const auto token_error =
        measured_string(context.response.token.value, kUserAuthBrowserExchangeMaximumTokenBytes);
    if (token_error != UserAuthDecodeError::none)
        return decode_failure(token_error, context.response.token.value.source_offset);
    const auto preflight = preflight_field(&context.response.token, storage.token, true);
    if (preflight != UserAuthDecodeError::none) return decode_failure(preflight, body.bytes);
    decode_field(context.response.token, storage.token);
    output->kind = UserAuthResponseKind::authenticated;
    output->has_user_id = true;
    output->user_id = context.response.user_id;
    output->token_bytes = context.response.token.bytes;
    return decode_failure(UserAuthDecodeError::none);
}

UserAuthDecodeResult user_auth_decode_phone_response(u16 status_code, Utf8View body,
    Slice<u32> structural_scratch, u32 maximum_depth, UserAuthResponseStorage storage,
    UserAuthResponse* output, bool verify) noexcept {
    if (!reset_ranges_valid(body, structural_scratch, storage, output))
        return decode_failure(UserAuthDecodeError::invalid_input);
    if (!verify && status_code >= 200 && status_code < 300 && !body.bytes) {
        reset_value(output); output->kind = UserAuthResponseKind::phone_code_sent;
        return decode_failure(UserAuthDecodeError::none);
    }
    return decode_response_impl(status_code, body, structural_scratch, maximum_depth, storage, output, 0, verify ? 2 : 1);
}

UserAuthDecodeResult user_auth_decode_recovery_response(u16 status_code, Utf8View body,
    Slice<u32> structural_scratch, u32 maximum_depth, UserAuthResponseStorage storage,
    UserAuthResponse* output) noexcept {
    if (!output) return decode_failure(UserAuthDecodeError::invalid_input);
    reset_value(output);
    if (status_code >= 200 && status_code < 300) {
        ResponseContext context{};
        UserAuthResponse parsed{};
        const auto result = classify_response(status_code, body, structural_scratch,
            maximum_depth, &context, &parsed, true);
        if (!result.ok() || parsed.kind != UserAuthResponseKind::recovery_sent)
            return result.ok() ? decode_failure(UserAuthDecodeError::unexpected_response) : result;
        output->kind = UserAuthResponseKind::recovery_sent;
        return result;
    }
    // Error/challenge schema is shared; recovery never copies an account token.
    reset_value(&storage.token);
    const auto result = user_auth_decode_response(status_code, body, structural_scratch,
        maximum_depth, storage, output);
    if (!result.ok()) return result;
    if (output->kind != UserAuthResponseKind::rejected && output->kind != UserAuthResponseKind::captcha_required) {
        reset_value(output);
        return decode_failure(UserAuthDecodeError::unexpected_response);
    }
    output->user_id = 0; output->has_user_id = false; output->mfa_capabilities = 0;
    if (output->kind == UserAuthResponseKind::rejected && output->has_error_code && output->error_code == 70007)
        output->kind = UserAuthResponseKind::recovery_phone_required;
    return result;
}

void user_auth_state_initialize(UserAuthState* state) noexcept {
    *state = UserAuthState{1, UserAuthPhase::idle, UserAuthPhase::idle, UserAuthMfaType::totp};
}

UserAuthTransitionResult user_auth_state_begin_password(UserAuthState* state) noexcept {
    if (state->phase != UserAuthPhase::idle && state->phase != UserAuthPhase::rejected)
        return transition_failure(state, UserAuthTransitionError::invalid_phase);
    state->generation = next_generation(state->generation);
    state->phase = UserAuthPhase::password_pending;
    state->captcha_return_phase = UserAuthPhase::idle;
    return transition_failure(state, UserAuthTransitionError::none);
}

UserAuthTransitionResult user_auth_state_begin_mfa(UserAuthState* state, UserAuthMfaType type) noexcept {
    if (user_auth_mfa_target(type).bytes == 0)
        return transition_failure(state, UserAuthTransitionError::invalid_input);
    if (state->phase != UserAuthPhase::mfa_required)
        return transition_failure(state, UserAuthTransitionError::invalid_phase);
    state->generation = next_generation(state->generation);
    state->phase = UserAuthPhase::mfa_pending;
    state->mfa_type = type;
    return transition_failure(state, UserAuthTransitionError::none);
}

UserAuthTransitionResult user_auth_state_begin_verification(UserAuthState* state) noexcept {
    const UserAuthTransitionResult result = user_auth_state_begin_password(state);
    if (result.ok())
        state->phase = UserAuthPhase::verification_pending;
    return result;
}

UserAuthTransitionResult user_auth_state_begin_recovery(UserAuthState* state) noexcept {
    const auto result = user_auth_state_begin_password(state);
    if (result.ok())
        state->phase = UserAuthPhase::recovery_pending;
    return result;
}
UserAuthTransitionResult user_auth_state_begin_reset(UserAuthState* state) noexcept {
    const auto result = user_auth_state_begin_password(state);
    if (result.ok())
        state->phase = UserAuthPhase::reset_pending;
    return result;
}
UserAuthTransitionResult user_auth_state_begin_browser_exchange(UserAuthState* state) noexcept {
    const auto result = user_auth_state_begin_password(state);
    if (result.ok())
        state->phase = UserAuthPhase::browser_exchange_pending;
    return result;
}

UserAuthTransitionResult user_auth_state_resume_captcha(UserAuthState* state) noexcept {
    if (state->phase != UserAuthPhase::captcha_required ||
        (state->captcha_return_phase != UserAuthPhase::password_pending &&
         state->captcha_return_phase != UserAuthPhase::mfa_pending &&
         state->captcha_return_phase != UserAuthPhase::verification_pending &&
         state->captcha_return_phase != UserAuthPhase::recovery_pending &&
         state->captcha_return_phase != UserAuthPhase::reset_pending &&
         state->captcha_return_phase != UserAuthPhase::phone_pending))
        return transition_failure(state, UserAuthTransitionError::invalid_phase);
    state->generation = next_generation(state->generation);
    state->phase = state->captcha_return_phase;
    return transition_failure(state, UserAuthTransitionError::none);
}

UserAuthTransitionResult user_auth_state_apply(UserAuthState* state, u64 generation,
                                               const UserAuthResponse& response) noexcept {
    if (generation != state->generation)
        return transition_failure(state, UserAuthTransitionError::stale_generation);
    if (state->phase != UserAuthPhase::password_pending && state->phase != UserAuthPhase::mfa_pending &&
        state->phase != UserAuthPhase::verification_pending && state->phase != UserAuthPhase::recovery_pending &&
        state->phase != UserAuthPhase::reset_pending && state->phase != UserAuthPhase::phone_pending &&
        state->phase != UserAuthPhase::browser_exchange_pending)
        return transition_failure(state, UserAuthTransitionError::invalid_phase);
    switch (response.kind) {
    case UserAuthResponseKind::authenticated:
        if (state->phase == UserAuthPhase::recovery_pending || state->phase == UserAuthPhase::phone_pending)
            return transition_failure(state, UserAuthTransitionError::invalid_response);
        if (state->phase == UserAuthPhase::reset_pending && !response.token_bytes)
            return transition_failure(state, UserAuthTransitionError::invalid_response);
        if ((state->phase == UserAuthPhase::verification_pending || state->phase == UserAuthPhase::browser_exchange_pending) &&
            (!response.has_user_id || response.user_id == 0 || response.token_bytes == 0))
            return transition_failure(state, UserAuthTransitionError::invalid_response);
        state->phase = UserAuthPhase::authenticated;
        break;
    case UserAuthResponseKind::mfa_required:
        if (state->phase != UserAuthPhase::password_pending && state->phase != UserAuthPhase::reset_pending)
            return transition_failure(state, UserAuthTransitionError::invalid_response);
        state->phase = UserAuthPhase::mfa_required;
        break;
    case UserAuthResponseKind::captcha_required:
        if (state->phase == UserAuthPhase::browser_exchange_pending)
            return transition_failure(state, UserAuthTransitionError::invalid_response);
        state->captcha_return_phase = state->phase;
        state->phase = UserAuthPhase::captcha_required;
        break;
    case UserAuthResponseKind::rejected:
        state->phase = UserAuthPhase::rejected;
        break;
    case UserAuthResponseKind::recovery_sent:
    case UserAuthResponseKind::recovery_phone_required:
        if (state->phase != UserAuthPhase::recovery_pending)
            return transition_failure(state, UserAuthTransitionError::invalid_response);
        state->phase = response.kind == UserAuthResponseKind::recovery_sent ?
            UserAuthPhase::recovery_sent : UserAuthPhase::recovery_phone_required;
        break;
    case UserAuthResponseKind::phone_code_sent:
    case UserAuthResponseKind::phone_verified_reset:
        if (state->phase != UserAuthPhase::phone_pending ||
            (response.kind == UserAuthResponseKind::phone_verified_reset && !response.token_bytes))
            return transition_failure(state, UserAuthTransitionError::invalid_response);
        state->phase = response.kind == UserAuthResponseKind::phone_code_sent ?
            UserAuthPhase::recovery_phone_required : UserAuthPhase::phone_token_ready;
        break;
    default:
        return transition_failure(state, UserAuthTransitionError::invalid_response);
    }
    return transition_failure(state, UserAuthTransitionError::none);
}

void user_auth_state_cancel(UserAuthState* state) noexcept {
    state->generation = next_generation(state->generation);
    state->phase = UserAuthPhase::idle;
    state->captcha_return_phase = UserAuthPhase::idle;
}

} // namespace maicord
