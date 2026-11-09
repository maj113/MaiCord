#pragma once

#include "maicord/protocol/http/rest.hpp"

namespace maicord {

// Account settings REST requests. The module serializes every body from
// typed fields; no caller-built JSON crosses the account boundary. The routes
// are Userdoccers' resources/user, authentication (sessions, MFA verification),
// topics/email-verification, topics/phone-verification, resources/connected-accounts
// and resources/user-settings (consents).
enum class AccountSettingsOperation : u8 {
    settings_fetch, // GET   /users/@me/settings-proto/1
    settings_patch, // PATCH /users/@me/settings-proto/1  partial PreloadedUserSettings
    self_user,      // GET   /users/@me
    sessions,       // GET   /auth/sessions
    session_logout, // POST  /auth/sessions/logout        {"session_id_hashes":[hash,…]}
    tokens,         // GET   /oauth2/tokens
    token_revoke,   // DELETE /oauth2/tokens/{id}
    profile,        // GET   /users/{id}/profile
    profile_patch,  // PATCH /users/@me/profile           {"pronouns":…} or {"bio":…}
    display_name,   // PATCH /users/@me                   {"global_name":…|null}
    // The rest take their strings from `texts`, in the order of their JSON keys; an empty optional text is left out.
    username,            // PATCH  /users/@me                         [username, password]
    password,            // PATCH  /users/@me                         [password, new_password] → token
    email_challenge,     // PUT    /users/@me/email                    (a code to the current address)
    email_verify_code,   // POST   /users/@me/email/verify-code        [code] → token
    email,               // PATCH  /users/@me                         [email, email_token?, password]
    phone_code,          // POST   /users/@me/phone                    [phone] (an SMS code)
    phone_verify,        // POST   /phone-verifications/verify         [phone, code] → token
    phone_add,           // POST   /users/@me/phone                    [phone_token, password]
    phone_remove,        // DELETE /users/@me/phone                    [password]
    totp_enable,         // POST   /users/@me/mfa/totp/enable          [password, secret?, code?] → token, backup_codes
    totp_disable,        // POST   /users/@me/mfa/totp/disable         → token
    sms_enable,          // POST   /users/@me/mfa/sms/enable           [password]
    sms_disable,         // POST   /users/@me/mfa/sms/disable          [password]
    backup_challenge,    // POST   /auth/verify/view-backup-codes-challenge [password] → nonce, regenerate_nonce
    backup_codes,        // POST   /users/@me/mfa/codes-verification   [key, nonce], option: regenerate
    security_keys,       // GET    /users/@me/mfa/webauthn/credentials
    security_key_create, // POST   /users/@me/mfa/webauthn/credentials {} → ticket, challenge; or [name, ticket, credential]
    security_key_rename, // PATCH  /users/@me/mfa/webauthn/credentials/{id} [name]
    security_key_delete, // DELETE /users/@me/mfa/webauthn/credentials/{id}
    account_disable,     // POST   /users/@me/disable                  [password]
    account_delete,      // POST   /users/@me/delete                   [password]
    mfa_finish,          // POST   /mfa/finish                         [ticket, mfa_type, data] → token
    mfa_sms_send,        // POST   /auth/mfa/sms/send                  [ticket]
    connections,         // GET    /users/@me/connections
    connection_patch,    // PATCH  /users/@me/connections/{type}/{id}  [type, id], option: AccountConnectionField, id: value
    connection_delete,   // DELETE /users/@me/connections/{type}/{id}  [type, id]
    consents,            // GET    /users/@me/consent
    consent_patch,       // POST   /users/@me/consent                  option: AccountConsent, id: 1 grants, 0 revokes
    harvest,             // GET    /users/@me/harvest
    harvest_create,      // POST   /users/@me/harvest                  {"backends":[…]}, option: backend bits
};
inline constexpr u32 kAccountSettingsOperationCount = static_cast<u32>(AccountSettingsOperation::harvest_create) + 1;
enum class AccountProfileField : u8 { pronouns, bio };
// connection_patch: the field and its value (id: 1 shows/true, 0 hides/false).
enum class AccountConnectionField : u8 { visibility, show_activity, metadata_visibility };
enum class AccountConsent : u8 { usage_statistics, personalization };
inline constexpr u32 kAccountSettingsPronounsBytes = 160;   // 40 code points
inline constexpr u32 kAccountSettingsBioBytes = 760;        // 190 code points
inline constexpr u32 kAccountSettingsDisplayNameBytes = 128; // 32 code points
inline constexpr u32 kAccountSettingsHashBytes = 64;
// A text argument (a security key's credential JSON is the longest), and the MFA verification token.
inline constexpr u32 kAccountSettingsTextBytes = 3072;
inline constexpr u32 kAccountSettingsMfaTokenBytes = 1024;
inline constexpr u32 kAccountSettingsMaximumTargetBytes = 256;
inline constexpr u32 kAccountSettingsMaximumBodyBytes = 8192;
inline constexpr u64 kAccountSettingsRouteId = 0x4143435345545453ull;
inline constexpr u64 kAccountSessionsRouteId = 0x4143435345535353ull;
inline constexpr u64 kAccountTokensRouteId = 0x4143434f41555448ull;
inline constexpr u64 kAccountProfileRouteId = 0x414343505246494cull;
inline constexpr u64 kAccountSelfRouteId = 0x41434353454c4655ull;
// The text operations: this plus the operation.
inline constexpr u64 kAccountActionRouteId = 0x4143434143540000ull;

// The payload is borrowed for the duration of the call: partial proto bytes
// for settings_patch, the session id hashes (comma separated) for
// session_logout, UTF-8 text for profile_patch/display_name (empty clears).
// id is the token, the self user for profile, the security key, or a value.
// settings_patch sends required_data_version when it is nonzero or
// required_data_version_present is set. texts and mfa are borrowed and may
// hold secrets; mfa is sent as X-Discord-MFA-Authorization.
struct AccountSettingsRequest {
    ConstSlice<u8> payload;
    ConstSlice<Utf8View> texts;
    Utf8View mfa;
    u64 id;
    u32 required_data_version;
    AccountSettingsOperation operation;
    AccountProfileField field;
    bool required_data_version_present;
    u8 option;
};
static_assert(sizeof(AccountSettingsRequest) == 64);

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
// authenticator_types, one bit per authenticator type (Userdoccers: 1 WebAuthn, 2 TOTP, 3 SMS).
inline constexpr u8 kAccountAuthenticatorWebauthn = 1u << 1, kAccountAuthenticatorTotp = 1u << 2,
                    kAccountAuthenticatorSms = 1u << 3;
struct AccountSelfUser {
    char username[64], global_name[kAccountSettingsDisplayNameBytes], email[128], phone[32];
    u8 username_bytes, global_name_bytes, email_bytes, phone_bytes;
    bool verified, mfa_enabled;
    u8 authenticators;
};
struct AccountProfileText {
    char pronouns[kAccountSettingsPronounsBytes], bio[kAccountSettingsBioBytes];
    u16 pronouns_bytes, bio_bytes;
};
inline constexpr u32 kAccountBackupCodeMaximum = 10;
struct AccountBackupCode {
    char code[16];
    u8 code_bytes;
    bool consumed;
};
inline constexpr u32 kAccountSecurityKeyMaximum = 16;
struct AccountSecurityKey {
    u64 id;
    char name[64];
    u8 name_bytes;
};
inline constexpr u32 kAccountConnectionMaximum = 32;
struct AccountConnection {
    char type[24], id[128], name[128];
    u8 type_bytes, id_bytes, name_bytes;
    bool verified, revoked, visible, show_activity, metadata_visible;
};
struct AccountConsents {
    bool usage_statistics, personalization;
};
// The latest data request (GET /users/@me/harvest; a 204 has none).
struct AccountHarvest {
    u64 created_ms, completed_ms;
    u8 status; // 0 queued, 1 running, 2 failed, 3 completed, 4 cancelled
};
// A 401 with code 60003: the ticket for /mfa/finish and the methods it allows (authenticator bits, plus
// kAccountMfaMethodPassword and kAccountMfaMethodBackup); the WebAuthn challenge is copied into `challenge`.
inline constexpr u8 kAccountMfaMethodPassword = 1u << 0, kAccountMfaMethodBackup = 1u << 4;
struct AccountMfaChallenge {
    char ticket[512];
    u16 ticket_bytes;
    u8 methods;
};
// Returns the number of decoded entries, or ~0u for malformed JSON.
u32 account_settings_decode_sessions(Utf8View body, Slice<u32> structurals, Slice<AccountSession>) noexcept;
u32 account_settings_decode_tokens(Utf8View body, Slice<u32> structurals, Slice<AccountAuthorizedApp>) noexcept;
u32 account_settings_decode_backup_codes(Utf8View body, Slice<u32> structurals, Slice<AccountBackupCode>) noexcept;
u32 account_settings_decode_security_keys(Utf8View body, Slice<u32> structurals, Slice<AccountSecurityKey>) noexcept;
u32 account_settings_decode_connections(Utf8View body, Slice<u32> structurals, Slice<AccountConnection>) noexcept;
bool account_settings_decode_self(Utf8View body, Slice<u32> structurals, AccountSelfUser*) noexcept;
bool account_settings_decode_profile(Utf8View body, Slice<u32> structurals, AccountProfileText*) noexcept;
bool account_settings_decode_consents(Utf8View body, Slice<u32> structurals, AccountConsents*) noexcept;
bool account_settings_decode_harvest(Utf8View body, Slice<u32> structurals, AccountHarvest*) noexcept;
bool account_settings_decode_mfa_challenge(Utf8View body, Slice<u32> structurals, AccountMfaChallenge*,
                                           Slice<char> challenge, u32* challenge_bytes) noexcept;
// One string member of the root object ("token", "nonce", "ticket", "challenge"), decoded whole into output; ~0u when
// it is absent, not a string or longer than output.
u32 account_settings_decode_member(Utf8View body, Slice<u32> structurals, Utf8View name, Slice<char> output) noexcept;
// API error code of an error body ({"code":N,…}); 0 when absent.
u64 account_settings_error_code(Utf8View body, Slice<u32> structurals) noexcept;
// The reason an error body gives: the first field error's message ({"errors":{"password":{"_errors":[{"message":…}]}}}),
// else the top-level message; 0 when it has none.
u32 account_settings_error_message(Utf8View body, Slice<u32> structurals, Slice<char> output) noexcept;
// Display masks: "j*****@example.com" and "*******1234".
u32 account_settings_mask_email(Utf8View email, Slice<char> output) noexcept;
u32 account_settings_mask_phone(Utf8View phone, Slice<char> output) noexcept;

} // namespace maicord
