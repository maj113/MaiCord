
#include "application_login_ui.hpp"
#include "application_state.hpp"
#include "application_login_geometry.hpp"

#include "maicord/core/log.hpp"
#include "maicord/app/login_continuations.hpp"
#include "maicord/ui/clipboard.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {

namespace {
bool resize_identifier(u32 capacity) noexcept {
    SecureEdit& edit = g_app.login.login;
    if (edit.storage.capacity == capacity) return true;
    SecureBuffer grown{};
    if (!secure_buffer_initialize(&grown, capacity).ok()) return false;
    if (!secure_buffer_assign(&grown, secure_buffer_view(edit.storage)).ok()) {
        secure_buffer_release(&grown);
        return false;
    }
    secure_text_store_unbind(&g_app.login.text_store);
    secure_buffer_release(&edit.storage);
    edit.storage = grown;
    return true;
}
bool grow_identifier() noexcept {
    return resize_identifier(kLoginVerificationMaximumLinkBytes);
}
bool phone_input_changed(SecureEditResult result) noexcept {
    if (result != SecureEditResult::changed) return true;
    application_login_code_edited(&g_app.login.status);
    if (!application_refresh_login_view()) return false;
    if (login_phone_editable(g_app.login.status))
        application_login_post_phone_verify();
    return true;
}
} // namespace

bool application_login_prepare_identifier() noexcept {
    if (login_phone_mode(g_app.login.status)) return true;
    SecureEdit& edit = g_app.login.login;
    if (g_app.login.reset_mode) {
        edit.password = true;
        return true;
    }
    bool verification = login_verification_link_candidate(
        {reinterpret_cast<const char*>(edit.storage.data), edit.storage.bytes}) || login_reset_link_candidate(
        {reinterpret_cast<const char*>(edit.storage.data), edit.storage.bytes});
    if (login_recovery_mode(g_app.login.status) && edit.storage.bytes &&
        !user_auth_recovery_identifier_valid({reinterpret_cast<const char*>(edit.storage.data), edit.storage.bytes})) {
        secure_edit_clear(&edit);
        edit.password = false;
        verification = false;
        g_app.login.status = LoginStatus::recovery_failed;
    }
    // Partial deletion of a URI must not reveal its remaining token. Clear or
    // full-field replacement resets masking in the input handlers below.
    edit.password = verification || (edit.password && edit.storage.bytes != 0);
    if (edit.password) secure_text_store_unbind(&g_app.login.text_store);
    if (!edit.password && edit.storage.capacity > kUserAuthMaximumLoginBytes &&
        edit.storage.bytes <= kUserAuthMaximumLoginBytes) {
        // Reclaim the exceptional URI allocation after clear/full replacement.
        // A failed shrink is harmless; the original locked field stays valid.
        resize_identifier(kUserAuthMaximumLoginBytes);
    }
    return login_recovery_mode(g_app.login.status) || !verification || grow_identifier();
}

