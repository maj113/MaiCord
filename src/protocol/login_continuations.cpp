#include "maicord/protocol/login_continuations.hpp"
#include "maicord/core/byte_range.hpp"
#include "json_write.hpp"

namespace maicord {
namespace {
using namespace protocol_detail;
bool valid_view(Utf8View value) noexcept {
    return value.data && value.bytes &&
        byte_range_valid({reinterpret_cast<const u8*>(value.data), value.bytes});
}
UserAuthWriteError encode_reset(Utf8View value, u32 maximum, EncodedString* encoded) noexcept {
    if (!valid_view(value)) return UserAuthWriteError::invalid_input;
    if (value.bytes > maximum) return UserAuthWriteError::value_too_large;
    return encoded_string(value, maximum, encoded) ? UserAuthWriteError::none : UserAuthWriteError::invalid_utf8;
}
bool valid_destination(Slice<u8> output, Utf8View a, Utf8View b = {}) noexcept {
    return output.data && byte_range_valid({output.data, output.count}) &&
        !byte_ranges_overlap({output.data, output.count}, {reinterpret_cast<const u8*>(a.data), a.bytes}) &&
        !byte_ranges_overlap({output.data, output.count}, {reinterpret_cast<const u8*>(b.data), b.bytes});
}
Utf8View reset_method(UserAuthMfaType type) noexcept {
    switch (type) {
    case UserAuthMfaType::totp: return utf8_literal("totp");
    case UserAuthMfaType::sms: return utf8_literal("sms");
    case UserAuthMfaType::backup: return utf8_literal("backup");
    case UserAuthMfaType::webauthn: return utf8_literal("webauthn");
    default: return {};
    }
}
// Native assertion strings contain only unescaped base64url binary fields.
bool binary_string(JsonValue value, Utf8View* text) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string) || string.has_escapes || !string.encoded.bytes ||
        (string.encoded.bytes & 3u) == 1) return false;
    for (u32 i = 0; i < string.encoded.bytes; ++i) {
        const char c = string.encoded.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    *text = string.encoded;
    return true;
}
struct AssertionContext {
    JsonIndexedDocument document;
    Utf8View id, raw_id;
    u32 fields;
};
bool mark_assertion(AssertionContext* context, u32 bit) noexcept {
    if (context->fields & bit) return false;
    context->fields |= bit;
    return true;
}
bool visit_assertion_response(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<AssertionContext*>(opaque);
    u32 bit = 0;
    if (json_string_equals_ascii(name, utf8_literal("authenticatorData"))) bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("clientDataJSON"))) bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("signature"))) bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("userHandle"))) bit = 8;
    else return false;
    if (!mark_assertion(context, bit)) return false;
    if (bit == 8 && value.kind == JsonValueKind::null_value) return true;
    Utf8View binary{};
    return binary_string(value, &binary);
}
bool visit_assertion(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<AssertionContext*>(opaque);
    u32 bit = 0;
    if (json_string_equals_ascii(name, utf8_literal("id"))) bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("rawId"))) bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("response"))) bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("type"))) bit = 8;
    else if (json_string_equals_ascii(name, utf8_literal("clientExtensionResults"))) bit = 16;
    else if (json_string_equals_ascii(name, utf8_literal("authenticatorAttachment"))) bit = 32;
    else return false;
    if (!mark_assertion(context, bit)) return false;
    if (bit == 1 || bit == 2) return binary_string(value, bit == 1 ? &context->id : &context->raw_id);
    if (bit == 4) {
        if (value.kind != JsonValueKind::object) return false;
        AssertionContext response{context->document, {}, {}, 0};
        return json_visit_indexed_object(context->document, value, 4, visit_assertion_response, &response).ok() &&
            response.fields == 15;
    }
    if (bit == 16) {
        // Extension results must be an empty object.
        if (value.kind != JsonValueKind::object) return false;
        const auto read = json_visit_indexed_object(context->document, value, 4, nullptr, nullptr);
        return read.ok() && read.member_count == 0;
    }
    if (bit == 32 && value.kind == JsonValueKind::null_value) return true;
    JsonString string{};
    return json_value_as_string(value, &string) &&
        (bit == 8 ? json_string_equals_ascii(string, utf8_literal("public-key")) :
         json_string_equals_ascii(string, utf8_literal("platform")) ||
         json_string_equals_ascii(string, utf8_literal("cross-platform")));
}
bool native_assertion_valid(Utf8View credential) noexcept {
    u32 scratch[128]{};
    const auto scan = json_scan_structurals_avx1(credential, {scratch, 128});
    if (!scan.ok()) return false;
    AssertionContext context{{credential, {scratch, scan.count}}, {}, {}, 0};
    const auto read = json_visit_root_object(credential, {scratch, scan.count}, 4, visit_assertion, &context);
    return read.ok() && context.fields == 63 && context.id.bytes == context.raw_id.bytes &&
        bytes_equal(context.id.data, context.raw_id.data, context.id.bytes);
}
} // namespace

