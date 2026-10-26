#include "maicord/protocol/webauthn_mfa.hpp"
#include "maicord/core/memory.hpp"


namespace maicord {
namespace {

constexpr Utf8View kWebAuthnRpId = utf8_literal("discord.com");

struct RootContext {
    JsonValue public_key;
    JsonString mediation;
    WebAuthnMfaError error;
    bool public_key_seen;
    bool mediation_seen;
};

struct PublicKeyContext {
    JsonString challenge;
    JsonString rp_id;
    JsonString user_verification;
    JsonValue allow_credentials;
    JsonValue extensions;
    u32 timeout_ms;
    WebAuthnMfaError error;
    u32 fields;
};

struct CredentialArrayContext {
    JsonValue values[kWebAuthnMfaMaximumAllowedCredentials];
    WebAuthnMfaError error;
    u32 count;
};

struct CredentialContext {
    JsonString id;
    JsonString type;
    JsonValue transports;
    WebAuthnMfaError error;
    u8 fields;
};

struct TransportContext {
    u32 flags;
    WebAuthnMfaError error;
};

struct ExtensionContext {
    WebAuthnMfaError error;
    bool uvm;
    bool uvm_seen;
};

WebAuthnMfaResult failure(WebAuthnMfaError error, u32 offset = 0,
                          JsonScanError scan_error = JsonScanError::none,
                          JsonReadError read_error = JsonReadError::none,
                          Base64UrlError base64_error = Base64UrlError::none, u32 bytes = 0) noexcept {
    return WebAuthnMfaResult{bytes, offset, error, scan_error, read_error, base64_error};
}

bool mark(u32* fields, u32 bit, WebAuthnMfaError* error) noexcept {
    if ((*fields & bit) != 0) {
        *error = WebAuthnMfaError::duplicate_field;
        return false;
    }
    *fields |= bit;
    return true;
}

bool reject(WebAuthnMfaError* error) noexcept {
    if (*error == WebAuthnMfaError::none)
        *error = WebAuthnMfaError::invalid_field;
    return false;
}

bool decode_string(const JsonString& string, const Slice<char>& output, u32* bytes) noexcept {
    const JsonStringDecodeResult decoded = json_string_decode_utf8(string, output);
    if (!decoded.ok()) {
        return false;
    }
    *bytes = decoded.bytes;
    return true;
}

bool visit_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<RootContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("publicKey"))) {
        if (context->public_key_seen) {
            context->error = WebAuthnMfaError::duplicate_field;
            return false;
        }
        context->public_key_seen = true;
        if (value.kind != JsonValueKind::object) {
            context->error = WebAuthnMfaError::invalid_field;
            return false;
        }
        context->public_key = value;
    } else if (json_string_equals_ascii(name, utf8_literal("mediation"))) {
        if (context->mediation_seen) {
            context->error = WebAuthnMfaError::duplicate_field;
            return false;
        }
        context->mediation_seen = true;
        if (!json_value_as_string(value, &context->mediation)) {
            context->error = WebAuthnMfaError::invalid_field;
            return false;
        }
    }
    return true;
}

bool visit_public_key(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<PublicKeyContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("challenge"))) {
        if (!mark(&context->fields, 1u << 0, &context->error) ||
            !json_value_as_string(value, &context->challenge)) {
            return reject(&context->error);
        }
    } else if (json_string_equals_ascii(name, utf8_literal("rpId"))) {
        if (!mark(&context->fields, 1u << 1, &context->error) ||
            !json_value_as_string(value, &context->rp_id)) {
            return reject(&context->error);
        }
    } else if (json_string_equals_ascii(name, utf8_literal("timeout"))) {
        u64 timeout = 0;
        if (!mark(&context->fields, 1u << 2, &context->error) || !json_value_to_u64(value, &timeout) ||
            timeout == 0 || timeout > 600'000ull) {
            return reject(&context->error);
        }
        context->timeout_ms = static_cast<u32>(timeout);
    } else if (json_string_equals_ascii(name, utf8_literal("allowCredentials"))) {
        if (!mark(&context->fields, 1u << 3, &context->error) || value.kind != JsonValueKind::array) {
            return reject(&context->error);
        }
        context->allow_credentials = value;
    } else if (json_string_equals_ascii(name, utf8_literal("userVerification"))) {
        if (!mark(&context->fields, 1u << 4, &context->error) ||
            !json_value_as_string(value, &context->user_verification)) {
            return reject(&context->error);
        }
    } else if (json_string_equals_ascii(name, utf8_literal("extensions"))) {
        if (!mark(&context->fields, 1u << 5, &context->error) || value.kind != JsonValueKind::object) {
            return reject(&context->error);
        }
        context->extensions = value;
    }
    return true;
}

