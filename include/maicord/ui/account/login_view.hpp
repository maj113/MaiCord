#pragma once

#include "maicord/ui/account/login_scene.hpp"
#include "maicord/ui/text/composition.hpp"

namespace maicord {
struct QrCode;

enum class LoginStatus : u8 {
    idle,
    submitting,
    remote_connecting,
    remote_qr_ready,
    remote_mobile_pending,
    remote_expired,
    mfa_required,
    mfa_backup_acknowledgement,
    mfa_input_invalid,
    mfa_code_rejected,
    captcha_required,
    profile_save_failed,
    rejected,
    webauthn_required,
    webauthn_failed,
    restoring_session,
    session_expired,
    connection_failed,
    cancelling,
    recovery_editing,
    recovery_pending,
    recovery_sent,
    recovery_phone_required,
    recovery_failed,
    recovery_rate_limited,
    recovery_captcha_required,
    reset_editing,
    reset_pending,
    reset_rejected,
    reset_rate_limited,
    reset_failed,
    reset_captcha_required,
    reset_mfa_pending,
    recovery_phone_verify_pending,
    recovery_phone_resend_pending,
    recovery_phone_input_invalid,
    recovery_phone_code_rejected,
    recovery_phone_resent,
    recovery_phone_resend_failed,
    recovery_phone_verify_rate_limited,
    recovery_phone_resend_rate_limited,
    recovery_phone_captcha_required,
    recovery_phone_failed,
    recovery_phone_token_ready,
    browser_starting,
    browser_waiting,
    browser_exchange_pending,
    browser_expired,
    browser_failed,
    signed_out,
    signed_out_unconfirmed,
};

bool login_browser_mode(LoginStatus status) noexcept;

bool login_phone_mode(LoginStatus status) noexcept;
bool login_phone_editable(LoginStatus status) noexcept;
bool login_phone_code_valid(const Utf8View& code, bool complete = true) noexcept;

// Empty cells occupy their original positions in the sole locked MFA field.
// Spaces are presentation/ACP placeholders and never accepted as wire digits.
bool login_phone_cells_valid(const Utf8View& cells) noexcept;

bool login_reset_editable(LoginStatus status) noexcept;

bool login_recovery_mode(LoginStatus status) noexcept;
bool login_recovery_editable(LoginStatus status) noexcept;

// QR remote authentication is an independent presentation state: the transport
// stays alive while the credential form is editable, so it must not overload
// LoginStatus or disable the password controls.
enum class LoginRemoteStatus : u8 {
    idle,
    connecting,
    qr_ready,
    mobile_pending,
    refreshing,
    captcha_required,
    failed,
    cancelled, // the phone declined; a fresh QR code follows automatically
};

// Browser CAPTCHA bridge presentation while a challenge is pending.
enum class LoginCaptchaStatus : u8 { none, ready, starting, waiting, retry, expired, failed };

bool login_captcha_mode(LoginStatus status, LoginRemoteStatus remote) noexcept;

bool login_browser_available(LoginStatus status, LoginRemoteStatus remote, bool reset_mode) noexcept;

bool login_requires_back(LoginStatus status, LoginRemoteStatus remote) noexcept;

bool login_credential_fields_hidden(LoginStatus status, LoginRemoteStatus remote,
                                    bool reset_mode) noexcept;

enum class LoginAction : u8 { none, submit, remote_auth, mfa, back, recovery, recovery_submit, phone_verify, phone_resend, browser_sign_in, captcha_verify };
// Shared mouse/keyboard dispatch; recovery never falls through to credentials.
LoginAction login_control_action(LoginControl control, LoginStatus status,
                                 LoginRemoteStatus remote, bool reset_mode = false) noexcept;

#if !defined(_WIN32)
// What the security key of the sign-in is doing or has said on Linux, where the app (not an OS prompt) asks the user for
// the touch and the PIN: it picks the screen's wording. Windows shows the system's own prompt.
enum class LoginSecurityKey : u8 {
    none,
    touch,           // status submitting: waiting for a key to be plugged in and touched
    pin,             // status mfa_required: the key wants its PIN, typed in the code field
    pin_wrong,       // the same, after a PIN the key refused
    no_key,          // status webauthn_failed: no key was plugged in
    cannot_open,     // the keys cannot be opened (no access to their devices)
    not_registered,  // the key holds no credential of this account
    pin_blocked,
    pin_not_set,
    timed_out,       // the key was not touched in time
    unavailable,     // libfido2 is not installed
    failed,
};
#endif

struct LoginView {
    Utf8View login;
    Utf8View password_mask;
    Utf8View remote_user;
    u32 edit_anchor;
    u32 edit_caret;
    bool edit_active;
    LoginControl focus;
    LoginStatus status;
    u8 mfa_capabilities;
    u8 selected_mfa_capability;
    u8 backup_acknowledgement_seconds;
    bool mfa_code_ready;
    const QrCode* qr_code;
    bool sms_sending;
    bool sms_sent;
    bool sms_failed;
    bool visible;
    // Composition offsets are codepoint boundaries relative to the visible
    // edit window, not UTF-16 ACP positions.
    ConstSlice<TextCompositionSpan> edit_composition;
    bool edit_composition_active;
    LoginRemoteStatus remote_status;
    bool reset_mode;
    bool phone_code_ready;
    LoginCaptchaStatus captcha_status;
    bool caps_lock;
    bool password_revealed;
    bool auth_rate_limited;
    bool auth_retry_time_unknown;
    // With status rejected: Discord suspended the account.
    bool account_suspended;
#if !defined(_WIN32)
    LoginSecurityKey security_key;
#endif
};

bool login_view_valid(const LoginView& view) noexcept;
bool login_view_action_disabled(const LoginView& view) noexcept;
bool login_view_submit_disabled(const LoginView& view) noexcept;
bool login_view_auth_retry_resume(const LoginView& view) noexcept;
bool login_view_recovery_disabled(const LoginView& view) noexcept;
// The challenge's "Verify in browser" action; disabled only while opening.
bool login_view_captcha_action(const LoginView& view) noexcept;
// Show-password applies only to a visible password edit, never MFA/phone codes.
bool login_view_password_reveal_available(const LoginView& view) noexcept;
bool login_view_matches_scene(const LoginView& view, const LoginScene* scene) noexcept;

} // namespace maicord
