#pragma once
#include "maicord/core/json_reader.hpp"

namespace maicord {
enum class OAuthProvider : u8 { spotify = 1 };

inline constexpr u32 kOAuthTokenBytes = 2048;
inline constexpr u32 kOAuthCodeBytes = 1024;
inline constexpr u32 kOAuthProofBytes = 43;
inline constexpr u32 kOAuthClientBytes = 32;
// Account API permissions only; neither grant promises native media playback.
Utf8View oauth_scope(OAuthProvider) noexcept;
enum class OAuthError : u8 {
    none, invalid_input, capacity, malformed, state_mismatch, denied, invalid_grant, invalid_client, scope,
    unavailable, cancelled, invalid_request
};
struct OAuthTokens {
    char access[kOAuthTokenBytes], refresh[kOAuthTokenBytes];
    u32 expires_seconds;
    u16 access_bytes, refresh_bytes;
};
struct OAuthCallback {
    u32 code_bytes;
    OAuthError error;
    bool complete;
};
bool oauth_client_valid(OAuthProvider provider, Utf8View) noexcept;
bool oauth_token_valid(Utf8View) noexcept;
// Caller owns transient credential storage. Writers are transactional and
// erase staging bytes before return. Spotify uses public-client PKCE,
// without a client secret, cookies or user password.
u32 oauth_authorize(OAuthProvider provider, Utf8View client, u16 port, Utf8View state, Utf8View challenge, Slice<u8>) noexcept;
u32 oauth_exchange_body(OAuthProvider provider, Utf8View client, u16 port, Utf8View code, Utf8View verifier, Slice<u8>) noexcept;
u32 oauth_refresh_body(OAuthProvider provider, Utf8View client, Utf8View refresh, Slice<u8>) noexcept;
// Only a complete, exact loopback GET with this attempt's state can yield a
// code. Rejected/partial requests leave output untouched and must not end the attempt.
OAuthCallback oauth_callback(OAuthProvider provider, Utf8View request, u16 port, Utf8View expected_state, Slice<u8> code) noexcept;
// Transactional bounded JSON parse. A refresh response may omit refresh_token;
// the caller retains the previous refresh token in that case.
OAuthError oauth_tokens(OAuthProvider provider, Utf8View json, u16 status, Slice<u32> scratch, OAuthTokens*) noexcept;
} // namespace maicord
