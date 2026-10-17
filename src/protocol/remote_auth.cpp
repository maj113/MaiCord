#include "maicord/protocol/remote_auth.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/asset_hash.hpp"

namespace maicord {
namespace {

enum RemoteAuthField : u32 {
    field_op = 1u << 0,
    field_timeout = 1u << 1,
    field_heartbeat_interval = 1u << 2,
    field_encrypted_nonce = 1u << 3,
    field_fingerprint = 1u << 4,
    field_encrypted_user_payload = 1u << 5,
    field_ticket = 1u << 6,
    field_encrypted_token = 1u << 7,
};

struct DecodeContext {
    JsonString op;
    JsonString encrypted_nonce;
    JsonString fingerprint;
    JsonString encrypted_user_payload;
    JsonString ticket;
    JsonString encrypted_token;
    u64 timeout_ms;
    u64 heartbeat_interval_ms;
    u32 fields;
    u32 error_offset;
    RemoteAuthError error;
};

RemoteAuthDecodeResult failure(RemoteAuthError error, u32 offset = 0,
                               JsonScanError scan_error = JsonScanError::none,
                               JsonReadError read_error = JsonReadError::none,
                               Base64Error base64_error = Base64Error::none) noexcept {
    return RemoteAuthDecodeResult{offset, error, scan_error, read_error, base64_error};
}

bool mark(DecodeContext* context, u32 field, u32 offset) noexcept {
    if ((context->fields & field) != 0) {
        context->error = RemoteAuthError::duplicate_field;
        context->error_offset = offset;
        return false;
    }
    context->fields |= field;
    return true;
}

bool read_string(DecodeContext* context, JsonString name, JsonValue value, u32 field,
                 JsonString* output) noexcept {
    if (!mark(context, field, name.source_offset)) {
        return false;
    }
    if (!json_value_as_string(value, output) || output->has_escapes) {
        context->error = RemoteAuthError::invalid_field;
        context->error_offset = value.source_offset;
        return false;
    }
    return true;
}

bool read_u64(DecodeContext* context, JsonString name, JsonValue value, u32 field, u64* output) noexcept {
    if (!mark(context, field, name.source_offset)) {
        return false;
    }
    if (!json_value_to_u64(value, output)) {
        context->error = RemoteAuthError::invalid_field;
        context->error_offset = value.source_offset;
        return false;
    }
    return true;
}

bool visit_packet(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<DecodeContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("op"))) {
        return read_string(context, name, value, field_op, &context->op);
    }
    if (json_string_equals_ascii(name, utf8_literal("timeout_ms"))) {
        return read_u64(context, name, value, field_timeout, &context->timeout_ms);
    }
    if (json_string_equals_ascii(name, utf8_literal("heartbeat_interval"))) {
        return read_u64(context, name, value, field_heartbeat_interval, &context->heartbeat_interval_ms);
    }
    if (json_string_equals_ascii(name, utf8_literal("encrypted_nonce"))) {
        return read_string(context, name, value, field_encrypted_nonce, &context->encrypted_nonce);
    }
    if (json_string_equals_ascii(name, utf8_literal("fingerprint"))) {
        return read_string(context, name, value, field_fingerprint, &context->fingerprint);
    }
    if (json_string_equals_ascii(name, utf8_literal("encrypted_user_payload"))) {
        return read_string(context, name, value, field_encrypted_user_payload,
                           &context->encrypted_user_payload);
    }
    if (json_string_equals_ascii(name, utf8_literal("ticket"))) {
        return read_string(context, name, value, field_ticket, &context->ticket);
    }
    if (json_string_equals_ascii(name, utf8_literal("encrypted_token"))) {
        return read_string(context, name, value, field_encrypted_token, &context->encrypted_token);
    }
    return true;
}

bool string_equals(JsonString string, Utf8View expected) noexcept {
    return !string.has_escapes && json_string_equals_ascii(string, expected);
}

bool validate_standard_base64(JsonString string, u32 expected_bytes, Base64Error* error) noexcept {
    const Base64Result result = base64_decode(string.encoded, Slice<u8>{nullptr, 0});
    if (result.error == Base64Error::output_too_small && result.bytes == expected_bytes) {
        return true;
    }
    *error = result.error;
    return false;
}

