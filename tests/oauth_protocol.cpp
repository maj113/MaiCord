#include "maicord/protocol/oauth.hpp"
#include "maicord/protocol/writer.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool value, const char* name) noexcept { test_check(&failures, value, name); }
constexpr auto client = utf8_literal("0123456789abcdef0123456789abcdef");
constexpr auto state = utf8_literal("0123456789012345678901234567890123456789012");
constexpr auto verifier = utf8_literal("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
constexpr auto challenge = utf8_literal("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
bool contains(Utf8View text, Utf8View part) noexcept {
    for (u32 i = 0; i + part.bytes <= text.bytes; ++i)
        if (bytes_equal(text.data + i, part.data, part.bytes)) return true;
    return false;
}
void requests() noexcept {
    u8 output[4096];
    const auto n = oauth_authorize(OAuthProvider::spotify, client, 49152, state, challenge, {output, sizeof(output)});
    const Utf8View url{reinterpret_cast<char*>(output), n};
    check(n && utf8_starts_with(url, utf8_literal("https://accounts.spotify.com/authorize?response_type=code&")) &&
        contains(url, utf8_literal("redirect_uri=http%3A%2F%2F127.0.0.1%3A49152%2Foauth2callback")) &&
        contains(url, utf8_literal("scope=user-read-private")) &&
        contains(url, utf8_literal("code_challenge=E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM&code_challenge_method=S256")) &&
        !contains(url, utf8_literal("access_type=offline")) && !contains(url, verifier) && !contains(url, utf8_literal("client_secret")),
        "authorization binds loopback, Spotify account scope and S256 without leaking verifier");
    output[0] = 42;
    check(!oauth_authorize(OAuthProvider::spotify, client, 49152, state, challenge, {output, 1}) && output[0] == 42 &&
          !oauth_authorize(OAuthProvider::spotify, client, 80, state, challenge, {output, sizeof(output)}) && output[0] == 42 &&
          !oauth_client_valid(OAuthProvider::spotify, utf8_literal("123.apps.googleusercontent.com.evil.test")) &&
          !oauth_client_valid(OAuthProvider::spotify, utf8_literal("a&b.apps.googleusercontent.com")),
          "invalid clients, ports and insufficient capacity preserve output");
    const auto exchanged = oauth_exchange_body(OAuthProvider::spotify, client, 49152, utf8_literal("code/+%=&"), verifier,
                                                      {output, sizeof(output)});
    const Utf8View body{reinterpret_cast<char*>(output), exchanged};
    check(exchanged && contains(body, utf8_literal("grant_type=authorization_code&")) &&
          contains(body, utf8_literal("&code=code%2F%2B%25%3D%26&code_verifier=")) &&
          contains(body, verifier) && !contains(body, utf8_literal("client_secret")),
          "exchange encodes opaque code and binds the same redirect and PKCE verifier");
    const auto refreshed = oauth_refresh_body(OAuthProvider::spotify, client, utf8_literal("refresh/+="), {output, sizeof(output)});
    check(refreshed && contains({reinterpret_cast<char*>(output), refreshed}, utf8_literal("&refresh_token=refresh%2F%2B%3D")),
          "refresh encodes opaque tokens without treating them as form syntax");

}
u32 request(Utf8View query, Utf8View headers, Slice<u8> out) noexcept {
    ProtocolWriter writer{out, 0};
    const Utf8View parts[]{utf8_literal("GET /oauth2callback?"), query, utf8_literal(" HTTP/1.1\r\n"), headers,
                           utf8_literal("\r\n")};
    for (auto part : parts)
        if (!protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(part.data), part.bytes)) return 0;
    return writer.used;
}
void callbacks() noexcept {
    constexpr auto query = utf8_literal("state=0123456789012345678901234567890123456789012&code=a%2Fb%2Bc%3D&scope=ignored");
    constexpr auto host = utf8_literal("Host: 127.0.0.1:49152\r\n");
    u8 input[2048], code[1024];
    const auto n = request(query, host, {input, sizeof(input)});
    for (u32 i = 0; i < n; ++i) {
        code[0] = 42;
        const auto result = oauth_callback(OAuthProvider::spotify, {reinterpret_cast<char*>(input), i}, 49152, state, {code, sizeof(code)});
        check(!result.complete && !result.code_bytes && code[0] == 42,
              "every partial callback waits without publishing a partial authorization code");
    }
    auto result = oauth_callback(OAuthProvider::spotify, {reinterpret_cast<char*>(input), n}, 49152, state, {code, sizeof(code)});
    check(result.complete && result.error == OAuthError::none &&
          utf8_equal({reinterpret_cast<char*>(code), result.code_bytes}, utf8_literal("a/b+c=")),
          "complete state-bound callback decodes exactly once");
    const Utf8View bad_queries[]{
        utf8_literal("state=012345678901234567890123456789012345678901X&code=a"),
        utf8_literal("code=a"),
        utf8_literal("state=0123456789012345678901234567890123456789012&code=a&code=b"),
        utf8_literal("state=0123456789012345678901234567890123456789012&state=x&code=a"),
        utf8_literal("state=0123456789012345678901234567890123456789012&code=%00"),
        utf8_literal("state=0123456789012345678901234567890123456789012&code=%0d%0a"),
        utf8_literal("state=0123456789012345678901234567890123456789012&code=%GG"),
        utf8_literal("state=0123456789012345678901234567890123456789012&code=a&error=access_denied"),
        utf8_literal("state=0123456789012345678901234567890123456789012&code=a&iss=https%3A%2F%2Fevil.test")};
    for (auto bad : bad_queries) {
        const auto size = request(bad, host, {input, sizeof(input)});
        code[0] = 42;
        result = oauth_callback(OAuthProvider::spotify, {reinterpret_cast<char*>(input), size}, 49152, state, {code, sizeof(code)});
        check(result.complete && result.error != OAuthError::none && !result.code_bytes && code[0] == 42,
              "missing, mismatched, duplicate and malformed callback parameters cannot publish a code");
    }
    const Utf8View bad_headers[]{utf8_literal("Host: evil.test\r\n"), utf8_literal("Host: 127.0.0.1:49153\r\n"),
        utf8_literal("Host: 127.0.0.1:49152\r\nHost: 127.0.0.1:49152\r\n"),
        utf8_literal("Host: 127.0.0.1:49152\r\nOrigin: https://evil.test\r\n"),
        utf8_literal("Host: 127.0.0.1:49152\r\nContent-Length: 1\r\n"),
        utf8_literal("Host: 127.0.0.1:49152\r\nTransfer-Encoding: chunked\r\n")};
    for (auto bad : bad_headers) {
        const auto size = request(query, bad, {input, sizeof(input)});
        code[0] = 42;
        result = oauth_callback(OAuthProvider::spotify, {reinterpret_cast<char*>(input), size}, 49152, state, {code, sizeof(code)});
        check(result.error == OAuthError::malformed && code[0] == 42,
              "loopback callback rejects foreign origin, wrong port and ambiguous HTTP framing");
    }
    const auto denied = request(utf8_literal("state=0123456789012345678901234567890123456789012&error=access_denied"),
                                host, {input, sizeof(input)});
    result = oauth_callback(OAuthProvider::spotify, {reinterpret_cast<char*>(input), denied}, 49152, state, {code, sizeof(code)});
    check(result.complete && result.error == OAuthError::denied, "state-bound user denial is explicit");
}
void responses() noexcept {
    u32 scratch[256];
    OAuthTokens tokens{};
    auto parse = [&](Utf8View json, u16 status = 200) noexcept {
        return oauth_tokens(OAuthProvider::spotify, json, status, {scratch, 256}, &tokens);
    };
    check(parse(utf8_literal(R"({"access_token":"access/+","refresh_token":"refresh/=","expires_in":3600,"token_type":"Bearer","scope":"user-read-private"})")) == OAuthError::none &&
          tokens.expires_seconds == 3600 && utf8_equal({tokens.access, tokens.access_bytes}, utf8_literal("access/+")) &&
          utf8_equal({tokens.refresh, tokens.refresh_bytes}, utf8_literal("refresh/=")), "token response preserves opaque credentials and lifetime");
    const auto saved = tokens;
    const Utf8View bad[]{
        utf8_literal(R"({"access_token":"a","access_token":"b","expires_in":3600,"token_type":"Bearer"})"),
        utf8_literal(R"({"access_token":"a","access\u005ftoken":"b","expires_in":3600,"token_type":"Bearer"})"),
        utf8_literal(R"({"access_token":"a\r\nX: y","expires_in":3600,"token_type":"Bearer"})"),
        utf8_literal(R"({"access_token":"a","expires_in":0,"token_type":"Bearer"})"),
        utf8_literal(R"({"access_token":"a","expires_in":86401,"token_type":"Bearer"})"),
        utf8_literal(R"({"access_token":"a","expires_in":3600,"token_type":"Basic"})"),
        utf8_literal(R"({"access_token":"a","expires_in":3600,"token_type":"Bearer","scope":"user-read-private.evil"})")};
    for (auto json : bad)
        check(parse(json) != OAuthError::none && bytes_equal(&tokens, &saved, sizeof(tokens)),
              "invalid response cannot partially replace live credentials");
    check(parse(utf8_literal(R"({"error":"invalid_grant"})"), 400) == OAuthError::invalid_grant &&
          bytes_equal(&tokens, &saved, sizeof(tokens)), "revoked refresh grant is distinct and transactional");
    check(parse(utf8_literal(R"({"error":"invalid_client"})"), 400) == OAuthError::invalid_client,
          "registration errors are distinct from user denial");
    check(parse(utf8_literal(R"({"error":"invalid_request","error_description":"client_secret is missing."})"), 400) == OAuthError::invalid_request &&
          parse(utf8_literal(R"({"error":"invalid_request","error_description":"other diagnostic"})"), 400) == OAuthError::invalid_request &&
          bytes_equal(&tokens, &saved, sizeof(tokens)), "error codes do not depend on provider diagnostic wording");
    check(parse(utf8_literal(R"({"access_token":"replacement","expires_in":3600,"token_type":"Bearer"})")) == OAuthError::none &&
          !tokens.refresh_bytes && tokens.access_bytes == 11,
          "refresh response may omit refresh token; caller must retain the old refresh credential");
    secure_clear_bytes(&tokens, sizeof(tokens));
}
}
}
extern "C" void maicord_oauth_protocol_entry() noexcept {
    maicord::requests(); maicord::callbacks(); maicord::responses();
    ExitProcess(maicord::failures ? 1 : 0);
}
