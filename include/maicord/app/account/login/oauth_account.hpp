#pragma once
#include "maicord/platform/crypto/secure_buffer.hpp"
#include "maicord/protocol/auth/oauth.hpp"

namespace maicord {
// One account per provider for this Windows user, independent of Discord.
// Persist only the refresh grant, encrypted for this Windows user and bound to
// the provider, client and record purpose. No access token or profile is saved.
// Save is atomic; failure preserves the previous record. Load requires empty
// output and transfers validated plaintext into locked storage on success.
bool oauth_account_save(Utf8View client, Utf8View refresh, OAuthProvider = OAuthProvider::spotify) noexcept;
bool oauth_account_load(const Utf8View& client, SecureBuffer* output,
                        OAuthProvider = OAuthProvider::spotify) noexcept;
// Public registration only; Spotify uses a locally configured 32-character ID.
u32 oauth_account_client(OAuthProvider, const Slice<char>& output) noexcept;
// Idempotent local sign-out. Does not revoke the provider's grant.
bool oauth_account_forget(OAuthProvider = OAuthProvider::spotify) noexcept;
} // namespace maicord
