#pragma once

#include "maicord/app/client_preferences.hpp"
#include "maicord/ui/settings_schema.hpp"
#include "maicord/ui/settings_scene.hpp"

namespace maicord {

// Storage bindings of catalog nodes. Values live in a backend; local is the
// ClientPreferences record. Synced account settings (settings-proto/1) and OS
// settings get their own backends without changing catalog data.
enum class SettingBinding : u16 {
    none,
    theme_base,
    theme_color,
    customize_theme,
    background_effect,
    gradient_direction,
    gradient_intensity,
    custom_import,
    custom_preview,
    custom_apply,
    custom_cancel,
    custom_builtin,
    custom_restore,
    custom_retry_read,
    custom_retry_save,
    custom_installed,
    member_list,
    reduced_motion,
    smooth_scroll,
    desktop_notifications,
    notification_setup,
    windows_notification_settings,
    message_sounds,
    selected_channel_sounds,
    output_volume,
    developer_mode,
    preferences_retry,
    update_status,
    update_channel,
    update_rollback,
    update_notice,
    voice_input_device,
    voice_output_device,
    voice_input_volume,
    voice_output_volume,
    voice_input_mode,
    voice_auto_sensitivity,
    voice_sensitivity,
    voice_push_to_talk_key,
    voice_push_to_talk_release,
    voice_processing,
    voice_soundboard_volume,
    voice_soundboard_muted,
    // Synced account settings (settings-proto/1, application_user_settings).
    dm_from_members,
    message_requests,
    nsfw_commands,
    dm_spam_filter,
    explicit_friends,
    explicit_others,
    explicit_guilds,
    friend_everyone,
    friend_mutual_friends,
    friend_mutual_guilds,
    profile_visibility,
    profile_updates,
    message_display,
    time_format,
    user_settings_notice,
    privacy_notice,
    language_notice,
    // Local display and taskbar attention.
    group_spacing,
    unread_badge,
    taskbar_flash,
    // Account pages (application_account_settings).
    account_card,
    account_display_name,
    account_username,
    account_email,
    account_phone,
    account_password,
    account_mfa,
    account_notice,
    profile_notice,
    devices_notice,
    apps_notice,
    profile_display_name,
    profile_pronouns,
    profile_bio,
    devices_list,
    apps_list,
    // Chat (settings-proto text_and_images, application_user_settings).
    chat_notice,
    inline_attachment_media,
    inline_embed_media,
    render_embeds,
    render_reactions,
    gif_autoplay,
    animate_emoji,
    animate_stickers,
    render_spoilers,
    convert_emoticons,
    // Keybinds (application_keybinds_settings): custom keybinds list, add, defaults list.
    keybinds_custom,
    keybinds_add,
    keybinds_defaults,
    // System (application_desktop): startup entry and protocol handler live
    // in the Windows registry (os backend); tray and spellcheck are local.
    open_on_startup,
    start_minimized,
    minimize_to_tray,
    protocol_handler,
    spellcheck,
    // Activity privacy (application_activity): show_current_game is synced
    // through the status settings owner, detection and added games are local.
    share_activity,
    game_detection,
    registered_games,
    add_game,
    // More synced settings (settings-proto/1, application_user_settings):
    // Messaging Permissions, Data & Privacy and Activity Privacy rows.
    nsfw_guilds,
    friend_request_notes,
    sponsored_activity,
    sponsored_third_party,
    share_online,
    share_streaming,
    activity_servers,
    join_friends,
    join_voice,
    activity_notice,
    accessibility_notice,
    // Updates page (application_updates) rows added with the changelog.
    update_problem,
    update_available,
    update_changelog,
    count
};
enum class SettingBackend : u8 {
    local,
    action,
    proto1,
    os,
    voice, // Voice preferences (voice.bin), applied to the account module.
    account, // REST-backed account pages (application_account_settings).
    keybinds // Custom keybinds and the shortcut registry (application_keybinds_settings).
};
SettingBackend settings_binding_backend(SettingBinding) noexcept;

enum class SettingsPanel : u8 {
    notifications,
    appearance,
    accessibility,
    plugins,
    advanced,
    updates,
    voice,
    account,
    profiles,
    devices,
    content_social,
    data_privacy,
    authorized_apps,
    language_time,
    registered_games,
    keybinds,
    system,
    activity_privacy,
    count
};
inline constexpr u32 kSettingsPanelCount = static_cast<u32>(SettingsPanel::count);
const UiSettingsSchema& settings_catalog() noexcept;
const UiSettingsSchemaPanel& settings_catalog_panel(SettingsPanel) noexcept;
// Sidebar: section headers, native panel pages (owner 1, page panel + 1),
// contributed plugin pages under MaiCord, then the destructive Log out action.
inline constexpr u64 kSettingsNativeOwner = 1;
inline constexpr u64 kSettingsLogOutAction = 1;
ConstSlice<UiSettingsNavEntry> settings_catalog_navigation(bool log_out) noexcept;
// Pages that need no signed-in account: local preferences, devices and
// updates. Before sign-in Settings offers only these.
constexpr bool settings_panel_signed_out(SettingsPanel panel) noexcept {
    switch (panel) {
    case SettingsPanel::appearance:
    case SettingsPanel::accessibility:
    case SettingsPanel::voice:
    case SettingsPanel::system:
    case SettingsPanel::keybinds:
    case SettingsPanel::language_time:
    case SettingsPanel::updates:
    case SettingsPanel::advanced:
        return true;
    default:
        return false;
    }
}
ConstSlice<UiSettingsNavEntry> settings_catalog_navigation_signed_out() noexcept;

// Dynamic list rows (devices, authorized apps): list binding in bits 8..15,
// row index in bits 0..7.
inline constexpr u64 kSettingsListFirst = 0x7E000000u;
constexpr u64 settings_list_control_id(SettingBinding list, u32 index) noexcept {
    return kSettingsListFirst + (static_cast<u64>(list) << 8) + index;
}

// Stable control ids: panel and node index, unique across the catalog so a
// search result and its panel row share one id.
constexpr u64 settings_control_id(u32 panel, u32 node) noexcept {
    return (static_cast<u64>(panel + 1) << 16) | (node + 1);
}
bool settings_control_location(u64 id, UiSettingsSchemaLocation*) noexcept;
const UiSettingsSchemaNode* settings_catalog_node(UiSettingsSchemaLocation) noexcept;
const UiSettingsSchemaNode* settings_catalog_binding(SettingBinding, UiSettingsSchemaLocation* = nullptr) noexcept;

// Local backend: read and write one binding of a preferences record. Theme
// choices map palette indexes onto ClientTheme; kUiSettingsNoChoice reports a
// theme from the other palette.
struct SettingValue {
    bool checked;
    u32 choice;
};
bool settings_store_read(const ClientPreferences&, SettingBinding, SettingValue*) noexcept;
bool settings_store_write(ClientPreferences*, SettingBinding, SettingValue) noexcept;
ClientTheme settings_theme_choice(SettingBinding palette, u32 choice) noexcept;
// The preview of one theme swatch under these preferences: a gradient theme's
// own stops (Custom: the user's), or a flat theme's surface as one repeated
// stop, which the swatch paints solid.
bool settings_theme_swatch(const ClientPreferences&, SettingBinding palette, u32 choice,
                           BackgroundTheme* output) noexcept;

} // namespace maicord
