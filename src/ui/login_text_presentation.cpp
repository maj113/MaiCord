#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/ui/foreground_color.hpp"

#include "maicord/core/utf.hpp"
#include "maicord/ui/text_style.hpp"

namespace maicord {
namespace {

Utf8View visible_text_tail(Utf8View text, u32 maximum_codepoints) noexcept {
    const Utf8Validation validation = utf8_validate(text);
    if (!validation.valid || validation.codepoints <= maximum_codepoints) {
        return validation.valid ? text : Utf8View{};
    }
    u32 offset = text.bytes;
    u32 remaining = maximum_codepoints;
    while (offset != 0 && remaining != 0) {
        --offset;
        while (offset != 0 && (static_cast<u8>(text.data[offset]) & 0xC0u) == 0x80u) {
            --offset;
        }
        --remaining;
    }
    return Utf8View{text.data + offset, text.bytes - offset};
}

// Every command is one of these roles: colour, style, alignment and wrapping
// come from the table, so a call names only the element, its text and the role.
enum class Ink : u8 { primary, secondary, accent, notification, submit };

struct TextRole {
    u32 flags;
    Ink ink;
    UiTextAlignment horizontal, vertical;
    UiTextWrapping wrapping;
};

enum class Role : u8 {
    heading,
    root,
    browser_link,
    subtitle,
    subtitle_wrapped,
    label,
    field,
    field_hint,
    link_accent,
    link_muted,
    submit,
    submit_muted,
    status,
    status_wrapped,
    status_error,
    status_wrapped_error,
    account_name,
    account_note,
    account_error,
    resend,
    resend_muted,
    method,
    qr_title,
    qr_subtitle,
    count
};

constexpr u32 kHeadingStyle = text_style_heading_1 | text_style_bold;
constexpr u32 kLabelStyle = text_style_subtext | text_style_bold;
constexpr UiTextAlignment kStart = UiTextAlignment::start;
constexpr UiTextAlignment kCenter = UiTextAlignment::center;
constexpr UiTextWrapping kLine = UiTextWrapping::no_wrap;
constexpr UiTextWrapping kWrap = UiTextWrapping::wrap;

constexpr TextRole kRoles[]{
    {kHeadingStyle, Ink::primary, kCenter, kStart, kLine},                   // heading
    {text_style_heading_1, Ink::primary, kCenter, kCenter, kLine},           // root
    {text_style_subtext, Ink::accent, UiTextAlignment::end, kCenter, kLine}, // browser_link
    {text_style_none, Ink::secondary, kCenter, kStart, kLine},               // subtitle
    {text_style_none, Ink::secondary, kCenter, kStart, kWrap},               // subtitle_wrapped
    {kLabelStyle, Ink::secondary, kStart, kStart, kLine},                    // label
    {text_style_none, Ink::primary, kStart, kCenter, kLine},                 // field
    {text_style_none, Ink::secondary, kStart, kCenter, kLine},               // field_hint
    {text_style_subtext, Ink::accent, kStart, kCenter, kLine},               // link_accent
    {text_style_subtext, Ink::secondary, kStart, kCenter, kLine},            // link_muted
    {text_style_bold, Ink::submit, kCenter, kCenter, kLine},                 // submit
    {text_style_bold, Ink::secondary, kCenter, kCenter, kLine},              // submit_muted
    {text_style_subtext, Ink::secondary, kStart, kStart, kLine},             // status
    {text_style_subtext, Ink::secondary, kStart, kStart, kWrap},             // status_wrapped
    {text_style_subtext, Ink::notification, kStart, kStart, kLine},          // status_error
    {text_style_subtext, Ink::notification, kStart, kStart, kWrap},          // status_wrapped_error
    {text_style_bold, Ink::primary, kStart, kStart, kLine},                  // account_name
    {text_style_subtext, Ink::secondary, kStart, kStart, kLine},             // account_note
    {text_style_subtext, Ink::notification, kStart, kStart, kLine},          // account_error
    {text_style_subtext, Ink::accent, kCenter, kCenter, kLine},              // resend
    {text_style_subtext, Ink::secondary, kCenter, kCenter, kLine},           // resend_muted
    {kLabelStyle, Ink::primary, kCenter, kCenter, kLine},                    // method
    {text_style_bold, Ink::primary, kCenter, kStart, kLine},                 // qr_title
    {text_style_subtext, Ink::secondary, kCenter, kStart, kLine},            // qr_subtitle
};
static_assert(sizeof(kRoles) / sizeof(kRoles[0]) == static_cast<u32>(Role::count));

struct Writer {
    const LoginScene& scene;
    UiTextCommandStream* stream;
    const Theme& theme;
    const Color& submit_ink;
};

bool append_command(const Writer& out, LoginElement element, const Utf8View& text, Role role) noexcept {
    if (text.bytes == 0) {
        return true;
    }
    const LoginScene& scene = out.scene;
    const TextRole& style = kRoles[static_cast<u32>(role)];
    const UiLayoutNode* node = login_scene_element(scene, element);
    const Color& color = style.ink == Ink::primary     ? out.theme.text_primary
                         : style.ink == Ink::secondary ? out.theme.text_secondary
                         : style.ink == Ink::accent    ? out.theme.accent
                         : style.ink == Ink::submit    ? out.submit_ink
                                                       : out.theme.notification;
    return node && ui_text_command_stream_push(
                       out.stream,
                       UiTextCommand{text, UiWidgetReference{&scene.widgets, scene.elements[static_cast<u32>(element)]},
                                     node->content_rectangle, node->clip_rectangle, color, style.flags,
                                     style.horizontal, style.vertical, style.wrapping, UiTextOverflow::clip});
}

Utf8View auth_cooldown_copy(const LoginView& view) noexcept {
    return view.auth_retry_time_unknown ? utf8_literal("No retry time was provided. Resume when ready.")
                                       : utf8_literal("Too many attempts. Wait before retrying.");
}

Utf8View auth_cooldown_action(const LoginView& view) noexcept {
    return login_view_auth_retry_resume(view) ? utf8_literal("Resume sign-in") : utf8_literal("Please wait");
}

Utf8View backup_acknowledgement_action(u8 seconds) noexcept {
    static constexpr Utf8View labels[]{utf8_literal("Continue"),       utf8_literal("Continue in 1"),
                                       utf8_literal("Continue in 2"),  utf8_literal("Continue in 3"),
                                       utf8_literal("Continue in 4"),  utf8_literal("Continue in 5"),
                                       utf8_literal("Continue in 6"),  utf8_literal("Continue in 7"),
                                       utf8_literal("Continue in 8"),  utf8_literal("Continue in 9"),
                                       utf8_literal("Continue in 10")};
    return labels[seconds <= 10 ? seconds : 0];
}

Utf8View captcha_action_label(LoginCaptchaStatus status) noexcept {
    switch (status) {
    case LoginCaptchaStatus::starting: return utf8_literal("Opening browser…");
    case LoginCaptchaStatus::waiting: return utf8_literal("Open the check again");
    case LoginCaptchaStatus::retry:
    case LoginCaptchaStatus::expired:
    case LoginCaptchaStatus::failed: return utf8_literal("Try again in browser");
    default: return utf8_literal("Verify in browser");
    }
}

Utf8View captcha_status_copy(LoginCaptchaStatus status) noexcept {
    switch (status) {
    case LoginCaptchaStatus::starting: return utf8_literal("Opening your browser…");
    case LoginCaptchaStatus::waiting: return utf8_literal("Finish the check in your browser, then come back.");
    case LoginCaptchaStatus::retry: return utf8_literal("That check didn't go through. Try again.");
    case LoginCaptchaStatus::expired: return utf8_literal("The browser check expired. Try again.");
    case LoginCaptchaStatus::failed: return utf8_literal("Couldn't open the browser check. Try again.");
    default: return utf8_literal("Complete a quick check in your browser to continue.");
    }
}

bool captcha_status_error(LoginCaptchaStatus status) noexcept {
    return status == LoginCaptchaStatus::retry || status == LoginCaptchaStatus::expired ||
           status == LoginCaptchaStatus::failed;
}

} // namespace

bool login_text_presentation_build(const LoginScene& scene, const LoginView& view, const Theme& theme,
                                   UiTextCommandStream* stream) noexcept {
    if (!stream || !stream->storage.data || stream->storage.count < kMaximumLoginTextCommands ||
        !login_view_matches_scene(view, &scene)) {
        saturating_increment_if(stream, stream->rejected);
        return false;
    }
    ui_text_command_stream_reset(stream);
    if (!view.visible) {
        return true;
    }
    const Color submit_ink =
        scene.submit_disabled ? theme.text_primary : ui_surface_foreground(theme, theme.tokens.emphasis.fill);
    const Writer out{scene, stream, theme, submit_ink};

    if (scene.browser_mode) {
        const Utf8View status =
            view.auth_rate_limited ? auth_cooldown_copy(view)
            : view.status == LoginStatus::browser_starting
                ? utf8_literal("Opening your browser…")
            : view.status == LoginStatus::browser_exchange_pending
                ? utf8_literal("Finishing sign-in…")
            : view.status == LoginStatus::browser_expired
                ? utf8_literal("This browser sign-in expired. Choose Back to start again.")
            : view.status == LoginStatus::browser_failed
                ? utf8_literal("The browser sign-in could not be completed. Choose Back to try again.")
                : utf8_literal("Finish signing in your browser, then come back here.");
        return append_command(out, LoginElement::heading, utf8_literal("Sign in with your browser"), Role::heading) &&
               append_command(out, LoginElement::forgot_password, utf8_literal("Back to sign in"),
                              Role::link_accent) &&
               append_command(out, LoginElement::submit,
                              view.auth_rate_limited ? auth_cooldown_action(view)
                              : view.status == LoginStatus::browser_expired ? utf8_literal("Sign-in expired")
                              : view.status == LoginStatus::browser_failed
                                  ? utf8_literal("Sign-in failed")
                                  : utf8_literal("Waiting for your browser"),
                              Role::submit_muted) &&
               append_command(out, LoginElement::status_message, status, Role::status_wrapped);
    }

    if (scene.phone_mode) {
        Utf8View status = utf8_literal("Enter the 6-digit code.");
        switch (view.status) {
        case LoginStatus::recovery_phone_verify_pending:
            status = utf8_literal("Verifying…");
            break;
        case LoginStatus::recovery_phone_resend_pending:
            status = utf8_literal("Sending a new code…");
            break;
        case LoginStatus::recovery_phone_input_invalid:
            status = utf8_literal("Enter exactly six digits.");
            break;
        case LoginStatus::recovery_phone_code_rejected:
            status = utf8_literal("That code didn't work. Try again or request a new one.");
            break;
        case LoginStatus::recovery_phone_resent:
            status = utf8_literal("New code sent.");
            break;
        case LoginStatus::recovery_phone_resend_failed:
            status = utf8_literal("Couldn't send a new code. Try again.");
            break;
        case LoginStatus::recovery_phone_verify_rate_limited:
            status = utf8_literal(
                "Too many attempts. Wait a moment and try again.");
            break;
        case LoginStatus::recovery_phone_resend_rate_limited:
            status =
                utf8_literal("Too many requests. Wait a moment and try again.");
            break;
        case LoginStatus::recovery_phone_captcha_required:
            status = captcha_status_copy(view.captcha_status);
            break;
        case LoginStatus::recovery_phone_failed:
            status = utf8_literal(
                "Couldn't confirm the request. Go back and try again.");
            break;
        case LoginStatus::recovery_phone_token_ready:
            status = utf8_literal("Preparing password reset…");
            break;
        default:
            break;
        }
        if (view.auth_retry_time_unknown) status = auth_cooldown_copy(view);
        const bool error = view.status == LoginStatus::recovery_phone_input_invalid ||
                           view.status == LoginStatus::recovery_phone_code_rejected ||
                           view.status == LoginStatus::recovery_phone_resend_failed ||
                           view.status == LoginStatus::recovery_phone_verify_rate_limited ||
                           view.status == LoginStatus::recovery_phone_resend_rate_limited ||
                           (view.status == LoginStatus::recovery_phone_captcha_required &&
                            captcha_status_error(view.captcha_status)) ||
                           view.status == LoginStatus::recovery_phone_failed;
        if (!append_command(out, LoginElement::heading, utf8_literal("Phone recovery"), Role::heading) ||
            !append_command(out, LoginElement::subtitle,
                            utf8_literal("Enter the six-digit SMS code to reset your password."),
                            Role::subtitle_wrapped) ||
            !append_command(out, LoginElement::password_label, utf8_literal("SMS CODE"), Role::label) ||
            !append_command(out, LoginElement::forgot_password, utf8_literal("Back to sign in"),
                            Role::link_accent) ||
            !append_command(out, LoginElement::mfa_sms, utf8_literal("Resend code"),
                            scene.phone_resend_disabled ? Role::resend_muted : Role::resend) ||
            !append_command(out, LoginElement::submit,
                            view.auth_rate_limited ? auth_cooldown_action(view)
                            : view.status == LoginStatus::recovery_phone_captcha_required
                                ? captcha_action_label(view.captcha_status)
                                : utf8_literal("Verify"),
                            Role::submit) ||
            !append_command(out, LoginElement::status_message, status,
                            error ? Role::status_wrapped_error : Role::status_wrapped))
            return false;
        const auto* field = login_scene_element(scene, LoginElement::password_field);
        const auto owner = scene.elements[static_cast<u32>(LoginElement::password_field)];
        if (!field)
            return false;
        for (u32 i = 0; i < view.password_mask.bytes; ++i) {
            if (view.password_mask.data[i] == ' ')
                continue;
            const UiTextCommand digit{{view.password_mask.data + i, 1},
                                      {&scene.widgets, owner},
                                      login_phone_cell_rectangle(field->content_rectangle, i),
                                      field->clip_rectangle,
                                      theme.text_primary,
                                      text_style_none,
                                      UiTextAlignment::center,
                                      UiTextAlignment::center,
                                      UiTextWrapping::no_wrap,
                                      UiTextOverflow::clip};
            if (!ui_text_command_stream_push(stream, digit))
                return false;
        }
        return true;
    }

    if (view.status == LoginStatus::restoring_session)
        return append_command(out, LoginElement::root,
                              scene.switching_accounts ? utf8_literal("Switching accounts…")
                                                       : utf8_literal("Connecting to Discord…"),
                              Role::root);
    const Utf8View login = visible_text_tail(view.login, 48);
    const Utf8View password = visible_text_tail(view.password_mask, 48);
    const Utf8View remote_user = visible_text_tail(view.remote_user, 32);
    const bool blocked = login_requires_back(view.status, view.remote_status);
    const bool signed_out =
        view.status == LoginStatus::signed_out || view.status == LoginStatus::signed_out_unconfirmed;
    const bool recovery = login_recovery_mode(view.status);
    const bool reset = view.reset_mode;
    const bool recovery_result = recovery && blocked;
    const u8 selected_mfa =
        blocked ? static_cast<u8>(login_mfa_capability_none) : view.selected_mfa_capability;
    const bool webauthn = !blocked && (view.status == LoginStatus::webauthn_required ||
                                       view.status == LoginStatus::webauthn_failed);
    const bool backup_acknowledgement = !blocked && view.status == LoginStatus::mfa_backup_acknowledgement;
    const LoginRemoteStatus remote_status =
        view.remote_status != LoginRemoteStatus::idle       ? view.remote_status
        : view.status == LoginStatus::remote_connecting     ? LoginRemoteStatus::connecting
        : view.status == LoginStatus::remote_qr_ready       ? LoginRemoteStatus::qr_ready
        : view.status == LoginStatus::remote_mobile_pending ? LoginRemoteStatus::mobile_pending
        : view.status == LoginStatus::remote_expired        ? LoginRemoteStatus::refreshing
                                                            : LoginRemoteStatus::idle;
    const bool mfa =
        !blocked && (view.mfa_capabilities != 0 || view.status == LoginStatus::mfa_required ||
                     view.status == LoginStatus::mfa_input_invalid ||
                     view.status == LoginStatus::mfa_code_rejected || backup_acknowledgement || webauthn);
    const Utf8View login_value = login.bytes != 0 ? login
                                 : recovery       ? utf8_literal("Account email or phone")
                                                  : utf8_literal("Email, phone number or reset link");
    const Utf8View password_value = webauthn              ? utf8_literal("Windows Hello or security key")
                                    : password.bytes != 0 ? password
                                    : selected_mfa == login_mfa_capability_sms ? utf8_literal("SMS code")
                                    : selected_mfa == login_mfa_capability_backup
                                        ? utf8_literal("Backup code")
                                    : mfa   ? utf8_literal("Authentication code")
                                    : reset ? utf8_literal("New password")
                                            : utf8_literal("Password");
    const bool captcha = login_captcha_mode(view.status, view.remote_status);
    const Utf8View action =
        view.auth_rate_limited ? auth_cooldown_action(view)
        : captcha ? captcha_action_label(view.captcha_status)
        : view.status == LoginStatus::reset_pending || view.status == LoginStatus::reset_mfa_pending
            ? utf8_literal("Resetting password…")
        : reset && view.status == LoginStatus::submitting ? utf8_literal("Signing in…")
        : reset && blocked                                ? utf8_literal("Reset paused")
        : recovery
            ? (view.status == LoginStatus::recovery_pending        ? utf8_literal("Sending request…")
               : view.status == LoginStatus::recovery_sent         ? utf8_literal("Request sent")
               : recovery_result                                   ? utf8_literal("Recovery unavailable")
               : view.status == LoginStatus::recovery_rate_limited ? utf8_literal("Please wait")
                                                                   : utf8_literal("Send instructions"))
        : blocked                                ? utf8_literal("Verification required")
        : view.sms_sending                       ? utf8_literal("Sending code…")
        : view.status == LoginStatus::submitting ? utf8_literal("Signing in…")
        : backup_acknowledgement ? backup_acknowledgement_action(view.backup_acknowledgement_seconds)
        : view.status == LoginStatus::profile_save_failed      ? utf8_literal("Try again")
        : webauthn                                             ? utf8_literal("Continue with security key")
        : mfa                                                  ? utf8_literal("Verify")
        : reset                                                ? utf8_literal("Reset password")
                                                               : utf8_literal("Log in");
    const Utf8View status =
        view.auth_retry_time_unknown ? auth_cooldown_copy(view)
        : view.auth_rate_limited && !reset && !recovery && !login_phone_mode(view.status)
            ? utf8_literal("Too many attempts. Wait before retrying.")
        : captcha                                              ? captcha_status_copy(view.captcha_status)
        : view.status == LoginStatus::signed_out             ? utf8_literal("You've been logged out.")
        : view.status == LoginStatus::signed_out_unconfirmed ? utf8_literal("Logged out of this device.")
        : reset && view.status == LoginStatus::submitting    ? utf8_literal("Signing in…")
        : view.status == LoginStatus::reset_editing
            ? utf8_literal("Enter a new password with at least 8 characters.")
        : view.status == LoginStatus::reset_pending || view.status == LoginStatus::reset_mfa_pending
            ? utf8_literal("Resetting password…")
        : view.status == LoginStatus::reset_rejected
            ? utf8_literal("That password can't be used. Try a different one.")
        : view.status == LoginStatus::reset_rate_limited
            ? utf8_literal(
                  "Too many attempts. Wait a moment and try again.")
        : view.status == LoginStatus::reset_failed
            ? utf8_literal("Couldn't reset your password. Go back and paste your reset link again.")
        : view.status == LoginStatus::recovery_pending ? utf8_literal("Sending instructions…")
        : view.status == LoginStatus::recovery_sent
            ? utf8_literal("Check your email or phone for instructions.")
        : view.status == LoginStatus::recovery_phone_required
            ? utf8_literal("Phone verification isn't supported yet. Use a browser.")
        : view.status == LoginStatus::recovery_rate_limited
            ? utf8_literal("Too many recovery requests. Wait before trying again.")
        : view.status == LoginStatus::recovery_failed
            ? utf8_literal(
                  "Couldn't send instructions. Check your details and try again.")
        : view.status == LoginStatus::cancelling
            ? utf8_literal("Couldn't cancel sign-in. Try again.")
        : view.status == LoginStatus::session_expired
            ? utf8_literal("Your session expired. Log in again.")
        : view.status == LoginStatus::connection_failed
            ? utf8_literal("Couldn't connect. Try again.")
        : view.status == LoginStatus::rejected
            ? utf8_literal("Login failed. Check your details and try again.")
        : remote_status == LoginRemoteStatus::connecting
            ? utf8_literal("Generating QR code…")
        : remote_status == LoginRemoteStatus::qr_ready
            ? utf8_literal("Scan the QR code with Discord on your phone.")
        : remote_status == LoginRemoteStatus::mobile_pending
            ? utf8_literal("Confirm on your phone to log in as:")
        : remote_status == LoginRemoteStatus::refreshing ? utf8_literal("Refreshing QR code…")
        : remote_status == LoginRemoteStatus::failed
            ? utf8_literal("Reconnecting…")
        : remote_status == LoginRemoteStatus::cancelled
            ? utf8_literal("Login cancelled on your phone.")
        : view.status == LoginStatus::mfa_required
            ? view.sms_sending ? utf8_literal("Sending code…")
              : selected_mfa == login_mfa_capability_sms && view.sms_sent
                  ? utf8_literal("Code sent.")
              : selected_mfa == login_mfa_capability_sms && view.sms_failed
                  ? utf8_literal("Couldn't send the code. Try again.")
                  : utf8_literal("Choose a verification method.")
        : view.status == LoginStatus::mfa_input_invalid
            ? selected_mfa == login_mfa_capability_backup
                  ? utf8_literal("Enter a valid backup code (8-11 characters).")
                  : utf8_literal("Enter a 6-digit authentication code.")
        : view.status == LoginStatus::mfa_code_rejected
            ? utf8_literal("That code didn't work. Try again.")
        : backup_acknowledgement
            ? utf8_literal("Backup codes can only be used once.")
        : view.status == LoginStatus::profile_save_failed ? utf8_literal("Couldn't save your sign-in.")
        : view.status == LoginStatus::webauthn_required
            ? utf8_literal("Complete verification in the Windows security prompt.")
        : view.status == LoginStatus::webauthn_failed
            ? utf8_literal("Security-key verification failed or was cancelled. Try again.")
            : Utf8View{};

    const bool credential_error = view.auth_rate_limited || view.status == LoginStatus::session_expired ||
                                  view.status == LoginStatus::connection_failed ||
                                  view.status == LoginStatus::rejected;
    const bool remote_message =
        !signed_out && !credential_error && remote_status != LoginRemoteStatus::idle;
    const bool status_error =
        captcha ? captcha_status_error(view.captcha_status) :
        view.status == LoginStatus::reset_rejected || view.status == LoginStatus::reset_failed ||
        view.status == LoginStatus::reset_rate_limited || view.status == LoginStatus::recovery_failed ||
        view.status == LoginStatus::recovery_phone_required ||
        view.status == LoginStatus::recovery_rate_limited || credential_error ||
        (remote_message
             ? remote_status == LoginRemoteStatus::failed
             : view.status == LoginStatus::mfa_input_invalid ||
                   view.status == LoginStatus::mfa_code_rejected ||
                   view.status == LoginStatus::profile_save_failed ||
                   view.status == LoginStatus::webauthn_failed ||
                   (view.status == LoginStatus::mfa_required && !view.sms_sending && !view.sms_sent &&
                    selected_mfa == login_mfa_capability_sms && view.sms_failed));
    const bool wrapped_status = recovery || reset;
    if (scene.browser_available &&
        !append_command(out, LoginElement::browser_sign_in,
                        captcha ? utf8_literal("Use browser sign-in") : utf8_literal("Use browser"),
                        Role::browser_link))
        return false;
    if (!append_command(out, LoginElement::heading,
                        reset                ? utf8_literal("Reset your password")
                        : recovery           ? utf8_literal("Password recovery")
                        : scene.add_account  ? utf8_literal("Add an account")
                                             : utf8_literal("Log in to MaiCord"),
                        Role::heading) ||
        !append_command(
            out, LoginElement::subtitle,
            reset                  ? utf8_literal("Choose a new password.")
            : recovery             ? utf8_literal("Get instructions to reset your password.")
            : scene.add_account    ? utf8_literal("Press Esc to go back.")
            : scene.account_return ? utf8_literal("Press Esc to use a saved account.")
                                   : utf8_literal("Use your Discord account."),
            wrapped_status ? Role::subtitle_wrapped : Role::subtitle) ||
        (!reset && !recovery_result &&
         !append_command(out, LoginElement::login_label,
                         recovery ? utf8_literal("ACCOUNT EMAIL / PHONE")
                                  : utf8_literal("EMAIL, PHONE OR RESET LINK"),
                         Role::label)) ||
        (!reset && !recovery_result &&
         !append_command(out, LoginElement::login_field, login_value,
                         login.bytes != 0 ? Role::field : Role::field_hint)) ||
        (!scene.credential_fields_hidden &&
         !append_command(out, LoginElement::password_label,
                         webauthn ? utf8_literal("PASSKEY OR SECURITY KEY")
                         : mfa    ? utf8_literal("AUTHENTICATION CODE")
                         : reset  ? utf8_literal("NEW PASSWORD")
                                  : utf8_literal("PASSWORD"),
                         Role::label)) ||
        (!scene.credential_fields_hidden &&
         !append_command(out, LoginElement::password_field, password_value,
                         !webauthn && password.bytes != 0 ? Role::field : Role::field_hint)) ||
        ((!mfa || reset) &&
         !append_command(out, LoginElement::forgot_password,
                         (reset || recovery || blocked)
                             ? utf8_literal("Back to sign in")
                             : utf8_literal("Forgot your password?"),
                         (reset || recovery || blocked || !login_view_recovery_disabled(view))
                             ? Role::link_accent
                             : Role::link_muted)) ||
        !append_command(out, LoginElement::submit, action, Role::submit) ||
        !append_command(out, LoginElement::status_message, status,
                        status_error ? (wrapped_status ? Role::status_wrapped_error : Role::status_error)
                                     : (wrapped_status ? Role::status_wrapped : Role::status)) ||
        (!signed_out && remote_status == LoginRemoteStatus::mobile_pending &&
         !append_command(out, LoginElement::status_account, remote_user, Role::account_name)) ||
        (view.status == LoginStatus::signed_out_unconfirmed &&
         !append_command(out, LoginElement::status_account, utf8_literal("Logged out on this device only."),
                         Role::account_note)) ||
        (view.caps_lock && !(!signed_out && remote_status == LoginRemoteStatus::mobile_pending) &&
         view.status != LoginStatus::signed_out_unconfirmed && view.status != LoginStatus::profile_save_failed &&
         !append_command(out, LoginElement::status_account, utf8_literal("Caps Lock is on."),
                         Role::account_note)) ||
        (view.status == LoginStatus::profile_save_failed &&
         !append_command(out, LoginElement::status_account,
                         view.reset_mode
                             ? utf8_literal("Try again, or go back to sign in.")
                             : utf8_literal("Try again, or close MaiCord to sign out."),
                         Role::account_error))) {
        return false;
    }

    for (u32 index = 0; index < kLoginMfaPresentationMethodCount; ++index) {
        const LoginMfaPresentationMethod& method = kLoginMfaPresentationMethods[index];
        const Utf8View label = method.element == LoginElement::mfa_webauthn ? utf8_literal("KEY")
                               : method.element == LoginElement::mfa_totp   ? utf8_literal("TOTP")
                               : method.element == LoginElement::mfa_backup ? utf8_literal("BACKUP")
                                                                            : utf8_literal("SMS");
        if (!blocked && (view.mfa_capabilities & method.capability) != 0 &&
            !append_command(out, method.element, label, Role::method)) {
            return false;
        }
    }
    const Utf8View qr_subtitle =
        remote_status == LoginRemoteStatus::connecting ? utf8_literal("Generating QR code")
        : remote_status == LoginRemoteStatus::refreshing || remote_status == LoginRemoteStatus::failed ||
                  remote_status == LoginRemoteStatus::cancelled
            ? utf8_literal("Refreshing automatically")
        : remote_status == LoginRemoteStatus::mobile_pending
            ? utf8_literal("Confirm the login on your phone")
        : remote_status == LoginRemoteStatus::captcha_required
            ? utf8_literal("Verification required")
            : utf8_literal("Scan from the Discord mobile app");
    if (scene.qr_visible &&
        (!append_command(out, LoginElement::qr_title, utf8_literal("Log in with QR code"), Role::qr_title) ||
         !append_command(out, LoginElement::qr_subtitle, qr_subtitle, Role::qr_subtitle))) {
        return false;
    }
    return stream->count != 0;
}

} // namespace maicord
