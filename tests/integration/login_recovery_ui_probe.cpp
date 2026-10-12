#include "probe_present.hpp"
#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
namespace {
bool recovery_geometry(const LoginScene& scene, const LoginView& view, u32* stage) noexcept {
    const u32 base = *stage;
    *stage = base + 1;
    if (!login_view_matches_scene(view, &scene) || scene.qr_visible || !scene.credential_fields_hidden ||
        view.password_mask.bytes || view.qr_code || !scene.focus_visible) return false;
    const UiRect back = login_scene_control_rectangle(scene, LoginControl::back);
    *stage = base + 2;
    if (!ui_rect_has_area(back) || login_scene_hit_test(scene, (back.left + back.right) * .5f,
        (back.top + back.bottom) * .5f) != LoginControl::back ||
        login_control_action(LoginControl::back, view.status, view.remote_status) != LoginAction::back) return false;
    const bool editing = view.status == LoginStatus::recovery_editing;
    // A recovery CAPTCHA offers only the browser check beside Back.
    const bool captcha = login_captcha_mode(view.status, view.remote_status);
    *stage = base + 3;
    if (scene.focus != (editing ? LoginControl::login : LoginControl::back) ||
        scene.submit_disabled == (editing || captcha) || login_control_action(LoginControl::submit, view.status, view.remote_status) !=
            (editing ? LoginAction::recovery_submit : captcha ? LoginAction::captcha_verify : LoginAction::none)) return false;
    if (editing) {
        *stage = base + 4;
        const auto field = login_scene_control_rectangle(scene, LoginControl::login);
        if (!ui_rect_has_area(field) || login_scene_hit_test(scene, (field.left + field.right) * .5f,
            (field.top + field.bottom) * .5f) != LoginControl::login) return false;
    }
    UiTextCommand commands[kMaximumLoginTextCommands]{}; UiTextCommandStream text{};
    *stage = base + 5;
    if (!ui_text_command_stream_initialize(&text, {commands, kMaximumLoginTextCommands}) ||
        !login_text_presentation_build(scene, view, kDefaultTheme, &text)) return false;
    bool heading = false, back_text = false, status = editing;
    for (u32 i = 0; i < text.count; ++i) {
        *stage = base + 6;
        const auto& command = commands[i];
        if (!ui_rect_has_area(ui_rect_intersection(command.rectangle, command.clip))) {
            log_u64(LogLevel::error, "probe", "login_recovery_clipped_command", i);
            log_u64(LogLevel::error, "probe", "login_recovery_clipped_owner_slot", command.owner.handle.slot);
            return false;
        }
        *stage = base + 7;
        if (command.owner.handle.slot == scene.elements[static_cast<u32>(LoginElement::password_field)].slot ||
            command.owner.handle.slot == scene.elements[static_cast<u32>(LoginElement::password_label)].slot ||
            command.owner.handle.slot == scene.elements[static_cast<u32>(LoginElement::qr_title)].slot ||
            command.owner.handle.slot == scene.elements[static_cast<u32>(LoginElement::qr_subtitle)].slot) return false;
        heading |= utf8_equal(command.text, utf8_literal("Password recovery"));
        back_text |= utf8_equal(command.text, utf8_literal("Back to sign in"));
        if (command.owner.handle.slot == scene.elements[static_cast<u32>(LoginElement::status_message)].slot && command.text.bytes)
            status = command.wrapping == UiTextWrapping::wrap && ui_rect_width(command.rectangle) > 240;
    }
    *stage = base + 8;
    if (!heading || !back_text || !status) {
        log_u64(LogLevel::error, "probe", "login_recovery_heading_found", heading);
        log_u64(LogLevel::error, "probe", "login_recovery_back_found", back_text);
        log_u64(LogLevel::error, "probe", "login_recovery_status_readable", status);
    }
    return heading && back_text && status;
}
}

