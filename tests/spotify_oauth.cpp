#include "maicord/app/oauth_account.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool value, const char* name) noexcept { test_check(&failures, value, name); }
constexpr auto provider = OAuthProvider::spotify;
constexpr auto client = utf8_literal("0123456789abcdef0123456789abcdef");
constexpr auto proof = utf8_literal("0123456789012345678901234567890123456789012");
bool contains(Utf8View text, Utf8View part) noexcept {
    for (u32 i = 0; i + part.bytes <= text.bytes; ++i)
        if (bytes_equal(text.data + i, part.data, part.bytes)) return true;
    return false;
}
void protocol() noexcept {
    u8 output[4096];
    const auto n = oauth_authorize(provider, client, 49152, proof, proof, {output, sizeof(output)});
    const Utf8View url{reinterpret_cast<char*>(output), n};
    check(n && utf8_starts_with(url, utf8_literal("https://accounts.spotify.com/authorize?response_type=code&")) &&
        contains(url, utf8_literal("redirect_uri=http%3A%2F%2F127.0.0.1%3A49152%2Foauth2callback")) &&
        contains(url, utf8_literal("scope=user-read-private&")) &&
        contains(url, utf8_literal("code_challenge_method=S256")) &&
        !contains(url, utf8_literal("access_type")) && !contains(url, utf8_literal("prompt=")) &&
        !contains(url, utf8_literal("code_verifier")) && !contains(url, utf8_literal("client_secret")),
        "Spotify uses its own public client, account scope, loopback and PKCE authorization parameters");
    output[0] = 77;
    check(!oauth_authorize(provider, client, 49152, proof, proof, {output, 1}) && output[0] == 77 &&
        !oauth_client_valid(provider, utf8_literal("invalid-client")) &&
        !oauth_client_valid(provider, utf8_literal("0123456789abcdef0123456789abcdeg")) &&
        !oauth_client_valid(static_cast<OAuthProvider>(99), client), "invalid registrations and short output are transactional");
    constexpr auto callback = utf8_literal("GET /oauth2callback?state=0123456789012345678901234567890123456789012&code=one%2Fuse"
        "&iss=https%3A%2F%2Faccounts.spotify.com HTTP/1.1\r\nHost: 127.0.0.1:49152\r\n\r\n");
    const auto accepted = oauth_callback(provider, callback, 49152, proof, {output, sizeof(output)});
    check(accepted.error == OAuthError::none && accepted.code_bytes == 7 && bytes_equal(output, "one/use", 7),
        "Spotify callback validates state, host and optional issuer before yielding the code");
    output[0] = 77;
    check(oauth_callback(static_cast<OAuthProvider>(0), callback, 49152, proof, {output, sizeof(output)}).error == OAuthError::invalid_input &&
        oauth_callback(provider, callback, 49153, proof, {output, sizeof(output)}).error == OAuthError::malformed && output[0] == 77,
        "callbacks cannot cross providers or loopback ports");
    u32 scratch[512]; OAuthTokens tokens{};
    auto parse = [&](Utf8View json) noexcept { return oauth_tokens(provider, json, 200, {scratch, 512}, &tokens); };
    check(parse(utf8_literal(R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"token_type":"Bearer","scope":"user-read-private"})")) == OAuthError::none,
        "Spotify account scope is admitted");
    const auto saved = tokens;
    check(oauth_tokens(provider, utf8_literal(R"({"error":"invalid_scope","error_description":"provider diagnostic"})"),
        400, {scratch, 512}, &tokens) == OAuthError::scope && bytes_equal(&tokens, &saved, sizeof(tokens)),
        "provider scope rejection is actionable without interpreting server prose");
    check(parse(utf8_literal(R"({"access_token":"next","expires_in":3600,"token_type":"Bearer","scope":"https://www.googleapis.com/auth/youtube"})")) == OAuthError::scope &&
        parse(utf8_literal(R"({"access_token":"next","expires_in":3600,"token_type":"Bearer","scope":"user-read-private-extra"})")) == OAuthError::scope &&
        bytes_equal(&tokens, &saved, sizeof(tokens)), "Google scopes and Spotify scope prefixes cannot replace a valid grant");
    check(parse(utf8_literal(R"({"access_token":"next","expires_in":3600,"token_type":"Bearer"})")) == OAuthError::none && !tokens.refresh_bytes,
        "omitted scope retains requested permissions and omitted refresh is distinguished");
    secure_clear_bytes(&tokens, sizeof(tokens));
}
struct Fake { u32 calls{}; u16 status{200}; Utf8View json; };
UpdateTransportError exchange(void* context, const UpdateHttpRequest& request, const HttpsExchangeOptions& options,
    UpdateHttpResponse* response) noexcept {
    auto& f = *static_cast<Fake*>(context); ++f.calls;
    const Utf8View body{reinterpret_cast<const char*>(options.body.data), options.body.count};
    check(utf8_equal(request.host_name, utf8_literal("accounts.spotify.com")) &&
        utf8_equal({request.target.data, request.target.count}, utf8_literal("/api/token")) &&
        options.headers.count == 2 && request.maximum_body_bytes == 16384 &&
        !contains(body, utf8_literal("client_secret")) && contains(body, client),
        "Spotify credentials are sent only to its fixed TLS token endpoint without Basic authentication");
    *response = {}; response->status = f.status;
    return request.sink(request.sink_context, {reinterpret_cast<const u8*>(f.json.data), f.json.bytes}) ?
        UpdateTransportError::none : UpdateTransportError::sink;
}
void account() noexcept {
    char configured[160]; configured[0] = '!';
    check(!oauth_account_client(provider, {configured, sizeof(configured)}) && configured[0] == '!', "missing Spotify config has no fallback credentials");
    wchar_t path[1024];
    const auto location = data_path(path, 1024, L"spotify-client.txt", true);
    check(location.ok() && file_persistence_write_atomic(path, location.length,
        {reinterpret_cast<const u8*>(client.data), client.bytes}).ok() &&
        oauth_account_client(provider, {configured, sizeof(configured)}) == client.bytes && bytes_equal(configured, client.data, client.bytes),
        "public Spotify registration loads from the bounded native configuration");
    SecureBuffer grant{}, tokens{};
    check(oauth_account_save(client, utf8_literal("previous/+="), provider) && oauth_account_load(client, &grant, provider) && grant.locked,
        "Spotify refresh grant is stored encrypted and read into locked memory");
    secure_buffer_release(&grant);
    check(!oauth_account_load(utf8_literal("1123456789abcdef0123456789abcdef"), &grant, provider) && !grant.data,
        "stored grant cannot be used by another Spotify registration");
    auto* owner = static_cast<OAuthAuthorization*>(VirtualAlloc(nullptr, sizeof(OAuthAuthorization), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(owner != nullptr, "lazy transport owner allocation");
    if (!owner) { oauth_account_forget(provider); DeleteFileW(path); return; }
    Fake fake{0, 200, utf8_literal(R"({"access_token":"next","refresh_token":"rotated","expires_in":3600,"token_type":"Bearer","scope":"user-read-private"})")};
    const OAuthAuthorizationTransport transport{&fake, exchange};
    auto result = oauth_account_refresh(owner, &tokens, transport, client, provider);
    check(result.ok() && fake.calls == 1 && tokens.locked && !owner->https.winsock, "Spotify refresh needs no Google client configuration or native socket with injected transport");
    if (tokens.data) {
        const auto& value = *reinterpret_cast<OAuthTokens*>(tokens.data);
        check(utf8_equal({value.refresh, value.refresh_bytes}, utf8_literal("rotated")), "refresh rotation reaches the caller for fenced persistence");
    }
    secure_buffer_release(&tokens);
    check(oauth_account_load(client, &grant, provider) && utf8_equal({reinterpret_cast<char*>(grant.data), grant.bytes}, utf8_literal("previous/+=")),
        "worker cannot overwrite storage after a concurrent sign-out");
    secure_buffer_release(&grant);
    fake.json = utf8_literal(R"({"access_token":"next","expires_in":3600,"token_type":"Bearer"})");
    result = oauth_account_refresh(owner, &tokens, transport, client, provider);
    check(result.ok() && tokens.data && utf8_equal({reinterpret_cast<OAuthTokens*>(tokens.data)->refresh,
        reinterpret_cast<OAuthTokens*>(tokens.data)->refresh_bytes}, utf8_literal("previous/+=")), "Spotify refresh retains the previous grant when rotation is omitted");
    secure_buffer_release(&tokens);
    fake.status = 302; fake.json = {};
    result = oauth_account_refresh(owner, &tokens, transport, client, provider);
    check(result.error == OAuthError::unavailable && fake.calls == 3 && !tokens.data, "Spotify token redirects are not followed");
    fake.status = 400; fake.json = utf8_literal(R"({"error":"invalid_grant"})");
    result = oauth_account_refresh(owner, &tokens, transport, client, provider);
    check(result.error == OAuthError::invalid_grant && !tokens.data, "revoked Spotify grant is reported without publication");
    oauth_authorization_cancel(owner);
    const auto calls = fake.calls;
    result = oauth_account_refresh(owner, &tokens, transport, client, provider);
    check(result.error == OAuthError::cancelled && fake.calls == calls, "cancelled account refresh performs no exchange");
    oauth_authorization_release(owner);
    VirtualFree(owner, 0, MEM_RELEASE);
    check(oauth_account_forget(provider) && oauth_account_forget(provider) && !oauth_account_load(client, &grant, provider),
        "Spotify sign-out is local, idempotent and removes the saved grant");
    DeleteFileW(path);
}
}
}
extern "C" void maicord_spotify_oauth_entry() noexcept {
    using namespace maicord;
    wchar_t original[1024], isolated[1024];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", original, 1024);
    u32 length = n;
    if (!n || n >= 1024) ExitProcess(1);
    copy_bytes(isolated, original, (n + 1) * sizeof(wchar_t));
    if (!wide_append(isolated, 1024, &length, L"\\spotify-oauth-tests") || !directory_ensure(isolated) ||
        !SetEnvironmentVariableW(L"LOCALAPPDATA", isolated)) ExitProcess(1);
    protocol(); account();
    SetEnvironmentVariableW(L"LOCALAPPDATA", original);
    ExitProcess(failures ? 1 : 0);
}
