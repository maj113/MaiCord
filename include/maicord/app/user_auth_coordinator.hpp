#pragma once

#include "maicord/app/account_profile.hpp"
#include "maicord/app/captcha_challenge.hpp"
#include "maicord/app/rest_client.hpp"

namespace maicord {

inline constexpr u32 kUserAuthCoordinatorRequestBytes = 32 * 1024;
inline constexpr u32 kUserAuthCoordinatorSecretBytes = 64 * 1024;
inline constexpr u32 kUserAuthResetCoordinatorRequestBytes = 72 * 1024;
inline constexpr u32 kUserAuthResetCoordinatorSecretBytes = 104 * 1024;
inline constexpr u32 kUserAuthCoordinatorMaximumTokenBytes = kAccountProfileMaximumCredentialBytes;

enum class UserAuthCoordinatorPhase : u8 {
    empty,
    idle,
    password_pending,
    mfa_required,
    mfa_pending,
    sms_send_pending,
    captcha_required,
    authenticated,
    rejected,
    session_promoted,
    failed,
    stopping,
    stopped,
    verification_pending,
    recovery_pending,
    recovery_sent,
    recovery_phone_required,
    recovery_rejected,
    recovery_rate_limited,
    reset_pending,
    reset_mfa_pending,
    reset_rejected,
    reset_rate_limited,
    phone_verify_pending,
    phone_resend_pending,
    phone_rejected,
    phone_rate_limited,
    phone_token_ready,
    phone_token_taken,
    browser_exchange_pending,
};

enum class UserAuthCoordinatorError : u8 {
    none,
    invalid_input,
    memory,
    transport_start,
    state,
    request_write,
    transport_submit,
    transport,
    decode,
    profile,
    stop,
};

enum class UserAuthCoordinatorEventKind : u8 {
    none,
    mfa_required,
    sms_code_sent,
    captcha_required,
    authenticated,
    rejected,
    transport_error,
    protocol_error,
    recovery_sent,
    recovery_phone_required,
    recovery_rejected,
    phone_code_sent,
    phone_verified_reset,
    phone_rejected,
};

enum class UserAuthCoordinatorSecretKind : u8 {
    webauthn_challenge,
    captcha_service,
    captcha_sitekey,
    captcha_rqdata,
    captcha_rqtoken,
    captcha_session_id,
    captcha_user_flow,
    suspended_user_token,
};

struct UserAuthCoordinatorEvent {
    UserAuthResponse response;
    u64 request_id;
    u64 generation;
    u64 retry_at_ms;
    u16 http_status;
    RestClientError transport_error;
    UserAuthDecodeError decode_error;
    UserAuthCoordinatorEventKind kind;
};

struct UserAuthCoordinatorResult {
    u64 generation;
    u64 request_id;
    u64 retry_at_ms;
    u32 native_error;
    u32 response_bytes;
    u32 decode_error_offset;
    u16 http_status;
    UserAuthCoordinatorError error;
    UserAuthCoordinatorPhase phase;
    RestClientSubmitError submit_error;
    RestClientError transport_error;
    UserAuthDecodeError decode_error;
    JsonScanError scan_error;
    JsonReadError read_error;
    UserAuthTransitionError transition_error;
    UserAuthResponseKind response_kind;
    RestContentType content_type;
    RestContentEncoding content_encoding;
    RestBodyKind body_kind;