bool visit_credential_array(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<CredentialArrayContext*>(opaque);
    if (value.kind != JsonValueKind::object) {
        context->error = WebAuthnMfaError::invalid_field;
        return false;
    }
    if (context->count >= kWebAuthnMfaMaximumAllowedCredentials) {
        context->error = WebAuthnMfaError::too_many_credentials;
        return false;
    }
    context->values[context->count++] = value;
    return true;
}

bool visit_credential(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<CredentialContext*>(opaque);
    u32 fields = context->fields;
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!mark(&fields, 1u << 0, &context->error) || !json_value_as_string(value, &context->id)) {
            return reject(&context->error);
        }
    } else if (json_string_equals_ascii(name, utf8_literal("type"))) {
        if (!mark(&fields, 1u << 1, &context->error) || !json_value_as_string(value, &context->type)) {
            return reject(&context->error);
        }
    } else if (json_string_equals_ascii(name, utf8_literal("transports"))) {
        if (!mark(&fields, 1u << 2, &context->error) || value.kind != JsonValueKind::array) {
            return reject(&context->error);
        }
        context->transports = value;
    }
    context->fields = static_cast<u8>(fields);
    return true;
}

bool visit_transport(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<TransportContext*>(opaque);
    JsonString string{};
    char decoded[24]{};
    u32 bytes = 0;
    if (!json_value_as_string(value, &string) ||
        !decode_string(string, Slice<char>{decoded, sizeof(decoded)}, &bytes)) {
        context->error = WebAuthnMfaError::invalid_field;
        return false;
    }
    if (utf8_equal({decoded, bytes}, utf8_literal("usb"))) {
        context->flags |= webauthn_transport_usb;
    } else if (utf8_equal({decoded, bytes}, utf8_literal("nfc"))) {
        context->flags |= webauthn_transport_nfc;
    } else if (utf8_equal({decoded, bytes}, utf8_literal("ble"))) {
        context->flags |= webauthn_transport_ble;
    } else if (utf8_equal({decoded, bytes}, utf8_literal("internal"))) {
        context->flags |= webauthn_transport_internal;
    } else if (utf8_equal({decoded, bytes}, utf8_literal("hybrid")) ||
               utf8_equal({decoded, bytes}, utf8_literal("cable"))) {
        context->flags |= webauthn_transport_hybrid;
    }
    return true;
}

bool visit_extension(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ExtensionContext*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("uvm"))) {
        return true;
    }
    if (context->uvm_seen) {
        context->error = WebAuthnMfaError::duplicate_field;
        return false;
    }
    context->uvm_seen = true;
    if (!json_value_to_bool(value, &context->uvm)) {
        context->error = WebAuthnMfaError::invalid_field;
        return false;
    }
    return true;
}

WebAuthnMfaResult visit_object(JsonValue value, Slice<u32> scratch, u32 maximum_depth,
                               JsonMemberVisitor visitor, void* context) noexcept {
    const JsonScanResult scan = json_scan_structurals_avx1(value.source, scratch);
    if (!scan.ok()) {
        return failure(WebAuthnMfaError::scan_failed, scan.error_offset, scan.error);
    }
    const JsonReadResult read = json_visit_value_object(
        value, ConstSlice<u32>{scratch.data, scan.count}, maximum_depth, visitor, context);
    return read.ok() ? failure(WebAuthnMfaError::none)
                     : failure(WebAuthnMfaError::invalid_json, read.error_offset, JsonScanError::none,
                               read.error);
}

WebAuthnMfaResult visit_array(JsonValue value, Slice<u32> scratch, u32 maximum_depth,
                              JsonArrayVisitor visitor, void* context) noexcept {
    const JsonScanResult scan = json_scan_structurals_avx1(value.source, scratch);
    if (!scan.ok()) {
        return failure(WebAuthnMfaError::scan_failed, scan.error_offset, scan.error);
    }
    const JsonReadResult read = json_visit_value_array(
        value, ConstSlice<u32>{scratch.data, scan.count}, maximum_depth, visitor, context);
    return read.ok() ? failure(WebAuthnMfaError::none)
                     : failure(WebAuthnMfaError::invalid_json, read.error_offset, JsonScanError::none,
                               read.error);
}

