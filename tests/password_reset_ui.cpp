#include "maicord/ui/login_view.hpp"
#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/ui/login_presentation.hpp"

namespace maicord {
void test_password_reset_ui(void (*check)(bool, const char*) noexcept) noexcept {
    const LoginStatus states[]{LoginStatus::reset_editing, LoginStatus::reset_pending,
        LoginStatus::reset_rejected, LoginStatus::reset_rate_limited, LoginStatus::reset_failed,
        LoginStatus::reset_captcha_required, LoginStatus::reset_mfa_pending, LoginStatus::mfa_required,
        LoginStatus::mfa_code_rejected, LoginStatus::mfa_backup_acknowledgement,
        LoginStatus::webauthn_required, LoginStatus::webauthn_failed, LoginStatus::profile_save_failed, LoginStatus::submitting};
    const float widths[]{320.f, 400.f, 560.f, 1000.f};
    const float heights[]{480.f, 700.f};
    LoginScene scene{};
    PaintStream paint{};
    check(paint_stream_initialize(&paint), "reset native paint owner initializes");
    UiTextCommand commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream stream{};
    check(ui_text_command_stream_initialize(&stream, {commands, kMaximumLoginTextCommands}),
        "reset native text command owner initializes");
    for (float width : widths) for (float height : heights) for (auto status : states) {
        LoginView view{}; view.visible = true; view.reset_mode = true; view.status = status;
        view.focus = login_requires_back(status, {}) ? LoginControl::back : status == LoginStatus::profile_save_failed ? LoginControl::submit : LoginControl::password;
        const bool mfa = status == LoginStatus::mfa_required || status == LoginStatus::mfa_code_rejected ||
            status == LoginStatus::mfa_backup_acknowledgement || status == LoginStatus::webauthn_required ||
            status == LoginStatus::webauthn_failed;
        if (mfa) {
            view.mfa_capabilities = login_mfa_capability_all;
            view.selected_mfa_capability = status == LoginStatus::mfa_backup_acknowledgement ? login_mfa_capability_backup :
                status == LoginStatus::webauthn_required || status == LoginStatus::webauthn_failed ? login_mfa_capability_webauthn : login_mfa_capability_totp;
        }
        if (status == LoginStatus::mfa_backup_acknowledgement) {
            view.backup_acknowledgement_seconds = 10; view.focus = LoginControl::back;
        }
        scene.reset_mode = true; scene.recovery_mode = false;
        scene.back_only = login_requires_back(status, {}); scene.focus = view.focus;
        scene.mfa_capabilities = view.mfa_capabilities; scene.selected_mfa_capability = view.selected_mfa_capability;
        scene.credential_fields_hidden = login_credential_fields_hidden(view.status, view.remote_status, view.reset_mode);
        scene.recovery_disabled = true; scene.action_disabled = login_view_action_disabled(view);
        scene.submit_disabled = login_view_submit_disabled(view);
        check(login_scene_layout(&scene, width, height), "reset all responsive native states lay out");
        view.focus = scene.focus;
        check(!scene.qr_visible && !login_scene_element(scene, LoginElement::login_field) &&
            !login_scene_element(scene, LoginElement::login_label) && scene.widgets.count <= kMaximumLoginWidgets &&
            login_view_matches_scene(view, &scene) && login_text_presentation_build(scene, view, kDefaultTheme, &stream),
            "reset presentation hides identifier and QR within fixed native widget budget");
        const auto back = login_scene_rectangle(scene, LoginElement::forgot_password);
        check(ui_rect_has_area(back) && login_scene_hit_test(scene, (back.left + back.right) / 2,
            (back.top + back.bottom) / 2) == LoginControl::back && login_scene_focus(&scene, LoginControl::back),
            "reset Back remains a native focusable semantic control through every MFA and terminal state");
        bool secret_free = true, heading = false, back_copy = false;
        for (u32 i = 0; i < stream.count; ++i) {
            const auto& command = commands[i];
            secret_free &= command.owner.handle.slot != scene.elements[static_cast<u32>(LoginElement::login_field)].slot &&
                command.owner.handle.slot != scene.elements[static_cast<u32>(LoginElement::qr_title)].slot &&
                ui_rect_has_area(ui_rect_intersection(command.rectangle, command.clip));
            heading |= utf8_equal(command.text, utf8_literal("Reset your password"));
            back_copy |= utf8_equal(command.text, utf8_literal("Back to sign in"));
        }
        check(secret_free && heading && back_copy, "reset native text includes clear heading and Back with no token-bearing field");
        if (status == LoginStatus::profile_save_failed || status == LoginStatus::submitting) {
            bool password_copy = false, retry_copy = false, saving_copy = false;
            for (u32 i = 0; i < stream.count; ++i) {
                password_copy |= utf8_equal(commands[i].text, utf8_literal("NEW PASSWORD")) ||
                    utf8_equal(commands[i].text, utf8_literal("New password"));
                retry_copy |= utf8_equal(commands[i].text, utf8_literal("Try again"));
                saving_copy |= utf8_equal(commands[i].text, utf8_literal("Signing in…"));
            }
            check(scene.credential_fields_hidden && !login_scene_element(scene, LoginElement::password_field) &&
                !login_scene_element(scene, LoginElement::password_label) && !login_scene_focus(&scene, LoginControl::password) &&
                !password_copy && (status != LoginStatus::profile_save_failed || retry_copy) &&
                (status != LoginStatus::submitting || saving_copy),
                "reset save-only states exclude password widgets input focus and password copy while preserving Try again");
            check(login_control_action(LoginControl::password, status, {}, true) == LoginAction::none,
                "reset save-only dispatch rejects a stale password-field keyboard or mouse activation");
            if (status == LoginStatus::profile_save_failed) check(login_scene_focus(&scene, LoginControl::back) &&
                login_scene_move_focus(&scene, false) == LoginControl::submit &&
                login_scene_move_focus(&scene, true) == LoginControl::back &&
                login_control_action(LoginControl::submit, status, {}, true) == LoginAction::submit,
                "reset save-only native Tab traverses Back and Try again without a credential edit");
        }
        if (mfa && !scene.back_only) for (const auto& method : kLoginMfaPresentationMethods)
            check(login_scene_element(scene, method.element) && login_scene_focus(&scene, method.control),
                "reset chooser retains all four advertised native MFA methods");
        if (mfa && !scene.back_only) {
            check(login_scene_focus(&scene, LoginControl::mfa_webauthn), "reset native KEY begins visible method-row focus order");
            const LoginControl forward[]{LoginControl::mfa_totp, LoginControl::mfa_backup, LoginControl::mfa_sms, LoginControl::back};
            for (auto control : forward) check(login_scene_move_focus(&scene, false) == control,
                "reset Tab traverses KEY TOTP BACKUP SMS then the separate Back row in visual order");
            check(login_scene_move_focus(&scene, true) == LoginControl::mfa_sms,
                "reset Shift+Tab returns from Back to the last visible method");
            for (const auto& method : kLoginMfaPresentationMethods)
                check(ui_rect_width(login_scene_rectangle(scene, method.element)) >= 60.f,
                    "compact reset all-four method buttons retain enough width for their full labels");
        }
        check(login_scene_focus(&scene, LoginControl::back), "reset Back can regain native focus after method traversal");
        scene.focus_visible = true;
        const bool painted = login_presentation_build(scene, {}, kDefaultTheme, width, height, 96, &paint);
        bool back_border = false;
        for (u32 i = 0; painted && i < paint.count; ++i) {
            const auto& quad = paint.quads[i];
            back_border |= quad.left == static_cast<float>(static_cast<i32>(back.left)) &&
                quad.top == static_cast<float>(static_cast<i32>(back.top)) &&
                quad.right == static_cast<float>(static_cast<i32>(back.right)) &&
                quad.bottom == static_cast<float>(static_cast<i32>(back.bottom)) &&
                quad.border_width == 1.f && quad.border.red == kDefaultTheme.accent.red;
        }
        check(painted && back_border, "reset editing MFA and terminal Back draw a visible native keyboard focus border");
        check(login_control_action(LoginControl::back, status, {}, true) == LoginAction::back &&
            login_control_action(LoginControl::login, status, {}, true) == LoginAction::none &&
            login_control_action(LoginControl::qr, status, {}, true) == LoginAction::none &&
            login_control_action(LoginControl::forgot_password, status, {}, true) == LoginAction::none,
            "reset mouse and keyboard dispatch share Back and suppress ordinary login actions");
        // A CAPTCHA only offers the browser check; it never replays the reset itself.
        if (scene.back_only) check(login_control_action(LoginControl::submit, status, {}, true) ==
                (status == LoginStatus::reset_captcha_required ? LoginAction::captcha_verify : LoginAction::none),
            "reset pending ambiguous rate-limited and CAPTCHA states cannot replay on Enter or Space");
        const auto rebuilds = scene.layout_rebuilds;
        check(login_scene_layout(&scene, width, height) && scene.layout_rebuilds == rebuilds,
            "unchanged reset scene retains layout owner and avoids resident rebuilds");
    }
    LoginView unsafe{}; unsafe.reset_mode = true; unsafe.login = utf8_literal("hidden-reset-token");
    check(!login_view_valid(unsafe), "reset presentation policy refuses raw token-bearing identifier text");
    scene.reset_mode = false; scene.back_only = false; scene.credential_fields_hidden = false;
    scene.mfa_capabilities = scene.selected_mfa_capability = 0; scene.action_disabled = scene.submit_disabled = false;
    scene.recovery_disabled = false; scene.focus = LoginControl::login;
    check(login_scene_layout(&scene, 1000, 700) && scene.qr_visible &&
        login_scene_element(scene, LoginElement::login_field) && login_scene_focus(&scene, LoginControl::login),
        "native reset teardown restores ordinary identifier QR and keyboard focus structure");
    login_scene_release(&scene);
    paint_stream_release(&paint);
}
} // namespace maicord
