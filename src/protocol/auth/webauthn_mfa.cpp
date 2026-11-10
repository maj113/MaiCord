#include "maicord/protocol/auth/webauthn_mfa.hpp"
#include "maicord/core/memory.hpp"
#include "../encoding/json_field_lists.hpp"


namespace maicord {
namespace {

constexpr Utf8View kWebAuthnRpId = utf8_literal("discord.com");

WebAuthnMfaResult failure(WebAuthnMfaError error, u32 offset = 0,
                          JsonScanError scan_error = JsonScanError::none,
                          JsonReadError read_error = JsonReadError::none,
                          Base64UrlError base64_error = Base64UrlError::none, u32 bytes = 0) noexcept {
    return WebAuthnMfaResult{bytes, offset, error, scan_error, read_error, base64_error};
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

// The structural index and depth limit the visits of one document share.
struct Document {
    Slice<u32> scratch;
    u32 maximum_depth;
};

// Visits an object (`members`) or an array (`items`). Every visitor context starts with the error its visitor stopped
// on; a visit that fails without one failed on the JSON itself.
WebAuthnMfaResult visit(const Document& document, const JsonValue& value, JsonMemberVisitor members,
                        JsonArrayVisitor items, void* context) noexcept {
    const JsonScanResult scan = json_scan_structurals_avx1(value.source, document.scratch);
    if (!scan.ok()) {
        return failure(WebAuthnMfaError::scan_failed, scan.error_offset, scan.error);
    }
    const ConstSlice<u32> structurals{document.scratch.data, scan.count};
    const JsonReadResult read =
        members ? json_visit_value_object(value, structurals, document.maximum_depth, members, context)
                : json_visit_value_array(value, structurals, document.maximum_depth, items, context);
    if (read.ok()) {
        return failure(WebAuthnMfaError::none);
    }
    const WebAuthnMfaError stopped = *static_cast<const WebAuthnMfaError*>(context);
    return failure(stopped != WebAuthnMfaError::none ? stopped : WebAuthnMfaError::invalid_json, read.error_offset,
                   JsonScanError::none, read.error);
}

// How a member is read: an object or array (the JsonValueKind values) or a string, kept as found, or a timeout (1 to
// 600000 ms), boolean or COSE algorithm, converted as it is read.
enum class MemberKind : u8 { object, array, string, timeout, flag, integer };

// The kinds of an object's members, four bits each in list order.
template <typename... Kinds> constexpr u32 member_kinds(Kinds... kinds) noexcept {
    u32 packed = 0;
    u32 shift = 0;
    ((packed |= static_cast<u32>(kinds) << shift, shift += 4), ...);
    return packed;
}

// The members of one object that `names` (a MemberNames list) lists, each at most once and of its kind.
struct Members {
    WebAuthnMfaError error;
    u32 fields; // 1 << position of every member seen
    const u8* names;
    u32 kinds;
    union Member {
        JsonValue value;
        JsonString string;
    } member[8];
    u32 timeout_ms;
    i32 integer;
    bool flag;
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

bool visit_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* members = static_cast<Members*>(opaque);
    const u32 index = protocol_detail::member_index(name, members->names);
    if (index >= members->names[0]) {
        return true;
    }
    if ((members->fields & (1u << index)) != 0) {
        members->error = WebAuthnMfaError::duplicate_field;
        return false;
    }
    members->fields |= 1u << index;
    const auto kind = static_cast<MemberKind>((members->kinds >> (index * 4)) & 15u);
    bool valid = false;
    switch (kind) {
    case MemberKind::object:
    case MemberKind::array:
        valid = value.kind == static_cast<JsonValueKind>(kind);
        members->member[index].value = value;
        break;
    case MemberKind::string:
        valid = json_value_as_string(value, &members->member[index].string);
        break;
    case MemberKind::timeout: {
        u64 timeout = 0;
        valid = json_value_to_u64(value, &timeout) && timeout != 0 && timeout <= 600'000ull;
        members->timeout_ms = static_cast<u32>(timeout);
        break;
    }
    case MemberKind::flag:
        valid = json_value_to_bool(value, &members->flag);
        break;
    default:
        valid = read_integer(value, &members->integer);
        break;
    }
    if (!valid) {
        members->error = WebAuthnMfaError::invalid_field;
    }
    return valid;
}

template <protocol_detail::MemberName... Names>
__forceinline WebAuthnMfaResult read_members(const Document& document, const JsonValue& value, u32 kinds,
                                             Members* members) noexcept {
    members->names = protocol_detail::MemberNames<Names...>::data.bytes;
    members->kinds = kinds;
    return visit(document, value, visit_member, nullptr, members);
}

// The root object: publicKey (an object, required) as member 0, mediation as member 1.
WebAuthnMfaResult read_root(const Document& document, const Utf8View& json, Members* root) noexcept {
    const WebAuthnMfaResult read = read_members<"publicKey", "mediation">(
        document, JsonValue{json, 0, JsonValueKind::object, false},
        member_kinds(MemberKind::object, MemberKind::string), root);
    return !read.ok() || (root->fields & 1u) != 0 ? read : failure(WebAuthnMfaError::missing_required_field);
}

struct CredentialArrayContext {
    WebAuthnMfaError error;
    u32 count;
    JsonValue values[kWebAuthnMfaMaximumAllowedCredentials];
};

struct TransportContext {
    WebAuthnMfaError error;
    u32 flags;
};

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

// The relying party id, which must be discord.com. Only then is `bytes` of `output` (request options, or creation
// options, which start with them) cleared and given the relying party and the timeout: a refused relying party leaves
// the output alone.
WebAuthnMfaResult start_options(const JsonString& rp_id, u32 timeout_ms, WebAuthnMfaRequestOptions* output,
                                u32 bytes) noexcept {
    char decoded[kWebAuthnMfaMaximumRpIdBytes];
    u32 decoded_bytes = 0;
    if (!decode_string(rp_id, Slice<char>{decoded, static_cast<u32>(sizeof(decoded))}, &decoded_bytes)) {
        return failure(WebAuthnMfaError::value_too_large, rp_id.source_offset);
    }
    if (!utf8_equal({decoded, decoded_bytes}, kWebAuthnRpId)) {
        return failure(WebAuthnMfaError::relying_party_mismatch, rp_id.source_offset);
    }
    clear_bytes(output, bytes);
    copy_bytes(output->rp_id, kWebAuthnRpId.data, kWebAuthnRpId.bytes);
    output->timeout_ms = timeout_ms;
    return failure(WebAuthnMfaError::none);
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
WebAuthnMfaResult decode_credential_list(const Document& document, const JsonValue& list,
                                         WebAuthnMfaRequestOptions* parsed) noexcept {
    CredentialArrayContext credential_values{};
    WebAuthnMfaResult visited = visit(document, list, nullptr, visit_credential_array, &credential_values);
    if (!visited.ok()) {
        return visited;
    }
    for (u32 index = 0; index < credential_values.count; ++index) {
        // id, type, transports
        Members credential{};
        visited = read_members<"id", "type", "transports">(
            document, credential_values.values[index],
            member_kinds(MemberKind::string, MemberKind::string, MemberKind::array), &credential);
        if (!visited.ok()) {
            return visited;
        }
        const JsonString& id = credential.member[0].string;
        if ((credential.fields & 3u) != 3u) {
            return failure(WebAuthnMfaError::missing_required_field, credential_values.values[index].source_offset);
        }
        if (short_string_index<"public-key">(credential.member[1].string) != 0) {
            return failure(WebAuthnMfaError::invalid_field, credential.member[1].string.source_offset);
        }
        if (parsed->credential_id_bytes >= kWebAuthnMfaCredentialIdStorageBytes) {
            return failure(WebAuthnMfaError::value_too_large, id.source_offset);
        }
        const u32 available = kWebAuthnMfaCredentialIdStorageBytes - parsed->credential_id_bytes;
        u32 id_bytes = 0;
        const WebAuthnMfaResult decoded_id =
            decode_binary(id,
                          Slice<u8>{parsed->credential_ids + parsed->credential_id_bytes,
                                    minimum(available, kWebAuthnMfaMaximumCredentialIdBytes)},
                          &id_bytes);
        if (!decoded_id.ok()) {
            return decoded_id;
        }
        WebAuthnAllowedCredential allowed{parsed->credential_id_bytes, id_bytes, webauthn_transport_none};
        if ((credential.fields & (1u << 2)) != 0) {
            TransportContext transports{};
            visited = visit(document, credential.member[2].value, nullptr, visit_transport, &transports);
            if (!visited.ok()) {
                return visited;
            }
            allowed.transports = transports.flags;
        }
        parsed->credentials[parsed->credential_count++] = allowed;
        parsed->credential_id_bytes += id_bytes;
    }
    return failure(WebAuthnMfaError::none);
}

// A name member (`index` of an entity) when present, into `output` (cleared, with room for the longest name and its
// terminator).
WebAuthnMfaResult decode_name(const Members& entity, u32 index, char* output) noexcept {
    const JsonString& name = entity.member[index].string;
    u32 bytes = 0;
    return (entity.fields & (1u << index)) == 0 ||
                   decode_string(name, Slice<char>{output, kWebAuthnMfaMaximumEntityNameBytes}, &bytes)
               ? failure(WebAuthnMfaError::none)
               : failure(WebAuthnMfaError::value_too_large, name.source_offset);
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

// Writes the credential JSON twice (counting, then for real): only a writing emitter stores.
struct Emitter {
    u8* destination;
    u64 at;
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

// The values of a PublicKeyCredential JSON (an assertion or an attestation).
struct CredentialValues {
    ConstSlice<u8> binary[4]; // binary[0] is the credential id
    ConstSlice<u8> user_handle;
    i32 algorithm;
    u32 transports;
    WebAuthnAuthenticatorAttachment attachment;
};

// Bytes of a credential template below 9 stand for values: 1-4 binary[0-3] as unpadded base64url, then the user
// handle (a string, or null when empty), the algorithm, the transports and the attachment.
enum : u8 { slot_user_handle = 5, slot_algorithm, slot_transports, slot_attachment };

constexpr char kCredentialHead[] = R"({"id":")" "\1" R"(","rawId":")" "\1" R"(","response":{)";
constexpr char kCredentialTail[] =
    R"(},"type":"public-key","clientExtensionResults":{},"authenticatorAttachment":)" "\x08" "}";

void emit_template(Emitter* e, const char* text, const CredentialValues& values) noexcept {
    for (; *text != 0; ++text) {
        const u8 slot = static_cast<u8>(*text);
        if (slot > slot_attachment) {
            emit_text(e, Utf8View{text, 1});
        } else if (slot < slot_user_handle) {
            emit_binary(e, values.binary[slot - 1]);
        } else if (slot == slot_user_handle) {
            if (values.user_handle.count == 0) {
                emit_text(e, utf8_literal("null"));
            } else {
                emit_text(e, utf8_literal("\""));
                emit_binary(e, values.user_handle);
                emit_text(e, utf8_literal("\""));
            }
        } else if (slot == slot_algorithm) {
            emit_integer(e, values.algorithm);
        } else if (slot == slot_transports) {
            constexpr Utf8View kNames[] = {utf8_literal("\"usb\""), utf8_literal("\"nfc\""), utf8_literal("\"ble\""),
                                           utf8_literal("\"internal\""), utf8_literal("\"hybrid\"")};
            constexpr u32 kBits[] = {webauthn_transport_usb, webauthn_transport_nfc, webauthn_transport_ble,
                                     webauthn_transport_internal, webauthn_transport_hybrid};
            bool first = true;
            for (u32 index = 0; index < 5; ++index) {
                if ((values.transports & kBits[index]) != 0) {
                    if (!first) {
                        emit_text(e, utf8_literal(","));
                    }
                    emit_text(e, kNames[index]);
                    first = false;
                }
            }
        } else {
            emit_text(e, values.attachment == WebAuthnAuthenticatorAttachment::platform ? utf8_literal(R"("platform")")
                         : values.attachment == WebAuthnAuthenticatorAttachment::cross_platform
                             ? utf8_literal(R"("cross-platform")")
                             : utf8_literal("null"));
        }
    }
}

// The credential: the head, the response members of `body`, the tail. Written twice: once to count, once for real,
// so a short buffer is never half written.
WebAuthnMfaResult write_credential(const char* body, const CredentialValues& values, Slice<u8> output) noexcept {
    Emitter emitter{output.data, 0, false};
    for (;;) {
        emit_template(&emitter, kCredentialHead, values);
        emit_template(&emitter, body, values);
        emit_template(&emitter, kCredentialTail, values);
        if (emitter.write) {
            return failure(WebAuthnMfaError::none, 0, JsonScanError::none, JsonReadError::none,
                           Base64UrlError::none, static_cast<u32>(emitter.at));
        }
        if (emitter.at > 0xFFFFFFFFull) {
            return failure(WebAuthnMfaError::value_too_large);
        }
        if (emitter.at > output.count) {
            return failure(WebAuthnMfaError::output_too_small, 0, JsonScanError::none, JsonReadError::none,
                           Base64UrlError::none, static_cast<u32>(emitter.at));
        }
        emitter = Emitter{output.data, 0, true};
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
    const Document document{structural_scratch, maximum_depth};
    Members root{};
    WebAuthnMfaResult result = read_root(document, json, &root);
    if (!result.ok()) {
        return result;
    }

    Members key{};
    key.timeout_ms = 60'000;
    result = read_members<"challenge", "rpId", "timeout", "allowCredentials", "userVerification", "extensions">(
        document, root.member[0].value,
        member_kinds(MemberKind::string, MemberKind::string, MemberKind::timeout, MemberKind::array,
                     MemberKind::string, MemberKind::object),
        &key);
    if (!result.ok()) {
        return result;
    }
    if ((key.fields & 3u) != 3u) {
        return failure(WebAuthnMfaError::missing_required_field);
    }
    result = start_options(key.member[1].string, key.timeout_ms, output, static_cast<u32>(sizeof(*output)));
    if (!result.ok()) {
        return result;
    }
    result = decode_challenge(key.member[0].string, output);
    if (!result.ok()) {
        return result;
    }

    if ((key.fields & (1u << 4)) != 0) {
        const JsonString& verification_text = key.member[4].string;
        const u32 verification = short_string_index<"preferred", "required", "discouraged">(verification_text);
        if (verification == ~0u) {
            return failure(WebAuthnMfaError::invalid_field);
        }
        if (verification > 2) {
            return failure(WebAuthnMfaError::invalid_field, verification_text.source_offset);
        }
        output->user_verification = static_cast<WebAuthnUserVerification>(verification);
    }

    if ((root.fields & 2u) != 0) {
        const JsonString& mediation_text = root.member[1].string;
        const u32 mediation = short_string_index<"conditional", "optional", "required", "silent">(mediation_text);
        if (mediation == ~0u) {
            return failure(WebAuthnMfaError::invalid_field);
        }
        if (mediation > 3) {
            return failure(WebAuthnMfaError::invalid_field, mediation_text.source_offset);
        }
    }

    if ((key.fields & (1u << 3)) != 0) {
        result = decode_credential_list(document, key.member[3].value, output);
        if (!result.ok()) {
            return result;
        }
    }

    if ((key.fields & (1u << 5)) != 0) {
        Members extensions{};
        return read_members<"uvm">(document, key.member[5].value, member_kinds(MemberKind::flag), &extensions);
    }
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
    return write_credential(R"("authenticatorData":")" "\2" R"(","clientDataJSON":")" "\3" R"(","signature":")" "\4"
                            R"(","userHandle":)" "\5",
                            CredentialValues{{assertion.credential_id, assertion.authenticator_data,
                                              assertion.client_data_json, assertion.signature},
                                             assertion.user_handle, 0, 0, assertion.attachment},
                            output);
}

WebAuthnMfaResult webauthn_mfa_decode_creation(Utf8View json, Slice<u32> structural_scratch, u32 maximum_depth,
                                               WebAuthnMfaCreationOptions* output) noexcept {
    if (!output || !json.data || json.bytes == 0 || !structural_scratch.data || structural_scratch.count == 0 ||
        maximum_depth == 0) {
        return failure(WebAuthnMfaError::invalid_input);
    }
    const Document document{structural_scratch, maximum_depth};
    Members root{};
    WebAuthnMfaResult result = read_root(document, json, &root);
    if (!result.ok()) {
        return result;
    }
    Members key{};
    key.timeout_ms = 60'000;
    result = read_members<"challenge", "rp", "user", "pubKeyCredParams", "timeout", "excludeCredentials",
                          "authenticatorSelection", "attestation">(
        document, root.member[0].value,
        member_kinds(MemberKind::string, MemberKind::object, MemberKind::object, MemberKind::array,
                     MemberKind::timeout, MemberKind::array, MemberKind::object, MemberKind::string),
        &key);
    if (!result.ok()) {
        return result;
    }
    if ((key.fields & 7u) != 7u) {
        return failure(WebAuthnMfaError::missing_required_field);
    }

    // rp ({id, name}) and user ({id, name, displayName}): strings all.
    constexpr u32 kEntity = member_kinds(MemberKind::string, MemberKind::string, MemberKind::string);
    Members rp{};
    result = read_members<"id", "name", "displayName">(document, key.member[1].value, kEntity, &rp);
    if (!result.ok()) {
        return result;
    }
    if ((rp.fields & 1u) == 0) {
        return failure(WebAuthnMfaError::missing_required_field, key.member[1].value.source_offset);
    }
    WebAuthnMfaRequestOptions& request = output->request;
    result = start_options(rp.member[0].string, key.timeout_ms, &request, static_cast<u32>(sizeof(*output)));
    if (!result.ok()) {
        return result;
    }
    output->attestation_none = true;
    result = decode_name(rp, 1, output->rp_name);
    if (!result.ok()) {
        return result;
    }

    Members user{};
    result = read_members<"id", "name", "displayName">(document, key.member[2].value, kEntity, &user);
    if (!result.ok()) {
        return result;
    }
    if ((user.fields & 1u) == 0) {
        return failure(WebAuthnMfaError::missing_required_field, key.member[2].value.source_offset);
    }
    result = decode_binary(user.member[0].string, Slice<u8>{output->user_id, kWebAuthnMfaMaximumUserIdBytes},
                           &output->user_id_bytes);
    if (!result.ok()) {
        return result;
    }
    result = decode_name(user, 1, output->user_name);
    if (!result.ok()) {
        return result;
    }
    result = decode_name(user, 2, output->user_display_name);
    if (!result.ok()) {
        return result;
    }
    // A key may show either name: each stands in for the other, and a user with neither is the account.
    if (output->user_name[0] == 0) {
        copy_bytes(output->user_name, output->user_display_name[0] ? output->user_display_name : "Discord",
                   output->user_display_name[0] ? c_string_length(output->user_display_name) + 1 : 8u);
    }
    if (output->user_display_name[0] == 0) {
        copy_bytes(output->user_display_name, output->user_name, c_string_length(output->user_name) + 1);
    }

    result = decode_challenge(key.member[0].string, &request);
    if (!result.ok()) {
        return result;
    }

    if ((key.fields & (1u << 3)) != 0) {
        CredentialArrayContext entries{};
        result = visit(document, key.member[3].value, nullptr, visit_credential_array, &entries);
        if (!result.ok()) {
            return result;
        }
        for (u32 index = 0; index < entries.count; ++index) {
            // {"type": "public-key", "alg": -7}
            Members parameter{};
            result = read_members<"type", "alg">(document, entries.values[index],
                                                 member_kinds(MemberKind::string, MemberKind::integer), &parameter);
            if (!result.ok()) {
                return result;
            }
            // Entries of another type, or of an algorithm no key here offers, are left out.
            if ((parameter.fields & 3u) != 3u || short_string_index<"public-key">(parameter.member[0].string) != 0 ||
                output->algorithm_count == kWebAuthnMfaMaximumAlgorithms) {
                continue;
            }
            bool listed = false;
            for (u32 known = 0; known < output->algorithm_count; ++known) {
                listed = listed || output->algorithms[known] == parameter.integer;
            }
            if (!listed) {
                output->algorithms[output->algorithm_count++] = parameter.integer;
            }
        }
    }
    if (output->algorithm_count == 0) {
        output->algorithms[0] = kWebAuthnCoseEs256;
        output->algorithms[1] = kWebAuthnCoseRs256;
        output->algorithm_count = 2;
    }

    if ((key.fields & (1u << 6)) != 0) {
        Members selection{};
        result = read_members<"authenticatorAttachment", "residentKey", "requireResidentKey", "userVerification">(
            document, key.member[6].value,
            member_kinds(MemberKind::string, MemberKind::string, MemberKind::flag, MemberKind::string), &selection);
        if (!result.ok()) {
            return result;
        }
        if ((selection.fields & 1u) != 0) {
            const u32 attachment = short_string_index<"platform", "cross-platform">(selection.member[0].string);
            if (attachment == ~0u) {
                return failure(WebAuthnMfaError::invalid_field, selection.member[0].string.source_offset);
            }
            static constexpr WebAuthnAuthenticatorAttachment kAttachments[] = {
                WebAuthnAuthenticatorAttachment::platform, WebAuthnAuthenticatorAttachment::cross_platform,
                WebAuthnAuthenticatorAttachment::unknown};
            output->attachment = kAttachments[attachment];
        }
        if ((selection.fields & 2u) != 0) {
            const u32 resident = short_string_index<"discouraged", "preferred", "required">(selection.member[1].string);
            if (resident > 2) {
                return failure(WebAuthnMfaError::invalid_field, selection.member[1].string.source_offset);
            }
            output->resident_key = static_cast<WebAuthnResidentKey>(resident);
        } else if (selection.flag) {
            output->resident_key = WebAuthnResidentKey::required;
        }
        if ((selection.fields & 8u) != 0) {
            const u32 verification =
                short_string_index<"preferred", "required", "discouraged">(selection.member[3].string);
            if (verification > 2) {
                return failure(WebAuthnMfaError::invalid_field, selection.member[3].string.source_offset);
            }
            request.user_verification = static_cast<WebAuthnUserVerification>(verification);
        }
    }
    if ((key.fields & (1u << 7)) != 0) {
        const u32 attestation =
            short_string_index<"none", "indirect", "direct", "enterprise">(key.member[7].string);
        if (attestation > 3) {
            return failure(WebAuthnMfaError::invalid_field, key.member[7].string.source_offset);
        }
        output->attestation_none = attestation == 0;
    }
    if ((key.fields & (1u << 5)) != 0) {
        return decode_credential_list(document, key.member[5].value, &request);
    }
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
    return write_credential(R"("attestationObject":")" "\2" R"(","authenticatorData":")" "\3"
                            R"(","clientDataJSON":")" "\4" R"(","publicKeyAlgorithm":)" "\6"
                            R"(,"transports":[)" "\7" "]",
                            CredentialValues{{attestation.credential_id, attestation.attestation_object,
                                              attestation.authenticator_data, attestation.client_data_json},
                                             {}, attestation.public_key_algorithm, attestation.transports,
                                             attestation.attachment},
                            output);
}

} // namespace maicord