u32 encoded_size(u32 bytes) noexcept {
    return (bytes / 3u) * 4u + (bytes % 3u == 0 ? 0u : bytes % 3u + 1u);
}

void write_literal(const Slice<u8>& output, u32* offset, const Utf8View& value) noexcept {
    copy_bytes(output.data + *offset, value.data, value.bytes);
    *offset += value.bytes;
}

bool valid_binary(const ConstSlice<u8>& value, bool require_nonempty) noexcept {
    return (!require_nonempty || value.count != 0) && (value.data || value.count == 0);
}

} // namespace

WebAuthnMfaResult webauthn_mfa_decode_request(Utf8View json, Slice<u32> structural_scratch,
                                              u32 maximum_depth,
                                              WebAuthnMfaRequestOptions* output) noexcept {
    if (!output || !json.data || json.bytes == 0 || !structural_scratch.data ||
        structural_scratch.count == 0 || maximum_depth == 0) {
        return failure(WebAuthnMfaError::invalid_input);
    }
    const JsonScanResult root_scan = json_scan_structurals_avx1(json, structural_scratch);
    if (!root_scan.ok()) {
        return failure(WebAuthnMfaError::scan_failed, root_scan.error_offset, root_scan.error);
    }
    RootContext root{};
    const JsonReadResult root_read = json_visit_root_object(
        json, ConstSlice<u32>{structural_scratch.data, root_scan.count}, maximum_depth, visit_root, &root);
    if (!root_read.ok()) {
        return root.error != WebAuthnMfaError::none
                   ? failure(root.error, root_read.error_offset, JsonScanError::none, root_read.error)
                   : failure(WebAuthnMfaError::invalid_json, root_read.error_offset, JsonScanError::none,
                             root_read.error);
    }
    if (!root.public_key_seen) {
        return failure(WebAuthnMfaError::missing_required_field);
    }

    PublicKeyContext public_key{};
    public_key.timeout_ms = 60'000;
    WebAuthnMfaResult visited =
        visit_object(root.public_key, structural_scratch, maximum_depth, visit_public_key, &public_key);
    if (!visited.ok()) {
        return public_key.error != WebAuthnMfaError::none
                   ? failure(public_key.error, visited.error_offset, visited.scan_error, visited.read_error)
                   : visited;
    }
    constexpr u32 required_fields = (1u << 0) | (1u << 1);
    if ((public_key.fields & required_fields) != required_fields) {
        return failure(WebAuthnMfaError::missing_required_field);
    }

    WebAuthnMfaRequestOptions parsed{};
    parsed.timeout_ms = public_key.timeout_ms;
    parsed.user_verification = WebAuthnUserVerification::preferred;
    u32 rp_bytes = 0;
    if (!decode_string(public_key.rp_id,
                       Slice<char>{parsed.rp_id, static_cast<u32>(sizeof(parsed.rp_id) - 1)}, &rp_bytes)) {
        return failure(WebAuthnMfaError::value_too_large, public_key.rp_id.source_offset);
    }
    if (!utf8_equal({parsed.rp_id, rp_bytes}, kWebAuthnRpId)) {
        return failure(WebAuthnMfaError::relying_party_mismatch, public_key.rp_id.source_offset);
    }
    parsed.rp_id[rp_bytes] = 0;

    char encoded_challenge[2048]{};
    u32 encoded_challenge_bytes = 0;
    if (!decode_string(public_key.challenge,
                       Slice<char>{encoded_challenge, static_cast<u32>(sizeof(encoded_challenge))},
                       &encoded_challenge_bytes)) {
        return failure(WebAuthnMfaError::value_too_large, public_key.challenge.source_offset);
    }
    const Base64UrlResult challenge =
        base64url_decode(Utf8View{encoded_challenge, encoded_challenge_bytes},
                         Slice<u8>{parsed.challenge, static_cast<u32>(sizeof(parsed.challenge))});
    secure_clear_bytes(encoded_challenge, sizeof(encoded_challenge));
    if (!challenge.ok() || challenge.bytes == 0) {
        return failure(challenge.error == Base64UrlError::output_too_small
                           ? WebAuthnMfaError::value_too_large
                           : WebAuthnMfaError::base64,
                       public_key.challenge.source_offset + challenge.error_offset, JsonScanError::none,
                       JsonReadError::none, challenge.error);
    }
    parsed.challenge_bytes = challenge.bytes;

    if ((public_key.fields & (1u << 4)) != 0) {
        char verification[24]{};
        u32 verification_bytes = 0;
        if (!decode_string(public_key.user_verification,
                           Slice<char>{verification, static_cast<u32>(sizeof(verification))},
                           &verification_bytes)) {
            return failure(WebAuthnMfaError::invalid_field);
        }
        if (utf8_equal({verification, verification_bytes}, utf8_literal("required"))) {
            parsed.user_verification = WebAuthnUserVerification::required;
        } else if (utf8_equal({verification, verification_bytes}, utf8_literal("preferred"))) {
            parsed.user_verification = WebAuthnUserVerification::preferred;
        } else if (utf8_equal({verification, verification_bytes}, utf8_literal("discouraged"))) {
            parsed.user_verification = WebAuthnUserVerification::discouraged;
        } else {
            return failure(WebAuthnMfaError::invalid_field, public_key.user_verification.source_offset);
        }
    }

    if (root.mediation_seen) {
        char mediation[24]{};
        u32 mediation_bytes = 0;
        if (!decode_string(root.mediation, Slice<char>{mediation, static_cast<u32>(sizeof(mediation))},
                           &mediation_bytes)) {
            return failure(WebAuthnMfaError::invalid_field);
        }
        if (!utf8_equal({mediation, mediation_bytes}, utf8_literal("conditional")) &&
            !utf8_equal({mediation, mediation_bytes}, utf8_literal("optional")) &&
            !utf8_equal({mediation, mediation_bytes}, utf8_literal("required")) &&
            !utf8_equal({mediation, mediation_bytes}, utf8_literal("silent"))) {
            return failure(WebAuthnMfaError::invalid_field, root.mediation.source_offset);
        }
    }

    CredentialArrayContext credential_values{};
    if ((public_key.fields & (1u << 3)) != 0) {
        visited = visit_array(public_key.allow_credentials, structural_scratch, maximum_depth,
                              visit_credential_array, &credential_values);
        if (!visited.ok()) {
            return credential_values.error != WebAuthnMfaError::none
                       ? failure(credential_values.error, visited.error_offset, visited.scan_error,
                                 visited.read_error)
                       : visited;
        }
    }
    for (u32 index = 0; index < credential_values.count; ++index) {
        CredentialContext credential{};
        visited = visit_object(credential_values.values[index], structural_scratch, maximum_depth,
                               visit_credential, &credential);
        if (!visited.ok()) {
            return credential.error != WebAuthnMfaError::none
                       ? failure(credential.error, visited.error_offset, visited.scan_error,
                                 visited.read_error)
                       : visited;
        }
        if ((credential.fields & 3u) != 3u) {
            return failure(WebAuthnMfaError::missing_required_field,
                           credential_values.values[index].source_offset);
        }
        char type[24]{};
        u32 type_bytes = 0;
        if (!decode_string(credential.type, Slice<char>{type, static_cast<u32>(sizeof(type))},
                           &type_bytes) ||
            !utf8_equal({type, type_bytes}, utf8_literal("public-key"))) {
            return failure(WebAuthnMfaError::invalid_field, credential.type.source_offset);
        }
        char encoded_id[2048]{};
        u32 encoded_id_bytes = 0;
        if (!decode_string(credential.id, Slice<char>{encoded_id, static_cast<u32>(sizeof(encoded_id))},
                           &encoded_id_bytes)) {
            return failure(WebAuthnMfaError::value_too_large, credential.id.source_offset);
        }
        if (parsed.credential_id_bytes >= kWebAuthnMfaCredentialIdStorageBytes) {
            secure_clear_bytes(encoded_id, sizeof(encoded_id));
            return failure(WebAuthnMfaError::value_too_large, credential.id.source_offset);
        }
        const u32 available = kWebAuthnMfaCredentialIdStorageBytes - parsed.credential_id_bytes;
        Base64UrlResult decoded_id =
            base64url_decode(Utf8View{encoded_id, encoded_id_bytes},
                             Slice<u8>{parsed.credential_ids + parsed.credential_id_bytes,
                                       minimum(available, kWebAuthnMfaMaximumCredentialIdBytes)});
        secure_clear_bytes(encoded_id, sizeof(encoded_id));
        if (!decoded_id.ok() || decoded_id.bytes == 0) {
            return failure(decoded_id.error == Base64UrlError::output_too_small
                               ? WebAuthnMfaError::value_too_large
                               : WebAuthnMfaError::base64,
                           credential.id.source_offset + decoded_id.error_offset, JsonScanError::none,
                           JsonReadError::none, decoded_id.error);
        }
        WebAuthnAllowedCredential allowed{parsed.credential_id_bytes, decoded_id.bytes,
                                          webauthn_transport_none};
        if ((credential.fields & (1u << 2)) != 0) {
            TransportContext transports{};
            visited = visit_array(credential.transports, structural_scratch, maximum_depth, visit_transport,
                                  &transports);
            if (!visited.ok()) {
                return transports.error != WebAuthnMfaError::none
                           ? failure(transports.error, visited.error_offset, visited.scan_error,
                                     visited.read_error)
                           : visited;
            }
            allowed.transports = transports.flags;
        }
        parsed.credentials[parsed.credential_count++] = allowed;
        parsed.credential_id_bytes += decoded_id.bytes;
    }

    if ((public_key.fields & (1u << 5)) != 0) {
        ExtensionContext extensions{};
        visited = visit_object(public_key.extensions, structural_scratch, maximum_depth, visit_extension,
                               &extensions);
        if (!visited.ok()) {
            return extensions.error != WebAuthnMfaError::none
                       ? failure(extensions.error, visited.error_offset, visited.scan_error,
                                 visited.read_error)
                       : visited;
        }
    }
    *output = parsed;
    return failure(WebAuthnMfaError::none);
}

