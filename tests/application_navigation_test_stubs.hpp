// Shared application boundary for navigation input tests. Include after the
// case's state declarations and alias that namespace as navigation_runtime_test.
// These definitions retain the same counters and inactive surfaces in both cases.
#pragma once

namespace maicord {
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept {
    return true;
}
bool text_edit_layout_hit_test(const TextEditLayoutOwner&, float, float, u32* position) noexcept {
    if (!position)
        return false;
    *position = 0;
    return true;
}
u64 text_edit_signature(Utf8View text) noexcept {
    return hash64_counted_bytes(kHash64Seed, text.data, text.bytes);
}
void renderer_bind_focus_owner(D3D11Renderer* r, const UiWidgetManager* owner) noexcept {
    if (r)
        r->focus_owner = owner;
}
bool clipboard_write_utf8(HWND, Utf8View) noexcept {
    return true;
}
ClipboardTextResult clipboard_read_utf8(HWND, Slice<char>) noexcept {
    return {0, ClipboardTextError::unavailable};
}
namespace app_detail {
bool handle_thread_header_key(u32, bool, bool) noexcept {
    return false;
}
bool refresh_account_text(bool) noexcept {
    return true;
}
UiRect application_viewport_dips() noexcept {
    return {0, 0, 1200, 900};
}
void application_composer_text_input_suspend() noexcept {
    ++navigation_runtime_test::suspends;
}
bool application_composer_text_input_synchronize() noexcept {
    ++navigation_runtime_test::synchronizations;
    return true;
}
void application_composer_text_input_notify_layout() noexcept {
}
bool application_composer_text_input_composing() noexcept {
    return false;
}
void application_sync_attachment_drop_target() noexcept {
}
void application_video_blur() noexcept {
}
bool synchronize_shell_layout() noexcept {
    return true;
}
void application_friends_hide() noexcept {
}
void application_pinned_messages_close() noexcept {
}
bool application_search_close_panel() noexcept {
    return true;
}
void application_account_menu_clear() noexcept {
}
void application_surface_motion_retire(RendererLayerSurface) noexcept {
}
void end_message_composer_pointer_selection() noexcept {
}
void end_message_text_selection() noexcept {
}
bool clear_message_text_selection(bool) noexcept {
    return true;
}
bool dismiss_popup() noexcept {
    popup_scene_close(&g_app.popup_scene);
    return true;
}
void application_media_picker_close() noexcept {
}
void application_settings_close(bool) noexcept {
}
bool application_profile_close() noexcept {
    return true;
}
bool select_content_guild(u64) noexcept {
    return true;
}
bool application_forum_form_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_forum_form_active() noexcept {
    return false;
}
bool application_text_form_input(UiInputKind, u32, bool) noexcept {
    return g_app.renderer.text_form && g_app.renderer.text_form->scene &&
           g_app.renderer.text_form->scene->active;
}
bool application_theme_editor_input(UiInputKind, u32, bool) noexcept {
    return navigation_runtime_test::theme_editor;
}
bool application_theme_editor_active() noexcept {
    return navigation_runtime_test::theme_editor;
}
bool application_settings_input(UiInputKind, u32, bool) noexcept {
    return navigation_runtime_test::settings;
}
bool application_settings_active() noexcept {
    return navigation_runtime_test::settings;
}
bool application_media_picker_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_media_picker_active() noexcept {
    return false;
}
bool application_pinned_messages_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_pinned_messages_active() noexcept {
    return false;
}
bool application_thread_browser_consume_key_tail(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_thread_browser_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_search_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_search_edit_focused() noexcept {
    return false;
}
bool application_search_active() noexcept {
    return false;
}
bool application_friends_requests_active() noexcept {
    return false;
}
bool application_friends_active() noexcept {
    return false;
}
const RelationshipStore* application_friends_relationships() noexcept {
    return &navigation_runtime_test::relationship_store;
}
bool application_friends_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_scoped_read_shortcut() noexcept {
    ++navigation_runtime_test::scoped_shortcuts;
    return navigation_runtime_test::scoped_shortcut_allowed;
}
bool application_account_menu_key(u32) noexcept {
    return false;
}
bool application_voice_message_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_login_handle_key(WPARAM, bool) noexcept {
    return true;
}
bool application_login_handle_character(WPARAM) noexcept {
    return true;
}
bool application_account_switch_login_escape() noexcept {
    return false;
}
bool handle_reaction_viewer_key(WPARAM) noexcept {
    return true;
}
bool handle_popup_key(WPARAM) noexcept {
    return false;
}
bool application_profile_key(u32, bool, bool) noexcept {
    return true;
}
bool application_profile_character(u32) noexcept {
    return true;
}
bool handle_shortcut_key(u32, bool) noexcept {
    ++navigation_runtime_test::underlying_input;
    return false;
}
ShortcutAction application_shortcut_action(u32 key) noexcept {
    return shortcut_first({ShortcutFocus::content, false}, shortcut_chord_from_keyboard(key));
}
bool application_shortcuts_overlay_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_custom_keybind_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_unread_banner_escape() noexcept {
    return false;
}
bool application_received_attachment_activate_focused() noexcept {
    return false;
}
bool handle_content_focus_key(u32) noexcept {
    ++navigation_runtime_test::underlying_input;
    return false;
}
bool handle_composer_button_key(u32, bool, bool) noexcept {
    return false;
}
bool application_attachment_strip_key(u32) noexcept {
    return false;
}
bool handle_composer_character(WPARAM) noexcept {
    ++navigation_runtime_test::underlying_input;
    return true;
}
bool handle_composer_key(WPARAM) noexcept {
    ++navigation_runtime_test::underlying_input;
    return true;
}
bool application_message_action_bar_modifiers_changed() noexcept {
    return false;
}
bool handle_message_key(WPARAM) noexcept {
    ++navigation_runtime_test::underlying_input;
    return true;
}
bool copy_message_text_selection() noexcept {
    return false;
}
float client_dip_x(LPARAM v) noexcept {
    return static_cast<i16>(LOWORD(v));
}
float client_dip_y(LPARAM v) noexcept {
    return static_cast<i16>(HIWORD(v));
}
} // namespace app_detail
} // namespace maicord
