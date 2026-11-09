// Registering a security key (Settings > My Account > Security Keys): the creation challenge Discord answers, the
// client data and the credential JSON the second request carries. Real authenticators are not exercised here.
#include "maicord/protocol/auth/webauthn_mfa.hpp"
#include "maicord/core/memory.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

constexpr char kCreation[] =
    R"({"publicKey":{"challenge":"AQIDBA","rp":{"id":"discord.com","name":"Discord"},)"
    R"("user":{"id":"dXNlci0x","name":"qa@example.test","displayName":"QA Tester"},)"
    R"("pubKeyCredParams":[{"type":"public-key","alg":-7},{"type":"public-key","alg":-257},)"
    R"({"type":"public-key","alg":-7},{"type":"other","alg":-8}],"timeout":45000,)"
    R"("excludeCredentials":[{"id":"Y3JlZC1pZC0x","type":"public-key","transports":["usb"]}],)"
    R"("authenticatorSelection":{"authenticatorAttachment":"cross-platform","residentKey":"preferred",)"
    R"("requireResidentKey":false,"userVerification":"required"},"attestation":"none"}})";

WebAuthnMfaResult decode(const char* json, u32 bytes, WebAuthnMfaCreationOptions* output) noexcept {
    u32 structurals[1024]{};
    return webauthn_mfa_decode_creation(Utf8View{json, bytes}, Slice<u32>{structurals, 1024}, 16, output);
}

template <u32 N> WebAuthnMfaResult decode(const char (&json)[N], WebAuthnMfaCreationOptions* output) noexcept {
    return decode(json, N - 1, output);
}

bool text_is(const char* actual, const char* expected) noexcept {
    return utf8_equal(Utf8View{actual, c_string_length(actual)}, Utf8View{expected, c_string_length(expected)});
}