WebAuthnMfaResult webauthn_mfa_write_client_data(const WebAuthnMfaRequestOptions& request,
                                                 Slice<u8> output) noexcept {
    if (request.challenge_bytes == 0 || request.challenge_bytes > kWebAuthnMfaMaximumChallengeBytes ||
        (!output.data && output.count != 0)) {
        return failure(WebAuthnMfaError::invalid_input);
    }
    const u32 challenge_encoded = encoded_size(request.challenge_bytes);
    constexpr Utf8View prefix = utf8_literal(R"({"type":"webauthn.get","challenge":")");
    constexpr Utf8View suffix = utf8_literal(R"(","origin":"https://discord.com","crossOrigin":false})");
    const u64 required64 = static_cast<u64>(prefix.bytes) + challenge_encoded + suffix.bytes;
    if (required64 > kWebAuthnMfaMaximumClientDataBytes) {
        return failure(WebAuthnMfaError::value_too_large);
    }
    const u32 required = static_cast<u32>(required64);
    if (required > output.count || (required != 0 && !output.data)) {
        return failure(WebAuthnMfaError::output_too_small, 0, JsonScanError::none, JsonReadError::none,
                       Base64UrlError::none, required);
    }
    u32 offset = 0;
    write_literal(output, &offset, prefix);
    const Base64UrlResult encoded =
        base64url_encode(ConstSlice<u8>{request.challenge, request.challenge_bytes},
                         Slice<char>{reinterpret_cast<char*>(output.data + offset), output.count - offset});
    if (!encoded.ok()) {
        return failure(WebAuthnMfaError::base64, encoded.error_offset, JsonScanError::none,
                       JsonReadError::none, encoded.error);
    }
    offset += encoded.bytes;
    write_literal(output, &offset, suffix);
    return failure(WebAuthnMfaError::none, 0, JsonScanError::none, JsonReadError::none,
                   Base64UrlError::none, offset);
}

