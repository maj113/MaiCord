#include "maicord/ui/login_view.hpp"

#include "maicord/core/utf.hpp"

namespace maicord {

bool login_view_valid(const LoginView& view) noexcept {
    const Utf8Validation login_validation = utf8_validate(view.login);
    const Utf8Validation secret_validation = utf8_validate(view.password_mask);
    const bool selected_mfa_is_single =
        view.selected_mfa_capability == login_mfa_capability_none ||
        (view.selected_mfa_capability & static_cast<u8>(view.selected_mfa_capability - 1)) == 0;
    const bool pointers_valid = (view.login.data || view.login.bytes == 0) &&
                                (view.password_mask.data || view.password_mask.bytes == 0) &&
                                (view.remote_user.data || view.remote_user.bytes == 0) &&
                                (view.edit_composition.data || view.edit_composition.count == 0) &&
                                view.edit_composition.count <= kTextCompositionSpanCapacity;
    const u32 edit_codepoints =
        view.focus == LoginControl::login ? login_validation.codepoints : secret_validation.codepoints;
    bool composition_valid = view.edit_composition_active || view.edit_composition.count == 0;
    for (u32 index = 0; composition_valid && index < view.edit_composition.count; ++index) {
        const TextCompositionSpan& span = view.edit_composition[index];
        composition_valid = span.start < span.end && span.end <= edit_codepoints &&
                            span.line_style <= TextCompositionLineStyle::squiggle &&
                            span.attribute <= TextCompositionAttribute::other &&
                            (span.color_flags & static_cast<u8>(~(text_composition_color_text |
                                                                 text_composition_color_background |
                                                                 text_composition_color_line))) == 0;
    }
    const bool backup_acknowledgement = view.status == LoginStatus::mfa_backup_acknowledgement;
    const bool backup_acknowledgement_valid =
        backup_acknowledgement ? view.backup_acknowledgement_seconds <= 10 &&
                                     (view.mfa_capabilities & login_mfa_capability_backup) != 0 &&
                                     view.selected_mfa_capability == login_mfa_capability_backup
                               : view.backup_acknowledgement_seconds == 0;
    const bool mfa_code_status =
        view.status == LoginStatus::mfa_required || view.status == LoginStatus::mfa_input_invalid ||
        view.status == LoginStatus::mfa_code_rejected;
    const bool mfa_code_ready_valid =
        !view.mfa_code_ready ||
        (mfa_code_status && view.selected_mfa_capability != login_mfa_capability_none &&
         view.selected_mfa_capability != login_mfa_capability_webauthn);
    const bool reset_valid = !view.reset_mode || (view.login.bytes == 0 && !view.qr_code && view.remote_status == LoginRemoteStatus::idle);
    const bool phone = login_phone_mode(view.status);
    const bool phone_valid = !phone || (!view.reset_mode && !view.login.bytes && !view.remote_user.bytes && !view.qr_code &&
        view.remote_status == LoginRemoteStatus::idle && !view.mfa_capabilities && !view.selected_mfa_capability &&
        login_phone_cells_valid(view.password_mask));
    const bool phone_ready_valid = !view.phone_code_ready || (phone && login_phone_editable(view.status) && login_phone_code_valid(view.password_mask));
    // Captcha presentation is read only while a challenge is pending.
    const bool captcha_valid = view.captcha_status <= LoginCaptchaStatus::failed;
    const bool reveal_valid = !view.password_revealed || login_view_password_reveal_available(view);
    return captcha_valid && reveal_valid && reset_valid && phone_valid && phone_ready_valid && pointers_valid && composition_valid && login_validation.valid && secret_validation.valid &&
           view.focus <= LoginControl::browser_sign_in &&
           view.status <= LoginStatus::signed_out_unconfirmed &&
           view.remote_status <= LoginRemoteStatus::cancelled &&
           (view.mfa_capabilities & static_cast<u8>(~login_mfa_capability_all)) == 0 &&
           selected_mfa_is_single &&
           (view.selected_mfa_capability & view.mfa_capabilities) == view.selected_mfa_capability &&
           backup_acknowledgement_valid && mfa_code_ready_valid &&
           (!view.qr_code || view.qr_code->size == kQrCodeSize) &&
           utf8_validate(view.remote_user).valid;
}

bool login_view_action_disabled(const LoginView& view) noexcept {
    return view.auth_rate_limited || view.status == LoginStatus::restoring_session || view.status == LoginStatus::submitting ||
           view.status == LoginStatus::recovery_rate_limited ||
           (view.status == LoginStatus::mfa_backup_acknowledgement &&
            view.backup_acknowledgement_seconds != 0) ||
           login_requires_back(view.status, view.remote_status);
}

bool login_view_captcha_action(const LoginView& view) noexcept {
    return !view.auth_rate_limited && login_captcha_mode(view.status, view.remote_status) && view.status != LoginStatus::cancelling &&
           view.captcha_status != LoginCaptchaStatus::starting;
}

bool login_view_password_reveal_available(const LoginView& view) noexcept {
    const LoginStatus status = view.status;
    return !login_phone_mode(status) && !login_recovery_mode(status) && !login_browser_mode(status) &&
           !login_requires_back(status, view.remote_status) &&
           !login_credential_fields_hidden(status, view.remote_status, view.reset_mode) && view.mfa_capabilities == 0 &&
           status != LoginStatus::restoring_session && status != LoginStatus::submitting &&
           status != LoginStatus::profile_save_failed && status != LoginStatus::cancelling &&
           status != LoginStatus::mfa_required && status != LoginStatus::mfa_input_invalid &&
           status != LoginStatus::mfa_code_rejected && status != LoginStatus::mfa_backup_acknowledgement &&
           status != LoginStatus::webauthn_required && status != LoginStatus::webauthn_failed;
}

bool login_view_auth_retry_resume(const LoginView& view) noexcept {
    return view.auth_rate_limited && view.auth_retry_time_unknown &&
        view.status != LoginStatus::restoring_session && view.status != LoginStatus::submitting &&
        view.status != LoginStatus::cancelling && view.status != LoginStatus::reset_pending &&
        view.status != LoginStatus::reset_mfa_pending && view.status != LoginStatus::recovery_pending &&
        view.status != LoginStatus::recovery_phone_verify_pending &&
        view.status != LoginStatus::recovery_phone_resend_pending &&
        view.status != LoginStatus::recovery_phone_token_ready && view.status != LoginStatus::browser_starting &&
        view.status != LoginStatus::browser_waiting && view.status != LoginStatus::browser_exchange_pending &&
        view.captcha_status != LoginCaptchaStatus::starting && view.captcha_status != LoginCaptchaStatus::waiting;
}

bool login_view_submit_disabled(const LoginView& view) noexcept {
    if (login_view_auth_retry_resume(view)) return false;
    if (login_captcha_mode(view.status, view.remote_status))
        return !login_view_captcha_action(view);
    const bool mfa_code_status =
        view.status == LoginStatus::mfa_required || view.status == LoginStatus::mfa_input_invalid ||
        view.status == LoginStatus::mfa_code_rejected;
    return login_view_action_disabled(view) || (mfa_code_status && !view.mfa_code_ready) ||
        (login_phone_mode(view.status) && !view.phone_code_ready);
}

bool login_view_recovery_disabled(const LoginView& view) noexcept {
    return view.reset_mode || login_control_action(LoginControl::forgot_password, view.status, view.remote_status) != LoginAction::recovery;
}

bool login_view_matches_scene(const LoginView& view, const LoginScene* scene) noexcept {
    if (!login_view_valid(view)) {
        return false;
    }
    if (!view.visible) {
        return true;
    }
    return scene && scene->widgets.count != 0 && scene->focus == view.focus &&
           scene->restoring_session == (view.status == LoginStatus::restoring_session) &&
           scene->back_only == login_requires_back(view.status, view.remote_status) &&
           scene->recovery_mode == login_recovery_mode(view.status) &&
           scene->reset_mode == view.reset_mode &&
           scene->phone_mode == login_phone_mode(view.status) &&
           scene->browser_mode == login_browser_mode(view.status) &&
           (!scene->browser_available || login_browser_available(view.status, view.remote_status, view.reset_mode)) &&
           scene->phone_resend_disabled == (login_phone_mode(view.status) && !login_phone_editable(view.status)) &&
           scene->recovery_disabled == login_view_recovery_disabled(view) &&
           scene->mfa_capabilities == (scene->back_only ? 0 : view.mfa_capabilities) &&
           scene->selected_mfa_capability == (scene->back_only ? 0 : view.selected_mfa_capability) &&
           scene->credential_fields_hidden == login_credential_fields_hidden(view.status, view.remote_status, view.reset_mode) &&
           scene->action_disabled == login_view_action_disabled(view) &&
           scene->submit_disabled == login_view_submit_disabled(view) &&
           scene->auth_retry_resume == login_view_auth_retry_resume(view) &&
           (!scene->captcha_action || login_view_captcha_action(view)) &&
           (!scene->password_reveal_available || login_view_password_reveal_available(view)) &&
           scene->password_revealed == (scene->password_reveal_available && view.password_revealed);
}

} // namespace maicord
