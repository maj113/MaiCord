#include "maicord/app/user_account_coordinator.hpp"
#include "maicord/ui/login_presentation.hpp"
#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
static_assert(static_cast<u8>(LoginStatus::idle) == 0 &&
              static_cast<u8>(LoginStatus::browser_starting) == 44 &&
              static_cast<u8>(LoginStatus::browser_failed) == 48 &&
              static_cast<u8>(LoginStatus::signed_out) == 49 &&
              static_cast<u8>(LoginStatus::signed_out_unconfirmed) == 50);
void test_account_auth_transition(void (*check)(bool, const char*) noexcept) noexcept {
    UserAccountCoordinator account{};
    account.state.phase = UserAccountCoordinatorPhase::gateway_connecting;
    u8 rest_runtime = 0;
    account.rest.runtime = &rest_runtime;
    check(!user_account_coordinator_rest(&account) && !user_account_coordinator_rest(&account, true) &&
              user_account_coordinator_startup_rest(&account) == &account.rest,
          "before READY only startup history reaches the PRIMARY lane");
    account.state.phase = UserAccountCoordinatorPhase::failed;
    check(!user_account_coordinator_startup_rest(&account), "a failed account has no startup lane");
    account.rest.runtime = nullptr;
    account.state.phase = UserAccountCoordinatorPhase::gateway_connecting;
    account.gateway.phase = static_cast<LONG>(GatewayClientPhase::ready);
    check(user_account_coordinator_pump(&account).phase == UserAccountCoordinatorPhase::running,
          "saved session becomes running only after gateway ready");
    account.gateway.close_code = 4004;
    account.gateway.error = static_cast<LONG>(GatewayClientError::session);
    account.gateway.phase = static_cast<LONG>(GatewayClientPhase::failed);
    check(user_account_coordinator_pump(&account).error ==
                  UserAccountCoordinatorError::authentication_rejected &&
              account.state.phase == UserAccountCoordinatorPhase::failed,
          "gateway authentication failure invalidates an already running session");
    account.gateway.close_code = 0;
    check(user_account_coordinator_pump(&account).error ==
              UserAccountCoordinatorError::authentication_rejected,
          "session rejection remains latched until owner teardown");
    // Failed client stop results can retain the old transport error. Fully
    // released owners must still complete coordinator cleanup.
    check(user_account_coordinator_stop(&account, 1, nullptr).phase == UserAccountCoordinatorPhase::stopped,
          "prior gateway failure does not turn completed cleanup into another stop failure");
    secure_clear_bytes(&account, sizeof(account));
    account.state.phase = UserAccountCoordinatorPhase::gateway_connecting;
    account.gateway.phase = static_cast<LONG>(GatewayClientPhase::ready);
    account.rest.authentication_rejected = 1;
    check(rest_client_status(&account.rest).authentication_rejected &&
              user_account_coordinator_pump(&account).error ==
                  UserAccountCoordinatorError::authentication_rejected,
          "REST session rejection wins over concurrently ready gateway");
    secure_clear_bytes(&account, sizeof(account));
    account.state.phase = UserAccountCoordinatorPhase::running;
    account.gateway.phase = static_cast<LONG>(GatewayClientPhase::reconnect_wait);
    account.gateway.close_code = 4009;
    check(user_account_coordinator_pump(&account).ok(),
          "resumable session timeout does not log the user out");
    account.gateway.phase = static_cast<LONG>(GatewayClientPhase::failed);
    account.gateway.close_code = 0;
    // A READY that cannot be decoded (the 18M-member guild failure) and any
    // other recoverable worker failure keep the session and UI and
    // schedule a worker restart instead of returning to the login screen.
    const GatewayClientError recoverable[] = {GatewayClientError::session, GatewayClientError::connect,
                                              GatewayClientError::timeout, GatewayClientError::memory};
    bool restarts = true;
    for (const auto error : recoverable) {
        account.state.gateway_restart_at_ms = 0;
        account.state.gateway_restart_attempt = 0;
        account.gateway.error = static_cast<LONG>(error);
        const auto pumped = user_account_coordinator_pump(&account, 1'000);
        restarts = restarts && pumped.ok() && pumped.phase == UserAccountCoordinatorPhase::running &&
                   pumped.gateway_error == error && pumped.gateway_restart_at_ms == 3'000 &&
                   !user_account_coordinator_gateway_restart_due(&account, 2'999) &&
                   user_account_coordinator_gateway_restart_due(&account, 3'000) &&
                   user_account_coordinator_pump(&account, 5'000).gateway_restart_at_ms == 3'000;
    }
    check(restarts, "recoverable Gateway failures keep the session; one restart is scheduled");
    check(user_account_coordinator_restart_delay(0) == 2'000 && user_account_coordinator_restart_delay(1) == 4'000 &&
              user_account_coordinator_restart_delay(7) == 256'000 && user_account_coordinator_restart_delay(30) == 300'000,
          "Gateway worker restarts back off from 2 s to a 5 minute cap");
    account.gateway.close_code = 4004;
    check(user_account_coordinator_pump(&account, 6'000).error == UserAccountCoordinatorError::authentication_rejected &&
              !user_account_coordinator_gateway_restart_due(&account, 10'000),
          "rejected credentials (4004) invalidate the session and clear a pending restart");
    check(!account.state.gateway_restart_at_ms,
          "credential rejection retires the previously scheduled Gateway wake");

    bool terminal_stops = true;
    constexpr UserAccountCoordinatorPhase active_phases[]{UserAccountCoordinatorPhase::gateway_connecting,
                                                          UserAccountCoordinatorPhase::running};
    for (const auto phase : active_phases) {
        for (u16 close_code = 4010; close_code <= 4014; ++close_code) {
            secure_clear_bytes(&account, sizeof(account));
            account.metadata.profile_id = 17;
            account.state.profile_id = 17;
            account.state.phase = phase;
            account.state.gateway_restart_at_ms = 3'000;
            account.state.gateway_restart_attempt = 2;
            account.gateway.phase = static_cast<LONG>(GatewayClientPhase::failed);
            account.gateway.error = static_cast<LONG>(GatewayClientError::session);
            account.gateway.close_code = close_code;
            account.gateway.native_error = 123;
            const auto stopped = user_account_coordinator_pump(&account, 2'000);
            terminal_stops =
                terminal_stops && stopped.error == UserAccountCoordinatorError::gateway &&
                stopped.phase == UserAccountCoordinatorPhase::failed && !stopped.gateway_restart_at_ms &&
                stopped.gateway_error == GatewayClientError::session && stopped.native_error == 123 &&
                account.metadata.profile_id == 17 && stopped.profile_id == 17 &&
                !rest_client_status(&account.rest).authentication_rejected &&
                !rest_client_status(&account.details).authentication_rejected &&
                !user_account_coordinator_gateway_restart_due(&account, 100'000) &&
                user_account_coordinator_pump(&account, 100'000).error ==
                    UserAccountCoordinatorError::gateway;
        }
    }
    check(terminal_stops,
          "terminal Gateway configuration closes latch failure without credential rejection or restart");

    LoginScene scene{};
    LoginView view{};
    view.visible = true;
    view.status = LoginStatus::restoring_session;
    scene.restoring_session = true;
    scene.action_disabled = login_view_action_disabled(view);
    scene.recovery_disabled = login_view_recovery_disabled(view);
    scene.submit_disabled = login_view_submit_disabled(view);
    UiTextCommand commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream text{};
    ui_text_command_stream_initialize(&text, {commands, kMaximumLoginTextCommands});
    PaintStream paint{};
    bool loading = true;
    constexpr float widths[] = {320.0f, 830.0f, 1920.0f};
    for (float width : widths) {
        loading = loading && login_scene_layout(&scene, width, 720) &&
                  login_presentation_build(scene, {}, kDefaultTheme, width, 720, 96, &paint) &&
                  paint.count == 1 && login_text_presentation_build(scene, view, kDefaultTheme, &text) &&
                  text.count == 1 &&
                  utf8_equal(text.storage[0].text, utf8_literal("Connecting to Discord…")) &&
                  login_scene_move_focus(&scene, false) == LoginControl::none;
        const auto field = login_scene_rectangle(scene, LoginElement::login_field);
        loading =
            loading && login_scene_hit_test(scene, field.left + 2, field.top + 2) == LoginControl::none;
    }
    check(
        loading,
        "session restoration paints only a neutral loading view and blocks credential input at all widths");
    view.visible = false;
    check(login_text_presentation_build(scene, view, kDefaultTheme, &text) && text.count == 0,
          "successful restoration hides all login text");
    view.visible = true;
    view.status = LoginStatus::session_expired;
    scene.restoring_session = false;
    scene.action_disabled = scene.submit_disabled = false;
    scene.recovery_disabled = login_view_recovery_disabled(view);
    login_scene_layout(&scene, 830, 720);
    login_scene_focus(&scene, LoginControl::login);
    view.focus = scene.focus;
    bool expired = login_text_presentation_build(scene, view, kDefaultTheme, &text);
    bool notice = false;
    for (u32 n = 0; n < text.count; ++n)
        notice |=
            utf8_equal(text.storage[n].text, utf8_literal("Your session expired. Log in again."));
    check(expired && notice && text.count > 1 && !login_view_submit_disabled(view),
          "expired session restores editable login with a specific explanation");
    constexpr LoginStatus signed_out_states[]{LoginStatus::signed_out, LoginStatus::signed_out_unconfirmed};
    constexpr LoginRemoteStatus remote_states[]{
        LoginRemoteStatus::idle,           LoginRemoteStatus::connecting, LoginRemoteStatus::qr_ready,
        LoginRemoteStatus::mobile_pending, LoginRemoteStatus::refreshing, LoginRemoteStatus::failed};
    constexpr const Theme* themes[]{&kDefaultTheme, &kLightTheme};
    QrCode qr{};
    qr.size = kQrCodeSize;
    bool ordinary_controls = true, neutral_outcome = true, remote_independent = true;
    u32 confirmed_commands[static_cast<u32>(LoginRemoteStatus::failed) + 1][3][2]{};
    for (auto status : signed_out_states) {
        const auto expected = status == LoginStatus::signed_out
                                  ? utf8_literal("You've been logged out.")
                                  : utf8_literal("Logged out of this device.");
        for (auto remote : remote_states) {
            view = {};
            view.visible = true;
            view.status = status;
            view.remote_status = remote;
            view.login = utf8_literal("native@example.com");
            view.password_mask = utf8_literal("****");
            view.remote_user = utf8_literal("Pending QR account");
            view.focus = LoginControl::login;
            view.qr_code =
                remote == LoginRemoteStatus::qr_ready || remote == LoginRemoteStatus::mobile_pending
                    ? &qr
                    : nullptr;
            scene.focus = view.focus;
            scene.browser_available = login_browser_available(status, remote, false);
            scene.recovery_disabled = login_view_recovery_disabled(view);
            scene.action_disabled = login_view_action_disabled(view);
            scene.submit_disabled = login_view_submit_disabled(view);
            ordinary_controls =
                ordinary_controls && login_view_valid(view) && !scene.action_disabled &&
                !scene.submit_disabled && !scene.recovery_disabled && scene.browser_available &&
                !login_browser_mode(status) && !login_phone_mode(status) && !login_recovery_mode(status) &&
                !login_requires_back(status, remote) &&
                !login_credential_fields_hidden(status, remote, false) &&
                login_control_action(LoginControl::login, status, remote) == LoginAction::submit &&
                login_control_action(LoginControl::password, status, remote) == LoginAction::submit &&
                login_control_action(LoginControl::submit, status, remote) == LoginAction::submit &&
                login_control_action(LoginControl::qr, status, remote) == LoginAction::remote_auth &&
                login_control_action(LoginControl::forgot_password, status, remote) ==
                    LoginAction::recovery &&
                login_control_action(LoginControl::browser_sign_in, status, remote) ==
                    LoginAction::browser_sign_in;
            for (u32 width_index = 0; width_index < 3; ++width_index) {
                const float width = widths[width_index];
                const bool laid_out =
                    login_scene_layout(&scene, width, 720) && login_view_matches_scene(view, &scene);
                ordinary_controls = ordinary_controls && laid_out &&
                                    login_scene_element(scene, LoginElement::login_field) &&
                                    login_scene_element(scene, LoginElement::password_field) &&
                                    login_scene_focus(&scene, LoginControl::browser_sign_in) &&
                                    login_scene_focus(&scene, LoginControl::login);
                remote_independent =
                    remote_independent && laid_out &&
                    scene.qr_visible == (width >= kDefaultLoginLayoutMetrics.qr_min_width) &&
                    (!scene.qr_visible || login_scene_focus(&scene, LoginControl::qr)) &&
                    login_scene_focus(&scene, LoginControl::login);
                for (u32 theme_index = 0; theme_index < 2; ++theme_index) {
                    const auto* theme = themes[theme_index];
                    const bool built = login_text_presentation_build(scene, view, *theme, &text);
                    bool outcome_command = false, welcome = false, password = false, browser = false;
                    u32 account_commands = 0;
                    bool account_notice = false, old_account_absent = true;
                    const auto owner = scene.elements[static_cast<u32>(LoginElement::status_message)];
                    const auto account_owner =
                        scene.elements[static_cast<u32>(LoginElement::status_account)];
                    for (u32 n = 0; built && n < text.count; ++n) {
                        const auto& command = text.storage[n];
                        if (command.owner.manager == &scene.widgets &&
                            command.owner.handle.slot == owner.slot &&
                            command.owner.handle.generation == owner.generation)
                            outcome_command = utf8_equal(command.text, expected) &&
                                              command.color.red == theme->text_secondary.red &&
                                              command.color.green == theme->text_secondary.green &&
                                              command.color.blue == theme->text_secondary.blue &&
                                              command.color.alpha == theme->text_secondary.alpha;
                        if (command.owner.manager == &scene.widgets &&
                            command.owner.handle.slot == account_owner.slot &&
                            command.owner.handle.generation == account_owner.generation) {
                            ++account_commands;
                            account_notice =
                                utf8_equal(command.text, utf8_literal("Logged out on this device only.")) &&
                                command.color.red == theme->text_secondary.red &&
                                command.color.green == theme->text_secondary.green &&
                                command.color.blue == theme->text_secondary.blue &&
                                command.color.alpha == theme->text_secondary.alpha;
                        }
                        old_account_absent =
                            old_account_absent && !utf8_equal(command.text, view.remote_user);
                        welcome = welcome || utf8_equal(command.text, utf8_literal("Log in to MaiCord"));
                        password = password || utf8_equal(command.text, view.password_mask);
                        browser = browser || utf8_equal(command.text, utf8_literal("Use browser"));
                    }
                    const u32 remote_index = static_cast<u32>(remote);
                    if (status == LoginStatus::signed_out)
                        confirmed_commands[remote_index][width_index][theme_index] = text.count;
                    const bool account_outcome =
                        status == LoginStatus::signed_out
                            ? account_commands == 0
                            : account_commands == 1 && account_notice &&
                                  text.count ==
                                      confirmed_commands[remote_index][width_index][theme_index] + 1;
                    neutral_outcome = neutral_outcome && built && outcome_command && account_outcome &&
                                      old_account_absent && welcome && password && browser;
                }
            }
        }
    }
    check(ordinary_controls, "signed-out outcomes retain ordinary editable credentials, recovery and "
                             "explicit browser actions at all widths");
    check(
        remote_independent,
        "signed-out outcomes preserve independent QR visibility and promotion through fresh remote phases");
    check(neutral_outcome,
          "signed-out truthful neutral status copy wins over QR progress and network error in both themes");
    bool rate_controls = true, rate_copy = true;
    constexpr LoginStatus rate_states[]{LoginStatus::rejected, LoginStatus::mfa_code_rejected,
                                        LoginStatus::captcha_required};
    for (const auto status : rate_states) {
        for (u32 unknown = 0; unknown < 2; ++unknown) {
            LoginView limited{};
            limited.visible = true;
            limited.status = status;
            limited.auth_rate_limited = true;
            limited.auth_retry_time_unknown = unknown != 0;
            if (status == LoginStatus::mfa_code_rejected) {
                limited.mfa_capabilities = limited.selected_mfa_capability = login_mfa_capability_totp;
                limited.password_mask = utf8_literal("123456");
                limited.mfa_code_ready = true;
            }
            LoginScene paused{};
            paused.back_only = login_requires_back(status, limited.remote_status);
            paused.credential_fields_hidden =
                login_credential_fields_hidden(status, limited.remote_status, false);
            paused.mfa_capabilities = limited.mfa_capabilities;
            paused.selected_mfa_capability = limited.selected_mfa_capability;
            paused.action_disabled = login_view_action_disabled(limited);
            paused.submit_disabled = login_view_submit_disabled(limited);
            paused.auth_retry_resume = login_view_auth_retry_resume(limited);
            paused.captcha_action = login_view_captcha_action(limited);
            paused.recovery_disabled = login_view_recovery_disabled(limited);
            for (float width : widths) {
                rate_controls = rate_controls && login_scene_layout(&paused, width, 720);
                limited.focus = paused.focus;
                const auto submit = login_scene_rectangle(paused, LoginElement::submit);
                rate_controls =
                    rate_controls && login_view_valid(limited) && paused.submit_disabled == !unknown &&
                    !paused.captcha_action && login_view_matches_scene(limited, &paused) &&
                    login_scene_hit_test(paused, (submit.left + submit.right) * 0.5f,
                                         (submit.top + submit.bottom) * 0.5f) ==
                        (unknown ? LoginControl::submit : LoginControl::none);
                for (const auto* theme : themes) {
                    bool label = false, explanation = false;
                    const bool built = login_text_presentation_build(paused, limited, *theme, &text);
                    for (u32 index = 0; index < text.count; ++index) {
                        label |=
                            utf8_equal(text.storage[index].text, unknown ? utf8_literal("Resume sign-in")
                                                                         : utf8_literal("Please wait"));
                        explanation |=
                            utf8_equal(text.storage[index].text,
                                       unknown ? utf8_literal("No retry time was provided. Resume when ready.")
                                               : utf8_literal("Too many attempts. Wait before retrying."));
                    }
                    rate_copy = rate_copy && built && label && explanation;
                }
            }
            login_scene_release(&paused);
        }
    }
    check(rate_controls,
          "actor cooldown disables native password MFA and CAPTCHA submission at all widths");
    check(rate_copy, "actor cooldown and missing retry time have truthful semantic copy in both themes");
    constexpr LoginStatus busy_states[]{LoginStatus::submitting, LoginStatus::cancelling,
        LoginStatus::reset_mfa_pending, LoginStatus::browser_exchange_pending};
    bool busy_paused = true;
    for (auto status : busy_states) {
        LoginView busy{};
        busy.status = status;
        busy.auth_rate_limited = busy.auth_retry_time_unknown = true;
        busy_paused = busy_paused && !login_view_auth_retry_resume(busy) && login_view_submit_disabled(busy);
    }
    check(busy_paused, "unknown retry Resume is unavailable while authentication or cancellation owns pending work");
    login_scene_release(&scene);
}
} // namespace maicord