UserAuthWriteResult user_auth_measure_reset_password_tail(Utf8View password) noexcept {
    EncodedString encoded{};
    const auto error = encode_reset(password, kUserAuthMaximumPasswordBytes, &encoded);
    if (error != UserAuthWriteError::none) return {0, error};
    constexpr u32 framing = sizeof(",\"password\":,\"source\":\"/reset\"}") - 1;
    const u64 required = static_cast<u64>(framing) + encoded.bytes;
    if (required > 0xFFFFFFFFull) return {0, UserAuthWriteError::value_too_large};
    return {static_cast<u32>(required), UserAuthWriteError::none};
}
UserAuthWriteResult user_auth_write_reset_password_tail(Utf8View password, Slice<u8> output) noexcept {
    const auto measured = user_auth_measure_reset_password_tail(password);
    if (!measured.ok()) return measured;
    if (!valid_destination(output, password)) return {0, UserAuthWriteError::invalid_input};
    if (measured.bytes > output.count) return {measured.bytes, UserAuthWriteError::output_too_small};
    EncodedString encoded{};
    encoded_string(password, kUserAuthMaximumPasswordBytes, &encoded);
    u32 offset = 0;
    write_literal(output.data, &offset, ",\"password\":");
    write_encoded(output.data, &offset, encoded);
    write_literal(output.data, &offset, ",\"source\":\"/reset\"}");
    return {offset, UserAuthWriteError::none};
}
UserAuthResetWriteResult user_auth_measure_reset_password_request(const UserAuthResetPasswordRequest& request) noexcept {
    EncodedString token{};
    const auto error = encode_reset(request.token, kLoginResetMaximumTokenBytes, &token);
    if (error != UserAuthWriteError::none) return {0, 0, error};
    const auto tail = user_auth_measure_reset_password_tail(request.password);
    if (!tail.ok()) return {0, 0, tail.error};
    constexpr u32 prefix = sizeof("{\"token\":") - 1;
    const u64 token_prefix = static_cast<u64>(prefix) + token.bytes;
    const u64 required = token_prefix + tail.bytes;
    if (required > 0xFFFFFFFFull) return {0, 0, UserAuthWriteError::value_too_large};
    return {static_cast<u32>(required), static_cast<u32>(token_prefix), UserAuthWriteError::none};
}
UserAuthResetWriteResult user_auth_write_reset_password_request(const UserAuthResetPasswordRequest& request,
    Slice<u8> output) noexcept {
    const auto measured = user_auth_measure_reset_password_request(request);
    if (!measured.ok()) return measured;
    if (!valid_destination(output, request.token, request.password)) return {0, 0, UserAuthWriteError::invalid_input};
    if (measured.bytes > output.count) return {measured.bytes, measured.token_prefix_bytes, UserAuthWriteError::output_too_small};
    EncodedString token{};
    encoded_string(request.token, kLoginResetMaximumTokenBytes, &token);
    u32 offset = 0;
    write_literal(output.data, &offset, "{\"token\":");
    write_encoded(output.data, &offset, token);
    user_auth_write_reset_password_tail(request.password, {output.data + offset, output.count - offset});
    return measured;
}
UserAuthWriteResult user_auth_measure_reset_mfa_suffix(Utf8View code, Utf8View ticket, UserAuthMfaType type) noexcept {
    const Utf8View method = reset_method(type);
    if (!method.bytes) return {0, UserAuthWriteError::invalid_input};
    EncodedString encoded_code{}, encoded_ticket{}, encoded_method{};
    auto error = encode_reset(code, user_auth_mfa_data_maximum(type), &encoded_code);
    if (error != UserAuthWriteError::none) return {0, error};
    error = encode_reset(ticket, kUserAuthMaximumTicketBytes, &encoded_ticket);
    if (error != UserAuthWriteError::none) return {0, error};
    if (type == UserAuthMfaType::webauthn && !native_assertion_valid(code)) return {0, UserAuthWriteError::invalid_input};
    encoded_string(method, true, &encoded_method);
    constexpr u32 framing = sizeof(",\"code\":,\"ticket\":,\"method\":}") - 1;
    const u64 required = static_cast<u64>(framing) + encoded_code.bytes + encoded_ticket.bytes + encoded_method.bytes;
    if (required > 0xFFFFFFFFull) return {0, UserAuthWriteError::value_too_large};
    return {static_cast<u32>(required), UserAuthWriteError::none};
}
UserAuthWriteResult user_auth_write_reset_mfa_suffix(Utf8View code, Utf8View ticket, UserAuthMfaType type,
    Slice<u8> output) noexcept {
    const auto measured = user_auth_measure_reset_mfa_suffix(code, ticket, type);
    if (!measured.ok()) return measured;
    if (!valid_destination(output, code, ticket)) return {0, UserAuthWriteError::invalid_input};
    if (measured.bytes > output.count) return {measured.bytes, UserAuthWriteError::output_too_small};
    EncodedString encoded_code{}, encoded_ticket{}, encoded_method{};
    encoded_string(code, user_auth_mfa_data_maximum(type), &encoded_code);
    encoded_string(ticket, kUserAuthMaximumTicketBytes, &encoded_ticket);
    encoded_string(reset_method(type), true, &encoded_method);
    u32 offset = 0;
    write_literal(output.data, &offset, ",\"code\":");
    write_encoded(output.data, &offset, encoded_code);
    write_literal(output.data, &offset, ",\"ticket\":");
    write_encoded(output.data, &offset, encoded_ticket);
    write_literal(output.data, &offset, ",\"method\":");
    write_encoded(output.data, &offset, encoded_method);
    write_literal(output.data, &offset, "}");
    return {offset, UserAuthWriteError::none};
}
UserAuthWriteResult login_verification_write_request(Utf8View token, Slice<u8> output) noexcept {
    if (!valid_view(token) || !valid_destination(output, token)) return {0, UserAuthWriteError::invalid_input};
    if (token.bytes > kLoginVerificationMaximumTokenBytes) return {0, UserAuthWriteError::value_too_large};
    EncodedString encoded{};
    if (!encoded_string(token, kLoginVerificationMaximumTokenBytes, &encoded))
        return {0, UserAuthWriteError::invalid_utf8};
    constexpr u32 framing = sizeof("{\"token\":}") - 1;
    const u32 required = framing + encoded.bytes;
    if (required > output.count) return {required, UserAuthWriteError::output_too_small};
    u32 offset = 0;
    write_literal(output.data, &offset, "{\"token\":");
    write_encoded(output.data, &offset, encoded);
    write_literal(output.data, &offset, "}");
    return {offset, UserAuthWriteError::none};
}
} // namespace maicord
