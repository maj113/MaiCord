#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {

inline constexpr u32 kUserAuthMaximumLoginBytes = 512;
inline constexpr u32 kUserAuthMaximumPasswordBytes = 1024;
inline constexpr u32 kUserAuthMaximumSourceBytes = 96;
inline constexpr u32 kUserAuthMaximumTicketBytes = 4096;
inline constexpr u32 kUserAuthMaximumCodeBytes = 256;
inline constexpr u32 kUserAuthMaximumWebauthnCredentialBytes = 16 * 1024;
inline constexpr u32 kUserAuthMaximumLoginInstanceBytes = 256;
inline constexpr u32 kUserAuthMaximumWebauthnChallengeBytes = 16 * 1024;
inline constexpr u32 kUserAuthMaximumCaptchaKeyBytes = 4096;
inline constexpr u32 kUserAuthMaximumCaptchaTokenBytes = 4096;
inline constexpr u32 kUserAuthMaximumCaptchaSessionBytes = 256;
inline constexpr u32 kUserAuthBrowserExchangeMaximumResponseBytes = 32 * 1024;
inline constexpr u32 kUserAuthBrowserExchangeMaximumTokenBytes = 2 * 1024;

inline constexpr char kUserAuthLoginTargetText[] = "/api/v9/auth/login";
inline constexpr Utf8View kUserAuthLoginTarget{kUserAuthLoginTargetText,
                                               static_cast<u32>(sizeof(kUserAuthLoginTargetText) - 1)};
inline constexpr Utf8View kUserAuthDefaultLoginSource = utf8_literal("The default username password Login");
inline constexpr char kUserAuthMfaSmsSendTargetText[] = "/api/v9/auth/mfa/sms/send";
inline constexpr Utf8View kUserAuthMfaSmsSendTarget{
    kUserAuthMfaSmsSendTargetText, static_cast<u32>(sizeof(kUserAuthMfaSmsSendTargetText) - 1)};
inline constexpr char kUserAuthRemoteAuthTicketTargetText[] = "/api/v9/users/@me/remote-auth/login";
inline constexpr Utf8View kUserAuthRemoteAuthTicketTarget{
    kUserAuthRemoteAuthTicketTargetText, static_cast<u32>(sizeof(kUserAuthRemoteAuthTicketTargetText) - 1)};

enum class UserAuthWriteError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    value_too_large,
    output_too_small,
};

struct UserAuthWriteResult {
    u32 bytes;
    UserAuthWriteError error;

    constexpr bool ok() const noexcept {
        return error == UserAuthWriteError::none;
    }
};

struct UserAuthPasswordRequest {
    Utf8View login;
    Utf8View password;
    Utf8View login_source;
    bool undelete;
};

enum class UserAuthMfaType : u8 {
    totp,
    sms,
    backup,
    webauthn,
};

enum class UserAuthOperation : u8 {
    password,
    mfa_totp,
    mfa_sms,
    mfa_backup,
    mfa_webauthn,
    mfa_sms_send,
    remote_auth_ticket,
    verification,
    recovery,
    reset_password,
    reset_mfa,
    phone_resend,
    phone_verify,
    browser_handoff_exchange,
};

struct UserAuthMfaRequest {
    Utf8View code;
    Utf8View ticket;
    Utf8View login_source;
    Utf8View login_instance_id;
    UserAuthMfaType type;
};

struct UserAuthMfaSmsSendRequest {
    Utf8View ticket;
};

struct UserAuthCaptchaHeaders {
    Utf8View key;
    Utf8View rqtoken;
    Utf8View session_id;
};

UserAuthWriteResult user_auth_write_password_request(const UserAuthPasswordRequest& request,
                                                     Slice<u8> output) noexcept;
UserAuthWriteResult user_auth_write_recovery_request(Utf8View login, Slice<u8> output) noexcept;
bool user_auth_recovery_identifier_valid(Utf8View login) noexcept;
inline constexpr u32 kUserAuthPhoneCodeBytes = 6;
inline constexpr u32 kUserAuthPhoneResetTokenBytes = 4096;
bool user_auth_phone_code_valid(Utf8View code) noexcept;
// Converts the already encoded recovery body in place, preserving its value.
UserAuthWriteResult user_auth_recovery_phone_base(Slice<u8> body, u32 bytes) noexcept;
// Appends to an immutable phone base; every rejection leaves it untouched.
UserAuthWriteResult user_auth_write_phone_verify_request(Slice<u8> body, u32 base_bytes,
    Utf8View code) noexcept;