    constexpr bool ok() const noexcept {
        return error == UserAuthCoordinatorError::none;
    }
};

struct UserAuthPasswordInput {
    const SecureBuffer* login;
    const SecureBuffer* password;
    const SecureBuffer* captcha_key;
    bool undelete;
};

struct UserAuthMfaInput {
    const SecureBuffer* code;
    const SecureBuffer* captcha_key;
    UserAuthMfaType type;
};

struct UserAuthCoordinator {
    RestClient transport;
    void* runtime;
    UserAuthCoordinatorResult state;
    u64 next_request_id;
};

// The coordinator creates a lazy auth transport and one 64 KiB locked arena.
// Submit copies caller secrets synchronously; callers may clear them on return.
UserAuthCoordinatorResult user_auth_coordinator_start(UserAuthCoordinator* coordinator) noexcept;
// Reset has one 104 KiB locked arena: 72 KiB request plus 32 KiB metadata.
UserAuthCoordinatorResult user_auth_coordinator_start_reset(UserAuthCoordinator* coordinator) noexcept;
// nullptr token is accepted only after an initial rejection kept
// its encoded token prefix. Caller secrets are copied synchronously.
UserAuthCoordinatorResult user_auth_coordinator_submit_reset(UserAuthCoordinator* coordinator,
    const SecureBuffer* token, const SecureBuffer* password) noexcept;
UserAuthCoordinatorResult user_auth_coordinator_submit_password(
    UserAuthCoordinator* coordinator, const UserAuthPasswordInput& input) noexcept;
UserAuthCoordinatorResult user_auth_coordinator_submit_recovery(
    UserAuthCoordinator* coordinator, const SecureBuffer* login) noexcept;
UserAuthCoordinatorResult user_auth_coordinator_submit_phone_verify(
    UserAuthCoordinator* coordinator, const SecureBuffer* code) noexcept;
UserAuthCoordinatorResult user_auth_coordinator_submit_phone_resend(UserAuthCoordinator* coordinator) noexcept;
// One-shot transfer to a separate empty, locked 4096-byte buffer. Rejections
// preserve token and generation; this credential is never a profile token.
SecureBufferResult user_auth_coordinator_take_phone_reset_token(UserAuthCoordinator* coordinator,
    u64 expected_generation, SecureBuffer* output) noexcept;
// Consumes an already ingested verification-link token synchronously into the
// locked request arena. CAPTCHA resumes via submit_captcha, preserving
// the verification operation. No account token entry surface is created.
UserAuthCoordinatorResult user_auth_coordinator_submit_verification(
    UserAuthCoordinator* coordinator, const SecureBuffer* link_token) noexcept;
// Copies a locked receiver snapshot synchronously into the HTTPS worker. The
// coordinator erases staging immediately after the request is accepted; the
// caller keeps its buffer and may erase it on return. No replay.
UserAuthCoordinatorResult user_auth_coordinator_submit_browser_handoff_exchange(
    UserAuthCoordinator* coordinator, const SecureBuffer* exchange_request) noexcept;
UserAuthCoordinatorResult user_auth_coordinator_submit_mfa(UserAuthCoordinator* coordinator,
                                                           const UserAuthMfaInput& input) noexcept;
UserAuthCoordinatorResult user_auth_coordinator_submit_sms_send(UserAuthCoordinator* coordinator) noexcept;
// Replays the pending password/MFA request after a server CAPTCHA
// challenge. The original serialized request remains only in locked memory;
// callers do not need to retain or resubmit credentials.
UserAuthCoordinatorResult user_auth_coordinator_submit_captcha(UserAuthCoordinator* coordinator,
                                                               const SecureBuffer* solution) noexcept;

// UI-thread pump. Events contain metadata and lengths only. Secret challenge
// values can be copied only into an initialized locked buffer through the
// accessor below.
UserAuthCoordinatorResult user_auth_coordinator_pump(UserAuthCoordinator* coordinator,
                                                     UserAuthCoordinatorEvent* event) noexcept;
SecureBufferResult user_auth_coordinator_copy_secret(const UserAuthCoordinator* coordinator,
                                                     UserAuthCoordinatorSecretKind kind,
                                                     SecureBuffer* output) noexcept;

// Metadata only; serialize with pump/submit/stop on the owning UI thread.
// Inactive outside the current CAPTCHA wait; no secret is copied.
CaptchaChallengeSnapshot user_auth_coordinator_captcha_snapshot(
    const UserAuthCoordinator* coordinator) noexcept;

// Stops the auth transport before allocating the destination token, then
// copies directly from locked coordinator storage into a normal-user profile.
// The caller may persist it with account_profile_save.
UserAuthCoordinatorResult user_auth_coordinator_take_profile(UserAuthCoordinator* coordinator,
                                                             u64 profile_id, AccountProfile* profile,
                                                             u32 stop_timeout_ms) noexcept;

HANDLE user_auth_coordinator_notification_handle(const UserAuthCoordinator* coordinator) noexcept;
UserAuthCoordinatorResult user_auth_coordinator_status(const UserAuthCoordinator* coordinator) noexcept;
UserAuthCoordinatorResult user_auth_coordinator_stop(UserAuthCoordinator* coordinator,
                                                     u32 timeout_ms) noexcept;
u32 user_auth_coordinator_runtime_bytes() noexcept;

} // namespace maicord
