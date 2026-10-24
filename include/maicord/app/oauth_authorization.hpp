#pragma once
#include "maicord/app/update_https.hpp"
#include "maicord/platform/secure_buffer.hpp"
#include "maicord/protocol/oauth.hpp"

namespace maicord {
struct OAuthProof {
    char verifier[kOAuthProofBytes], challenge[kOAuthProofBytes], state[kOAuthProofBytes];
};
// Produces a fresh 256-bit verifier and independent 256-bit state in locked
// storage. The challenge is unpadded base64url(SHA256(ASCII(verifier))).
// Output must be empty; caller releases it after the one-shot attempt ends.
bool oauth_authorization_proof(SecureBuffer* output) noexcept;
struct OAuthAuthorizationTransport {
    void* context;
    UpdateTransportError (*exchange)(void*, const UpdateHttpRequest&, const HttpsExchangeOptions&,
                                     UpdateHttpResponse*) noexcept;
};
struct OAuthAuthorization {
    UpdateHttpsTransport https;
    volatile LONG native_state; // 0 unused, 1 initializing, 2 ready, 3 cancelled.
};
struct OAuthAuthorizationResult {
    OAuthError error;
    UpdateTransportError transport_error;
    u16 status;
    u32 retry_after_seconds;
    bool ok() const noexcept { return error == OAuthError::none && transport_error == UpdateTransportError::none; }
};
// Serial worker-only, with no browser, thread, retry loop or persistent token
// store. Callback state/attempt identity must be checked before exchanging.
// On success, transfers a locked OAuthTokens into EMPTY output. Never
// mutates the caller's existing credentials on failure. Refresh preserves the
// previous refresh token when the provider omits a replacement.
OAuthAuthorizationResult oauth_authorization_exchange(OAuthAuthorization*, Utf8View client, u16 port,
    Utf8View code, Utf8View verifier, SecureBuffer* output, OAuthAuthorizationTransport = {}, OAuthProvider = OAuthProvider::spotify) noexcept;
OAuthAuthorizationResult oauth_authorization_refresh(OAuthAuthorization*, Utf8View client,
    Utf8View refresh, SecureBuffer* output, OAuthAuthorizationTransport = {}, OAuthProvider = OAuthProvider::spotify) noexcept;
// Cancel from any thread, release only after the worker joins. The caller must
// also fence completion by attempt identity before installing credentials.
void oauth_authorization_cancel(OAuthAuthorization*) noexcept;
void oauth_authorization_release(OAuthAuthorization*) noexcept;
} // namespace maicord