bool validate_base64url(JsonString string, u32 expected_bytes) noexcept {
    const Base64UrlResult result = base64url_decode(string.encoded, Slice<u8>{nullptr, 0});
    return result.error == Base64UrlError::output_too_small && result.bytes == expected_bytes;
}

void copy_string(JsonString source, Slice<char> destination) noexcept {
    copy_bytes(destination.data, source.encoded.data, source.encoded.bytes);
}

RemoteAuthDecodeResult parse(Utf8View body, Slice<u32> structural_scratch, u32 maximum_depth,
                             DecodeContext* context) noexcept {
    if (!body.data || body.bytes == 0 || body.bytes > kRemoteAuthMaximumPacketBytes ||
        !structural_scratch.data || structural_scratch.count == 0 || maximum_depth == 0) {
        return failure(body.bytes > kRemoteAuthMaximumPacketBytes ? RemoteAuthError::packet_too_large
                                                                  : RemoteAuthError::invalid_input);
    }
    const JsonScanResult scan = json_scan_structurals_avx1(body, structural_scratch);
    if (!scan.ok()) {
        return failure(RemoteAuthError::scan_failed, scan.error_offset, scan.error);
    }
    const JsonReadResult read = json_visit_root_object(
        body, ConstSlice<u32>{structural_scratch.data, scan.count}, maximum_depth, visit_packet, context);
    if (!read.ok()) {
        return failure(context->error != RemoteAuthError::none ? context->error
                                                               : RemoteAuthError::invalid_json,
                       context->error != RemoteAuthError::none ? context->error_offset : read.error_offset,
                       JsonScanError::none, read.error);
    }
    return failure(RemoteAuthError::none);
}

RemoteAuthWriteResult write_literal(Utf8View literal, Slice<char> output) noexcept {
    if ((!output.data && output.count != 0) || output.count < literal.bytes ||
        (literal.bytes != 0 && !output.data)) {
        return RemoteAuthWriteResult{literal.bytes, RemoteAuthWriteError::output_too_small};
    }
    copy_bytes(output.data, literal.data, literal.bytes);
    return RemoteAuthWriteResult{literal.bytes, RemoteAuthWriteError::none};
}

RemoteAuthWriteResult write_safe_field(Utf8View prefix, Utf8View value, Utf8View suffix,
                                       Slice<char> output) noexcept {
    if ((!value.data && value.bytes != 0) || (!output.data && output.count != 0)) {
        return RemoteAuthWriteResult{0, RemoteAuthWriteError::invalid_input};
    }
    const u64 required64 = static_cast<u64>(prefix.bytes) + value.bytes + suffix.bytes;
    if (required64 > 0xFFFFFFFFull) {
        return RemoteAuthWriteResult{0, RemoteAuthWriteError::invalid_input};
    }
    const u32 required = static_cast<u32>(required64);
    if (required > output.count || (required != 0 && !output.data)) {
        return RemoteAuthWriteResult{required, RemoteAuthWriteError::output_too_small};
    }
    copy_bytes(output.data, prefix.data, prefix.bytes);
    copy_bytes(output.data ? output.data + prefix.bytes : nullptr, value.data, value.bytes);
    copy_bytes(output.data ? output.data + prefix.bytes + value.bytes : nullptr, suffix.data, suffix.bytes);
    return RemoteAuthWriteResult{required, RemoteAuthWriteError::none};
}

bool safe_wire_string(Utf8View value) noexcept {
    if (!value.data || value.bytes == 0 || !utf8_validate(value).valid) {
        return false;
    }
    for (u32 index = 0; index < value.bytes; ++index) {
        const u8 character = static_cast<u8>(value.data[index]);
        if (character < 0x20 || character == '"' || character == '\\') {
            return false;
        }
    }
    return true;
}

} // namespace

