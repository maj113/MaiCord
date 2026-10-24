#include "maicord/app/oauth_authorization.hpp"
#include "maicord/platform/hkdf_sha256.hpp"
#include "maicord/platform/random.hpp"
#include "maicord/protocol/base64url.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using E = OAuthError;
struct Locked {
    SecureBuffer value{};
    ~Locked() noexcept { secure_buffer_release(&value); }
};
bool empty(const SecureBuffer* b) noexcept {
    return b && !b->data && !b->capacity && !b->bytes && !b->locked;
}
bool cancelled(OAuthAuthorization& owner) noexcept {
    return InterlockedCompareExchange(&owner.native_state, 0, 0) == 3;
}
struct Scratch {
    u8 body[4096], response[16384];
    u32 response_bytes;
    u32 structurals[1024];
};
bool receive(void* context, ConstSlice<u8> bytes) noexcept {
    auto& scratch = *static_cast<Scratch*>(context);
    if ((!bytes.data && bytes.count) || bytes.count > sizeof(scratch.response) - scratch.response_bytes) return false;
    copy_bytes(scratch.response + scratch.response_bytes, bytes.data, bytes.count);
    scratch.response_bytes += bytes.count;
    return true;
}
UpdateTransportError native_exchange(void* context, const UpdateHttpRequest& request,
    const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
    return https_exchange(static_cast<UpdateHttpsTransport*>(context), request, options, response);
}
OAuthAuthorizationResult exchange(OAuthAuthorization* owner, Utf8View client, u16 port,
    Utf8View credential, Utf8View verifier, bool refresh, SecureBuffer* output,
    OAuthAuthorizationTransport transport, OAuthProvider provider) noexcept {
    if (!owner || !empty(output) || !oauth_client_valid(provider, client)) return {E::invalid_input};
    if (cancelled(*owner)) return {E::cancelled, UpdateTransportError::cancelled};
    Locked work;
    if (!secure_buffer_initialize(&work.value, sizeof(Scratch)).ok()) return {E::capacity};
    auto& scratch = *reinterpret_cast<Scratch*>(work.value.data);
    const auto count = refresh
        ? oauth_refresh_body(provider, client, credential, {scratch.body, sizeof(scratch.body)})
        : oauth_exchange_body(provider, client, port, credential, verifier, {scratch.body, sizeof(scratch.body)});
    if (!count) return {E::invalid_input};
    if (!transport.exchange) {
        const LONG state = InterlockedCompareExchange(&owner->native_state, 1, 0);
        if (state == 3) return {E::cancelled, UpdateTransportError::cancelled};
        if (!state) {
            const bool ready = update_https_initialize(&owner->https);
            if (InterlockedCompareExchange(&owner->native_state, 2, 1) != 1) {
                if (ready) update_https_cancel(&owner->https);
                return {E::cancelled, UpdateTransportError::cancelled};
            }
            if (!ready) return {E::unavailable, UpdateTransportError::connect};
        }
        if (!owner->https.winsock) return {E::unavailable, UpdateTransportError::connect};
        transport = {&owner->https, native_exchange};
    }
    if (cancelled(*owner)) return {E::cancelled, UpdateTransportError::cancelled};
    const HttpsHeader headers[]{{utf8_literal("Content-Type"), utf8_literal("application/x-www-form-urlencoded")},
                                {utf8_literal("Accept"), utf8_literal("application/json")}};
    const UpdateHttpRequest request{UpdateHost::provider,
                                   utf8_literal("accounts.spotify.com"),
                                   ConstSlice<char>{"/api/token", 10},
                                   sizeof(scratch.response), receive, &scratch};
    UpdateHttpResponse response{};
    const auto fetched = transport.exchange(transport.context, request, {{headers, 2}, {scratch.body, count}, true}, &response);
    secure_clear_bytes(scratch.body, sizeof(scratch.body));
    if (cancelled(*owner) || fetched == UpdateTransportError::cancelled)
        return {E::cancelled, UpdateTransportError::cancelled, response.status};
    const u32 retry = static_cast<u32>(minimum(response.retry_after_seconds, u64{86400}));
    if (fetched != UpdateTransportError::none) return {E::unavailable, fetched, response.status, retry};
    // Redirects never forward credentials. Empty/non-JSON transient errors
    // remain retryable without losing the caller's previous refresh token.
    if (response.status == 429 || response.status >= 500 ||
        (response.status >= 300 && response.status < 400)) return {E::unavailable, {}, response.status, retry};
    Locked tokens;
    if (!secure_buffer_initialize(&tokens.value, sizeof(OAuthTokens)).ok()) return {E::capacity};
    auto& value = *reinterpret_cast<OAuthTokens*>(tokens.value.data);
    const auto parsed = oauth_tokens(provider, {reinterpret_cast<char*>(scratch.response), scratch.response_bytes},
        response.status, {scratch.structurals, 1024}, &value);
    if (parsed != E::none) return {parsed, {}, response.status, retry};
    if (refresh && !value.refresh_bytes) {
        copy_bytes(value.refresh, credential.data, credential.bytes);
        value.refresh_bytes = static_cast<u16>(credential.bytes);
    }
    tokens.value.bytes = sizeof(value);
    if (cancelled(*owner)) return {E::cancelled, UpdateTransportError::cancelled, response.status};
    if (!secure_buffer_transfer(output, &tokens.value)) return {E::invalid_input};
    return {E::none, {}, response.status};
}
}
bool oauth_authorization_proof(SecureBuffer* output) noexcept {
    if (!empty(output)) return false;
    struct Secrets { OAuthProof proof; u8 random[64], digest[32]; };
    Locked staging, result;
    if (!secure_buffer_initialize(&staging.value, sizeof(Secrets)).ok() ||
        !secure_buffer_initialize(&result.value, sizeof(OAuthProof)).ok()) return false;
    auto& data = *reinterpret_cast<Secrets*>(staging.value.data);
    if (!secure_random_fill({data.random, sizeof(data.random)}).ok() ||
        !base64url_encode({data.random, 32}, {data.proof.verifier, kOAuthProofBytes}).ok() ||
        !base64url_encode({data.random + 32, 32}, {data.proof.state, kOAuthProofBytes}).ok()) return false;
    KdfSha256Workspace hash{};
    if (!kdf_sha256_initialize(&hash).ok()) return false;
    const ConstSlice<u8> segment{reinterpret_cast<u8*>(data.proof.verifier), kOAuthProofBytes};
    const auto hashed = sha256(hash, {&segment, 1}, {data.digest, sizeof(data.digest)});
    kdf_sha256_release(&hash);
    if (!hashed.ok() || !base64url_encode({data.digest, sizeof(data.digest)},
                                        {data.proof.challenge, kOAuthProofBytes}).ok() ||
        !secure_buffer_assign(&result.value, {reinterpret_cast<u8*>(&data.proof), sizeof(data.proof)}).ok()) return false;
    return secure_buffer_transfer(output, &result.value);
}
OAuthAuthorizationResult oauth_authorization_exchange(OAuthAuthorization* owner, Utf8View client, u16 port,
    Utf8View code, Utf8View verifier, SecureBuffer* output, OAuthAuthorizationTransport transport, OAuthProvider provider) noexcept {
    return exchange(owner, client, port, code, verifier, false, output, transport, provider);
}
OAuthAuthorizationResult oauth_authorization_refresh(OAuthAuthorization* owner, Utf8View client,
    Utf8View refresh, SecureBuffer* output, OAuthAuthorizationTransport transport, OAuthProvider provider) noexcept {
    return exchange(owner, client, 0, refresh, {}, true, output, transport, provider);
}
void oauth_authorization_cancel(OAuthAuthorization* owner) noexcept {
    if (!owner) return;
    const LONG state = InterlockedExchange(&owner->native_state, 3);
    if (state == 2) update_https_cancel(&owner->https);
}
void oauth_authorization_release(OAuthAuthorization* owner) noexcept {
    if (!owner) return;
    if (owner->https.winsock) update_https_release(&owner->https);
    secure_clear_bytes(owner, sizeof(*owner));
}
} // namespace maicord
