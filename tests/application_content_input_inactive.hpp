#pragma once

// Boundaries of the application_content_input.cpp click routing that these
// fixtures keep inactive: the shipping TU also reaches menus, the guild rail,
// private-channel rows, the media and plugin surfaces and the thread browser.
// Include after the application headers; a fixture that observes or answers
// one of these calls defines it itself and does not include this header.
namespace maicord::app_detail {
bool application_guild_menu_toggle(bool) noexcept {
    return false;
}
bool application_private_channel_row_close(u64, float, float) noexcept {
    return false;
}
bool application_private_channel_requests_mode() noexcept {
    return false;
}
void application_private_channel_set_requests_mode(bool) noexcept {}
bool application_message_action_bar_activate(const MessageActionBarHit&, bool) noexcept {
    return false;
}
bool application_guild_card_open(u64, UiRect) noexcept {
    return false;
}
bool application_interaction_card_toggle(u64) noexcept {
    return false;
}
bool application_profile_open(u64, const UiRect&, ProfileAnchor) noexcept {
    return false;
}
bool application_plugins_slot_activate(u64, NativePluginSdkSlotTarget, u64, UiRect) noexcept {
    return false;
}
bool application_plugins_render_click(const MessageTextHit&, UiRect) noexcept {
    return false;
}
bool open_reaction_picker(u64, UiRect) noexcept {
    return false;
}
bool application_poll_cursor(UiPoint, UiCursor*) noexcept {
    return false;
}
bool application_video_cursor(UiPoint, UiCursor*) noexcept {
    return false;
}
bool application_gif_favorite_star_target(float, float) noexcept {
    return false;
}
bool application_image_viewer_open_at(float, float, bool) noexcept {
    return false;
}
bool application_video_open_focused() noexcept {
    return false;
}
bool application_commands_insert(u64, u64, Utf8View) noexcept {
    return true;
}
bool queue_message_reaction(u64, u32) noexcept {
    return false;
}
bool dismiss_popup() noexcept {
    return false;
}
bool application_thread_browser_read_blocked() noexcept {
    return false;
}
bool application_thread_browser_active() noexcept {
    return false;
}
bool application_thread_browser_open(u64) noexcept {
    return false;
}
void application_thread_browser_close() noexcept {
}
void application_read_state_activity() noexcept {
}
bool application_stop_account() noexcept; // each fixture defines its own
void application_account_stop_recoverable(const char*) noexcept {
    application_stop_account();
}
bool animate_guild_rail_scroll_from(float) noexcept {
    return true;
}
bool application_guild_invite_open_url(Utf8View) noexcept {
    return false;
}
bool application_link_open(Utf8View, bool) noexcept {
    return true;
}
bool application_link_channel_trusted(u64) noexcept {
    return true;
}
bool application_guild_folders_toggle(u64) noexcept {
    return false;
}
bool application_navigation_drag_press(LPARAM) noexcept {
    return false;
}
bool application_guild_lifecycle_open_create() noexcept {
    return false;
}
bool application_guild_community_open_events(u64) noexcept {
    return false;
}
bool application_invite_card_click(u64, UiWidgetHandle, float, float) noexcept {
    return false;
}
bool application_message_component_click(u64, UiWidgetHandle, UiRect) noexcept {
    return false;
}
bool application_pinned_messages_toggle() noexcept {
    return true;
}
bool refresh_account_frame(bool) noexcept {
    return true;
}
void application_user_settings_request_content_confirmation() noexcept {}
bool application_voice_join(u64, u64) noexcept {
    return true;
}
} // namespace maicord::app_detail