RemoteAuthDecodeResult remote_auth_decode_server_packet(Utf8View body, Slice<u32> structural_scratch,
                                                        u32 maximum_depth, Slice<char> text,
                                                        RemoteAuthPacket* packet) noexcept {
    if (!packet || (!text.data && text.count != 0)) {
        return failure(RemoteAuthError::invalid_input);
    }
    DecodeContext context{};
    const RemoteAuthDecodeResult parsed = parse(body, structural_scratch, maximum_depth, &context);
    if (!parsed.ok()) {
        return parsed;
    }
    if ((context.fields & field_op) == 0) {
        return failure(RemoteAuthError::missing_field);
    }

    RemoteAuthPacket decoded{};
    JsonString packet_text{};
    if (string_equals(context.op, utf8_literal("hello"))) {
        if ((context.fields & (field_timeout | field_heartbeat_interval)) !=
                (field_timeout | field_heartbeat_interval) ||
            context.timeout_ms == 0 || context.timeout_ms > 600000ull ||
            context.heartbeat_interval_ms == 0 || context.heartbeat_interval_ms > 120000ull) {
            return failure(RemoteAuthError::invalid_field, context.op.source_offset);
        }
        decoded.kind = RemoteAuthPacketKind::hello;
        decoded.timeout_ms = static_cast<u32>(context.timeout_ms);
        decoded.heartbeat_interval_ms = static_cast<u32>(context.heartbeat_interval_ms);
    } else if (string_equals(context.op, utf8_literal("nonce_proof"))) {
        if ((context.fields & field_encrypted_nonce) == 0) {
            return failure(RemoteAuthError::missing_field);
        }
        Base64Error base64_error = Base64Error::none;
        if (!validate_standard_base64(context.encrypted_nonce, kRemoteAuthRsaBytes, &base64_error)) {
            return failure(RemoteAuthError::invalid_field, context.encrypted_nonce.source_offset,
                           JsonScanError::none, JsonReadError::none, base64_error);
        }
        decoded.kind = RemoteAuthPacketKind::nonce_proof;
        packet_text = context.encrypted_nonce;
    } else if (string_equals(context.op, utf8_literal("pending_remote_init"))) {
        if ((context.fields & field_fingerprint) == 0) {
            return failure(RemoteAuthError::missing_field);
        }
        if (!validate_base64url(context.fingerprint, kRemoteAuthFingerprintBytes)) {
            return failure(RemoteAuthError::invalid_field, context.fingerprint.source_offset);
        }
        decoded.kind = RemoteAuthPacketKind::pending_remote_init;
        packet_text = context.fingerprint;
    } else if (string_equals(context.op, utf8_literal("pending_ticket"))) {
        if ((context.fields & field_encrypted_user_payload) == 0) {
            return failure(RemoteAuthError::missing_field);
        }
        Base64Error base64_error = Base64Error::none;
        if (!validate_standard_base64(context.encrypted_user_payload, kRemoteAuthRsaBytes, &base64_error)) {
            return failure(RemoteAuthError::invalid_field, context.encrypted_user_payload.source_offset,
                           JsonScanError::none, JsonReadError::none, base64_error);
        }
        decoded.kind = RemoteAuthPacketKind::pending_ticket;
        packet_text = context.encrypted_user_payload;
    } else if (string_equals(context.op, utf8_literal("pending_login"))) {
        if ((context.fields & field_ticket) == 0 || context.ticket.encoded.bytes == 0 ||
            context.ticket.encoded.bytes > kRemoteAuthMaximumTicketBytes) {
            return failure(RemoteAuthError::invalid_field, context.ticket.source_offset);
        }
        decoded.kind = RemoteAuthPacketKind::pending_login;
        packet_text = context.ticket;
    } else if (string_equals(context.op, utf8_literal("cancel"))) {
        decoded.kind = RemoteAuthPacketKind::cancel;
    } else if (string_equals(context.op, utf8_literal("heartbeat_ack"))) {
        decoded.kind = RemoteAuthPacketKind::heartbeat_ack;
    } else {
        return failure(RemoteAuthError::unsupported_operation, context.op.source_offset);
    }

    if (packet_text.encoded.bytes > text.count || (packet_text.encoded.bytes != 0 && !text.data)) {
        return failure(RemoteAuthError::output_too_small, packet_text.source_offset);
    }
    copy_string(packet_text, text);
    decoded.text_bytes = packet_text.encoded.bytes;
    *packet = decoded;
    return failure(RemoteAuthError::none);
}

