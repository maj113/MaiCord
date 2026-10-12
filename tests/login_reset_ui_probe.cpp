#include "integration/probe_present.hpp"
#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
// Synthetic presentation only. Root owns invocation and capture review.
bool login_reset_ui_probe(D3D11Renderer& renderer,
    bool (*capture)(const wchar_t*) noexcept,
    bool (*text_pixels)(ConstSlice<UiRect>) noexcept) noexcept {
    if (!capture || !text_pixels || renderer.login_visible || renderer.content_text || !renderer.dpi) return false;
    LoginScene scene{};
    const auto width = renderer.width, height = renderer.height;
    struct Restore {
        D3D11Renderer& renderer; LoginScene& scene; u32 width, height;
        ~Restore() noexcept {
            if (renderer.login_scene == &scene) renderer_set_login_view(&renderer, nullptr, {}, kDefaultTheme);
            login_scene_release(&scene); renderer_resize(&renderer, width, height);
        }
    } restore{renderer, scene, width, height};
    const LoginStatus statuses[]{LoginStatus::reset_editing, LoginStatus::reset_pending,
        LoginStatus::mfa_required, LoginStatus::mfa_backup_acknowledgement,
        LoginStatus::webauthn_required, LoginStatus::reset_captcha_required,
        LoginStatus::profile_save_failed, LoginStatus::idle, LoginStatus::mfa_required};
    const wchar_t* names[]{L"login-reset-editing.bmp", L"login-reset-pending-compact.bmp",
        L"login-reset-mfa-sms.bmp", L"login-reset-backup-ack.bmp",
        L"login-reset-webauthn.bmp", L"login-reset-captcha-compact.bmp",
        L"login-reset-save-failed.bmp", L"login-reset-restored.bmp", L"login-reset-mfa-compact-back.bmp"};
    bool valid = true;
    for (u32 i = 0; valid && i < 9; ++i) {
        const float pixels = static_cast<float>(renderer.dpi) / 96.f;
        valid = renderer_resize(&renderer, i == 1 ? static_cast<u32>(560 * pixels) :
            i == 5 ? static_cast<u32>(400 * pixels) : i == 8 ? static_cast<u32>(320 * pixels) : width,
            i == 1 ? static_cast<u32>(480 * pixels) : i == 5 || i == 8 ? static_cast<u32>(600 * pixels) : height);
        LoginView view{}; view.visible = true; view.status = statuses[i]; view.reset_mode = i != 7;
        view.focus = i == 7 ? LoginControl::login : login_requires_back(view.status, {}) || i == 3 || i == 8 ? LoginControl::back :
            i == 6 || i == 4 ? LoginControl::submit : LoginControl::password;
        if (i == 2 || i == 3 || i == 4 || i == 8) {
            view.mfa_capabilities = login_mfa_capability_all;
            view.selected_mfa_capability = i == 2 || i == 8 ? login_mfa_capability_sms :
                i == 3 ? login_mfa_capability_backup : login_mfa_capability_webauthn;
            if (i == 2 || i == 8) view.sms_sent = true;
            if (i == 3) view.backup_acknowledgement_seconds = 10;
        }
        if (i == 0) view.password_mask = utf8_literal("************");
        scene.reset_mode = view.reset_mode; scene.recovery_mode = false;
        scene.restoring_session = false; scene.back_only = login_requires_back(view.status, {});
        scene.mfa_capabilities = view.mfa_capabilities; scene.selected_mfa_capability = view.selected_mfa_capability;
        scene.credential_fields_hidden = login_credential_fields_hidden(view.status, view.remote_status, view.reset_mode);
        scene.focus = view.focus; scene.focus_visible = true;
        scene.action_disabled = login_view_action_disabled(view); scene.submit_disabled = login_view_submit_disabled(view);
        scene.recovery_disabled = login_view_recovery_disabled(view);
        valid = valid && login_scene_layout(&scene, renderer.width / pixels, renderer.height / pixels);
        view.focus = scene.focus;
        UiTextCommand commands[kMaximumLoginTextCommands]{}; UiTextCommandStream stream{};
        valid = valid && ui_text_command_stream_initialize(&stream, {commands, kMaximumLoginTextCommands}) &&
            login_text_presentation_build(scene, view, kDefaultTheme, &stream);
        if (view.reset_mode) valid = valid && !scene.qr_visible && !login_scene_element(scene, LoginElement::login_field);
        else valid = valid && login_scene_element(scene, LoginElement::login_field);
        if (i == 6) valid = valid && scene.credential_fields_hidden &&
            !login_scene_element(scene, LoginElement::password_label) && !login_scene_element(scene, LoginElement::password_field) &&
            login_scene_focus(&scene, LoginControl::submit) && !login_scene_focus(&scene, LoginControl::password) &&
            login_control_action(LoginControl::submit, view.status, view.remote_status, true) == LoginAction::submit;
        for (u32 n = 0; valid && n < stream.count; ++n)
            valid = ui_rect_has_area(ui_rect_intersection(commands[n].rectangle, commands[n].clip));
        valid = valid && renderer_set_login_view(&renderer, &scene, view, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
        if (!valid) break;
        const LoginElement elements[]{LoginElement::heading, LoginElement::forgot_password,
            i == 7 ? LoginElement::login_field : scene.credential_fields_hidden ? LoginElement::status_message : LoginElement::password_field,
            LoginElement::submit};
        UiRect rectangles[4]{};
        for (u32 n = 0; n < 4; ++n) {
            const auto* node = login_scene_element(scene, elements[n]);
            if (!node) { valid = false; break; }
            rectangles[n] = ui_rect_intersection(node->content_rectangle, node->clip_rectangle);
            if (!ui_rect_has_area(rectangles[n])) { valid = false; break; }
        }
        // Empty code/password placeholders still have actual native text pixels.
        valid = valid && text_pixels({rectangles, 4}) && capture(names[i]);
        const auto layouts = scene.layout_passes, cache = renderer.glyph_text.cache_builds,
            edit = renderer.login_edit.builds;
        valid = valid && probe_present(&renderer, kDefaultTheme) && scene.layout_passes == layouts &&
            renderer.glyph_text.cache_builds == cache && renderer.login_edit.builds == edit;
        if (!valid) log_u64(LogLevel::error, "probe", "login_reset_failed_iteration", i);
    }
    valid = renderer_set_login_view(&renderer, nullptr, {}, kDefaultTheme) && valid;
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
        valid ? "login_reset_native_scene_text_and_repeat_passed" : "login_reset_native_scene_text_failed");
    return valid;
}
} // namespace maicord
