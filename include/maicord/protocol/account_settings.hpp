#pragma once

#include "maicord/protocol/rest.hpp"

namespace maicord {

// Account settings REST requests. The module serializes every body from
// typed fields; no caller-built JSON crosses the account boundary.
enum class AccountSettingsOperation : u8 {
    settings_fetch, // GET   /users/@me/settings-proto/1
    settings_patch, // PATCH /users/@me/settings-proto/1  partial PreloadedUserSettings
    self_user,      // GET   /users/@me
    sessions,       // GET   /auth/sessions
    session_logout, // POST  /auth/sessions/logout        {"session_id_hashes":[hash]}
    tokens,         // GET   /oauth2/tokens
    token_revoke,   // DELETE /oauth2/tokens/{id}
    profile,        // GET   /users/{id}/profile
    profile_patch,  // PATCH /users/@me/profile           {"pronouns":…} or {"bio":…}
    display_name,   // PATCH /users/@me                   {"global_name":…|null}
};
enum class AccountProfileField : u8 { pronouns, bio };
inline constexpr u32 kAccountSettingsPronounsBytes = 160;   // 40 code points
inline constexpr u32 kAccountSettingsBioBytes = 760;        // 190 code points
inline constexpr u32 kAccountSettingsDisplayNameBytes = 128; // 32 code points
inline constexpr u32 kAccountSettingsHashBytes = 64;
inline constexpr u32 kAccountSettingsMaximumTargetBytes = 96;
inline constexpr u32 kAccountSettingsMaximumBodyBytes = 6 * kAccountSettingsBioBytes + 64;
inline constexpr u64 kAccountSettingsRouteId = 0x4143435345545453ull;
inline constexpr u64 kAccountSessionsRouteId = 0x4143435345535353ull;
inline constexpr u64 kAccountTokensRouteId = 0x4143434f41555448ull;
inline constexpr u64 kAccountProfileRouteId = 0x414343505246494cull;
inline constexpr u64 kAccountSelfRouteId = 0x41434353454c4655ull;

// The payload is borrowed for the duration of the call: partial proto bytes
// for settings_patch, the session id hash for session_logout, UTF-8 text for
// profile_patch/display_name (empty clears). id is the token, or the self
// user for profile.
struct AccountSettingsRequest {
    ConstSlice<u8> payload;
    u64 id;
    u32 required_data_version;
    AccountSettingsOperation operation;
    AccountProfileField field;
    u8 reserved[2];
};
static_assert(sizeof(AccountSettingsRequest) == 32);

enum class AccountSettingsWriteError : u8 { none, invalid_input, output_capacity };
struct AccountSettingsWriteResult {
    u64 route_id;
    u32 target_bytes, body_bytes;
    RestMethod method;
    AccountSettingsWriteError error;
    constexpr bool ok() const noexcept { return error == AccountSettingsWriteError::none; }
};
bool account_settings_request_valid(const AccountSettingsRequest&) noexcept;
// settings_* run on the primary lane, everything else on the details lane.
bool account_settings_primary(AccountSettingsOperation) noexcept;
// settings_patch bodies need user_settings_proto_write_patch_json capacity.
AccountSettingsWriteResult account_settings_write_request(const AccountSettingsRequest&, Slice<u8> target,
                                                          Slice<u8> body) noexcept;

// Decoded responses. Text is copied and truncated at code point boundaries.
inline constexpr u32 kAccountSessionMaximum = 32;
struct AccountSession {
    u64 last_used_ms;
    char id_hash[kAccountSettingsHashBytes];
    char os[32], platform[48], location[64];
    u8 id_hash_bytes, os_bytes, platform_bytes, location_bytes;
};
inline constexpr u32 kAccountAuthorizedAppMaximum = 48;
struct AccountAuthorizedApp {
    u64 token_id, application_id;
    char name[64], scopes[112];
    u8 name_bytes, scopes_bytes;
};
struct AccountSelfUser {
    char username[64], global_name[kAccountSettingsDisplayNameBytes], email[128], phone[32];
    u8 username_bytes, global_name_bytes, email_bytes, phone_bytes;
    bool verified, mfa_enabled, email_known, phone_known;
};
struct AccountProfileText {
    char pronouns[kAccountSettingsPronounsBytes], bio[kAccountSettingsBioBytes];
    u16 pronouns_bytes, bio_bytes;
};
// Returns the number of decoded entries, or ~0u for malformed JSON.
u32 account_settings_decode_sessions(Utf8View body, Slice<u32> structurals, Slice<AccountSession>) noexcept;
u32 account_settings_decode_tokens(Utf8View body, Slice<u32> structurals, Slice<AccountAuthorizedApp>) noexcept;
bool account_settings_decode_self(Utf8View body, Slice<u32> structurals, AccountSelfUser*) noexcept;
bool account_settings_decode_profile(Utf8View body, Slice<u32> structurals, AccountProfileText*) noexcept;
// API error code of an error body ({"code":N,…}); 0 when absent.
u64 account_settings_error_code(Utf8View body, Slice<u32> structurals) noexcept;
// Display masks: "j*****@example.com" and "*******1234".
u32 account_settings_mask_email(Utf8View email, Slice<char> output) noexcept;
u32 account_settings_mask_phone(Utf8View phone, Slice<char> output) noexcept;

} // namespace maicord
