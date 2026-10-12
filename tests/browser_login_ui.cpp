#include "maicord/ui/login_view.hpp"
#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
void test_browser_login_ui(void (*check)(bool, const char*) noexcept) noexcept {
    check(login_control_action(LoginControl::browser_sign_in, LoginStatus::idle, {}) ==
                  LoginAction::browser_sign_in &&
              login_control_action(LoginControl::browser_sign_in, LoginStatus::captcha_required, {}) ==
                  LoginAction::browser_sign_in &&
              login_control_action(LoginControl::browser_sign_in, LoginStatus::reset_captcha_required, {},
                                   true) == LoginAction::none &&
              login_control_action(LoginControl::browser_sign_in, LoginStatus::recovery_phone_required,
                                   {}) == LoginAction::none &&
              login_control_action(LoginControl::browser_sign_in, LoginStatus::submitting, {}) ==
                  LoginAction::none,
          "browser UI: explicit browser action admits idle/CAPTCHA alternatives and rejects reset, phone "
          "and pending owners");
    check(login_requires_back(LoginStatus::browser_waiting, {}) &&
              login_credential_fields_hidden(LoginStatus::browser_exchange_pending, {}, false) &&
              login_control_action(LoginControl::submit, LoginStatus::browser_waiting, {}) ==
                  LoginAction::none &&
              login_control_action(LoginControl::back, LoginStatus::browser_expired, {}) ==
                  LoginAction::back,
          "browser UI: browser phases keep cancellation available and prohibit credential fallback");
    LoginScene scene{};
    LoginView view{};
    view.visible = true;
    view.status = LoginStatus::idle;
    scene.browser_available = true;
    scene.focus = view.focus = LoginControl::browser_sign_in;
    check(login_scene_layout(&scene, 360, 640) && login_view_matches_scene(view, &scene) &&
              login_scene_focus(&scene, LoginControl::browser_sign_in),
          "browser UI: compact layout exposes keyboard focusable browser control");
    const auto box = login_scene_control_rectangle(scene, LoginControl::browser_sign_in);
    check(login_scene_hit_test(scene, (box.left + box.right) / 2, (box.top + box.bottom) / 2) ==
              LoginControl::browser_sign_in,
          "browser UI: browser link hit testing resolves the same action as keyboard activation");
    constexpr LoginStatus phases[]{LoginStatus::browser_starting, LoginStatus::browser_waiting,
                                   LoginStatus::browser_exchange_pending, LoginStatus::browser_expired,
                                   LoginStatus::browser_failed};
    for (auto phase : phases) {
        view.status = phase;
        view.focus = LoginControl::back;
        scene.focus = LoginControl::back;
        scene.browser_available = false;
        scene.browser_mode = true;
        scene.back_only = true;
        scene.credential_fields_hidden = true;
        scene.action_disabled = scene.submit_disabled = scene.recovery_disabled = true;
        check(login_scene_layout(&scene, 360, 640) && login_view_matches_scene(view, &scene) &&
                  !scene.qr_visible && !login_scene_element(scene, LoginElement::login_field) &&
                  !login_scene_element(scene, LoginElement::password_field),
              "browser UI: native waiting/terminal layouts expose no credentials or stale QR");
        UiTextCommand commands[kMaximumLoginTextCommands]{};
        UiTextCommandStream text{};
        check(ui_text_command_stream_initialize(&text, {commands, kMaximumLoginTextCommands}) &&
                  login_text_presentation_build(scene, view, kDefaultTheme, &text) && text.count == 4,
              "browser UI: complete bounded browser status and cancellation copy render through native "
              "text scene");
        const auto expected = phase == LoginStatus::browser_expired  ? utf8_literal("Sign-in expired")
                              : phase == LoginStatus::browser_failed ? utf8_literal("Sign-in failed")
                                                                     : utf8_literal("Waiting for your browser");
        const auto submit = scene.elements[static_cast<u32>(LoginElement::submit)];
        bool submit_label_correct = false;
        for (u32 i = 0; i < text.count; ++i)
            if (submit.generation && commands[i].owner.handle.slot == submit.slot)
                submit_label_correct = utf8_equal(commands[i].text, expected);
        check(submit_label_correct, "browser UI: terminal submit copy does not imply an expired or failed "
                                    "sign-in is still waiting");
    }
    login_scene_release(&scene);
}
} // namespace maicord
