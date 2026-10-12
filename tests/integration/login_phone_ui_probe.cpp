#include "probe_present.hpp"
#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
// Synthetic presentation only; no account, transport, provider, credential or
// recovery owner. Root invokes captures and reviews the rendered pixels.
bool login_phone_ui_probe(D3D11Renderer& renderer,
    bool (*capture)(const wchar_t*) noexcept,
    bool (*text_pixels)(ConstSlice<UiRect>) noexcept) noexcept {
    if (!capture || !text_pixels || renderer.login_visible || renderer.content_text || !renderer.dpi) return false;
    LoginScene scene{}; const u32 width = renderer.width, height = renderer.height;
    struct Restore {
        D3D11Renderer& renderer; LoginScene& scene; u32 width, height;
        ~Restore() noexcept {
            if (renderer.login_scene == &scene) renderer_set_login_view(&renderer, nullptr, {}, kDefaultTheme);
            login_scene_release(&scene); renderer_resize(&renderer, width, height);
        }
    } restore{renderer, scene, width, height};
    const LoginStatus statuses[]{LoginStatus::recovery_phone_required, LoginStatus::recovery_phone_required,
        LoginStatus::recovery_phone_verify_pending, LoginStatus::recovery_phone_resend_pending,
        LoginStatus::recovery_phone_resent, LoginStatus::recovery_phone_verify_rate_limited,
        LoginStatus::recovery_phone_code_rejected, LoginStatus::recovery_phone_captcha_required, LoginStatus::reset_editing};
    const wchar_t* names[]{L"login-phone-code-sparse.bmp", L"login-phone-six-cells-selection.bmp",
        L"login-phone-verify-pending-compact.bmp", L"login-phone-resending-compact.bmp", L"login-phone-resend-accepted.bmp",
        L"login-phone-cooldown.bmp", L"login-phone-code-rejected-compact.bmp", L"login-phone-captcha.bmp", L"login-phone-reset-transition.bmp"};
    bool valid = true; u32 failed_iteration = 0, stage = 0;
    for (u32 i = 0; valid && i < 9; ++i) {
        failed_iteration = i; stage = 10;
        const float pixels = static_cast<float>(renderer.dpi) / 96.0f;
        const u32 compact_width = i == 3 ? 320 : 400;
        valid = renderer_resize(&renderer, i == 2 || i == 3 || i == 6 ? static_cast<u32>(compact_width * pixels) : width,
            i == 2 || i == 3 || i == 6 ? static_cast<u32>(600 * pixels) : height);
        LoginView view{}; view.visible = true; view.status = statuses[i]; view.reset_mode = i == 8;
        view.password_mask = i == 0 ? utf8_literal("1 3 5 ") : i == 6 || i == 8 ? Utf8View{} : utf8_literal("123456");
        view.phone_code_ready = login_phone_editable(view.status) && login_phone_code_valid(view.password_mask);
        view.edit_active = i < 2;
        view.edit_anchor = i == 1 ? 2 : i == 0 ? 5 : 3; view.edit_caret = i == 1 ? 4 : i == 0 ? 5 : 3;
        TextCompositionSpan sparse_composition{};
        if (i == 0) {
            sparse_composition.start = 2; sparse_composition.end = 5;
            sparse_composition.line_style = TextCompositionLineStyle::solid;
            sparse_composition.attribute = TextCompositionAttribute::input;
            view.edit_composition_active = view.edit_composition_complete = true;
            view.edit_composition = {&sparse_composition, 1};
        }
        view.focus = i == 8 ? LoginControl::password : login_phone_editable(view.status) ? LoginControl::phone_code : LoginControl::back;
        scene.phone_mode = login_phone_mode(view.status); scene.reset_mode = view.reset_mode;
        scene.recovery_mode = false; scene.back_only = login_requires_back(view.status, {});
        scene.credential_fields_hidden = login_credential_fields_hidden(view.status, {}, view.reset_mode);
        scene.phone_resend_disabled = scene.phone_mode && !login_phone_editable(view.status);
        scene.recovery_disabled = login_view_recovery_disabled(view);
        scene.action_disabled = login_view_action_disabled(view); scene.submit_disabled = login_view_submit_disabled(view);
        scene.focus = view.focus; scene.focus_visible = true;
        stage = 20;
        valid = valid && login_scene_layout(&scene, renderer.width / pixels, renderer.height / pixels);
        view.focus = scene.focus;
        stage = 30;
        valid = valid && login_view_matches_scene(view, &scene) && !scene.qr_visible &&
            !login_scene_element(scene, LoginElement::login_field) && scene.widgets.count <= kMaximumLoginWidgets;
        UiTextCommand commands[kMaximumLoginTextCommands]{}; UiTextCommandStream text{};
        valid = valid && ui_text_command_stream_initialize(&text, {commands, kMaximumLoginTextCommands}) &&
            login_text_presentation_build(scene, view, kDefaultTheme, &text);
        for (u32 n = 0; valid && n < text.count; ++n) valid = ui_rect_has_area(ui_rect_intersection(commands[n].rectangle, commands[n].clip));
        stage = 40;
        valid = valid && renderer_set_login_view(&renderer, &scene, view, kDefaultTheme);
        if (valid && view.edit_active) {
            const auto field = login_scene_content_rectangle(scene, LoginElement::password_field);
            const auto expected = login_phone_caret_rectangle(field, view.edit_caret);
            const auto& caret = renderer.login_edit.caret;
            valid = caret.left == expected.left && caret.top == expected.top && caret.width == ui_rect_width(expected) &&
                caret.height == ui_rect_height(expected) && renderer.login_edit.selection_count == (i == 1 ? 2u : 0u);
            for (u32 cell = 0; valid && cell < view.password_mask.bytes; ++cell) {
                const auto rectangle = login_phone_cell_rectangle(field, cell); u32 position = ~u32{0};
                valid = renderer_login_edit_hit_test(renderer, field, view.password_mask,
                    static_cast<i32>((rectangle.left + 2) * pixels), static_cast<i32>((rectangle.top + 20) * pixels), &position) && position == cell;
            }
            if (i == 0) {
                valid = valid && !view.phone_code_ready && renderer.login_composition_metric_count == 3;
                for (u32 n = 0; valid && n < 3; ++n) {
                    const auto expected_cell = login_phone_cell_rectangle(field, n + 2);
                    const auto& metric = renderer.login_composition_metrics[n].metric;
                    valid = metric.left == expected_cell.left && metric.top == expected_cell.top &&
                        metric.width == ui_rect_width(expected_cell) && metric.height == ui_rect_height(expected_cell);
                }
            }
        }
        stage = 50;
        valid = valid && probe_present(&renderer, kDefaultTheme);
        if (!valid) break;
        UiRect rectangles[4]{};
        const LoginElement elements[]{LoginElement::heading, LoginElement::forgot_password, LoginElement::password_label, LoginElement::submit};
        for (u32 n = 0; n < 4; ++n) {
            const auto* element = login_scene_element(scene, elements[n]);
            if (!element) { valid = false; break; }
            rectangles[n] = ui_rect_intersection(element->content_rectangle, element->clip_rectangle);
        }
        stage = 60;
        valid = valid && text_pixels({rectangles, 4}) && capture(names[i]);
        const u32 cache = renderer.glyph_text.cache_builds, edits = renderer.login_edit.builds, layouts = scene.layout_passes;
        stage = 70;
        valid = valid && probe_present(&renderer, kDefaultTheme) && cache == renderer.glyph_text.cache_builds &&
            edits == renderer.login_edit.builds && layouts == scene.layout_passes;
    }
    valid = renderer_set_login_view(&renderer, nullptr, {}, kDefaultTheme) && valid;
    if (!valid) { log_u64(LogLevel::error, "probe", "login_phone_failed_iteration", failed_iteration); log_u64(LogLevel::error, "probe", "login_phone_failed_stage", stage); }
    log_write(valid ? LogLevel::info : LogLevel::error, "probe", valid ?
        "login_phone_native_cells_actions_geometry_repeat_and_reset_transition_passed" : "login_phone_native_ui_failed");
    return valid;
}
} // namespace maicord