bool application_login_paste() noexcept {
    SecureEdit* edit = application_active_login_edit();
    if (!edit || !g_app.window) {
        return true;
    }
    if (login_phone_mode(g_app.login.status)) {
        char digits[kLoginPhoneCodeDigits]{};
        const auto pasted = clipboard_read_utf8(g_app.window, {digits, kLoginPhoneCodeDigits});
        const auto result = pasted.ok() && pasted.bytes ? application_phone_insert_utf8(edit, {digits, pasted.bytes}) : SecureEditResult::unchanged;
        SecureZeroMemory(digits, sizeof(digits));
        return phone_input_changed(result);
    }
    if (edit == &g_app.login.login) {
        SecureBuffer converted{};
        if (!secure_buffer_initialize(&converted, login_recovery_mode(g_app.login.status) ?
            kUserAuthMaximumLoginBytes : kLoginVerificationMaximumLinkBytes).ok()) return true;
        const ClipboardTextResult pasted = clipboard_read_utf8(
            g_app.window, {reinterpret_cast<char*>(converted.data), converted.capacity});
        const Utf8View inserted{reinterpret_cast<const char*>(converted.data), pasted.ok() ? pasted.bytes : 0};
        const bool verification = pasted.ok() && (login_verification_link_candidate(inserted) || login_reset_link_candidate(inserted));
        if (pasted.ok() && login_recovery_mode(g_app.login.status) && !user_auth_recovery_identifier_valid(inserted)) {
            secure_buffer_release(&converted);
            g_app.login.status = LoginStatus::recovery_failed;
            return application_refresh_login_view();
        }
        const bool replaced_all = minimum(edit->anchor, edit->caret) == 0 &&
                                  maximum(edit->anchor, edit->caret) == edit->storage.bytes;
        bool changed = false;
        if (verification) {
            edit->password = true;
            secure_text_store_unbind(&g_app.login.text_store);
        }
        if (pasted.ok() && pasted.bytes && (!verification || grow_identifier())) {
            // Ordinary account identifiers retain the protocol's 512-byte limit.
            if (verification || pasted.bytes <= kUserAuthMaximumLoginBytes)
                changed = secure_edit_insert_utf8(edit, inserted) == SecureEditResult::changed;
        }
        secure_buffer_release(&converted);
        if (changed && replaced_all && !verification) edit->password = false;
        const bool prepared = application_login_prepare_identifier();
        return prepared && (changed ? application_refresh_login_view() : true);
    }
    char converted[kUserAuthMaximumPasswordBytes]{};
    const ClipboardTextResult pasted = clipboard_read_utf8(
        g_app.window,
        Slice<char>{converted, minimum(edit->storage.capacity, static_cast<u32>(sizeof(converted)))});
    bool changed = false;
    if (pasted.ok() && pasted.bytes != 0) {
        changed =
            secure_edit_insert_utf8(edit, Utf8View{converted, pasted.bytes}) == SecureEditResult::changed;
    }
    SecureZeroMemory(converted, sizeof(converted));
    if (changed && edit == &g_app.login.mfa)
        application_login_code_edited(&g_app.login.status);
    return changed ? application_refresh_login_view() : true;
}

void application_login_tick_caret(u64 now_ms) noexcept {
    LoginRuntime& login = g_app.login;
    if (login.caret_blink_at_ms == 0 || now_ms < login.caret_blink_at_ms) {
        return;
    }
    if (!g_app.renderer_ready || !g_app.renderer.login_edit.active || GetFocus() != g_app.window ||
        login.caret_blink_ms == INFINITE) {
        login.caret_blink_at_ms = 0;
        login.caret_visible = false;
        renderer_set_login_caret_visible(&g_app.renderer, false);
        return;
    }
    login.caret_visible = !login.caret_visible;
    renderer_set_login_caret_visible(&g_app.renderer, login.caret_visible);
    login.caret_blink_at_ms = now_ms + login.caret_blink_ms;
    application_request_frame();
}