RemoteAuthUserPreviewResult remote_auth_parse_user_preview(Utf8View payload,
                                                           RemoteAuthUserPreview* preview) noexcept {
    const auto user_failure = [](RemoteAuthUserPreviewError error, u32 offset = 0) noexcept {
        return RemoteAuthUserPreviewResult{offset, error};
    };
    if (!preview || (!payload.data && payload.bytes != 0) || payload.bytes == 0) {
        return user_failure(RemoteAuthUserPreviewError::invalid_input);
    }
    if (payload.bytes > kRemoteAuthMaximumUserPayloadBytes) {
        return user_failure(RemoteAuthUserPreviewError::payload_too_large);
    }
    const Utf8Validation validated = utf8_validate(payload);
    if (!validated.valid) {
        return user_failure(RemoteAuthUserPreviewError::invalid_utf8, validated.error_offset);
    }
    u32 separators[3]{};
    u32 separator_count = 0;
    for (u32 index = 0; index < payload.bytes; ++index) {
        const u8 value = static_cast<u8>(payload.data[index]);
        if (value < 0x20 || value == 0x7F) {
            return user_failure(RemoteAuthUserPreviewError::invalid_username, index);
        }
        if (value == ':') {
            if (separator_count == 3) {
                return user_failure(RemoteAuthUserPreviewError::invalid_username, index);
            }
            separators[separator_count++] = index;
        }
    }
    if (separator_count != 3 || separators[0] == 0 || separators[1] == separators[0] + 1 ||
        separators[2] == separators[1] + 1 || separators[2] + 1 >= payload.bytes) {
        return user_failure(RemoteAuthUserPreviewError::missing_field);
    }

    u64 user_id = 0;
    for (u32 index = 0; index < separators[0]; ++index) {
        const u8 value = static_cast<u8>(payload.data[index]);
        if (value < '0' || value > '9') {
            return user_failure(RemoteAuthUserPreviewError::invalid_user_id, index);
        }
        const u32 digit = value - '0';
        if (user_id > (~0ull - digit) / 10ull) {
            return user_failure(RemoteAuthUserPreviewError::invalid_user_id, index);
        }
        user_id = user_id * 10ull + digit;
    }
    if (user_id == 0) {
        return user_failure(RemoteAuthUserPreviewError::invalid_user_id);
    }

    const Utf8View discriminator{payload.data + separators[0] + 1, separators[1] - separators[0] - 1};
    bool valid_discriminator = discriminator.bytes == 1 && discriminator.data[0] == '0';
    if (!valid_discriminator && discriminator.bytes == 4) {
        valid_discriminator = true;
        for (u32 index = 0; index < discriminator.bytes; ++index) {
            valid_discriminator =
                valid_discriminator && discriminator.data[index] >= '0' && discriminator.data[index] <= '9';
        }
    }
    if (!valid_discriminator) {
        return user_failure(RemoteAuthUserPreviewError::invalid_discriminator, separators[0] + 1);
    }

    const Utf8View avatar{payload.data + separators[1] + 1, separators[2] - separators[1] - 1};
    u32 avatar_hex_offset = 0;
    bool valid_avatar = avatar.bytes == 1 && avatar.data[0] == '0';
    if (!valid_avatar && asset_hash_animated(avatar)) {
        avatar_hex_offset = 2;
        valid_avatar = true;
    } else if (!valid_avatar && avatar.bytes == 32) {
        valid_avatar = true;
    }
    for (u32 index = avatar_hex_offset; valid_avatar && index < avatar.bytes; ++index) {
        const char value = avatar.data[index];
        valid_avatar = (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                       (value >= 'A' && value <= 'F');
    }
    if (!valid_avatar) {
        return user_failure(RemoteAuthUserPreviewError::invalid_avatar, separators[1] + 1);
    }

    const Utf8View username{payload.data + separators[2] + 1, payload.bytes - separators[2] - 1};
    const Utf8Validation username_validation = utf8_validate(username);
    if (!username_validation.valid || username_validation.codepoints == 0 ||
        username_validation.codepoints > 32) {
        return user_failure(RemoteAuthUserPreviewError::invalid_username,
                            separators[2] + 1 + username_validation.error_offset);
    }

    *preview = RemoteAuthUserPreview{user_id, discriminator, avatar, username};
    return user_failure(RemoteAuthUserPreviewError::none);
}

RemoteAuthWriteResult remote_auth_write_init(Utf8View encoded_public_key, Slice<char> output) noexcept {
    const Base64Result checked = base64_decode(encoded_public_key, Slice<u8>{nullptr, 0});
    if (checked.error != Base64Error::output_too_small || checked.bytes < 256 || checked.bytes > 320) {
        return RemoteAuthWriteResult{0, RemoteAuthWriteError::invalid_input};
    }
    return write_safe_field(utf8_literal("{\"op\":\"init\",\"encoded_public_key\":\""), encoded_public_key,
                            utf8_literal("\"}"), output);
}

RemoteAuthWriteResult remote_auth_write_nonce_proof(Utf8View nonce, Slice<char> output) noexcept {
    const Base64UrlResult checked = base64url_decode(nonce, Slice<u8>{nullptr, 0});
    if (checked.error != Base64UrlError::output_too_small || checked.bytes == 0 ||
        checked.bytes > kRemoteAuthMaximumNonceBytes) {
        return RemoteAuthWriteResult{0, RemoteAuthWriteError::invalid_input};
    }
    return write_safe_field(utf8_literal("{\"op\":\"nonce_proof\",\"nonce\":\""), nonce,
                            utf8_literal("\"}"), output);
}

RemoteAuthWriteResult remote_auth_write_heartbeat(Slice<char> output) noexcept {
    return write_literal(utf8_literal("{\"op\":\"heartbeat\"}"), output);
}

RemoteAuthWriteResult remote_auth_write_ticket_request(Utf8View ticket, Slice<char> output) noexcept {
    if (ticket.bytes > kRemoteAuthMaximumTicketBytes || !safe_wire_string(ticket)) {
        return RemoteAuthWriteResult{0, RemoteAuthWriteError::invalid_input};
    }
    return write_safe_field(utf8_literal("{\"ticket\":\""), ticket, utf8_literal("\"}"), output);
}

RemoteAuthWriteResult remote_auth_write_qr_url(Utf8View fingerprint, Slice<char> output) noexcept {
    const Base64UrlResult checked = base64url_decode(fingerprint, Slice<u8>{nullptr, 0});
    if (checked.error != Base64UrlError::output_too_small || checked.bytes != kRemoteAuthFingerprintBytes) {
        return RemoteAuthWriteResult{0, RemoteAuthWriteError::invalid_input};
    }
    return write_safe_field(utf8_literal("https://discord.com/ra/"), fingerprint, Utf8View{}, output);
}

RemoteAuthDecodeResult remote_auth_decode_ticket_response(Utf8View body, Slice<u32> structural_scratch,
                                                          u32 maximum_depth, Slice<char> encrypted_token,
                                                          u32* encrypted_token_bytes) noexcept {
    if (!encrypted_token_bytes || (!encrypted_token.data && encrypted_token.count != 0)) {
        return failure(RemoteAuthError::invalid_input);
    }
    DecodeContext context{};
    const RemoteAuthDecodeResult parsed = parse(body, structural_scratch, maximum_depth, &context);
    if (!parsed.ok()) {
        return parsed;
    }
    if ((context.fields & field_encrypted_token) == 0) {
        return failure(RemoteAuthError::missing_field);
    }
    Base64Error base64_error = Base64Error::none;
    if (!validate_standard_base64(context.encrypted_token, kRemoteAuthRsaBytes, &base64_error)) {
        return failure(RemoteAuthError::invalid_field, context.encrypted_token.source_offset,
                       JsonScanError::none, JsonReadError::none, base64_error);
    }
    if (context.encrypted_token.encoded.bytes > encrypted_token.count || !encrypted_token.data) {
        return failure(RemoteAuthError::output_too_small, context.encrypted_token.source_offset);
    }
    copy_string(context.encrypted_token, encrypted_token);
    *encrypted_token_bytes = context.encrypted_token.encoded.bytes;
    return failure(RemoteAuthError::none);
}

} // namespace maicord
