#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/model/notifications/soundpack.hpp"
#include "maicord/ui/theme/background_theme.hpp"

namespace maicord {

enum class ClientTheme : u8 {
    dark = 0,
    amoled = 1,
    aurora = 2,
    sunset = 3,
    ocean = 4,
    custom_gradient = 5,
    light = 6,
    custom_light_gradient = 7
};

struct ClientPreferences {
    ClientTheme theme;
    bool reduced_motion;
    bool smooth_scroll;
    bool member_pane_visible;
    bool desktop_enabled;
    bool message_sounds_enabled;
    bool selected_message_sounds_enabled = false;
    u8 output_volume = 100;
    NotificationSoundpack soundpack = NotificationSoundpack::classic;
    BackgroundTheme gradient = background_theme_defaults();
    BackgroundEffectSettings effect_profiles[kBackgroundEffectProfileCount] = {
        background_effect_settings_defaults(BackgroundEffect::stars),
        background_effect_settings_defaults(BackgroundEffect::comets),
        background_effect_settings_defaults(BackgroundEffect::stars_and_comets)};
    bool developer_mode = false;
    // Taskbar attention, independent of desktop notifications.
    bool taskbar_flash = true;
    bool unread_badge = true;
    // Windows integration (Settings > System) and the composer spellchecker.
    // Closing the window hides it to the notification area when enabled.
    bool minimize_to_tray = true;
    // A launch from the startup entry stays out of the way.
    bool start_minimized = false;
    bool spellcheck = true;
    // Opt-in: scan running programs for registered games (Activity privacy).
    bool game_detection = false;
    // The recipient's profile beside a one-to-one DM.
    bool dm_profile_visible = true;
    // Built-in extras. Role colours also on mentions and voice participants;
    // the replied-to message's time in a reply preview; folders
    // as an accordion, closed by Home, or following the selected server; channels
    // the account cannot view listed locked.
    // On by default: client_preferences_defaults() sets it (no nonzero member initializers
    // here, so the client's global stays in .bss).
    bool role_colors_everywhere;
    bool reply_timestamps = false;
    bool folders_close_others = false;
    bool folders_close_on_home = false;
    bool folders_follow_selection = false;
    bool show_hidden_channels = false;
    // Image zoom lens (hold Ctrl over an image, or hold the pointer on one in
    // the viewer): on by default (client_preferences_defaults); zoom in half
    // units (2..100 = 1x..50x) and lens size in tens of DIPs (5..100 = 50..1000).
    bool image_zoom;
    bool image_zoom_square = false;
    u8 image_zoom_factor;
    u8 image_zoom_size;
    // Space between message groups: 0 = default for the display mode,
    // 1..5 = 0, 4, 8, 16, 24 px.
    u8 message_group_spacing = 0;
    // Splitter-resized pane widths in DIPs; 0 keeps the theme's default.
    u16 channel_pane_width = 0;
    u16 member_pane_width = 0;
    // Shared expression drawer width in DIPs; layout clamps it to the current
    // theme minimum and viewport. Zero keeps the theme default.
    u16 expression_picker_width = 0;
    // The last built-in theme chosen, kept while a custom or catalog theme is active: leaving the custom
    // theme (Use built-in, removing it) restores this one.
    ClientTheme builtin_theme = ClientTheme::dark;
    // Accessibility. Chat text size in px (12..24; 0 keeps the theme's body size); how far interface colours are
    // desaturated (100 minus the Saturation slider's percent, so zero is full colour) and the zoom in percent; the
    // text-to-speech rate as an index of the rates 0, 0.25 .. 10 (x1 at 4); reduced motion following the system
    // setting; the saturation also applied to user colours (role colours, banners); links always underlined; how role
    // colours show (ClientRoleStyle). Nonzero defaults come from client_preferences_defaults().
    u8 chat_font_size = 0;
    u8 desaturation = 0;
    u8 zoom;
    u8 speech_rate;
    bool reduced_motion_sync;
    bool desaturate_user_colors = false;
    bool underline_links = false;
    u8 role_style = 0;
    // Streamer Mode: on, turned on while a streaming tool runs (opt-in: it scans running programs every 20 seconds,
    // like game detection), and what it hides or silences.
    bool streamer_mode = false;
    bool streamer_auto = false;
    bool streamer_hide_personal;
    bool streamer_hide_invites;
    bool streamer_disable_sounds;
    bool streamer_disable_notifications;
    bool streamer_hide_window = false;
    // Notifications > "Speak all messages out loud" (ClientSpeakMessages; never by default).
    u8 speak_messages = 0;
    // Developer > Application Test Mode: the application in test mode (0 for none), kept across restarts.
    u64 test_mode_application = 0;
    // Appearance > Layout: channel tabs in the title bar (opt-in).
    bool tabs_enabled = false;
};
// Role colours on names (the default), as a dot next to names, or not at all.
enum ClientRoleStyle : u8 { client_role_style_names, client_role_style_dot, client_role_style_hidden };
// Which messages are read aloud besides /tts ones: none, those of the selected channel, or all.
enum ClientSpeakMessages : u8 { client_speak_messages_never, client_speak_messages_selected, client_speak_messages_all };
inline constexpr u8 kClientPreferencesSpeechRateCount = 41, kClientPreferencesSpeechRateDefault = 4;
inline constexpr u8 kClientPreferencesZoomMinimum = 50, kClientPreferencesZoomMaximum = 200;
// Zoom level's marks (percent), which the slider and Ctrl +/- step through, and Text size in chat's (px).
inline constexpr u8 kClientPreferencesZoomScales[]{50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200};
inline constexpr u8 kClientPreferencesFontSizes[]{12, 14, 15, 16, 18, 20, 24};
// The index of the mark nearest `value` (the first of two equally near).
template <u32 Count>
constexpr u32 client_preferences_nearest_mark(const u8 (&marks)[Count], float value) noexcept {
    u32 best = 0;
    for (u32 i = 1; i < Count; ++i)
        if ((marks[i] > value ? marks[i] - value : value - marks[i]) <
            (marks[best] > value ? marks[best] - value : value - marks[best]))
            best = i;
    return best;
}
// The speech rate (speech.hpp's -10..10, 0 the system's own) of a Text-to-Speech rate index x(index / 4): 10 log3 of
// the factor, as SAPI steps; SAPI tops out at three times as fast.
constexpr i32 client_preferences_speech_rate(u8 index) noexcept {
    constexpr i16 rates[]{-10, -10, -6, -3, 0, 2, 4, 5, 6, 7, 8, 9};
    return index < sizeof(rates) / sizeof(rates[0]) ? rates[index] : 10;
}
// Reduced motion as chosen: the user's switch, or while it follows the computer's setting, that setting.
constexpr bool client_preferences_reduced_motion(const ClientPreferences& preferences, bool system_reduced) noexcept {
    return preferences.reduced_motion || (preferences.reduced_motion_sync && system_reduced);
}
inline constexpr u8 kClientPreferencesGroupSpacingCount = 6;
inline constexpr u16 kClientPreferencesPaneWidthMinimum = 120;
inline constexpr u16 kClientPreferencesPaneWidthMaximum = 1024;
inline constexpr u32 kClientPreferencesVersion = 10;
inline constexpr u32 kClientPreferencesRecordBytes = 96;

enum class ClientPreferencesError : u8 {
    none,
    invalid_input,
    path,
    directory,
    missing,
    open,
    size,
    read,
    format,
    version,
    checksum,
    write,
    flush,
    commit,
};

struct ClientPreferencesResult {
    u32 native_error;
    ClientPreferencesError error;
    constexpr bool ok() const noexcept {
        return error == ClientPreferencesError::none;
    }
};

inline constexpr u8 kClientPreferencesDefaultZoomFactor = 4, kClientPreferencesDefaultZoomSize = 20;
constexpr ClientPreferences client_preferences_defaults() noexcept {
    ClientPreferences preferences{ClientTheme::dark, false, true, true, true, true};
    preferences.role_colors_everywhere = true;
    preferences.image_zoom = true;
    preferences.image_zoom_factor = kClientPreferencesDefaultZoomFactor;
    preferences.image_zoom_size = kClientPreferencesDefaultZoomSize;
    preferences.zoom = 100;
    preferences.speech_rate = kClientPreferencesSpeechRateDefault;
    preferences.reduced_motion_sync = true;
    preferences.streamer_hide_personal = preferences.streamer_hide_invites = true;
    preferences.streamer_disable_sounds = preferences.streamer_disable_notifications = true;
    return preferences;
}
bool client_preferences_valid(const ClientPreferences& preferences) noexcept;
bool client_preferences_equal(const ClientPreferences& a, const ClientPreferences& b) noexcept;

// Bytes 48..56 hold the three effect profiles; 58..59 hold the expression
// drawer width (little-endian u16). Byte 57 holds the last built-in theme + 1;
// zero (records written before it existed) restores the record's theme. Bytes
// 63..64 are reserved and ignored.
// Byte 60 bits: 0 image zoom off, 1 square lens (other bits must be zero);
// byte 61 is the zoom in half units (2..100) and byte 62 the lens size in tens
// of DIPs (5..100); zero in either restores its default.
// Byte 66 bits: 0 taskbar flash off, 1 unread badge off, 2 minimize to tray
// off, 3 start minimized, 4 spellcheck off, 5 game detection on, 6 DM profile
// panel hidden (defaults encode as zero bits).
// Byte 65 bits: 0 role colours everywhere off, 1 reply timestamps on, 2 close
// other folders, 3 Home closes folders, 4 folders follow the selection, 5 show hidden
// channels (defaults encode as zero bits); the other bits must be zero.
// A stored effect id outside the catalog restores "none" with its default
// tuning. Bytes 68..71 hold the two pane widths (little-endian u16; zero in
// records written before they existed, which restores the theme defaults).
// Bytes 72..77: chat text size, desaturation, zoom, speech rate, then flags:
// byte 76 bits 0 reduced motion follows the system, 1 saturation applies to user
// colours, 2 links underlined, 3..4 role style; byte 77 bits 0 Streamer Mode, 1
// automatic, 2 hide personal information, 3 hide invite links, 4 disable sounds,
// 5 disable notifications, 6 hide the window from capture. Byte 78 is Speak all messages out loud
// (ClientSpeakMessages), 79 bit 0 shows the title bar's channel tabs (other bits must be zero), and 80..87 hold
// the test mode application id (little-endian u64).
// Exactly 96 bytes; portable byte encoding, zero reserved fields, accidental
// corruption checksum at 88..95. A version 9 record (80 bytes, checksum at
// 72..79) decodes with the newer fields at their defaults; any other version
// fails with `version` and callers treat it like a missing file.
// No heap allocation.
// Decode preserves output on failure.
ClientPreferencesResult client_preferences_encode(const ClientPreferences& preferences,
                                                  Slice<u8> record) noexcept;
ClientPreferencesResult client_preferences_decode(ConstSlice<u8> record,
                                                  ClientPreferences* preferences) noexcept;

// Load preserves preferences on every failure, including missing/corrupt files.
// Initialize defaults before startup load. Save uses the shared atomic sibling
// commit contract; neither file operation creates parent directories.
ClientPreferencesResult client_preferences_load_file(const wchar_t* path,
                                                     ClientPreferences* preferences) noexcept;
ClientPreferencesResult client_preferences_save_file(const wchar_t* path,
                                                     const ClientPreferences& preferences) noexcept;

// Current Windows user's %LOCALAPPDATA%\MaiCord\preferences.bin. Save creates
// the MaiCord directory if needed; load performs no writes. Call once at startup
// and on explicit preference edits, never from a frame/poll loop.
ClientPreferencesResult client_preferences_load(ClientPreferences* preferences) noexcept;
ClientPreferencesResult client_preferences_save(const ClientPreferences& preferences) noexcept;

} // namespace maicord