void test_decode() noexcept {
    static WebAuthnMfaCreationOptions options;
    clear_bytes(&options, static_cast<u32>(sizeof(options)));
    check(decode(kCreation, &options).ok(), "the creation challenge decodes");
    const WebAuthnMfaRequestOptions& request = options.request;
    check(text_is(request.rp_id, "discord.com") && text_is(options.rp_name, "Discord") &&
              request.challenge_bytes == 4 && request.challenge[0] == 1 && request.challenge[3] == 4 &&
              request.timeout_ms == 45000,
          "the relying party, its name, the challenge and the timeout are read");
    check(options.user_id_bytes == 6 && bytes_equal(options.user_id, "user-1", 6) &&
              text_is(options.user_name, "qa@example.test") && text_is(options.user_display_name, "QA Tester"),
          "the user's id and names are read");
    check(options.algorithm_count == 2 && options.algorithms[0] == kWebAuthnCoseEs256 &&
              options.algorithms[1] == kWebAuthnCoseRs256,
          "the algorithms keep the server's order, once each, and only for public keys");
    check(request.credential_count == 1 && request.credentials[0].id_bytes == 9 &&
              request.credentials[0].transports == webauthn_transport_usb &&
              bytes_equal(request.credential_ids, "cred-id-1", 9),
          "the credentials the key must not hold are the excluded ones");
    check(request.user_verification == WebAuthnUserVerification::required &&
              options.resident_key == WebAuthnResidentKey::preferred &&
              options.attachment == WebAuthnAuthenticatorAttachment::cross_platform && options.attestation_none,
          "the authenticator selection and the attestation preference are read");

    // Only what the server must give: everything else has a default.
    clear_bytes(&options, static_cast<u32>(sizeof(options)));
    check(decode(R"({"publicKey":{"challenge":"AQIDBA","rp":{"id":"discord.com"},"user":{"id":"AQID","displayName":"Mai"}}})",
                 &options)
              .ok() &&
              options.algorithm_count == 2 && options.algorithms[0] == kWebAuthnCoseEs256 &&
              options.algorithms[1] == kWebAuthnCoseRs256 && options.resident_key == WebAuthnResidentKey::discouraged &&
              options.request.user_verification == WebAuthnUserVerification::preferred &&
              options.request.timeout_ms == 60000 && options.attestation_none &&
              options.attachment == WebAuthnAuthenticatorAttachment::unknown && options.request.credential_count == 0 &&
              text_is(options.user_name, "Mai") && text_is(options.user_display_name, "Mai"),
          "a minimal challenge takes ES256 and RS256, no resident key, preferred verification, 60 s and no attestation");

    clear_bytes(&options, static_cast<u32>(sizeof(options)));
    check(decode(R"({"publicKey":{"challenge":"AQIDBA","rp":{"id":"discord.com"},"user":{"id":"AQID"},)"
                 R"("authenticatorSelection":{"requireResidentKey":true},"attestation":"direct"}})",
                 &options)
              .ok() &&
              options.resident_key == WebAuthnResidentKey::required && !options.attestation_none &&
              text_is(options.user_name, "Discord"),
          "requireResidentKey means a resident key, a direct attestation is not 'none', a nameless user is Discord");

    // Refusals leave the output alone.
    WebAuthnMfaCreationOptions untouched{};
    untouched.request.rp_id[0] = 'X';
    check(decode(R"({"publicKey":{"challenge":"AQIDBA","rp":{"id":"example.test"},"user":{"id":"AQID"}}})", &untouched)
                      .error == WebAuthnMfaError::relying_party_mismatch &&
              untouched.request.rp_id[0] == 'X',
          "a challenge for another relying party is refused");
    check(decode(R"({"publicKey":{"challenge":"AQIDBA","rp":{"id":"discord.com"}}})", &untouched).error ==
                  WebAuthnMfaError::missing_required_field &&
              decode(R"({"publicKey":{"challenge":"AQIDBA","rp":{"id":"discord.com"},"user":{"name":"x"}}})", &untouched)
                      .error == WebAuthnMfaError::missing_required_field &&
              decode(R"({"publicKey":{"rp":{"id":"discord.com"},"user":{"id":"AQID"}}})", &untouched).error ==
                  WebAuthnMfaError::missing_required_field,
          "a challenge without its user, its user id or its challenge is refused");
    check(decode(R"({"publicKey":{"challenge":"!!","rp":{"id":"discord.com"},"user":{"id":"AQID"}}})", &untouched).error ==
                  WebAuthnMfaError::base64 &&
              decode(R"({"publicKey":{"challenge":"AQIDBA","rp":{"id":"discord.com"},"user":{"id":"AQID"},)"
                     R"("attestation":"bogus"}})",
                     &untouched)
                      .error == WebAuthnMfaError::invalid_field &&
              decode(R"({"publicKey":{"challenge":"AQIDBA","rp":"discord.com","user":{"id":"AQID"}}})", &untouched)
                      .error == WebAuthnMfaError::invalid_field &&
              decode("{}", &untouched).error == WebAuthnMfaError::missing_required_field,
          "a challenge that is not base64url, of the wrong shape or empty is refused");
    u8 long_id[100];
    for (u32 i = 0; i < sizeof(long_id); ++i)
        long_id[i] = 'A';
    char long_json[320];
    const char head[] = R"({"publicKey":{"challenge":"AQIDBA","rp":{"id":"discord.com"},"user":{"id":")";
    u32 at = 0;
    copy_bytes(long_json, head, sizeof(head) - 1);
    at = sizeof(head) - 1;
    copy_bytes(long_json + at, long_id, sizeof(long_id));
    at += sizeof(long_id);
    const char tail[] = R"("}}})";
    copy_bytes(long_json + at, tail, sizeof(tail) - 1);
    at += sizeof(tail) - 1;
    check(decode(long_json, at, &untouched).error == WebAuthnMfaError::value_too_large,
          "a user id longer than 64 bytes is refused");
}

