#include "maicord/app/oauth_authorization.hpp"
#include "maicord/protocol/base64url.hpp"
#include "maicord/sys/crypto.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
constexpr auto client = utf8_literal("0123456789abcdef0123456789abcdef");
constexpr auto verifier = utf8_literal("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
bool contains(ConstSlice<u8> text, Utf8View part) noexcept {
    for (u32 i = 0; i + part.bytes <= text.count; ++i)
        if (bytes_equal(text.data + i, part.data, part.bytes)) return true;
    return false;
}
void proof() noexcept {
    SecureBuffer first{}, second{};
    const bool created = oauth_authorization_proof(&first) && oauth_authorization_proof(&second);
    check(created, "native PKCE generation");
    if (created) {
        const auto& a = *reinterpret_cast<OAuthProof*>(first.data);
        const auto& b = *reinterpret_cast<OAuthProof*>(second.data);
        u8 digest[32], decoded[32]; char encoded[43];
        const bool hash = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<char*>(a.verifier)), 43, digest, 32) >= 0;
        check(first.locked && second.locked && first.bytes == sizeof(a) && hash &&
            base64url_encode({digest, 32}, {encoded, 43}).ok() && bytes_equal(encoded, a.challenge, 43) &&
            base64url_decode({a.verifier, 43}, {decoded, 32}).bytes == 32 &&
            base64url_decode({a.state, 43}, {decoded, 32}).bytes == 32 &&
            !bytes_equal(a.state, a.verifier, 43) && !bytes_equal(a.verifier, b.verifier, 43) &&
            !bytes_equal(a.state, b.state, 43), "PKCE uses independent random proofs and hashes ASCII verifier exactly");
        auto* saved = first.data;
        check(!oauth_authorization_proof(&first) && first.data == saved, "starting over cannot orphan locked proof storage");
        secure_clear_bytes(decoded, sizeof(decoded)); secure_clear_bytes(digest, sizeof(digest));
    }
    secure_buffer_release(&first); secure_buffer_release(&second);
}
struct Fake {
    OAuthAuthorization* owner;
    Utf8View response;
    u32 calls{};
    u16 status{200};
    bool refresh{}, cancel{};
};
UpdateTransportError exchange(void* context, const UpdateHttpRequest& request,
    const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
    auto& f = *static_cast<Fake*>(context);
    ++f.calls;
    check(utf8_equal(request.host_name, utf8_literal("accounts.spotify.com")) &&
          utf8_equal({request.target.data, request.target.count}, utf8_literal("/api/token")) &&
          request.maximum_body_bytes == 16384 && options.headers.count == 2 && options.read_error_body &&
          utf8_equal(options.headers[0].value, utf8_literal("application/x-www-form-urlencoded")) &&
          !contains(options.body, utf8_literal("client_secret")), "token exchange is a bounded form POST to Spotify's fixed TLS origin");
    check(f.refresh ? contains(options.body, utf8_literal("grant_type=refresh_token&")) &&
                         contains(options.body, utf8_literal("refresh_token=previous%2F%2B%3D"))
                    : contains(options.body, utf8_literal("grant_type=authorization_code&")) &&
                         contains(options.body, utf8_literal("code=one-use%2F%2B%3D&code_verifier=")) &&
                         contains(options.body, verifier), "native exchange selects the correct grant and preserves escaped credentials");
    *response = {};
    response->status = f.status;
    response->retry_after_seconds = 7;
    for (u32 i = 0; i < f.response.bytes; ++i)
        if (!request.sink(request.sink_context, {reinterpret_cast<const u8*>(f.response.data + i), 1}))
            return UpdateTransportError::sink;
    if (f.cancel) oauth_authorization_cancel(f.owner);
    return UpdateTransportError::none;
}
void grants() noexcept {
    auto* owner = static_cast<OAuthAuthorization*>(VirtualAlloc(nullptr, sizeof(OAuthAuthorization), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(owner != nullptr, "lazy authorization owner");
    if (!owner) return;
    Fake fake{owner, utf8_literal(R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"token_type":"Bearer"})")};
    const OAuthAuthorizationTransport transport{&fake, exchange};
    SecureBuffer output{};
    auto result = oauth_authorization_exchange(owner, client, 49152, utf8_literal("one-use/+="), verifier, &output, transport);
    check(result.ok() && result.status == 200 && fake.calls == 1 && !owner->https.winsock &&
          output.locked && output.bytes == sizeof(OAuthTokens), "exchange publishes locked credentials without a native socket in the injected transport");
    if (output.data) {
        const auto* value = reinterpret_cast<OAuthTokens*>(output.data);
        check(value->access_bytes == 6 && value->refresh_bytes == 7 && value->expires_seconds == 3600,
              "exchange preserves token output and expiry");
        auto* saved = output.data;
        result = oauth_authorization_exchange(owner, client, 49152, utf8_literal("one-use/+="), verifier, &output, transport);
        check(!result.ok() && fake.calls == 1 && output.data == saved, "nonempty output cannot overwrite live credentials or issue a second request");
    }
    secure_buffer_release(&output);
    fake.refresh = true;
    fake.response = utf8_literal(R"({"access_token":"next","expires_in":3600,"token_type":"Bearer"})");
    result = oauth_authorization_refresh(owner, client, utf8_literal("previous/+="), &output, transport);
    check(result.ok() && output.data, "native refresh exchange");
    if (output.data) {
        const auto& value = *reinterpret_cast<OAuthTokens*>(output.data);
        check(utf8_equal({value.refresh, value.refresh_bytes}, utf8_literal("previous/+=")), "omitted refresh token retains the original credential");
    }
    secure_buffer_release(&output);
    fake.status = 400; fake.response = utf8_literal(R"({"error":"invalid_grant"})");
    result = oauth_authorization_refresh(owner, client, utf8_literal("previous/+="), &output, transport);
    check(result.error == OAuthError::invalid_grant && !output.data, "revoked grant cannot publish credentials");
    fake.status = 302; fake.response = {};
    const auto before = fake.calls;
    result = oauth_authorization_refresh(owner, client, utf8_literal("previous/+="), &output, transport);
    check(result.error == OAuthError::unavailable && result.status == 302 && fake.calls == before + 1 && !output.data,
          "redirect cannot forward the refresh credential or trigger another exchange");
    fake.status = 429;
    result = oauth_authorization_refresh(owner, client, utf8_literal("previous/+="), &output, transport);
    check(result.error == OAuthError::unavailable && result.retry_after_seconds == 7 && !output.data,
          "rate limit exposes retry delay without an automatic retry loop");
    fake.status = 200; fake.response = utf8_literal(R"({"access_token":"next","expires_in":3600,"token_type":"Bearer"})");
    fake.cancel = true;
    result = oauth_authorization_refresh(owner, client, utf8_literal("previous/+="), &output, transport);
    check(result.error == OAuthError::cancelled && !output.data, "cancel during exchange discards otherwise valid credentials");
    const auto cancelled_calls = fake.calls;
    result = oauth_authorization_refresh(owner, client, utf8_literal("previous/+="), &output, transport);
    check(result.error == OAuthError::cancelled && fake.calls == cancelled_calls && !output.data && !owner->https.winsock,
          "cancel before start performs no transport or credential allocation");
    oauth_authorization_release(owner);
    check(test_filled(reinterpret_cast<u8*>(owner), sizeof(*owner), 0), "joined authorization release erases the owner");
    VirtualFree(owner, 0, MEM_RELEASE);
}
}
}
extern "C" void maicord_oauth_authorization_entry() noexcept {
    maicord::proof(); maicord::grants();
    ExitProcess(maicord::failures ? 1 : 0);
}
