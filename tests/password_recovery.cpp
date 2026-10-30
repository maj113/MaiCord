#include "maicord/protocol/user_auth.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/login_view.hpp"
#include "maicord/ui/login_text_presentation.hpp"

namespace maicord {
void test_password_recovery(void (*check)(bool, const char*) noexcept) noexcept {
    u8 bytes[4096]{};
    constexpr auto identifier = utf8_literal("a\"b\\c\xc3\xa9@example.test");
    constexpr auto expected = utf8_literal("{\"login\":\"a\\\"b\\\\c\xc3\xa9@example.test\"}");
    auto written = user_auth_write_recovery_request(identifier, {bytes, sizeof(bytes)});
    check(written.ok() && written.bytes == expected.bytes && bytes_equal(bytes, expected.data, expected.bytes),
        "recovery writer emits only exact escaped Unicode identifier");
    for (auto& byte : bytes) byte = 0xa5;
    written = user_auth_write_recovery_request(identifier, {bytes, expected.bytes - 1});
    bool untouched = true; for (const auto byte : bytes) untouched = untouched && byte == 0xa5;
    check(written.error == UserAuthWriteError::output_too_small && untouched,
        "recovery writer capacity failure leaves output untouched");
    check(!user_auth_write_recovery_request({}, {bytes, sizeof(bytes)}).ok() &&
        !user_auth_write_recovery_request(utf8_literal("https://discord.com/reset#secret"), {bytes, sizeof(bytes)}).ok() &&
        !user_auth_recovery_identifier_valid(utf8_literal("discord://verify?token=secret")),
        "recovery rejects empty identifiers and unsupported token links");
    check(utf8_equal(user_auth_target(UserAuthOperation::recovery), utf8_literal("/api/v9/auth/forgot")),
        "recovery operation selects existing v9 auth forgot POST target");
    written = user_auth_write_request_head(UserAuthOperation::recovery, expected.bytes, {}, {bytes, sizeof(bytes)});
    constexpr auto head = utf8_literal("POST /api/v9/auth/forgot HTTP/1.1\r\n");
    check(written.ok() && bytes_equal(bytes, head.data, head.bytes), "recovery actual allowlisted request head uses forgot route");
    u32 scratch[256]{};
    char secrets[4096]{};
    UserAuthResponseStorage storage{};
    storage.captcha_service = {secrets, 128}; storage.captcha_sitekey = {secrets + 128, 256};
    storage.captcha_rqdata = {secrets + 384, 512}; storage.captcha_rqtoken = {secrets + 896, 512};
    storage.captcha_session_id = {secrets + 1408, 256}; storage.captcha_user_flow = {secrets + 1664, 128};
    const auto decode = [&](u16 status, Utf8View body, UserAuthResponse* out) noexcept {
        return user_auth_decode_recovery_response(status, body, {scratch, 256}, 16, storage, out);
    };
    const Utf8View accepted[]{utf8_literal("{}"), utf8_literal("{\"method\":null}"),
        utf8_literal("{\"method\":\"unverified-value\"}"), utf8_literal("{\"method\":17}")};
    UserAuthResponse response{};
    for (auto body : accepted)
        check(decode(200, body, &response).ok() && response.kind == UserAuthResponseKind::recovery_sent &&
            !response.token_bytes && !response.has_user_id && !response.mfa_capabilities,
            "recovery unknown absent null or numeric delivery method remains neutral non-auth success");
    check(decode(400, utf8_literal("{\"code\":70007}"), &response).ok() &&
        response.kind == UserAuthResponseKind::recovery_phone_required && response.error_code == 70007,
        "recovery phone verification required is distinct from sent");
    check(decode(400, utf8_literal("{\"code\":50035,\"errors\":{\"login\":{\"_errors\":[{\"message\":\"bad identifier\"}]}}}"), &response).ok() &&
        response.kind == UserAuthResponseKind::rejected, "recovery nested form rejection stays a non-auth rejection");
    check(decode(400, utf8_literal("{\"captcha_key\":[\"required\"],\"captcha_service\":\"hcaptcha\",\"captcha_sitekey\":\"site\",\"captcha_rqtoken\":\"opaque\"}"), &response).ok() &&
        response.kind == UserAuthResponseKind::captcha_required && response.captcha_rqtoken_bytes == 6,
        "recovery consumes existing CAPTCHA challenge schema without authenticating");
    check(!decode(200, utf8_literal("{\"token\":\"never-adopt\"}"), &response).ok() &&
        !decode(200, utf8_literal("{\"mfa\":true,\"ticket\":\"never-adopt\"}"), &response).ok() &&
        !decode(200, utf8_literal("{\"method\":1,\"method\":2}"), &response).ok() &&
        !decode(200, utf8_literal("{\"method\":"), &response).ok(),
        "recovery rejects auth-shaped duplicate or malformed success without output adoption");
    UserAuthState state{}; user_auth_state_initialize(&state);
    check(user_auth_state_begin_recovery(&state).ok(), "recovery generation begins in its own phase");
    response = {}; response.kind = UserAuthResponseKind::authenticated; response.token_bytes = 5;
    check(!user_auth_state_apply(&state, state.generation, response).ok() && state.phase == UserAuthPhase::recovery_pending,
        "recovery transition cannot promote an authenticated response");
    response.kind = UserAuthResponseKind::captcha_required;
    check(user_auth_state_apply(&state, state.generation, response).ok() &&
        state.captcha_return_phase == UserAuthPhase::recovery_pending && user_auth_state_resume_captcha(&state).ok(),
        "recovery CAPTCHA replay returns to exact recovery operation generation");
    const auto old = state.generation; user_auth_state_cancel(&state);
    response.kind = UserAuthResponseKind::recovery_sent;
    check(!user_auth_state_apply(&state, old, response).ok(), "recovery cancellation rejects late response generation");
    check(login_control_action(LoginControl::forgot_password, LoginStatus::idle, LoginRemoteStatus::idle) == LoginAction::recovery &&
        login_control_action(LoginControl::login, LoginStatus::recovery_editing, LoginRemoteStatus::idle) == LoginAction::recovery_submit &&
        login_control_action(LoginControl::password, LoginStatus::recovery_editing, LoginRemoteStatus::idle) == LoginAction::none &&
        login_control_action(LoginControl::qr, LoginStatus::recovery_editing, LoginRemoteStatus::idle) == LoginAction::none &&
        login_control_action(LoginControl::back, LoginStatus::recovery_pending, LoginRemoteStatus::idle) == LoginAction::back &&
        login_control_action(LoginControl::submit, LoginStatus::recovery_rate_limited, LoginRemoteStatus::idle) == LoginAction::none,
        "recovery native shared actions preserve identifier-only submit pending Back and cooldown gate");
    LoginScene scene{};
    UiTextCommand commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream stream{}; ui_text_command_stream_initialize(&stream, {commands, kMaximumLoginTextCommands});
    const LoginStatus states[]{LoginStatus::recovery_editing, LoginStatus::recovery_pending, LoginStatus::recovery_sent,
        LoginStatus::recovery_failed, LoginStatus::recovery_rate_limited, LoginStatus::recovery_captcha_required};
    const float widths[]{320.0f, 400.0f, 1000.0f};
    for (float width : widths) for (auto status : states) {
        LoginView view{}; view.visible = true; view.status = status;
        view.focus = login_recovery_editable(status) ? LoginControl::login : LoginControl::back;
        scene.recovery_mode = true; scene.back_only = login_requires_back(status, LoginRemoteStatus::idle);
        scene.recovery_disabled = login_view_recovery_disabled(view);
        scene.credential_fields_hidden = true; scene.focus = view.focus;
        scene.action_disabled = login_view_action_disabled(view); scene.submit_disabled = login_view_submit_disabled(view);
        check(login_scene_layout(&scene, width, 700) && !scene.qr_visible &&
            !login_scene_element(scene, LoginElement::password_field) &&
            login_view_matches_scene(view, &scene) && login_text_presentation_build(scene, view, kDefaultTheme, &stream),
            "recovery native editing pending terminal and cooldown scenes render within ordinary responsive widget owner");
        const auto heading = login_scene_rectangle(scene, LoginElement::heading);
        const auto subtitle = login_scene_rectangle(scene, LoginElement::subtitle);
        const auto form = login_scene_element(scene, LoginElement::form);
        const auto next = login_scene_rectangle(scene, scene.back_only ? LoginElement::auxiliary : LoginElement::login_label);
        check(form && stream.count >= 2 && ui_rect_width(heading) == ui_rect_width(form->content_rectangle) &&
            ui_rect_width(subtitle) == ui_rect_width(form->content_rectangle) && ui_rect_width(heading) >= 288 &&
            heading.bottom <= subtitle.top && ui_rect_height(subtitle) >= 44 && subtitle.bottom <= next.top &&
            ui_rect_equal(stream.storage[0].rectangle, heading) && ui_rect_equal(stream.storage[1].rectangle, subtitle) &&
            stream.storage[1].wrapping == TextWrapping::wrap,
            "recovery heading uses the content width and a wrapped two-line subtitle stays before Back or the identifier label");
    }
    const LoginStatus availability[]{LoginStatus::idle, LoginStatus::profile_save_failed, LoginStatus::idle};
    for (auto status : availability) {
        LoginView view{}; view.visible = true; view.status = status;
        scene.recovery_mode = false; scene.back_only = false; scene.focus = LoginControl::none;
        scene.credential_fields_hidden = false; scene.mfa_capabilities = 0;
        scene.action_disabled = login_view_action_disabled(view);
        scene.submit_disabled = login_view_submit_disabled(view);
        scene.recovery_disabled = login_view_recovery_disabled(view);
        const bool ready = login_scene_layout(&scene, 500, 700);
        const auto forgot = login_scene_rectangle(scene, LoginElement::forgot_password);
        const auto submit = login_scene_rectangle(scene, LoginElement::submit);
        const auto expected_control = status == LoginStatus::profile_save_failed ? LoginControl::none : LoginControl::forgot_password;
        check(ready && login_view_matches_scene(view, &scene) &&
            login_scene_hit_test(scene, forgot.left + 1, forgot.top + 1) == expected_control &&
            login_scene_hit_test(scene, submit.left + 1, submit.top + 1) == LoginControl::submit,
            "native Forgot eligibility follows actual shared action through same-layout profile-save and restored-idle changes while Retry saving stays enabled");
        check(login_scene_focus(&scene, LoginControl::forgot_password) == (expected_control != LoginControl::none),
            "native unavailable Forgot rejects focus and restored idle admits it");
    }
    login_scene_release(&scene);
}
} // namespace maicord