UserAuthWriteResult user_auth_write_mfa_request(const UserAuthMfaRequest& request,
                                                Slice<u8> output) noexcept;
UserAuthWriteResult user_auth_write_mfa_sms_send_request(const UserAuthMfaSmsSendRequest& request,
                                                         Slice<u8> output) noexcept;
// Writes only complete CRLF-terminated retry fields. The returned block has no
// final empty line and is meant to be appended to the authenticated request head.
UserAuthWriteResult user_auth_write_captcha_headers(const UserAuthCaptchaHeaders& headers,
                                                    Slice<u8> output) noexcept;
Utf8View user_auth_mfa_target(UserAuthMfaType type) noexcept;
// The API names every MFA answer `code`; WebAuthn carries a serialized
// credential and therefore has a larger payload limit.
u32 user_auth_mfa_data_maximum(UserAuthMfaType type) noexcept;
Utf8View user_auth_target(UserAuthOperation operation) noexcept;
// Accepts only the receiver JSON {key,handoff_token}; never copies it.
bool user_auth_browser_handoff_request_valid(Utf8View body, Slice<u32> structural_scratch,
    u32 maximum_depth) noexcept;
// Writes the complete allowlisted POST head used by the user-auth transport.
// An empty CAPTCHA tuple omits retry fields. Capacity and every field are
// validated before the destination is touched. The native transport does not
// emit browser Origin, Referer, or device-fingerprint metadata. In particular,
// the QR fingerprint is not an HTTP client fingerprint.
UserAuthWriteResult user_auth_write_request_head(UserAuthOperation operation, u64 content_length,
                                                 const UserAuthCaptchaHeaders& captcha,
                                                 Slice<u8> output) noexcept;

enum class UserAuthResponseKind : u8 {
    none,
    authenticated,
    mfa_required,
    captcha_required,
    rejected,
    recovery_sent,
    recovery_phone_required,
    phone_code_sent,
    phone_verified_reset,
};

enum UserAuthMfaCapability : u8 {
    user_auth_mfa_capability_none = 0,
    user_auth_mfa_capability_totp = 1u << 0,
    user_auth_mfa_capability_sms = 1u << 1,
    user_auth_mfa_capability_backup = 1u << 2,
    user_auth_mfa_capability_webauthn = 1u << 3,
};

struct UserAuthResponseStorage {
    Slice<char> token;
    Slice<char> ticket;
    Slice<char> login_instance_id;
    Slice<char> webauthn_challenge;
    Slice<char> captcha_service;
    Slice<char> captcha_sitekey;
    Slice<char> captcha_rqdata;
    Slice<char> captcha_rqtoken;
    Slice<char> captcha_session_id;
    Slice<char> captcha_user_flow;
    Slice<char> suspended_user_token;
};

struct UserAuthResponse {
    u64 user_id;
    u64 error_code;
    u32 token_bytes;
    u32 ticket_bytes;
    u32 login_instance_id_bytes;
    u32 webauthn_challenge_bytes;
    u32 captcha_service_bytes;
    u32 captcha_sitekey_bytes;
    u32 captcha_rqdata_bytes;
    u32 captcha_rqtoken_bytes;
    u32 captcha_session_id_bytes;
    u32 captcha_user_flow_bytes;
    u32 suspended_user_token_bytes;
    UserAuthResponseKind kind;
    u8 mfa_capabilities;
    bool has_user_id;
    bool has_error_code;
    bool captcha_invisible;
    bool suspended;
};

enum class UserAuthDecodeError : u8 {
    none,
    invalid_input,
    scan_failed,
    invalid_json,
    duplicate_field,
    invalid_field,
    missing_required_field,
    value_too_large,
    output_too_small,
    unexpected_response,
};

struct UserAuthDecodeResult {
    u32 error_offset;
    UserAuthDecodeError error;
    JsonScanError scan_error;
    JsonReadError read_error;

    constexpr bool ok() const noexcept {
        return error == UserAuthDecodeError::none;
    }
};

