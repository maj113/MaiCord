#include "maicord/protocol/auth/webauthn_mfa.hpp"
#include "maicord/core/memory.hpp"
#include "../encoding/json_field_lists.hpp"


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

// The position of a string of at most 24 decoded bytes in the list (the list's count when it is not listed), or ~0u
// when it is longer or does not decode.
template <protocol_detail::MemberName... Names> u32 short_string_index(const JsonString& string) noexcept {
    char decoded[24];
    const JsonStringDecodeResult result = json_string_decode_utf8(string, Slice<char>{decoded, sizeof(decoded)});
    return result.ok() ? protocol_detail::member_index<Names...>(JsonString{{decoded, result.bytes}, 0, false}) : ~0u;
}

bool visit_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<RootContext*>(opaque);
    const u32 index = protocol_detail::member_index<"publicKey", "mediation">(name);
    if (index == 0) {
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
    } else if (index == 1) {
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
    const u32 index = protocol_detail::member_index<"challenge", "rpId", "timeout", "allowCredentials",
                                                    "userVerification", "extensions">(name);
    if (index > 5) {
        return true;
    }
    if (!mark(&context->fields, 1u << index, &context->error)) {
        return reject(&context->error);
    }
    switch (index) {
    case 0:
        if (!json_value_as_string(value, &context->challenge)) {
            return reject(&context->error);
        }
        break;
    case 1:
        if (!json_value_as_string(value, &context->rp_id)) {
            return reject(&context->error);
        }
        break;
    case 2: {
        u64 timeout = 0;
        if (!json_value_to_u64(value, &timeout) || timeout == 0 || timeout > 600'000ull) {
            return reject(&context->error);
        }
        context->timeout_ms = static_cast<u32>(timeout);
        break;
    }
    case 3:
        if (value.kind != JsonValueKind::array) {
            return reject(&context->error);
        }
        context->allow_credentials = value;
        break;
    case 4:
        if (!json_value_as_string(value, &context->user_verification)) {
            return reject(&context->error);
        }
        break;
    default:
        if (value.kind != JsonValueKind::object) {
            return reject(&context->error);
        }
        context->extensions = value;
        break;
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
    const u32 index = protocol_detail::member_index<"id", "type", "transports">(name);
    if (index < 3) {
        if (!mark(&fields, 1u << index, &context->error)) {
            return reject(&context->error);
        }
        if (index == 2) {
            if (value.kind != JsonValueKind::array) {
                return reject(&context->error);
            }
            context->transports = value;
        } else if (!json_value_as_string(value, index == 0 ? &context->id : &context->type)) {
            return reject(&context->error);
        }
    }
    context->fields = static_cast<u8>(fields);
    return true;
}

bool visit_transport(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<TransportContext*>(opaque);
    JsonString string{};
    const u32 index = json_value_as_string(value, &string)
                          ? short_string_index<"usb", "nfc", "ble", "internal", "hybrid", "cable">(string)
                          : ~0u;
    if (index == ~0u) {
        context->error = WebAuthnMfaError::invalid_field;
        return false;
    }
    static constexpr u8 kFlags[] = {webauthn_transport_usb, webauthn_transport_nfc, webauthn_transport_ble,
                                    webauthn_transport_internal, webauthn_transport_hybrid,
                                    webauthn_transport_hybrid, webauthn_transport_none};
    context->flags |= kFlags[index];
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

WebAuthnMfaResult visit_object(JsonValue value, const Slice<u32>& scratch, u32 maximum_depth,
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

WebAuthnMfaResult visit_array(JsonValue value, const Slice<u32>& scratch, u32 maximum_depth,
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

// The root object, read up to its publicKey member (root->public_key).
WebAuthnMfaResult read_root(const Utf8View& json, const Slice<u32>& structural_scratch, u32 maximum_depth,
                            RootContext* root) noexcept {
    const JsonScanResult root_scan = json_scan_structurals_avx1(json, structural_scratch);
    if (!root_scan.ok()) {
        return failure(WebAuthnMfaError::scan_failed, root_scan.error_offset, root_scan.error);
    }
    const JsonReadResult root_read = json_visit_root_object(
        json, ConstSlice<u32>{structural_scratch.data, root_scan.count}, maximum_depth, visit_root, root);
    if (!root_read.ok()) {
        return root->error != WebAuthnMfaError::none
                   ? failure(root->error, root_read.error_offset, JsonScanError::none, root_read.error)
                   : failure(WebAuthnMfaError::invalid_json, root_read.error_offset, JsonScanError::none,
                             root_read.error);
    }
    return root->public_key_seen ? failure(WebAuthnMfaError::none)
                                 : failure(WebAuthnMfaError::missing_required_field);
}

// A string member holding unpadded base64url, decoded into `output`.
WebAuthnMfaResult decode_binary(const JsonString& string, Slice<u8> output, u32* bytes) noexcept {
    char encoded[2048];
    u32 encoded_bytes = 0;
    if (!decode_string(string, Slice<char>{encoded, static_cast<u32>(sizeof(encoded))}, &encoded_bytes)) {
        return failure(WebAuthnMfaError::value_too_large, string.source_offset);
    }
    const Base64UrlResult decoded = base64url_decode(Utf8View{encoded, encoded_bytes}, output);
    secure_clear_bytes(encoded, sizeof(encoded));
    if (!decoded.ok() || decoded.bytes == 0) {
        return failure(decoded.error == Base64UrlError::output_too_small ? WebAuthnMfaError::value_too_large
                                                                         : WebAuthnMfaError::base64,
                       string.source_offset + decoded.error_offset, JsonScanError::none, JsonReadError::none,
                       decoded.error);
    }
    *bytes = decoded.bytes;
    return failure(WebAuthnMfaError::none);
}

WebAuthnMfaResult decode_challenge(const JsonString& string, WebAuthnMfaRequestOptions* parsed) noexcept {
    return decode_binary(string, Slice<u8>{parsed->challenge, static_cast<u32>(sizeof(parsed->challenge))},
                         &parsed->challenge_bytes);
}

// A credential list (allowCredentials, or excludeCredentials of a creation) appended to `parsed`.
WebAuthnMfaResult decode_credential_list(const JsonValue& list, const Slice<u32>& structural_scratch,
                                         u32 maximum_depth, WebAuthnMfaRequestOptions* parsed) noexcept {
    CredentialArrayContext credential_values{};
    WebAuthnMfaResult visited = visit_array(list, structural_scratch, maximum_depth, visit_credential_array,
                                            &credential_values);
    if (!visited.ok()) {
        return credential_values.error != WebAuthnMfaError::none
                   ? failure(credential_values.error, visited.error_offset, visited.scan_error, visited.read_error)
                   : visited;
    }
    for (u32 index = 0; index < credential_values.count; ++index) {
        CredentialContext credential{};
        visited = visit_object(credential_values.values[index], structural_scratch, maximum_depth, visit_credential,
                               &credential);
        if (!visited.ok()) {
            return credential.error != WebAuthnMfaError::none
                       ? failure(credential.error, visited.error_offset, visited.scan_error, visited.read_error)
                       : visited;
        }
        if ((credential.fields & 3u) != 3u) {
            return failure(WebAuthnMfaError::missing_required_field, credential_values.values[index].source_offset);
        }
        if (short_string_index<"public-key">(credential.type) != 0) {
            return failure(WebAuthnMfaError::invalid_field, credential.type.source_offset);
        }
        if (parsed->credential_id_bytes >= kWebAuthnMfaCredentialIdStorageBytes) {
            return failure(WebAuthnMfaError::value_too_large, credential.id.source_offset);
        }
        const u32 available = kWebAuthnMfaCredentialIdStorageBytes - parsed->credential_id_bytes;
        u32 id_bytes = 0;
        const WebAuthnMfaResult decoded_id =
            decode_binary(credential.id,
                          Slice<u8>{parsed->credential_ids + parsed->credential_id_bytes,
                                    minimum(available, kWebAuthnMfaMaximumCredentialIdBytes)},
                          &id_bytes);
        if (!decoded_id.ok()) {
            return decoded_id;
        }
        WebAuthnAllowedCredential allowed{parsed->credential_id_bytes, id_bytes, webauthn_transport_none};
        if ((credential.fields & (1u << 2)) != 0) {
            TransportContext transports{};
            visited = visit_array(credential.transports, structural_scratch, maximum_depth, visit_transport,
                                  &transports);
            if (!visited.ok()) {
                return transports.error != WebAuthnMfaError::none
                           ? failure(transports.error, visited.error_offset, visited.scan_error, visited.read_error)
                           : visited;
            }
            allowed.transports = transports.flags;
        }
        parsed->credentials[parsed->credential_count++] = allowed;
        parsed->credential_id_bytes += id_bytes;
    }
    return failure(WebAuthnMfaError::none);
}

// A creation challenge's publicKey members (rp, user, authenticatorSelection and the lists are read afterwards).
struct CreationKeyContext {
    JsonString challenge;
    JsonString attestation;
    JsonValue rp, user, parameters, excluded, selection;
    u32 timeout_ms;
    WebAuthnMfaError error;
    u32 fields;
};

bool visit_creation_key(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<CreationKeyContext*>(opaque);
    const u32 index = protocol_detail::member_index<"challenge", "rp", "user", "pubKeyCredParams", "timeout",
                                                    "excludeCredentials", "authenticatorSelection",
                                                    "attestation">(name);
    if (index > 7) {
        return true;
    }
    if (!mark(&context->fields, 1u << index, &context->error)) {
        return reject(&context->error);
    }
    switch (index) {
    case 0:
        return json_value_as_string(value, &context->challenge) || reject(&context->error);
    case 1:
    case 2:
    case 6:
        if (value.kind != JsonValueKind::object) {
            return reject(&context->error);
        }
        (index == 1 ? context->rp : index == 2 ? context->user : context->selection) = value;
        return true;
    case 3:
    case 5:
        if (value.kind != JsonValueKind::array) {
            return reject(&context->error);
        }
        (index == 3 ? context->parameters : context->excluded) = value;
        return true;
    case 4: {
        u64 timeout = 0;
        if (!json_value_to_u64(value, &timeout) || timeout == 0 || timeout > 600'000ull) {
            return reject(&context->error);
        }
        context->timeout_ms = static_cast<u32>(timeout);
        return true;
    }
    default:
        return json_value_as_string(value, &context->attestation) || reject(&context->error);
    }
}

// rp ({id, name}) and user ({id, name, displayName}): strings all.
struct EntityContext {
    JsonString id, name, display;
    WebAuthnMfaError error;
    u32 fields;
};

bool visit_entity(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EntityContext*>(opaque);
    const u32 index = protocol_detail::member_index<"id", "name", "displayName">(name);
    if (index > 2) {
        return true;
    }
    if (!mark(&context->fields, 1u << index, &context->error)) {
        return reject(&context->error);
    }
    return json_value_as_string(value, index == 0 ? &context->id : index == 1 ? &context->name : &context->display) ||
           reject(&context->error);
}

struct SelectionContext {
    JsonString attachment, resident, verification;
    bool require_resident;
    WebAuthnMfaError error;
    u32 fields;
};

bool visit_selection(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<SelectionContext*>(opaque);
    const u32 index = protocol_detail::member_index<"authenticatorAttachment", "residentKey", "requireResidentKey",
                                                    "userVerification">(name);
    if (index > 3) {
        return true;
    }
    if (!mark(&context->fields, 1u << index, &context->error)) {
        return reject(&context->error);
    }
    if (index == 2) {
        return json_value_to_bool(value, &context->require_resident) || reject(&context->error);
    }
    return json_value_as_string(value, index == 0 ? &context->attachment
                                      : index == 1 ? &context->resident
                                                   : &context->verification) ||
           reject(&context->error);
}

// One pubKeyCredParams entry: {"type": "public-key", "alg": -7}.
struct ParameterContext {
    JsonString type;
    i32 algorithm;
    WebAuthnMfaError error;
    u32 fields;
};

// A COSE algorithm: a whole number of at most seven digits.
bool read_integer(const JsonValue& value, i32* output) noexcept {
    const Utf8View text = value.source;
    if (value.kind != JsonValueKind::number || text.bytes == 0 || text.bytes > 8) {
        return false;
    }
    u32 at = text.data[0] == '-' ? 1u : 0u;
    if (at == text.bytes) {
        return false;
    }
    i32 result = 0;
    for (; at < text.bytes; ++at) {
        if (text.data[at] < '0' || text.data[at] > '9') {
            return false;
        }
        result = result * 10 + (text.data[at] - '0');
    }
    *output = text.data[0] == '-' ? -result : result;
    return true;
}

bool visit_parameter(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ParameterContext*>(opaque);
    const u32 index = protocol_detail::member_index<"type", "alg">(name);
    if (index > 1) {
        return true;
    }
    if (!mark(&context->fields, 1u << index, &context->error)) {
        return reject(&context->error);
    }
    return (index == 0 ? json_value_as_string(value, &context->type) : read_integer(value, &context->algorithm)) ||
           reject(&context->error);
}

// CollectedClientData: the type's prefix up to the challenge, the challenge, then discord.com as the origin.
WebAuthnMfaResult write_client_data(const u8* challenge, u32 challenge_bytes, const Utf8View& prefix,
                                    Slice<u8> output) noexcept {
    if (challenge_bytes == 0 || challenge_bytes > kWebAuthnMfaMaximumChallengeBytes ||
        (!output.data && output.count != 0)) {
        return failure(WebAuthnMfaError::invalid_input);
    }
    const u32 challenge_encoded = encoded_size(challenge_bytes);
    constexpr Utf8View suffix = utf8_literal(R"(","origin":"https://discord.com","crossOrigin":false})");
    const u64 required64 = static_cast<u64>(prefix.bytes) + challenge_encoded + suffix.bytes;
    if (required64 > kWebAuthnMfaMaximumClientDataBytes) {
        return failure(WebAuthnMfaError::value_too_large);
    }
    const u32 required = static_cast<u32>(required64);
    if (required > output.count) {
        return failure(WebAuthnMfaError::output_too_small, 0, JsonScanError::none, JsonReadError::none,
                       Base64UrlError::none, required);
    }
    u32 offset = 0;
    write_literal(output, &offset, prefix);
    const Base64UrlResult encoded =
        base64url_encode(ConstSlice<u8>{challenge, challenge_bytes},
                         Slice<char>{reinterpret_cast<char*>(output.data + offset), output.count - offset});
    if (!encoded.ok()) {
        return failure(WebAuthnMfaError::base64, encoded.error_offset, JsonScanError::none, JsonReadError::none,
                       encoded.error);
    }
    offset += encoded.bytes;
    write_literal(output, &offset, suffix);
    return failure(WebAuthnMfaError::none, 0, JsonScanError::none, JsonReadError::none, Base64UrlError::none, offset);
}

// Writes the credential JSON twice (counting, then for real): a null destination only counts.
struct Emitter {
    u8* destination;
    u32 at;
    bool write;
};

void emit_text(Emitter* e, const Utf8View& text) noexcept {
    if (e->write) {
        copy_bytes(e->destination + e->at, text.data, text.bytes);
    }
    e->at += text.bytes;
}

void emit_binary(Emitter* e, const ConstSlice<u8>& value) noexcept {
    const u32 bytes = encoded_size(value.count);
    if (e->write) {
        base64url_encode(value, Slice<char>{reinterpret_cast<char*>(e->destination + e->at), bytes});
    }
    e->at += bytes;
}

void emit_integer(Emitter* e, i32 value) noexcept {
    char digits[12];
    u32 count = 0;
    u32 magnitude = value < 0 ? 0u - static_cast<u32>(value) : static_cast<u32>(value);
    do {
        digits[count++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (value < 0) {
        emit_text(e, utf8_literal("-"));
    }
    while (count != 0) {
        emit_text(e, Utf8View{&digits[--count], 1});
    }
}

} // namespace

WebAuthnMfaResult webauthn_mfa_decode_request(Utf8View json, Slice<u32> structural_scratch,
                                              u32 maximum_depth,
                                              WebAuthnMfaRequestOptions* output) noexcept {
    if (!output || !json.data || json.bytes == 0 || !structural_scratch.data ||
        structural_scratch.count == 0 || maximum_depth == 0) {
        return failure(WebAuthnMfaError::invalid_input);
    }
    RootContext root{};
    const WebAuthnMfaResult rooted = read_root(json, structural_scratch, maximum_depth, &root);
    if (!rooted.ok()) {
        return rooted;
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

    const WebAuthnMfaResult challenge = decode_challenge(public_key.challenge, &parsed);
    if (!challenge.ok()) {
        return challenge;
    }

    if ((public_key.fields & (1u << 4)) != 0) {
        const u32 verification =
            short_string_index<"preferred", "required", "discouraged">(public_key.user_verification);
        if (verification == ~0u) {
            return failure(WebAuthnMfaError::invalid_field);
        }
        if (verification > 2) {
            return failure(WebAuthnMfaError::invalid_field, public_key.user_verification.source_offset);
        }
        parsed.user_verification = static_cast<WebAuthnUserVerification>(verification);
    }

    if (root.mediation_seen) {
        const u32 mediation = short_string_index<"conditional", "optional", "required", "silent">(root.mediation);
        if (mediation == ~0u) {
            return failure(WebAuthnMfaError::invalid_field);
        }
        if (mediation > 3) {
            return failure(WebAuthnMfaError::invalid_field, root.mediation.source_offset);
        }
    }

    if ((public_key.fields & (1u << 3)) != 0) {
        const WebAuthnMfaResult listed = decode_credential_list(public_key.allow_credentials, structural_scratch,
                                                                maximum_depth, &parsed);
        if (!listed.ok()) {
            return listed;
        }
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
    return write_client_data(request.challenge, request.challenge_bytes,
                             utf8_literal(R"({"type":"webauthn.get","challenge":")"), output);
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
    if (required > output.count) {
        return failure(WebAuthnMfaError::output_too_small, 0, JsonScanError::none, JsonReadError::none,
                       Base64UrlError::none, required);
    }
    u32 offset = 0;
    const auto binary = [&](const ConstSlice<u8>& value) noexcept {
        offset += base64url_encode(value, Slice<char>{reinterpret_cast<char*>(output.data + offset), output.count - offset})
                      .bytes;
    };
    write_literal(output, &offset, prefix);
    binary(assertion.credential_id);
    write_literal(output, &offset, raw_id);
    binary(assertion.credential_id);
    write_literal(output, &offset, response);
    binary(assertion.authenticator_data);
    write_literal(output, &offset, client);
    binary(assertion.client_data_json);
    write_literal(output, &offset, signature);
    binary(assertion.signature);
    write_literal(output, &offset, user);
    if (assertion.user_handle.count == 0) {
        write_literal(output, &offset, null_value);
    } else {
        output.data[offset++] = '"';
        binary(assertion.user_handle);
        output.data[offset++] = '"';
    }
    write_literal(output, &offset, tail);
    write_literal(output, &offset, attachment);
    output.data[offset++] = '}';
    return failure(WebAuthnMfaError::none, 0, JsonScanError::none, JsonReadError::none,
                   Base64UrlError::none, offset);
}

WebAuthnMfaResult webauthn_mfa_decode_creation(Utf8View json, Slice<u32> structural_scratch, u32 maximum_depth,
                                               WebAuthnMfaCreationOptions* output) noexcept {
    if (!output || !json.data || json.bytes == 0 || !structural_scratch.data || structural_scratch.count == 0 ||
        maximum_depth == 0) {
        return failure(WebAuthnMfaError::invalid_input);
    }
    RootContext root{};
    WebAuthnMfaResult visited = read_root(json, structural_scratch, maximum_depth, &root);
    if (!visited.ok()) {
        return visited;
    }
    CreationKeyContext key{};
    key.timeout_ms = 60'000;
    visited = visit_object(root.public_key, structural_scratch, maximum_depth, visit_creation_key, &key);
    if (!visited.ok()) {
        return key.error != WebAuthnMfaError::none
                   ? failure(key.error, visited.error_offset, visited.scan_error, visited.read_error)
                   : visited;
    }
    constexpr u32 required_fields = (1u << 0) | (1u << 1) | (1u << 2);
    if ((key.fields & required_fields) != required_fields) {
        return failure(WebAuthnMfaError::missing_required_field);
    }

    WebAuthnMfaCreationOptions parsed{};
    WebAuthnMfaRequestOptions& request = parsed.request;
    request.timeout_ms = key.timeout_ms;
    request.user_verification = WebAuthnUserVerification::preferred;
    parsed.attestation_none = true;

    EntityContext rp{};
    visited = visit_object(key.rp, structural_scratch, maximum_depth, visit_entity, &rp);
    if (!visited.ok()) {
        return rp.error != WebAuthnMfaError::none
                   ? failure(rp.error, visited.error_offset, visited.scan_error, visited.read_error)
                   : visited;
    }
    if ((rp.fields & 1u) == 0) {
        return failure(WebAuthnMfaError::missing_required_field, key.rp.source_offset);
    }
    u32 text_bytes = 0;
    if (!decode_string(rp.id, Slice<char>{request.rp_id, static_cast<u32>(sizeof(request.rp_id) - 1)}, &text_bytes)) {
        return failure(WebAuthnMfaError::value_too_large, rp.id.source_offset);
    }
    if (!utf8_equal({request.rp_id, text_bytes}, kWebAuthnRpId)) {
        return failure(WebAuthnMfaError::relying_party_mismatch, rp.id.source_offset);
    }
    request.rp_id[text_bytes] = 0;
    if ((rp.fields & 2u) != 0) {
        if (!decode_string(rp.name, Slice<char>{parsed.rp_name, kWebAuthnMfaMaximumEntityNameBytes}, &text_bytes)) {
            return failure(WebAuthnMfaError::value_too_large, rp.name.source_offset);
        }
        parsed.rp_name[text_bytes] = 0;
    }

    EntityContext user{};
    visited = visit_object(key.user, structural_scratch, maximum_depth, visit_entity, &user);
    if (!visited.ok()) {
        return user.error != WebAuthnMfaError::none
                   ? failure(user.error, visited.error_offset, visited.scan_error, visited.read_error)
                   : visited;
    }
    if ((user.fields & 1u) == 0) {
        return failure(WebAuthnMfaError::missing_required_field, key.user.source_offset);
    }
    WebAuthnMfaResult decoded = decode_binary(user.id, Slice<u8>{parsed.user_id, kWebAuthnMfaMaximumUserIdBytes},
                                              &parsed.user_id_bytes);
    if (!decoded.ok()) {
        return decoded;
    }
    if ((user.fields & 2u) != 0) {
        if (!decode_string(user.name, Slice<char>{parsed.user_name, kWebAuthnMfaMaximumEntityNameBytes}, &text_bytes)) {
            return failure(WebAuthnMfaError::value_too_large, user.name.source_offset);
        }
        parsed.user_name[text_bytes] = 0;
    }
    if ((user.fields & 4u) != 0) {
        if (!decode_string(user.display, Slice<char>{parsed.user_display_name, kWebAuthnMfaMaximumEntityNameBytes},
                           &text_bytes)) {
            return failure(WebAuthnMfaError::value_too_large, user.display.source_offset);
        }
        parsed.user_display_name[text_bytes] = 0;
    }
    // A key may show either name: each stands in for the other, and a user with neither is the account.
    if (parsed.user_name[0] == 0) {
        copy_bytes(parsed.user_name, parsed.user_display_name[0] ? parsed.user_display_name : "Discord",
                   parsed.user_display_name[0] ? c_string_length(parsed.user_display_name) + 1 : 8u);
    }
    if (parsed.user_display_name[0] == 0) {
        copy_bytes(parsed.user_display_name, parsed.user_name, c_string_length(parsed.user_name) + 1);
    }

    decoded = decode_challenge(key.challenge, &request);
    if (!decoded.ok()) {
        return decoded;
    }

    if ((key.fields & (1u << 3)) != 0) {
        CredentialArrayContext entries{};
        visited = visit_array(key.parameters, structural_scratch, maximum_depth, visit_credential_array, &entries);
        if (!visited.ok()) {
            return entries.error != WebAuthnMfaError::none
                       ? failure(entries.error, visited.error_offset, visited.scan_error, visited.read_error)
                       : visited;
        }
        for (u32 index = 0; index < entries.count; ++index) {
            ParameterContext parameter{};
            visited = visit_object(entries.values[index], structural_scratch, maximum_depth, visit_parameter,
                                   &parameter);
            if (!visited.ok()) {
                return parameter.error != WebAuthnMfaError::none
                           ? failure(parameter.error, visited.error_offset, visited.scan_error, visited.read_error)
                           : visited;
            }
            // Entries of another type, or of an algorithm no key here offers, are left out.
            if ((parameter.fields & 3u) != 3u || short_string_index<"public-key">(parameter.type) != 0 ||
                parsed.algorithm_count == kWebAuthnMfaMaximumAlgorithms) {
                continue;
            }
            bool listed = false;
            for (u32 known = 0; known < parsed.algorithm_count; ++known) {
                listed = listed || parsed.algorithms[known] == parameter.algorithm;
            }
            if (!listed) {
                parsed.algorithms[parsed.algorithm_count++] = parameter.algorithm;
            }
        }
    }
    if (parsed.algorithm_count == 0) {
        parsed.algorithms[0] = kWebAuthnCoseEs256;
        parsed.algorithms[1] = kWebAuthnCoseRs256;
        parsed.algorithm_count = 2;
    }

    if ((key.fields & (1u << 6)) != 0) {
        SelectionContext selection{};
        visited = visit_object(key.selection, structural_scratch, maximum_depth, visit_selection, &selection);
        if (!visited.ok()) {
            return selection.error != WebAuthnMfaError::none
                       ? failure(selection.error, visited.error_offset, visited.scan_error, visited.read_error)
                       : visited;
        }
        if ((selection.fields & 1u) != 0) {
            const u32 attachment = short_string_index<"platform", "cross-platform">(selection.attachment);
            if (attachment == ~0u) {
                return failure(WebAuthnMfaError::invalid_field, selection.attachment.source_offset);
            }
            parsed.attachment = attachment == 0   ? WebAuthnAuthenticatorAttachment::platform
                                : attachment == 1 ? WebAuthnAuthenticatorAttachment::cross_platform
                                                  : WebAuthnAuthenticatorAttachment::unknown;
        }
        if ((selection.fields & 2u) != 0) {
            const u32 resident = short_string_index<"discouraged", "preferred", "required">(selection.resident);
            if (resident > 2) {
                return failure(WebAuthnMfaError::invalid_field, selection.resident.source_offset);
            }
            parsed.resident_key = static_cast<WebAuthnResidentKey>(resident);
        } else if (selection.require_resident) {
            parsed.resident_key = WebAuthnResidentKey::required;
        }
        if ((selection.fields & 8u) != 0) {
            const u32 verification =
                short_string_index<"preferred", "required", "discouraged">(selection.verification);
            if (verification > 2) {
                return failure(WebAuthnMfaError::invalid_field, selection.verification.source_offset);
            }
            request.user_verification = static_cast<WebAuthnUserVerification>(verification);
        }
    }
    if ((key.fields & (1u << 7)) != 0) {
        const u32 attestation = short_string_index<"none", "indirect", "direct", "enterprise">(key.attestation);
        if (attestation > 3) {
            return failure(WebAuthnMfaError::invalid_field, key.attestation.source_offset);
        }
        parsed.attestation_none = attestation == 0;
    }
    if ((key.fields & (1u << 5)) != 0) {
        decoded = decode_credential_list(key.excluded, structural_scratch, maximum_depth, &request);
        if (!decoded.ok()) {
            return decoded;
        }
    }
    *output = parsed;
    return failure(WebAuthnMfaError::none);
}

WebAuthnMfaResult webauthn_mfa_write_creation_client_data(const WebAuthnMfaCreationOptions& creation,
                                                          Slice<u8> output) noexcept {
    return write_client_data(creation.request.challenge, creation.request.challenge_bytes,
                             utf8_literal(R"({"type":"webauthn.create","challenge":")"), output);
}

WebAuthnMfaResult webauthn_mfa_write_attestation_object(const WebAuthnMfaAttestationObjectView& object,
                                                        Slice<u8> output) noexcept {
    const ConstSlice<u8>& statement = object.attestation_statement;
    const ConstSlice<u8>& data = object.authenticator_data;
    if (!object.format.data || object.format.bytes == 0 || object.format.bytes > 23 || !valid_binary(statement, false) ||
        !valid_binary(data, true) || data.count > 0xFFFFu || (!output.data && output.count != 0)) {
        return failure(WebAuthnMfaError::invalid_input);
    }
    // A map of three: "fmt" and its text, "attStmt" and its map, "authData" and its byte string.
    const u32 header = data.count < 24 ? 1u : data.count < 256 ? 2u : 3u;
    const u32 required = 1 + 4 + 1 + object.format.bytes + 8 + (statement.count ? statement.count : 1u) + 9 + header +
                         data.count;
    if (required > output.count) {
        return failure(WebAuthnMfaError::output_too_small, 0, JsonScanError::none, JsonReadError::none,
                       Base64UrlError::none, required);
    }
    u32 offset = 0;
    const auto put = [&](u8 byte) noexcept { output.data[offset++] = byte; };
    const auto text = [&](const char* name, u32 length) noexcept {
        put(static_cast<u8>(0x60 + length));
        copy_bytes(output.data + offset, name, length);
        offset += length;
    };
    put(0xA3);
    text("fmt", 3);
    put(static_cast<u8>(0x60 + object.format.bytes));
    copy_bytes(output.data + offset, object.format.data, object.format.bytes);
    offset += object.format.bytes;
    text("attStmt", 7);
    if (statement.count) {
        copy_bytes(output.data + offset, statement.data, statement.count);
        offset += statement.count;
    } else {
        put(0xA0);
    }
    text("authData", 8);
    if (header == 1) {
        put(static_cast<u8>(0x40 + data.count));
    } else if (header == 2) {
        put(0x58);
        put(static_cast<u8>(data.count));
    } else {
        put(0x59);
        put(static_cast<u8>(data.count >> 8));
        put(static_cast<u8>(data.count));
    }
    copy_bytes(output.data + offset, data.data, data.count);
    offset += data.count;
    return failure(WebAuthnMfaError::none, 0, JsonScanError::none, JsonReadError::none, Base64UrlError::none, offset);
}

namespace {

struct CborHead {
    u8 major;
    u32 argument;
};

// The head of one CBOR item at `*at` (arguments up to 16 bits only: nothing in a COSE key needs more).
bool cbor_head(const ConstSlice<u8>& data, u32* at, CborHead* head) noexcept {
    if (*at >= data.count) {
        return false;
    }
    const u8 initial = data.data[(*at)++];
    const u32 information = initial & 31u;
    head->major = static_cast<u8>(initial >> 5);
    if (information < 24) {
        head->argument = information;
    } else if (information == 24 && *at + 1 <= data.count) {
        head->argument = data.data[(*at)++];
    } else if (information == 25 && *at + 2 <= data.count) {
        head->argument = static_cast<u32>(data.data[*at]) << 8 | data.data[*at + 1];
        *at += 2;
    } else {
        return false;
    }
    return true;
}

} // namespace

i32 webauthn_mfa_credential_algorithm(ConstSlice<u8> authenticator_data) noexcept {
    // rpIdHash (32), flags (1), signCount (4), AAGUID (16), the credential id's length (2) and the id; the COSE key
    // follows as a CBOR map whose entry 3 is the algorithm.
    if (!authenticator_data.data || authenticator_data.count < 55 || (authenticator_data.data[32] & 0x40u) == 0) {
        return 0;
    }
    u32 at = 55u + (static_cast<u32>(authenticator_data.data[53]) << 8 | authenticator_data.data[54]);
    CborHead map{};
    if (at > authenticator_data.count || !cbor_head(authenticator_data, &at, &map) || map.major != 5 ||
        map.argument > 16) {
        return 0;
    }
    for (u32 entry = 0; entry < map.argument; ++entry) {
        CborHead key{}, value{};
        if (!cbor_head(authenticator_data, &at, &key) || !cbor_head(authenticator_data, &at, &value)) {
            return 0;
        }
        if (key.major == 0 && key.argument == 3) {
            return value.major == 0   ? static_cast<i32>(value.argument)
                   : value.major == 1 ? -1 - static_cast<i32>(value.argument)
                                      : 0;
        }
        if (value.major == 2 || value.major == 3) {
            at += value.argument;
            if (at > authenticator_data.count) {
                return 0;
            }
        } else if (value.major > 1) {
            return 0;
        }
    }
    return 0;
}

void webauthn_mfa_clear_aaguid(u8* authenticator_data, u32 bytes) noexcept {
    // rpIdHash (32 bytes), flags (1), signCount (4); the AAGUID (16) follows when the AT flag (0x40) is set.
    if (authenticator_data && bytes >= 53 && (authenticator_data[32] & 0x40u) != 0) {
        clear_bytes(authenticator_data + 37, 16);
    }
}

WebAuthnMfaResult webauthn_mfa_write_attestation(const WebAuthnMfaAttestationView& attestation,
                                                 Slice<u8> output) noexcept {
    if (!valid_binary(attestation.credential_id, true) || !valid_binary(attestation.client_data_json, true) ||
        !valid_binary(attestation.attestation_object, true) || !valid_binary(attestation.authenticator_data, true) ||
        (!output.data && output.count != 0)) {
        return failure(WebAuthnMfaError::invalid_input);
    }
    // Written twice: once to count, once for real, so a short buffer is never half written.
    const auto run = [&](Emitter* e) noexcept {
        emit_text(e, utf8_literal(R"({"id":")"));
        emit_binary(e, attestation.credential_id);
        emit_text(e, utf8_literal(R"(","rawId":")"));
        emit_binary(e, attestation.credential_id);
        emit_text(e, utf8_literal(R"(","response":{"attestationObject":")"));
        emit_binary(e, attestation.attestation_object);
        emit_text(e, utf8_literal(R"(","authenticatorData":")"));
        emit_binary(e, attestation.authenticator_data);
        emit_text(e, utf8_literal(R"(","clientDataJSON":")"));
        emit_binary(e, attestation.client_data_json);
        emit_text(e, utf8_literal(R"(","publicKeyAlgorithm":)"));
        emit_integer(e, attestation.public_key_algorithm);
        emit_text(e, utf8_literal(R"(,"transports":[)"));
        constexpr Utf8View kNames[] = {utf8_literal("\"usb\""), utf8_literal("\"nfc\""), utf8_literal("\"ble\""),
                                       utf8_literal("\"internal\""), utf8_literal("\"hybrid\"")};
        constexpr u32 kBits[] = {webauthn_transport_usb, webauthn_transport_nfc, webauthn_transport_ble,
                                 webauthn_transport_internal, webauthn_transport_hybrid};
        bool first = true;
        for (u32 index = 0; index < 5; ++index) {
            if ((attestation.transports & kBits[index]) != 0) {
                if (!first) {
                    emit_text(e, utf8_literal(","));
                }
                emit_text(e, kNames[index]);
                first = false;
            }
        }
        emit_text(e, utf8_literal(R"(]},"type":"public-key","clientExtensionResults":{},"authenticatorAttachment":)"));
        emit_text(e, attestation.attachment == WebAuthnAuthenticatorAttachment::platform
                         ? utf8_literal(R"("platform")")
                     : attestation.attachment == WebAuthnAuthenticatorAttachment::cross_platform
                         ? utf8_literal(R"("cross-platform")")
                         : utf8_literal("null"));
        emit_text(e, utf8_literal("}"));
    };
    Emitter counter{};
    run(&counter);
    if (counter.at > output.count) {
        return failure(WebAuthnMfaError::output_too_small, 0, JsonScanError::none, JsonReadError::none,
                       Base64UrlError::none, counter.at);
    }
    Emitter writer{output.data, 0, true};
    run(&writer);
    return failure(WebAuthnMfaError::none, 0, JsonScanError::none, JsonReadError::none, Base64UrlError::none,
                   writer.at);
}

} // namespace maicord