WebAuthnMfaResult webauthn_mfa_write_assertion(const WebAuthnMfaAssertionView& assertion,
                                               Slice<u8> output) noexcept {
    if (!valid_binary(assertion.credential_id, true) || !valid_binary(assertion.client_data_json, true) ||
        !valid_binary(assertion.authenticator_data, true) || !valid_binary(assertion.signature, true) ||
        !valid_binary(assertion.user_handle, false) || (!output.data && output.count != 0)) {
        return failure(WebAuthnMfaError::invalid_input);
    }
    constexpr Utf8View prefix = utf8_literal(R"({"id":")");
    constexpr Utf8View raw_id = utf8_literal(R"(","rawId":")");
    constexpr Utf8View response = utf8_literal(R"(","response":{"authenticatorData":")");
    constexpr Utf8View client = utf8_literal(R"(","clientDataJSON":")");
    constexpr Utf8View signature = utf8_literal(R"(","signature":")");
    constexpr Utf8View user = utf8_literal(R"(","userHandle":)");
    constexpr Utf8View tail =
        utf8_literal(R"(},"type":"public-key","clientExtensionResults":{},"authenticatorAttachment":)");
    constexpr Utf8View null_value = utf8_literal("null");
    constexpr Utf8View platform = utf8_literal(R"("platform")");
    constexpr Utf8View cross_platform = utf8_literal(R"("cross-platform")");

    const u32 id_bytes = encoded_size(assertion.credential_id.count);
    const u32 client_bytes = encoded_size(assertion.client_data_json.count);
    const u32 authenticator_bytes = encoded_size(assertion.authenticator_data.count);
    const u32 signature_bytes = encoded_size(assertion.signature.count);
    const u32 user_bytes = encoded_size(assertion.user_handle.count);
    const Utf8View attachment = assertion.attachment == WebAuthnAuthenticatorAttachment::platform ? platform
                                : assertion.attachment == WebAuthnAuthenticatorAttachment::cross_platform
                                    ? cross_platform
                                    : null_value;
    const u64 required64 = static_cast<u64>(prefix.bytes) + id_bytes + raw_id.bytes + id_bytes +
                           response.bytes + authenticator_bytes + client.bytes + client_bytes +
                           signature.bytes + signature_bytes + user.bytes +
                           (assertion.user_handle.count == 0 ? null_value.bytes : user_bytes + 2u) +
                           tail.bytes + attachment.bytes + 1u;
    if (required64 > 0xFFFFFFFFull) {
        return failure(WebAuthnMfaError::value_too_large);
    }
    const u32 required = static_cast<u32>(required64);
    if (required > output.count || (required != 0 && !output.data)) {
        return failure(WebAuthnMfaError::output_too_small, 0, JsonScanError::none, JsonReadError::none,
                       Base64UrlError::none, required);
    }
    u32 offset = 0;
    write_literal(output, &offset, prefix);
    Base64UrlResult encoded =
        base64url_encode(assertion.credential_id,
                         Slice<char>{reinterpret_cast<char*>(output.data + offset), output.count - offset});
    offset += encoded.bytes;
    write_literal(output, &offset, raw_id);
    encoded =
        base64url_encode(assertion.credential_id,
                         Slice<char>{reinterpret_cast<char*>(output.data + offset), output.count - offset});
    offset += encoded.bytes;
    write_literal(output, &offset, response);
    encoded =
        base64url_encode(assertion.authenticator_data,
                         Slice<char>{reinterpret_cast<char*>(output.data + offset), output.count - offset});
    offset += encoded.bytes;
    write_literal(output, &offset, client);
    encoded =
        base64url_encode(assertion.client_data_json,
                         Slice<char>{reinterpret_cast<char*>(output.data + offset), output.count - offset});
    offset += encoded.bytes;
    write_literal(output, &offset, signature);
    encoded =
        base64url_encode(assertion.signature,
                         Slice<char>{reinterpret_cast<char*>(output.data + offset), output.count - offset});
    offset += encoded.bytes;
    write_literal(output, &offset, user);
    if (assertion.user_handle.count == 0) {
        write_literal(output, &offset, null_value);
    } else {
        output.data[offset++] = '"';
        encoded = base64url_encode(
            assertion.user_handle,
            Slice<char>{reinterpret_cast<char*>(output.data + offset), output.count - offset});
        offset += encoded.bytes;
        output.data[offset++] = '"';
    }
    write_literal(output, &offset, tail);
    write_literal(output, &offset, attachment);
    output.data[offset++] = '}';
    return failure(WebAuthnMfaError::none, 0, JsonScanError::none, JsonReadError::none,
                   Base64UrlError::none, offset);
}

} // namespace maicord