void test_client_data() noexcept {
    static WebAuthnMfaCreationOptions options;
    clear_bytes(&options, static_cast<u32>(sizeof(options)));
    check(decode(kCreation, &options).ok(), "the challenge decodes for its client data");
    u8 output[512]{};
    const WebAuthnMfaResult written =
        webauthn_mfa_write_creation_client_data(options, Slice<u8>{output, static_cast<u32>(sizeof(output))});
    constexpr Utf8View kExpected =
        utf8_literal(R"({"type":"webauthn.create","challenge":"AQIDBA","origin":"https://discord.com","crossOrigin":false})");
    check(written.ok() && written.bytes == kExpected.bytes &&
              bytes_equal(output, kExpected.data, kExpected.bytes),
          "a creation's client data is webauthn.create over discord.com");
    u8 small[16]{};
    check(webauthn_mfa_write_creation_client_data(options, Slice<u8>{small, 16}).error ==
              WebAuthnMfaError::output_too_small,
          "client data that does not fit says so");
    WebAuthnMfaCreationOptions none{};
    check(webauthn_mfa_write_creation_client_data(none, Slice<u8>{output, 512}).error == WebAuthnMfaError::invalid_input,
          "client data needs a challenge");
}

void test_attestation_object() noexcept {
    u8 output[600]{};
    const u8 authenticator[3] = {1, 2, 3};
    WebAuthnMfaAttestationObjectView object{utf8_literal("none"), {}, {authenticator, 3}};
    WebAuthnMfaResult written = webauthn_mfa_write_attestation_object(object, Slice<u8>{output, 600});
    const u8 kNone[] = {0xA3, 0x63, 'f', 'm', 't', 0x64, 'n', 'o', 'n', 'e', 0x67, 'a', 't', 't', 'S', 't', 'm', 't', 0xA0,
                        0x68, 'a', 'u', 't', 'h', 'D', 'a', 't', 'a', 0x43, 1, 2, 3};
    check(written.ok() && written.bytes == sizeof(kNone) && bytes_equal(output, kNone, sizeof(kNone)),
          "an attestation object of format none holds an empty statement and the authenticator data");

    const u8 statement[] = {0xA1, 0x63, 'a', 'l', 'g', 0x26};
    u8 data[300];
    for (u32 i = 0; i < sizeof(data); ++i)
        data[i] = static_cast<u8>(i);
    object = {utf8_literal("packed"), {statement, sizeof(statement)}, {data, 200}};
    written = webauthn_mfa_write_attestation_object(object, Slice<u8>{output, 600});
    // map, "fmt" + 7 bytes, "attStmt" + the statement, "authData" + 0x58 0xC8 + 200 bytes.
    const u32 expected = 1 + 4 + 7 + 8 + sizeof(statement) + 9 + 2 + 200;
    check(written.ok() && written.bytes == expected && output[5] == 0x66 &&
              bytes_equal(output + 1 + 4 + 7 + 8, statement, sizeof(statement)) &&
              output[1 + 4 + 7 + 8 + sizeof(statement) + 9] == 0x58 &&
              output[1 + 4 + 7 + 8 + sizeof(statement) + 9 + 1] == 200 && output[expected - 1] == 199,
          "a statement is copied and authenticator data of 200 bytes takes a one-byte length");
    object.authenticator_data = {data, 300};
    written = webauthn_mfa_write_attestation_object(object, Slice<u8>{output, 600});
    check(written.ok() && written.bytes == 1 + 4 + 7 + 8 + sizeof(statement) + 9 + 3 + 300 &&
              output[1 + 4 + 7 + 8 + sizeof(statement) + 9] == 0x59 &&
              output[1 + 4 + 7 + 8 + sizeof(statement) + 9 + 1] == 0x01 &&
              output[1 + 4 + 7 + 8 + sizeof(statement) + 9 + 2] == 0x2C,
          "authenticator data of 300 bytes takes a two-byte length");
    u8 guard[8];
    for (u8& byte : guard)
        byte = 0xEE;
    check(webauthn_mfa_write_attestation_object(object, Slice<u8>{guard, 8}).error == WebAuthnMfaError::output_too_small &&
              guard[0] == 0xEE,
          "an attestation object that does not fit is not written");
    object.format = utf8_literal("a-format-name-of-more-than-23-bytes");
    check(webauthn_mfa_write_attestation_object(object, Slice<u8>{output, 600}).error == WebAuthnMfaError::invalid_input,
          "a format name past 23 bytes is refused");

    // "none" attestations drop the key's AAGUID; data without attested credential data is left alone.
    u8 attested[60];
    for (u32 i = 0; i < sizeof(attested); ++i)
        attested[i] = 0x55;
    attested[32] = 0x45;
    webauthn_mfa_clear_aaguid(attested, sizeof(attested));
    bool cleared = true, kept = attested[36] == 0x55 && attested[53] == 0x55;
    for (u32 i = 37; i < 53; ++i)
        cleared = cleared && attested[i] == 0;
    check(cleared && kept, "the AAGUID's 16 bytes are zeroed, and nothing around them");
    attested[32] = 0x05;
    for (u32 i = 37; i < 53; ++i)
        attested[i] = 0x77;
    webauthn_mfa_clear_aaguid(attested, sizeof(attested));
    check(attested[37] == 0x77 && attested[52] == 0x77, "authenticator data without the AT flag is left alone");
}

// Authenticator data of a new credential: header, AAGUID, a 3-byte id and the COSE key's bytes.
u32 attested_data(u8* output, const u8* cose, u32 cose_bytes) noexcept {
    clear_bytes(output, 58);
    output[32] = 0x41;
    output[54] = 3;
    output[55] = 'i';
    output[56] = 'd';
    output[57] = '!';
    copy_bytes(output + 58, cose, cose_bytes);
    return 58 + cose_bytes;
}

void test_algorithm() noexcept {
    u8 data[400];
    // {1: 2, 3: -7, -1: 1, -2: x (2 bytes), -3: y (2 bytes)}
    const u8 es256[] = {0xA5, 0x01, 0x02, 0x03, 0x26, 0x20, 0x01, 0x21, 0x42, 9, 9, 0x22, 0x42, 8, 8};
    u32 bytes = attested_data(data, es256, sizeof(es256));
    check(webauthn_mfa_credential_algorithm({data, bytes}) == kWebAuthnCoseEs256, "an ES256 key names -7");
    // {1: 3, 3: -257, -1: n (a 20-byte string)}
    u8 rs256[40] = {0xA3, 0x01, 0x03, 0x03, 0x39, 0x01, 0x00, 0x20, 0x58, 20};
    bytes = attested_data(data, rs256, 10 + 20);
    check(webauthn_mfa_credential_algorithm({data, bytes}) == kWebAuthnCoseRs256, "an RS256 key names -257");
    const u8 eddsa[] = {0xA2, 0x01, 0x01, 0x03, 0x27};
    bytes = attested_data(data, eddsa, sizeof(eddsa));
    check(webauthn_mfa_credential_algorithm({data, bytes}) == kWebAuthnCoseEdDsa, "an EdDSA key names -8");
    const u8 no_algorithm[] = {0xA1, 0x01, 0x02};
    bytes = attested_data(data, no_algorithm, sizeof(no_algorithm));
    check(webauthn_mfa_credential_algorithm({data, bytes}) == 0 && webauthn_mfa_credential_algorithm({data, 20}) == 0 &&
              webauthn_mfa_credential_algorithm({data, bytes - 1}) == 0,
          "a key without an algorithm, or data cut short, names none");
    bytes = attested_data(data, es256, sizeof(es256));
    data[32] = 0x01;
    check(webauthn_mfa_credential_algorithm({data, bytes}) == 0, "data without attested credential data names none");
}

void test_attestation() noexcept {
    const u8 credential[3] = {1, 2, 3}, client[2] = {'{', '}'}, object[1] = {0xA0}, data[4] = {1, 2, 3, 4};
    WebAuthnMfaAttestationView view{{credential, 3}, {client, 2}, {object, 1}, {data, 4}, kWebAuthnCoseEs256,
                                    webauthn_transport_usb | webauthn_transport_internal,
                                    WebAuthnAuthenticatorAttachment::cross_platform};
    u8 output[512]{};
    WebAuthnMfaResult written = webauthn_mfa_write_attestation(view, Slice<u8>{output, 512});
    constexpr Utf8View kExpected = utf8_literal(
        R"({"id":"AQID","rawId":"AQID","response":{"attestationObject":"oA","authenticatorData":"AQIDBA",)"
        R"("clientDataJSON":"e30","publicKeyAlgorithm":-7,"transports":["usb","internal"]},"type":"public-key",)"
        R"("clientExtensionResults":{},"authenticatorAttachment":"cross-platform"})");
    check(written.ok() && written.bytes == kExpected.bytes && bytes_equal(output, kExpected.data, kExpected.bytes),
          "the credential JSON of a creation holds the id, the response, its algorithm, transports and attachment");

    view.transports = 0;
    view.attachment = WebAuthnAuthenticatorAttachment::unknown;
    view.public_key_algorithm = kWebAuthnCoseRs256;
    written = webauthn_mfa_write_attestation(view, Slice<u8>{output, 512});
    check(written.ok() && test_contains(Utf8View{reinterpret_cast<const char*>(output), written.bytes},
                                        utf8_literal(R"("publicKeyAlgorithm":-257,"transports":[]})")) &&
              test_contains(Utf8View{reinterpret_cast<const char*>(output), written.bytes},
                            utf8_literal(R"("authenticatorAttachment":null})")),
          "no transports is an empty list, an unknown attachment null and RS256 -257");

    u8 guard[40];
    for (u8& byte : guard)
        byte = 0xEE;
    written = webauthn_mfa_write_attestation(view, Slice<u8>{guard, 40});
    check(written.error == WebAuthnMfaError::output_too_small && written.bytes > 40 && guard[0] == 0xEE &&
              guard[39] == 0xEE,
          "a credential that does not fit says how much it needs and is not written");
    view.credential_id = {};
    check(webauthn_mfa_write_attestation(view, Slice<u8>{output, 512}).error == WebAuthnMfaError::invalid_input,
          "a credential without an id is refused");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_webauthn_registration_entry) {
    maicord::test_decode();
    maicord::test_client_data();
    maicord::test_attestation_object();
    maicord::test_attestation();
    maicord::test_algorithm();
    maicord::test_exit(maicord::g_failures, "webauthn registration");
}
