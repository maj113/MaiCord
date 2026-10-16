#pragma once

#include "maicord/ui/login_scene.hpp"
#include "maicord/ui/qr_code.hpp"
#include "maicord/ui/text_composition.hpp"

namespace maicord {

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

constexpr bool login_browser_mode(LoginStatus status) noexcept {
    return status >= LoginStatus::browser_starting && status <= LoginStatus::browser_failed;
}

constexpr bool login_phone_mode(LoginStatus status) noexcept {
    return status == LoginStatus::recovery_phone_required ||
        (status >= LoginStatus::recovery_phone_verify_pending && status <= LoginStatus::recovery_phone_token_ready);
}
constexpr bool login_phone_editable(LoginStatus status) noexcept {
    return status == LoginStatus::recovery_phone_required || status == LoginStatus::recovery_phone_input_invalid ||
        status == LoginStatus::recovery_phone_code_rejected || status == LoginStatus::recovery_phone_resent ||
        status == LoginStatus::recovery_phone_resend_failed;
}
constexpr bool login_phone_code_valid(Utf8View code, bool complete = true) noexcept {
    if ((!code.data && code.bytes) || code.bytes > kLoginPhoneCodeDigits || (complete && code.bytes != kLoginPhoneCodeDigits)) return false;
    for (u32 i = 0; i < code.bytes; ++i) if (code.data[i] < '0' || code.data[i] > '9') return false;
    return true;
}

// Empty cells occupy their original positions in the sole locked MFA field.
// Spaces are presentation/ACP placeholders and never accepted as wire digits.
constexpr bool login_phone_cells_valid(Utf8View cells) noexcept {
    if ((!cells.data && cells.bytes) || cells.bytes > kLoginPhoneCodeDigits) return false;
    for (u32 i = 0; i < cells.bytes; ++i)
        if (cells.data[i] != ' ' && (cells.data[i] < '0' || cells.data[i] > '9')) return false;
    return true;
}

constexpr bool login_reset_editable(LoginStatus status) noexcept {
    return status == LoginStatus::reset_editing || status == LoginStatus::reset_rejected;
}

constexpr bool login_recovery_mode(LoginStatus status) noexcept {
    return !login_phone_mode(status) && status >= LoginStatus::recovery_editing && status <= LoginStatus::recovery_captcha_required;
}
constexpr bool login_recovery_editable(LoginStatus status) noexcept {
    return status == LoginStatus::recovery_editing || status == LoginStatus::recovery_failed;
}

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

constexpr bool login_captcha_mode(LoginStatus status, LoginRemoteStatus remote) noexcept {
    return status == LoginStatus::captcha_required || status == LoginStatus::recovery_captcha_required ||
           status == LoginStatus::reset_captcha_required || status == LoginStatus::recovery_phone_captcha_required ||
           remote == LoginRemoteStatus::captcha_required;
}

constexpr bool login_browser_available(LoginStatus status, LoginRemoteStatus remote, bool reset_mode) noexcept {
    return !reset_mode && !login_phone_mode(status) && !login_recovery_mode(status) && !login_browser_mode(status) &&
        status != LoginStatus::restoring_session && status != LoginStatus::submitting && status != LoginStatus::cancelling &&
        status != LoginStatus::profile_save_failed &&
        (status == LoginStatus::idle || status == LoginStatus::rejected || status == LoginStatus::connection_failed ||
         status == LoginStatus::session_expired || status == LoginStatus::remote_expired ||
         status == LoginStatus::signed_out || status == LoginStatus::signed_out_unconfirmed ||
         status == LoginStatus::captcha_required || remote == LoginRemoteStatus::captcha_required);
}

constexpr bool login_requires_back(LoginStatus status, LoginRemoteStatus remote) noexcept {
    return login_browser_mode(status) || status == LoginStatus::captcha_required ||
           status == LoginStatus::cancelling || status == LoginStatus::recovery_pending ||
           status == LoginStatus::recovery_sent ||
           (login_phone_mode(status) && !login_phone_editable(status)) ||
           status == LoginStatus::recovery_captcha_required || status == LoginStatus::reset_pending ||
           status == LoginStatus::reset_mfa_pending || status == LoginStatus::reset_failed ||
           status == LoginStatus::reset_rate_limited || status == LoginStatus::reset_captcha_required ||
           remote == LoginRemoteStatus::captcha_required;
}

constexpr bool login_credential_fields_hidden(LoginStatus status, LoginRemoteStatus remote,
                                              bool reset_mode) noexcept {
    const bool blocked = login_requires_back(status, remote);
    return login_browser_mode(status) || login_phone_mode(status) || login_recovery_mode(status) ||
           (reset_mode &&
            (blocked || status == LoginStatus::profile_save_failed || status == LoginStatus::submitting)) ||
           (!blocked && status == LoginStatus::mfa_backup_acknowledgement);
}

enum class LoginAction : u8 { none, submit, remote_auth, mfa, back, recovery, recovery_submit, phone_verify, phone_resend, browser_sign_in, captcha_verify };
// Shared mouse/keyboard dispatch; recovery never falls through to credentials.
constexpr LoginAction login_control_action(LoginControl control, LoginStatus status,
                                           LoginRemoteStatus remote, bool reset_mode = false) noexcept {
    if (control == LoginControl::browser_sign_in)
        return login_browser_available(status, remote, reset_mode) ? LoginAction::browser_sign_in : LoginAction::none;
    if (control == LoginControl::submit && login_captcha_mode(status, remote))
        return LoginAction::captcha_verify;
    if (login_phone_mode(status)) {
        if (control == LoginControl::back) return LoginAction::back;
        if (!login_phone_editable(status)) return LoginAction::none;
        if (control == LoginControl::phone_code || control == LoginControl::submit) return LoginAction::phone_verify;
        return control == LoginControl::phone_resend ? LoginAction::phone_resend : LoginAction::none;
    }
    if (reset_mode) {
        if (control == LoginControl::back) return LoginAction::back;
        if (control == LoginControl::login || control == LoginControl::qr || control == LoginControl::forgot_password) return LoginAction::none;
        if (status == LoginStatus::profile_save_failed && control == LoginControl::password) return LoginAction::none;
    }
    if (status == LoginStatus::restoring_session || status == LoginStatus::submitting)
        return LoginAction::none;
    if (login_requires_back(status, remote))
        return control == LoginControl::back ? LoginAction::back : LoginAction::none;
    if (login_recovery_mode(status)) {
        if (control == LoginControl::back) return LoginAction::back;
        return login_recovery_editable(status) && (control == LoginControl::login || control == LoginControl::submit) ?
            LoginAction::recovery_submit : LoginAction::none;
    }
    if (login_reset_editable(status)) {
        if (control == LoginControl::back) return LoginAction::back;
        return control == LoginControl::password || control == LoginControl::submit ? LoginAction::submit : LoginAction::none;
    }
    switch (control) {
    case LoginControl::login:
    case LoginControl::password:
    case LoginControl::submit: return LoginAction::submit;
    case LoginControl::qr: return LoginAction::remote_auth;
    case LoginControl::forgot_password:
        return status == LoginStatus::idle || status == LoginStatus::rejected ||
            status == LoginStatus::connection_failed || status == LoginStatus::session_expired ||
            status == LoginStatus::signed_out || status == LoginStatus::signed_out_unconfirmed ||
            status == LoginStatus::remote_expired ? LoginAction::recovery : LoginAction::none;
    case LoginControl::mfa_totp:
    case LoginControl::mfa_sms:
    case LoginControl::mfa_backup:
    case LoginControl::mfa_webauthn: return LoginAction::mfa;
    default: return LoginAction::none;
    }
}

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
    bool edit_composition_complete;
    LoginRemoteStatus remote_status;
    bool reset_mode;
    bool phone_code_ready;
    LoginCaptchaStatus captcha_status;
    bool caps_lock;
    bool password_revealed;
    bool auth_rate_limited;
    bool auth_retry_time_unknown;
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