// The decoder does not keep credentials. Secret response strings are copied only
// after every destination has been validated, so a capacity failure leaves all
// caller-provided storage untouched.
// The classifier reads the same schema but never copies any string field. It is
// meant for coarse diagnostics on rejected responses.
UserAuthDecodeResult user_auth_classify_response(u16 status_code, Utf8View body,
                                                 Slice<u32> structural_scratch, u32 maximum_depth,
                                                 UserAuthResponse* output) noexcept;
UserAuthDecodeResult user_auth_decode_response(u16 status_code, Utf8View body,
                                               Slice<u32> structural_scratch, u32 maximum_depth,
                                               UserAuthResponseStorage storage,
                                               UserAuthResponse* output) noexcept;
// Dedicated exchange schema: a successful response requires root token and
// nested user.id. Challenges cannot create a replay or MFA continuation.
UserAuthDecodeResult user_auth_decode_browser_handoff_response(u16 status_code, Utf8View body,
    Slice<u32> structural_scratch, u32 maximum_depth, UserAuthResponseStorage storage,
    UserAuthResponse* output) noexcept;
UserAuthDecodeResult user_auth_decode_recovery_response(u16 status_code, Utf8View body,
    Slice<u32> structural_scratch, u32 maximum_depth, UserAuthResponseStorage storage,
    UserAuthResponse* output) noexcept;
UserAuthDecodeResult user_auth_decode_phone_response(u16 status_code, Utf8View body,
    Slice<u32> structural_scratch, u32 maximum_depth, UserAuthResponseStorage storage,
    UserAuthResponse* output, bool verify) noexcept;
// Reset-link credentials are inputs only. Initial reset may request MFA;
// final reset MFA must yield a session token or a categorical error/challenge.
UserAuthDecodeResult user_auth_classify_reset_response(u16 status_code, Utf8View body,
    Slice<u32> structural_scratch, u32 maximum_depth, UserAuthResponse* output,
    bool final_mfa = false) noexcept;
UserAuthDecodeResult user_auth_decode_reset_response(u16 status_code, Utf8View body,
    Slice<u32> structural_scratch, u32 maximum_depth, UserAuthResponseStorage storage,
    UserAuthResponse* output, bool final_mfa = false) noexcept;

enum class UserAuthPhase : u8 {
    idle,
    password_pending,
    captcha_required,
    mfa_required,
    mfa_pending,
    authenticated,
    rejected,
    verification_pending,
    recovery_pending,
    recovery_sent,
    recovery_phone_required,
    reset_pending,
    phone_pending,
    phone_token_ready,
    phone_token_taken,
    browser_exchange_pending,
};

struct UserAuthState {
    u64 generation;
    UserAuthPhase phase;
    UserAuthPhase captcha_return_phase;
    UserAuthMfaType mfa_type;
};

enum class UserAuthTransitionError : u8 {
    none,
    invalid_input,
    invalid_phase,
    stale_generation,
    invalid_response,
};

struct UserAuthTransitionResult {
    u64 generation;
    UserAuthTransitionError error;

    constexpr bool ok() const noexcept {
        return error == UserAuthTransitionError::none;
    }
};

void user_auth_state_initialize(UserAuthState* state) noexcept;
UserAuthTransitionResult user_auth_state_begin_password(UserAuthState* state) noexcept;
UserAuthTransitionResult user_auth_state_begin_verification(UserAuthState* state) noexcept;
UserAuthTransitionResult user_auth_state_begin_recovery(UserAuthState* state) noexcept;
UserAuthTransitionResult user_auth_state_begin_reset(UserAuthState* state) noexcept;
UserAuthTransitionResult user_auth_state_begin_browser_exchange(UserAuthState* state) noexcept;
UserAuthTransitionResult user_auth_state_begin_mfa(UserAuthState* state, UserAuthMfaType type) noexcept;
UserAuthTransitionResult user_auth_state_resume_captcha(UserAuthState* state) noexcept;
UserAuthTransitionResult user_auth_state_apply(UserAuthState* state, u64 generation,
                                               const UserAuthResponse& response) noexcept;
void user_auth_state_cancel(UserAuthState* state) noexcept;

} // namespace maicord