// Presentation fixture only: no account, worker, provider, credential or REST
// owner. Call after hiding ordinary login, before account text storage binds.
bool login_recovery_ui_probe(D3D11Renderer& renderer,
    bool (*capture)(const wchar_t*) noexcept,
    bool (*text_pixels)(ConstSlice<UiRect>) noexcept) noexcept {
    if (!capture || !text_pixels || renderer.login_visible || renderer.content_text || !renderer.dpi ||
        (renderer.popup_scene && renderer.popup_scene->active)) return false;
    LoginScene scene{};
    const u32 width = renderer.width, height = renderer.height;
    struct Restore {
        D3D11Renderer& renderer; LoginScene& scene; u32 width, height;
        ~Restore() noexcept { if (renderer.login_scene == &scene)
                renderer_set_login_view(&renderer, nullptr, {}, kDefaultTheme);
            login_scene_release(&scene); renderer_resize(&renderer, width, height); }
    } restore{renderer, scene, width, height};
    const LoginStatus statuses[] = {LoginStatus::recovery_editing, LoginStatus::recovery_pending,
        LoginStatus::recovery_sent, LoginStatus::recovery_captcha_required};
    const wchar_t* names[] = {L"login-recovery-editing.bmp", L"login-recovery-pending-compact.bmp",
        L"login-recovery-sent.bmp", L"login-recovery-captcha-compact.bmp"};
    bool valid = true; u32 stage = 0, iteration = 0;
    for (u32 i = 0; valid && i < 4; ++i) {
        iteration = i;
        stage = 100 * i + 10;
        const float pixels = static_cast<float>(renderer.dpi) / 96.f;
        valid = renderer_resize(&renderer, i == 1 ? static_cast<u32>(640 * pixels) :
            i == 3 ? static_cast<u32>(400 * pixels) : width,
            i == 1 ? static_cast<u32>(480 * pixels) : i == 3 ? static_cast<u32>(600 * pixels) : height);
        LoginView view{};
        view.login = utf8_literal("native.recovery@example.test");
        view.status = statuses[i]; view.visible = true;
        view.focus = i == 0 ? LoginControl::login : LoginControl::back;
        scene.restoring_session = false;
        scene.back_only = login_requires_back(view.status, view.remote_status);
        scene.recovery_mode = true; scene.credential_fields_hidden = true;
        scene.mfa_capabilities = 0; scene.selected_mfa_capability = 0;
        scene.focus = view.focus; scene.focus_visible = true;
        scene.action_disabled = login_view_action_disabled(view);
        scene.recovery_disabled = login_view_recovery_disabled(view);
        scene.submit_disabled = login_view_submit_disabled(view);
        scene.captcha_action = login_view_captcha_action(view);
        if (!valid) break;
        stage = 100 * i + 20;
        valid = login_scene_layout(&scene, renderer.width / pixels, renderer.height / pixels);
        if (!valid) break;
        view.focus = scene.focus;
        stage = 100 * i + 30;
        valid = recovery_geometry(scene, view, &stage);
        if (!valid) break;
        stage = 100 * i + 40;
        valid = renderer_set_login_view(&renderer, &scene, view, kDefaultTheme);
        if (!valid) break;
        stage = 100 * i + 50;
        valid = probe_present(&renderer, kDefaultTheme);
        if (!valid) break;
        stage = 100 * i + 51;
        valid = renderer.login_visible && renderer.login_scene == &scene && !renderer.content_text;
        if (!valid) break;
        const LoginElement elements[] = {LoginElement::heading, LoginElement::forgot_password,
            i == 0 ? LoginElement::login_field : LoginElement::status_message, LoginElement::submit};
        UiRect rectangles[4]{};
        for (u32 n = 0; n < 4; ++n) {
            const auto* node = login_scene_element(scene, elements[n]);
            if (!node) { valid = false; break; }
            rectangles[n] = ui_rect_intersection(node->content_rectangle, node->clip_rectangle);
            if (!ui_rect_has_area(rectangles[n])) { valid = false; break; }
        }
        stage = 100 * i + 52;
        valid = valid && text_pixels({rectangles, 4});
        if (!valid) break;
        stage = 100 * i + 60;
        valid = capture(names[i]);
        if (!valid) break;
        const u32 builds = renderer.paint_stream_builds;
        const u32 quads = renderer.glyph_text.title_quad_count;
        const u32 cache_builds = renderer.glyph_text.cache_builds;
        const u32 font_faces = renderer.glyph_text.font_face_count;
        const u32 edit_builds = renderer.login_edit.builds;
        const u32 layout_passes = scene.layout_passes;
        stage = 100 * i + 70;
        valid = probe_present(&renderer, kDefaultTheme);
        if (!valid) break;
        // Login intentionally rebuilds paint every present. Static text and
        // edit layouts are already materialized by renderer_set_login_view;
        // a repeated present must not replace those unchanged owners.
        log_u64(LogLevel::info, "probe", "login_recovery_repeat_iteration", i);
        log_u64(LogLevel::info, "probe", "login_recovery_repeat_paint_build_delta", renderer.paint_stream_builds - builds);
        stage = 100 * i + 71;
        valid = quads != 0 && renderer.glyph_text.title_quad_count == quads &&
            renderer.glyph_text.cache_builds == cache_builds && renderer.glyph_text.font_face_count == font_faces &&
            renderer.login_edit.builds == edit_builds && scene.layout_passes == layout_passes;
        if (!valid) break;
        stage = 100 * i + 72;
        valid = renderer.login_visible && renderer.login_scene == &scene && !renderer.content_text &&
            text_pixels({rectangles, 4});
    }
    const bool hidden = renderer_set_login_view(&renderer, nullptr, {}, kDefaultTheme);
    // No account/navigation scenes are admitted yet. Restore hidden login and
    // dimensions; the caller initializes account content before its next draw.
    const bool restored = hidden && renderer_resize(&renderer, width, height) &&
        !renderer.login_visible && !renderer.login_scene && !renderer.content_text;
    if (valid && !restored) stage = 900;
    valid = restored && valid;
    if (!valid) {
        log_u64(LogLevel::error, "probe", "login_recovery_failure_iteration", iteration);
        log_u64(LogLevel::error, "probe", "login_recovery_failure_stage", stage);
    }
    log_u64(LogLevel::info, "probe", "login_recovery_dpi", renderer.dpi);
    log_write(valid ? LogLevel::info : LogLevel::error, "probe", valid ?
        "login_recovery_projection_focus_clipping_and_static_text_repeat_passed" : "login_recovery_hardware_failed");
    return valid;
}
} // namespace maicord
