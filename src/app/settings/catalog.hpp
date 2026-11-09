#pragma once

#include "maicord/ui/settings/schema.hpp"

namespace maicord {

struct ClientPreferences;
enum class ClientTheme : u8;

struct UiSettingsNavEntry;

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
    // Appearance's "Your themes" palette: the installed themes, then a Browse themes tile.
    installed_themes,
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
    update_interval,
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
    // My Account's Devices row: opens the nested Devices page.
    account_devices,
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
    // My Account's security and removal rows, Devices' log-out-all, Connected Apps' connections and Data & Privacy's
    // consents and data request (application_account_settings).
    account_totp,
    account_sms,
    account_backup_codes,
    security_keys_list,
    account_key_add,
    account_disable,
    account_delete,
    devices_logout_all,
    connections_list,
    data_notice,
    data_usage_statistics,
    data_activity_link,
    data_request,
    // The data request's types, in harvest_create's option bit order.
    harvest_account,
    harvest_analytics,
    harvest_activities,
    harvest_ads,
    harvest_messages,
    harvest_servers,
    harvest_zendesk,
    // Messaging Permissions' blocked and ignored accounts, each with Unblock or Unignore (application_account_settings
    // over application_friends_user_action).
    blocked_list,
    ignored_list,
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
    update_changelog,
    update_release_notes,
    // Built-in extras (local): colours, reply times, folders, hidden channels.
    role_colors_everywhere,
    reply_timestamps,
    folders_close_others,
    folders_close_on_home,
    folders_follow_selection,
    show_hidden_channels,
    // Image zoom lens (local): enabled, square shape, zoom and size sliders.
    image_zoom,
    image_zoom_square,
    image_zoom_factor,
    image_zoom_size,
    spotify_account,
    // Appearance's link row to the Effects page.
    browse_effects,
    // Themes' header action, reached from settings search: pick a theme file, install it and apply it.
    import_theme,
    // Accessibility (local): Text Readability, Visual Density, Color & Contrast, Reduced Motion and
    // Audio & Screen Reader rows.
    chat_font_size,
    underline_links,
    zoom,
    saturation,
    desaturate_user_colors,
    role_style,
    reduced_motion_sync,
    speech_rate,
    speech_preview,
    // Accessibility's preview card (kind preview), drawn from the current choices.
    accessibility_preview,
    // Streamer Mode (local, Appearance).
    streamer_mode,
    streamer_auto,
    streamer_hide_personal,
    streamer_hide_invites,
    streamer_disable_sounds,
    streamer_disable_notifications,
    streamer_hide_window,
    // Synced (settings-proto/1): localization.locale, the Notifications page's "Notify me when..." rows and
    // mobile push delay, and Chat Box's game suggestions.
    language,
    notifications_notice,
    notify_go_live,
    notify_friend_online,
    notify_profile_updates,
    notify_anniversaries,
    notify_reactions,
    mobile_push_delay,
    game_mentions_autocomplete,
    // Synced: UI Density (appearance.ui_density), Show user avatars in compact mode, Show image descriptions,
    // Chat Box's sticker suggestions and the /tts command (text_and_images).
    ui_density,
    compact_avatars,
    image_descriptions,
    sticker_autocomplete,
    tts_command,
    // Notifications' "Speak all messages out loud" (local) and Developer's Application Test Mode.
    speak_messages,
    test_mode,
    // Registered Games' running game row (application_activity_settings_*).
    current_game,
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
#if !defined(_WIN32)
// A binding this system has nothing behind: the Windows notification settings page, the voice processing switch
// (the audio server owns echo cancellation and noise suppression on Linux) and hiding the window from screen capture
// (Wayland compositors decide what a capture shows). Its row is not shown and not searched.
bool settings_binding_unavailable(SettingBinding) noexcept;
#endif

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
    themes,
    effects,
    count
};
inline constexpr u32 kSettingsPanelCount = static_cast<u32>(SettingsPanel::count);
const UiSettingsSchema& settings_catalog() noexcept;
const UiSettingsSchemaPanel& settings_catalog_panel(SettingsPanel) noexcept;
// Sidebar: section headers, native panel pages (owner 1, page panel + 1),
// the destructive Log out action.
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
// `navigation` copied into `storage` (kUiSettingsNavigationMaximum entries) with the attention dot on Updates, shown
// while an update waits for Restart to update; empty when the list does not fit.
ConstSlice<UiSettingsNavEntry> settings_catalog_navigation_dotted(ConstSlice<UiSettingsNavEntry> navigation,
                                                                   UiSettingsNavEntry* storage) noexcept;

// Dynamic list rows (devices, authorized apps): list binding in bits 8..15,
// row index in bits 0..7.
inline constexpr u64 kSettingsListFirst = 0x7E000000u;
constexpr u64 settings_list_control_id(SettingBinding list, u32 index) noexcept {
    return kSettingsListFirst + (static_cast<u64>(list) << 8) + index;
}
constexpr bool settings_list_control(u64 id) noexcept {
    return id >= kSettingsListFirst && id < kSettingsListFirst + 0x10000;
}
constexpr SettingBinding settings_list_binding(u64 id) noexcept {
    return static_cast<SettingBinding>((id - kSettingsListFirst) >> 8);
}
constexpr u32 settings_list_index(u64 id) noexcept {
    return static_cast<u32>((id - kSettingsListFirst) & 0xff);
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
