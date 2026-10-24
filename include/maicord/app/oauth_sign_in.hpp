#pragma once
#include "maicord/app/oauth_authorization.hpp"

namespace maicord {
enum class OAuthSignInPhase : u8 { empty, starting, waiting, exchanging, ready, complete, cancelled, expired, failed };
struct OAuthSignInStatus {
    u64 generation;
    OAuthSignInPhase phase;
    OAuthError error;
    u16 port, http_status;
    u32 native_error;
};
struct OAuthSignIn { void* runtime; };
// UI-thread owner; one temporary worker, no browser/webview and no polling.
// Starts only on explicit user action. Uses an exclusive OS-assigned loopback
// port and fresh PKCE/state. Injected transport must outlive joined stop.
bool oauth_sign_in_start(OAuthSignIn*, Utf8View client, u64 generation, u32 timeout_ms = 300000,
                           OAuthAuthorizationTransport = {}, OAuthProvider = OAuthProvider::spotify) noexcept;
OAuthSignInStatus oauth_sign_in_status(const OAuthSignIn*) noexcept;
HANDLE oauth_sign_in_notification(const OAuthSignIn*) noexcept;
// Transfers to empty locked output. URL is available only while waiting;
// credentials are available exactly once, for the matching generation.
bool oauth_sign_in_url(const OAuthSignIn*, u64 generation, SecureBuffer*) noexcept;
bool oauth_sign_in_take(OAuthSignIn*, u64 generation, SecureBuffer*) noexcept;
// Cancellation prevents publication immediately. Secrets are erased when the
// worker exits. Failed join retains the runtime and must be retried before
// unloading its module. timeout_ms: 0..5000. No running user client is closed.
bool oauth_sign_in_stop(OAuthSignIn*, u32 timeout_ms) noexcept;
} // namespace maicord
