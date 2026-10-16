#include "application_login_ui.hpp"
#include "application_login_input.hpp"
#include "application_composer_text_input.hpp"
#include "application_login_geometry.hpp"
#include "application_login_auth.hpp"

#include "application_state.hpp"

#include "maicord/app/login_mfa_policy.hpp"
#include "maicord/core/log.hpp"

namespace maicord::app_detail {

namespace {

SecureTextInputScope active_input_scope(const Application& application, const SecureEdit* edit) noexcept {
    if (edit == &application.login.login) {
        return SecureTextInputScope::login_name;
    }
    if (edit == &application.login.mfa) {
        return SecureTextInputScope::digits;
    }
    return SecureTextInputScope::text;
}

bool text_store_geometry(void*, SecureTextStoreGeometry* geometry) noexcept {
    Application& application = g_app;
    const SecureEdit* edit = application_active_login_edit();
    if (!geometry || !edit || !application.window || !application.renderer_ready ||
        application.renderer.dpi == 0) {
        return false;
    }
    const LoginElement element =
        edit == &application.login.login ? LoginElement::login_field : LoginElement::password_field;
    const UiRect logical = login_scene_content_rectangle(application.login_scene, element);
    if (!ui_rect_has_area(logical)) {
        return false;
    }
    const float scale = static_cast<float>(application.renderer.dpi) / 96.0f;
    RECT text{static_cast<LONG>(logical.left * scale + 0.5f), static_cast<LONG>(logical.top * scale + 0.5f),
              static_cast<LONG>(logical.right * scale + 0.5f),
              static_cast<LONG>(logical.bottom * scale + 0.5f)};
    RECT window{};
    POINT origin{};
    if (!GetClientRect(application.window, &window) || !ClientToScreen(application.window, &origin)) {
        return false;
    }
    geometry->text_screen = {text.left + origin.x, text.top + origin.y,
                             text.right + origin.x, text.bottom + origin.y};
    geometry->window_screen = {window.left + origin.x, window.top + origin.y,
                               window.right + origin.x, window.bottom + origin.y};
    return true;
}

bool text_store_point(void*, POINT point, bool round, u32* position, bool* inside) noexcept {
    const auto* edit = application_active_login_edit();
    if (!edit || edit->password || !g_app.renderer.dpi ||
        !ScreenToClient(g_app.window, &point)) return false;
    char visible[48*4]{};
    u32 bytes = 0, relative = 0, first = 0;
    SecureEditRenderWindow window{};
    const auto& layout = g_app.renderer.login_edit;
    const float scale = 96.0f / g_app.renderer.dpi;
    if (login_phone_mode(g_app.login.status)) {
        const auto* node = login_scene_element(g_app.login_scene, LoginElement::password_field);
        if (!node || !login_phone_point_position(node->content_rectangle, edit->storage.bytes,
            point.x * scale, point.y * scale, round, position, inside)) return false;
        const auto clip = node->clip_rectangle;
        *inside = *inside && point.x * scale >= clip.left && point.x * scale <= clip.right &&
            point.y * scale >= clip.top && point.y * scale <= clip.bottom;
        return true;
    }
    const bool valid = secure_edit_render_window(*edit, 48, {visible, sizeof(visible)}, &bytes, &window) ==
            SecureEditResult::unchanged &&
        layout.text_signature == text_edit_signature({visible, bytes}) &&
        text_edit_layout_point_geometry(layout, point.x*scale, point.y*scale, round, &relative, inside) &&
        utf8_utf16_position_for_codepoint(
            {reinterpret_cast<const char*>(edit->storage.data), edit->storage.bytes}, window.first_codepoint, &first);
    SecureZeroMemory(visible, sizeof(visible));
    if (valid) *position = first + relative;
    return valid;
}

bool text_store_range(void*, u32 first, u32 last, RECT* output, bool* clipped) noexcept {
    const auto* edit = application_active_login_edit();
    POINT origin{};
    if (!edit || !g_app.renderer_ready || !ClientToScreen(g_app.window, &origin)) return false;
    if (IsIconic(g_app.window)) {
        if (!output || !clipped) return false;
        *output = {}; *clipped = true; return true;
    }
    const LoginElement element = edit == &g_app.login.login ?
        LoginElement::login_field : LoginElement::password_field;
    if (login_phone_mode(g_app.login.status)) {
        const auto* node = login_scene_element(g_app.login_scene, element);
        return node && application_phone_range_geometry(*edit, node->content_rectangle, g_app.renderer.dpi,
            origin, first, last, output, clipped, &node->clip_rectangle);
    }
    return application_login_range_geometry(*edit, g_app.renderer.login_edit,
        login_scene_content_rectangle(g_app.login_scene, element), g_app.renderer.dpi,
        origin, first, last, output, clipped);
}

void text_store_changed(void*) noexcept {
    Application& application = g_app;
    const bool phone = login_phone_editable(application.login.status) &&
        application_active_login_edit() == &application.login.mfa;
    if (phone && (application.login.status == LoginStatus::recovery_phone_input_invalid ||
        application.login.status == LoginStatus::recovery_phone_code_rejected)) application.login.status = LoginStatus::recovery_phone_required;
    if (application.login.status == RendererLoginStatus::mfa_input_invalid &&
        application_active_login_edit() == &application.login.mfa) {
        application.login.status = RendererLoginStatus::mfa_required;
    }
    if (!application_refresh_login_view()) {
        log_write(LogLevel::error, "ui", "login_text_refresh_failed");
        if (application.window) {
            application_request_frame();
        }
    }
    if (phone && application_phone_tsf_mutation_stamp() == static_cast<u64>(application.login.mfa.revision) + 1 &&
        login_phone_code_valid({reinterpret_cast<const char*>(application.login.mfa.storage.data), application.login.mfa.storage.bytes})) {
        SecureTextComposition composition{};
        if (secure_text_store_composition(application.login.text_store, &composition) && !composition.active &&
            PostMessageW(application.window, kApplicationPhoneVerifyMessage,
                static_cast<WPARAM>(application.login.application_auth_epoch), static_cast<LPARAM>(application.login.mfa.revision)))
            application_phone_tsf_mutation_stamp() = 0;
    }
}

} // namespace

bool application_synchronize_login_view() noexcept {
    Application& application = g_app;
    if (!application.login.initialized || !application.renderer_ready) {
        return true;
    }
    if (application_login_auth_cooldown_active()) {
        LoginView paused{};
        paused.status = application.login.status;
        paused.captcha_status = application.login.captcha_status;
        paused.auth_rate_limited = true;
        paused.auth_retry_time_unknown = application.login.auth_retry_time_unknown;
        const bool resume = login_view_auth_retry_resume(paused);
        const bool back = login_requires_back(paused.status, application.login.remote_status) ||
            application.login.reset_mode || login_phone_mode(paused.status) || login_recovery_mode(paused.status);
        if (!(resume && application.login.focus == LoginControl::submit) &&
            !(back && application.login.focus == LoginControl::back))
            application.login.focus = resume ? LoginControl::submit : back ? LoginControl::back : LoginControl::none;
    }
    const bool phone = login_phone_mode(application.login.status);
    if (phone && !application_phone_materialize(&application.login.mfa)) return false;
    if (!phone || !login_phone_editable(application.login.status)) application_phone_tsf_mutation_stamp() = 0;
    if (!phone && !application_login_prepare_identifier()) return false;
    constexpr u32 kVisibleEditCodepoints = 48;
    char login_display[kVisibleEditCodepoints * 4]{};
    char secret_display[kVisibleEditCodepoints * 4]{};
    u32 login_display_bytes = 0;
    u32 secret_display_bytes = 0;
    SecureEditRenderWindow login_window{};
    SecureEditRenderWindow secret_window{};
    const bool mfa_edit = application.login.status == RendererLoginStatus::mfa_required ||
                          application.login.status == RendererLoginStatus::mfa_input_invalid ||
                          application.login.status == RendererLoginStatus::mfa_code_rejected;
    const bool mfa_presented = application.login.status == RendererLoginStatus::mfa_required ||
                               application.login.status == RendererLoginStatus::mfa_backup_acknowledgement ||
                               application.login.status == RendererLoginStatus::mfa_code_rejected ||
                               application.login.status == RendererLoginStatus::mfa_input_invalid ||
                               application.login.status == RendererLoginStatus::webauthn_required ||
                               application.login.status == RendererLoginStatus::webauthn_failed;
    // Reveal is an explicit per-field choice; any other state re-masks it.
    LoginView reveal_probe{};
    reveal_probe.status = application.login.status;
    reveal_probe.remote_status = application.login.remote_status;
    reveal_probe.reset_mode = application.login.reset_mode;
    reveal_probe.mfa_capabilities = mfa_presented ? application.login.mfa_capabilities : static_cast<u8>(0);
    const bool password_revealed = application.login.password_revealed && !phone && !mfa_edit &&
                                   login_view_password_reveal_available(reveal_probe);
    application.login.password_revealed = password_revealed;
    application.login.password.password = !password_revealed;
    const SecureEdit& active_secret = phone || mfa_edit ? application.login.mfa : application.login.password;
    const SecureEditResult rendered_login =
        phone ? SecureEditResult::unchanged : secure_edit_render_window(application.login.login, kVisibleEditCodepoints,
                                  Slice<char>{login_display, static_cast<u32>(sizeof(login_display))},
                                  &login_display_bytes, &login_window);
    const SecureEditResult rendered_secret =
        secure_edit_render_window(active_secret, kVisibleEditCodepoints,
                                  Slice<char>{secret_display, static_cast<u32>(sizeof(secret_display))},
                                  &secret_display_bytes, &secret_window);
    if (rendered_login != SecureEditResult::unchanged || rendered_secret != SecureEditResult::unchanged) {
        SecureZeroMemory(login_display, sizeof(login_display));
        SecureZeroMemory(secret_display, sizeof(secret_display));
        return false;
    }
    const bool mfa_visible = application.login.status == RendererLoginStatus::mfa_required ||
                             application.login.status == RendererLoginStatus::mfa_backup_acknowledgement ||
                             application.login.status == RendererLoginStatus::mfa_code_rejected ||
                             application.login.status == RendererLoginStatus::mfa_input_invalid ||
                             application.login.status == RendererLoginStatus::webauthn_required ||
                             application.login.status == RendererLoginStatus::webauthn_failed;
    const u8 selected_mfa_capability = login_mfa_capability(application.login.mfa_type);
    SecureEdit* focused_edit = application_active_login_edit();
    const bool edit_active = focused_edit != nullptr;
    const SecureEditRenderWindow& edit_window =
        application.login.focus == LoginControl::login ? login_window : secret_window;
    const bool mfa_code_ready =
        mfa_edit && login_mfa_validate_code(application.login.mfa_type,
                                            secure_buffer_view(application.login.mfa.storage))
                        .ok();
    RendererLoginView view{Utf8View{login_display, login_display_bytes},
                           Utf8View{secret_display, secret_display_bytes},
                           Utf8View{application.login.remote_user, application.login.remote_user_bytes},
                           edit_window.anchor,
                           edit_window.caret,
                           edit_active,
                           application.login.focus,
                           application.login.status,
                           mfa_visible ? application.login.mfa_capabilities : static_cast<u8>(0),
                           mfa_visible ? selected_mfa_capability : static_cast<u8>(0),
                           application.login.backup_acknowledgement_seconds,
                           mfa_code_ready,
                           application.login.qr_ready ? &application.login.qr_code : nullptr,
                           application.login.sms_sending,
                           application.login.sms_requested,
                           application.login.sms_failed,
                           application.login.visible};
    view.remote_status = application.login.remote_status;
    view.reset_mode = application.login.reset_mode;
    view.phone_code_ready = phone && login_phone_editable(view.status) && login_phone_code_valid({secret_display, secret_display_bytes});
    view.captcha_status = application.login.captcha_status;
    view.password_revealed = password_revealed;
    view.auth_rate_limited = application_login_auth_cooldown_active();
    view.auth_retry_time_unknown = application.login.auth_retry_time_unknown;
    view.caps_lock = focused_edit == &application.login.password && (GetKeyState(VK_CAPITAL) & 1) != 0;
    if (phone) {
        view.login = {}; view.remote_user = {}; view.remote_status = LoginRemoteStatus::idle; view.qr_code = nullptr;
        view.mfa_capabilities = view.selected_mfa_capability = view.backup_acknowledgement_seconds = 0;
        view.mfa_code_ready = false; view.sms_sending = view.sms_sent = view.sms_failed = false;
    }
    if (view.reset_mode) view.login = {};
    if (view.reset_mode && login_credential_fields_hidden(view.status, view.remote_status, true))
        view.password_mask = {};
    LoginScene& scene = application.login_scene;
    scene.restoring_session = view.status == RendererLoginStatus::restoring_session;
    scene.back_only = login_requires_back(view.status, view.remote_status);
    scene.recovery_mode = login_recovery_mode(view.status);
    scene.recovery_disabled = login_view_recovery_disabled(view);
    scene.reset_mode = view.reset_mode;
    scene.phone_mode = phone;
    scene.browser_mode = login_browser_mode(view.status);
    scene.browser_available = !view.auth_rate_limited &&
                              login_browser_available(view.status, view.remote_status, view.reset_mode);
    scene.phone_resend_disabled = phone && !login_phone_editable(view.status);
    scene.focus = view.focus;
    scene.mfa_capabilities = view.mfa_capabilities;
    scene.selected_mfa_capability = view.selected_mfa_capability;
    scene.credential_fields_hidden = login_credential_fields_hidden(view.status, view.remote_status, view.reset_mode);
    scene.action_disabled = login_view_action_disabled(view);
    scene.submit_disabled = login_view_submit_disabled(view);
    scene.auth_retry_resume = login_view_auth_retry_resume(view);
    scene.captcha_action = login_view_captcha_action(view);
    scene.password_reveal_available = login_view_password_reveal_available(view);
    scene.password_revealed = scene.password_reveal_available && view.password_revealed;
    const float width_dip = static_cast<float>(application.renderer.width) * 96.0f /
                            static_cast<float>(maximum<u32>(application.renderer.dpi, 1));
    const float height_dip = static_cast<float>(application.renderer.height) * 96.0f /
                             static_cast<float>(maximum<u32>(application.renderer.dpi, 1));
    const bool layout_ready = !view.visible || login_scene_layout(&scene, width_dip, height_dip);
    if (layout_ready && view.visible) {
        view.focus = scene.focus;
        application.login.focus = scene.focus;
    }
    bool synchronized = layout_ready;
    if (synchronized) {
        // Password fields disable text services; IS_PASSWORD is only a UI hint.
        // Identifier and short-lived MFA fields retain native TSF input.
        const bool text_services_allowed = (focused_edit == &application.login.login &&
                                            !application.login.login.password) ||
                                           (focused_edit == &application.login.mfa &&
                                            (phone || application.login.mfa_type != UserAuthMfaType::backup));
        if (text_services_allowed) {
            synchronized = phone ? secure_text_store_bind_target(&application.login.text_store,
                application_phone_input_target(focused_edit), SecureTextInputScope::digits) :
                secure_text_store_bind(&application.login.text_store, focused_edit, active_input_scope(application, focused_edit));
        } else {
            secure_text_store_unbind(&application.login.text_store);
        }
    }
    TextCompositionSpan visible_composition[kTextCompositionSpanCapacity]{};
    if (synchronized && focused_edit) {
        SecureTextComposition native{};
        if (!secure_text_store_composition(application.login.text_store, &native)) {
            synchronized = false;
        } else if (native.active) {
            view.edit_composition_active = true;
            view.edit_composition_complete = native.complete;
            const u32 window_first = edit_window.first_codepoint;
            const u32 window_end = window_first + edit_window.codepoints;
            u32 visible_count = 0;
            for (u32 index = 0; index < native.span_count; ++index) {
                u32 start = 0;
                u32 end = 0;
                if (secure_edit_codepoint_from_utf16(*focused_edit, native.spans[index].start, &start) !=
                        SecureEditResult::unchanged ||
                    secure_edit_codepoint_from_utf16(*focused_edit, native.spans[index].end, &end) !=
                        SecureEditResult::unchanged) {
                    view.edit_composition_complete = false;
                    continue;
                }
                start = maximum(start, window_first);
                end = minimum(end, window_end);
                if (end <= start) {
                    continue;
                }
                if (visible_count == kTextCompositionSpanCapacity) {
                    view.edit_composition_complete = false;
                    break;
                }
                TextCompositionSpan span = native.spans[index];
                span.start = start - window_first;
                span.end = end - window_first;
                visible_composition[visible_count++] = span;
            }
            view.edit_composition = ConstSlice<TextCompositionSpan>{visible_composition, visible_count};
        }
    }
    if (synchronized) {
        synchronized = renderer_set_login_view(&application.renderer, view.visible ? &scene : nullptr, view,
                                               application_theme());
    }
    SecureZeroMemory(login_display, sizeof(login_display));
    SecureZeroMemory(secret_display, sizeof(secret_display));
    if (application.login.pointer_selection != LoginControl::none &&
        (!application.login.visible || !scene.widgets.captured.generation)) {
        application_login_end_pointer_selection();
    }
    if (synchronized) {
        application_reset_login_caret_timing(edit_active);
    }
    return synchronized;
}

SecureEdit* application_active_login_edit() noexcept {
    Application& application = g_app;
    LoginRuntime& login = application.login;
    if (login_phone_mode(login.status)) return login.initialized && login.visible &&
        login_phone_editable(login.status) && login.focus == LoginControl::phone_code ? &login.mfa : nullptr;
    if (login_recovery_mode(login.status) && !login_recovery_editable(login.status)) return nullptr;
    if (login.reset_mode && login_credential_fields_hidden(login.status, login.remote_status, true)) return nullptr;
    if (!login.initialized || !login.visible || login.status == RendererLoginStatus::restoring_session || login.status == RendererLoginStatus::submitting ||
        login_requires_back(login.status, login.remote_status) ||
        login.status == RendererLoginStatus::mfa_backup_acknowledgement ||
        login.status == RendererLoginStatus::webauthn_required ||
        login.status == RendererLoginStatus::webauthn_failed) {
        return nullptr;
    }
    if (login.focus == LoginControl::login && !login.reset_mode) {
        return &login.login;
    }
    if (login_recovery_mode(login.status)) return nullptr;
    const bool mfa_edit = login.status == RendererLoginStatus::mfa_required ||
                          login.status == RendererLoginStatus::mfa_input_invalid ||
                          login.status == RendererLoginStatus::mfa_code_rejected;
    return login.focus == LoginControl::password ? mfa_edit ? &login.mfa : &login.password : nullptr;
}

bool application_initialize_login_ui() noexcept {
    Application& application = g_app;
    LoginRuntime& login = application.login;
    if (login.initialized) {
        return true;
    }
    if (!secure_edit_initialize(&login.login, kUserAuthMaximumLoginBytes, false)) {
        return false;
    }
    if (!secure_edit_initialize(&login.password, kUserAuthMaximumPasswordBytes, true)) {
        secure_edit_release(&login.login);
        return false;
    }
    if (!secure_edit_initialize(&login.mfa, kUserAuthMaximumCodeBytes, false)) {
        secure_edit_release(&login.password);
        secure_edit_release(&login.login);
        return false;
    }
    login.focus = LoginControl::login;
    login.status = RendererLoginStatus::idle;
    const UINT caret_blink_ms = GetCaretBlinkTime();
    login.caret_blink_ms = caret_blink_ms == 0 ? kDefaultCaretBlinkMilliseconds : caret_blink_ms;
    login.initialized = true;
    login.visible = true;
    application_composer_text_input_suspend();
    if (!secure_text_store_initialize(
            &login.text_store,
            SecureTextStoreConfig{application.window, text_store_geometry, text_store_changed, nullptr, text_store_range, text_store_point}) ||
        !secure_text_store_bind(&login.text_store, &login.login, SecureTextInputScope::login_name) ||
        !secure_text_store_attach(&login.text_store)) {
        secure_text_store_release(&login.text_store);
        secure_edit_release(&login.mfa);
        secure_edit_release(&login.password);
        secure_edit_release(&login.login);
        const u64 epoch = login.application_auth_epoch + 1;
        login = LoginRuntime{};
        login.application_auth_epoch = epoch;
        return false;
    }
    log_write(LogLevel::info, "ui", "native_tsf_text_store_ready");
    return true;
}

void application_release_login_ui() noexcept {
    Application& application = g_app;
    application_login_end_pointer_selection();
    login_scene_release(&application.login_scene);
    if (!application.login.initialized) {
        return;
    }
    secure_text_store_release(&application.login.text_store);
    secure_edit_release(&application.login.mfa);
    secure_edit_release(&application.login.password);
    secure_edit_release(&application.login.login);
    const u64 epoch = application.login.application_auth_epoch + 1;
    application.login = LoginRuntime{};
    application.login.application_auth_epoch = epoch;
}

void application_reset_login_caret_timing(bool edit_active) noexcept {
    Application& application = g_app;
    LoginRuntime& login = application.login;
    const bool visible = edit_active && application.window && GetFocus() == application.window;
    login.caret_visible = visible;
    renderer_set_login_caret_visible(&application.renderer, visible);
    login.caret_blink_at_ms = visible && login.caret_blink_ms != INFINITE
                                  ? clock_monotonic_milliseconds() + login.caret_blink_ms
                                  : 0;
}

bool application_refresh_login_view() noexcept {
    Application& application = g_app;
    if (!application_synchronize_login_view()) {
        return false;
    }
    secure_text_store_synchronize(&application.login.text_store);
    application_request_frame();
    return true;
}

} // namespace maicord::app_detail