bool application_login_position_edit(LoginControl control, i32 client_x, i32 client_y,
                                     bool extend_selection) noexcept {
    if (control != LoginControl::login && control != LoginControl::password && control != LoginControl::phone_code) {
        return false;
    }
    if (g_app.login.focus != control) {
        g_app.login.focus = control;
        if (!application_refresh_login_view()) {
            return false;
        }
    }
    SecureEdit* edit = application_active_login_edit();
    if (!edit) {
        return true;
    }
    constexpr u32 kVisibleEditCodepoints = 48;
    char visible[kVisibleEditCodepoints * 4]{};
    u32 visible_bytes = 0;
    SecureEditRenderWindow window{};
    const SecureEditResult rendered = secure_edit_render_window(
        *edit, kVisibleEditCodepoints, Slice<char>{visible, static_cast<u32>(sizeof(visible))},
        &visible_bytes, &window);
    u32 relative_codepoint = 0;
    const UiRect field = login_scene_content_rectangle(
        g_app.login_scene,
        control == LoginControl::login ? LoginElement::login_field : LoginElement::password_field);
    bool inside = false;
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    const bool hit = rendered == SecureEditResult::unchanged && (control == LoginControl::phone_code ?
        login_phone_point_position(field, visible_bytes, client_x * scale, client_y * scale, true, &relative_codepoint, &inside) :
        renderer_login_edit_hit_test(g_app.renderer, field, Utf8View{visible, visible_bytes}, client_x, client_y, &relative_codepoint));
    SecureZeroMemory(visible, sizeof(visible));
    if (!hit || relative_codepoint > (control == LoginControl::phone_code ? kLoginPhoneCodeDigits : window.codepoints)) {
        return false;
    }
    const SecureEditResult positioned =
        control == LoginControl::phone_code ? application_phone_select(edit,
            {extend_selection ? edit->anchor : relative_codepoint, relative_codepoint}) :
        secure_edit_set_codepoint(edit, window.first_codepoint + relative_codepoint, extend_selection);
    return positioned == SecureEditResult::unchanged ||
           (positioned == SecureEditResult::changed && application_refresh_login_view());
}

void application_login_end_pointer_selection() noexcept {
    g_app.login.pointer_selection = LoginControl::none;
    ui_widget_manager_capture(&g_app.login_scene.widgets, ui_widget_invalid_handle());
    if (GetCapture() == g_app.window) {
        ReleaseCapture();
    }
}

bool application_login_handle_drag(WPARAM buttons, LPARAM position) noexcept {
    const LoginControl control = g_app.login.pointer_selection;
    if (control != LoginControl::login && control != LoginControl::password && control != LoginControl::phone_code) {
        return false;
    }
    if ((buttons & MK_LBUTTON) == 0) {
        application_login_end_pointer_selection();
        return true;
    }
    const i32 x = static_cast<short>(LOWORD(position));
    const i32 y = static_cast<short>(HIWORD(position));
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    auto& widgets = g_app.login_scene.widgets;
    const auto target = ui_widget_manager_pointer_target(&widgets, x * scale, y * scale);
    const auto expected = g_app.login_scene.elements[static_cast<u32>(
        control == LoginControl::login ? LoginElement::login_field : LoginElement::password_field)];
    if (!widgets.captured.generation || target.slot != expected.slot ||
        target.generation != expected.generation) {
        application_login_end_pointer_selection();
        return true;
    }
    if (!application_login_position_edit(control, x, y, true)) {
        application_login_end_pointer_selection();
        log_write(LogLevel::error, "ui", "login_pointer_drag_failed");
    }
    return true;
}

bool application_login_handle_character(WPARAM character) noexcept {
    if (!g_app.login.visible) {
        return false;
    }
    SecureEdit* edit = application_active_login_edit();
    if (!edit || character < 0x20u || character > 0xFFFFu) {
        return true;
    }
    if (login_phone_mode(g_app.login.status)) {
        const char digit = static_cast<char>(character);
        return character >= '0' && character <= '9' ? phone_input_changed(application_phone_insert_utf8(edit, {&digit, 1})) : true;
    }
    const bool replaced_all = edit == &g_app.login.login && minimum(edit->anchor, edit->caret) == 0 &&
                              maximum(edit->anchor, edit->caret) == edit->storage.bytes;
    const SecureEditResult result = secure_edit_insert_utf16(edit, static_cast<u16>(character));
    if (replaced_all && result == SecureEditResult::changed) edit->password = false;
    if (result == SecureEditResult::capacity) {
        MessageBeep(MB_OK);
        return true;
    }
    if (result == SecureEditResult::changed && edit == &g_app.login.mfa)
        application_login_code_edited(&g_app.login.status);
    return (result == SecureEditResult::changed || result == SecureEditResult::pending_surrogate) &&
           application_refresh_login_view();
}

} // namespace maicord::app_detail
