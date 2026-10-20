#include "settings_catalog.hpp"
#include "maicord/app/client_theme.hpp"
#include "maicord/core/decimal.hpp"

namespace maicord {
namespace {
constexpr Utf8View base_themes[]{utf8_literal("Light"), utf8_literal("Dark"), utf8_literal("AMOLED")};
constexpr ClientTheme base_theme_values[]{ClientTheme::light, ClientTheme::dark, ClientTheme::amoled};
constexpr Utf8View color_themes[]{utf8_literal("Aurora"), utf8_literal("Sunset"), utf8_literal("Ocean"),
                                  utf8_literal("Custom"), utf8_literal("Custom light")};
constexpr ClientTheme color_theme_values[]{ClientTheme::aurora, ClientTheme::sunset, ClientTheme::ocean,
                                           ClientTheme::custom_gradient, ClientTheme::custom_light_gradient};
constexpr Utf8View background_effects[]{utf8_literal("None"), utf8_literal("Stars"), utf8_literal("Comets"),
                                        utf8_literal("Stars and comets")};
static_assert(sizeof(background_effects) / sizeof(background_effects[0]) == kBackgroundEffectCount);

// Numeric slider labels: "0°".."360°", "0%".."200%", and for voice
// "−100 dB".."0 dB" (sensitivity) and "0 ms".."2000 ms" in 10 ms steps. They are
// formatted into zeroed storage by the catalog accessors on first use rather
// than shipping ~20 KB of text, views and relocations in the image.
template <u32 Count, u32 Width>
struct NumberLabels {
    char text[Count][Width];
    Utf8View values[Count];
};
NumberLabels<361, 6> degree_labels;
NumberLabels<201, 5> percent_labels;
NumberLabels<101, 10> decibel_labels;
NumberLabels<201, 8> millisecond_labels;

template <u32 Count, u32 Width>
void append_suffix(NumberLabels<Count, Width>& labels, u32 value, u32 at, const char* suffix,
                   u32 suffix_bytes) noexcept {
    for (u32 i = 0; i < suffix_bytes; ++i)
        labels.text[value][at++] = suffix[i];
    labels.values[value] = {labels.text[value], at};
}
void number_labels_initialize() noexcept {
    for (u32 value = 0; value <= 360; ++value)
        append_suffix(degree_labels, value, decimal_write(degree_labels.text[value], value), "°", 2);
    for (u32 value = 0; value <= 200; ++value)
        append_suffix(percent_labels, value, decimal_write(percent_labels.text[value], value), "%", 1);
    for (u32 value = 0; value <= 100; ++value) {
        const u32 db = 100 - value;
        char* text = decibel_labels.text[value];
        u32 at = 0;
        if (db) {
            constexpr char minus[] = "\xE2\x88\x92";
            for (u32 i = 0; i < 3; ++i)
                text[at++] = minus[i];
        }
        append_suffix(decibel_labels, value, at + decimal_write(text + at, db), " dB", 3);
    }
    for (u32 value = 0; value <= 200; ++value)
        append_suffix(millisecond_labels, value, decimal_write(millisecond_labels.text[value], value * 10),
                      " ms", 3);
}

using K = UiSettingsControlKind;
constexpr u32 none = kUiSettingsNoChoice;
constexpr u16 dynamic = ui_settings_schema_dynamic, hidden = ui_settings_schema_search_hidden;
constexpr u32 bind(SettingBinding binding) noexcept {
    return static_cast<u32>(binding);
}
constexpr Utf8View message_displays[]{utf8_literal("Default"), utf8_literal("Compact")};
constexpr Utf8View group_spacings[]{utf8_literal("0px"), utf8_literal("4px"), utf8_literal("8px"),
                                    utf8_literal("16px"), utf8_literal("24px")};
constexpr Utf8View spoiler_choices[]{utf8_literal("On click"), utf8_literal("Always"),
                                     utf8_literal("On servers I moderate")};
constexpr Utf8View sticker_animations[]{
    utf8_literal("Always animate"), utf8_literal("Animate on interaction"), utf8_literal("Never animate")};
constexpr Utf8View update_channels[]{utf8_literal("Stable"), utf8_literal("Preview")};
constexpr Utf8View update_intervals[]{utf8_literal("Every hour"), utf8_literal("Every 4 hours"),
                                      utf8_literal("Every 12 hours"), utf8_literal("Once a day")};
constexpr Utf8View explicit_choices[]{utf8_literal("Show"), utf8_literal("Blur"), utf8_literal("Block")};
constexpr Utf8View spam_choices[]{utf8_literal("Filter all spam"),
                                  utf8_literal("Filter messages from non-friends (Recommended)"),
                                  utf8_literal("Don't filter spam")};
constexpr Utf8View visibility_choices[]{utf8_literal("Friends & All Servers"),
                                        utf8_literal("Friends & Small Servers Only"),
                                        utf8_literal("Friends Only")};
constexpr Utf8View time_formats[]{utf8_literal("Auto"), utf8_literal("12-hour"), utf8_literal("24-hour")};
constexpr Utf8View activity_server_choices[]{utf8_literal("Share in all servers"),
                                             utf8_literal("Share in servers with 200 or fewer members"),
                                             utf8_literal("Do not share in any servers")};

enum class C : u8 {
    none,
    base_themes,
    color_themes,
    background_effects,
    degrees,
    percent_100,
    percent_200,
    spoilers,
    message_displays,
    group_spacings,
    sticker_animations,
    update_channels,
    update_intervals,
    decibels,
    milliseconds,
    explicit_content,
    explicit_guilds,
    spam,
    visibility,
    time_formats,
    activity_servers,
    count
};
constexpr ConstSlice<Utf8View> choice_sets[]{
    {},
    {base_themes, 3},
    {color_themes, 5},
    {background_effects, kBackgroundEffectCount},
    {degree_labels.values, 361},
    {percent_labels.values, 101},
    {percent_labels.values, 201},
    {spoiler_choices, 3},
    {message_displays, 2},
    {group_spacings, 5},
    {sticker_animations, 3},
    {update_channels, 2},
    {update_intervals, 4},
    {decibel_labels.values, 101},
    {millisecond_labels.values, 201},
    {explicit_choices, 3},
    {explicit_choices, 2},
    {spam_choices, 3},
    {visibility_choices, 3},
    {time_formats, 3},
    {activity_server_choices, 3},
};
static_assert(sizeof(choice_sets) / sizeof(choice_sets[0]) == static_cast<u32>(C::count));

// A node's key, label, description, keywords and action label, NUL-separated
// in one literal: one pointer per node instead of five views, expanded into
// UiSettingsSchemaNode storage on first use.
#define SETTING_TEXT(key, label, description, keywords, action)                                            \
    key "\0" label "\0" description "\0" keywords "\0" action
struct PackedNode {
    const char* text;
    K kind;
    C choices;
    u8 variant;
    u16 flags;
    u16 default_choice;
    u16 binding;
};
constexpr u16 kPackedNoChoice = 0xffff;
constexpr PackedNode node(const char* text, K kind, SettingBinding binding = SettingBinding::none,
                          C choices = C::none, u32 default_choice = none, u16 flags = 0,
                          u8 variant = 0) noexcept {
    return {text,
            kind,
            choices,
            variant,
            flags,
            static_cast<u16>(default_choice == none ? kPackedNoChoice : default_choice),
            static_cast<u16>(binding)};
}

constexpr PackedNode notifications[]{
    node(SETTING_TEXT("notifications.overview", "Overview", "", "", ""), K::category),
    node(
        SETTING_TEXT(
            "notifications.desktop", "Enable Desktop Notifications",
            "If you're looking for per-channel or per-server notifications, right-click the desired server "
            "icon and select Notification Settings.",
            "alerts toast popup banner", ""),
        K::toggle, SettingBinding::desktop_notifications),
    node(SETTING_TEXT("notifications.setup", "Set up desktop notifications", "",
                      "permission enable windows", "Set up"),
         K::action, SettingBinding::notification_setup),
    node(SETTING_TEXT("notifications.windows", "Windows notification settings",
                      "Manage notification permissions in Windows.", "system permission focus",
                      "Open settings"),
         K::action, SettingBinding::windows_notification_settings),
    node(SETTING_TEXT("notifications.taskbar_flash", "Enable Taskbar Flashing",
                      "Flashes the app in your taskbar when you have new notifications.",
                      "taskbar flash attention blink", ""),
         K::toggle, SettingBinding::taskbar_flash),
    node(SETTING_TEXT("notifications.sounds", "Sounds", "", "", ""), K::category),
    node(SETTING_TEXT("notifications.message_sounds", "New Message", "Play a sound for new messages.",
                      "audio ding chime", ""),
         K::toggle, SettingBinding::message_sounds),
    node(SETTING_TEXT("notifications.selected_channel_sounds",
                      "New Message in the channel I\xE2\x80\x99m currently reading", "",
                      "audio focused current channel", ""),
         K::toggle, SettingBinding::selected_channel_sounds),
    node(SETTING_TEXT("notifications.volume", "Sound volume", "Volume of notification sounds.",
                      "loudness audio output", ""),
         K::slider, SettingBinding::output_volume, C::percent_200, 100),
    node(SETTING_TEXT("notifications.badges", "Badges", "", "", ""), K::category),
    node(SETTING_TEXT("notifications.unread_badge", "Enable Unread Message Badge",
                      "Shows a red badge on the app icon when you have unread messages.",
                      "taskbar overlay count dot", ""),
         K::toggle, SettingBinding::unread_badge),
};

constexpr PackedNode appearance[]{
    node(SETTING_TEXT("appearance.theme", "Theme", "Choose a light or dark look for MaiCord.",
                      "dark mode light mode amoled black", ""),
         K::palette, SettingBinding::theme_base, C::base_themes),
    node(SETTING_TEXT("appearance.color_theme", "Color themes",
                      "Gradient themes tint the background behind every surface.",
                      "gradient colour color aurora sunset ocean", ""),
         K::palette, SettingBinding::theme_color, C::color_themes),
    node(SETTING_TEXT("appearance.customize", "Customize theme",
                      "Edit colors, the gradient and background effects.", "editor colors accent",
                      "Customize"),
         K::action, SettingBinding::customize_theme),
    node(SETTING_TEXT("appearance.background", "Background", "", "", ""), K::category),
    node(SETTING_TEXT("appearance.background_effect", "Background effect",
                      "Animated effect drawn behind gradient themes.", "stars comets animation", ""),
         K::choice, SettingBinding::background_effect, C::background_effects),
    node(SETTING_TEXT("appearance.gradient_direction", "Gradient direction",
                      "Angle of the background gradient.", "angle rotation degrees", ""),
         K::slider, SettingBinding::gradient_direction, C::degrees),
    node(SETTING_TEXT("appearance.gradient_intensity", "Gradient intensity",
                      "How strongly the gradient shows behind content.", "strength opacity", ""),
         K::slider, SettingBinding::gradient_intensity, C::percent_100),
    node(SETTING_TEXT("appearance.imported", "Imported theme", "", "custom theme file import", ""),
         K::category, SettingBinding::none, C::none, none, dynamic),
    node(SETTING_TEXT("appearance.imported.import", "Current theme", "", "custom theme file import",
                      "Import theme…"),
         K::action, SettingBinding::custom_import, C::none, none, dynamic),
    node(SETTING_TEXT("appearance.imported.installed", "Installed themes",
                      "Theme files in the MaiCord themes folder. Choose one to preview it.",
                      "custom theme catalog installed folder json", ""),
         K::choice, SettingBinding::custom_installed, C::none, none, dynamic),
    node(SETTING_TEXT("appearance.imported.preview", "Imported theme", "", "", "Preview theme"), K::action,
         SettingBinding::custom_preview, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("appearance.imported.apply", "Imported theme", "", "", "Apply theme"), K::action,
         SettingBinding::custom_apply, C::none, none, dynamic | hidden, 1),
    node(SETTING_TEXT("appearance.imported.cancel", "Discard preview", "", "", "Cancel"), K::action,
         SettingBinding::custom_cancel, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("appearance.imported.builtin", "Built-in appearance",
                      "Keep the imported theme available for later.", "", "Use built-in"),
         K::action, SettingBinding::custom_builtin, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("appearance.imported.restore", "Saved imported theme", "", "", "Restore imported"),
         K::action, SettingBinding::custom_restore, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("appearance.imported.retry_read", "Couldn't read theme file", "", "", "Try again"),
         K::action, SettingBinding::custom_retry_read, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("appearance.imported.retry_save", "Couldn't save theme", "", "", "Try again"),
         K::action, SettingBinding::custom_retry_save, C::none, none, dynamic | hidden),
    // Messages and Chat Box read text_and_images (settings-proto/1).
    node(SETTING_TEXT("appearance.messages", "Messages", "Show images, videos, and lolcats...", "", ""),
         K::category),
    node(SETTING_TEXT("appearance.synced_notice", "Synced settings", "", "", ""), K::notice,
         SettingBinding::chat_notice, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("appearance.inline_embed_media", "When posted as links to chat", "",
                      "images videos gifs links preview inline media", ""),
         K::toggle, SettingBinding::inline_embed_media, C::none, none, dynamic),
    node(SETTING_TEXT("appearance.inline_attachment_media", "When uploaded directly to Discord", "",
                      "images videos gifs attachments uploads inline", ""),
         K::toggle, SettingBinding::inline_attachment_media, C::none, none, dynamic),
    node(SETTING_TEXT("appearance.render_embeds", "Show embeds and link previews", "",
                      "embeds link previews websites cards", ""),
         K::toggle, SettingBinding::render_embeds, C::none, none, dynamic),
    node(SETTING_TEXT("appearance.render_reactions", "Show emoji reactions", "", "reactions emoji", ""),
         K::toggle, SettingBinding::render_reactions, C::none, none, dynamic),
    node(SETTING_TEXT("appearance.render_spoilers", "Show spoiler content", "", "spoilers hidden reveal",
                      ""),
         K::choice, SettingBinding::render_spoilers, C::spoilers, none, dynamic),
    node(SETTING_TEXT("appearance.chat_box", "Chat Box", "", "", ""), K::category),
    node(SETTING_TEXT("appearance.convert_emoticons",
                      "Automatically convert emoticons in your messages to emoji",
                      "For example, when you type :) MaiCord will convert it to 🙂",
                      "emoticons smiley emoji convert", ""),
         K::toggle, SettingBinding::convert_emoticons, C::none, none, dynamic),
    node(SETTING_TEXT("appearance.spellcheck", "Spellcheck", "Underline misspelled words as you type.",
                      "spelling dictionary typo", ""),
         K::toggle, SettingBinding::spellcheck),
    node(SETTING_TEXT("appearance.layout", "Layout", "", "", ""), K::category),
    node(SETTING_TEXT("appearance.member_list", "Show member list",
                      "Show the member list beside server channels.", "members users sidebar", ""),
         K::toggle, SettingBinding::member_list),
};

constexpr PackedNode accessibility[]{
    node(SETTING_TEXT("accessibility.synced_notice", "Synced settings", "", "", ""), K::notice,
         SettingBinding::accessibility_notice, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("accessibility.density", "Visual Density", "", "", ""), K::category),
    node(SETTING_TEXT("accessibility.message_display", "Chat Message Display",
                      "Change the appearance of chat messages.", "compact cozy density chat", ""),
         K::radio, SettingBinding::message_display, C::message_displays, 0, dynamic),
    node(SETTING_TEXT("accessibility.group_spacing", "Space Between Message Groups",
                      "Adjust the spacing between message groups.", "spacing gap padding density", ""),
         K::slider, SettingBinding::group_spacing, C::group_spacings, none, ui_settings_schema_marks),
    node(SETTING_TEXT("accessibility.motion", "Reduced Motion", "", "", ""), K::category),
    node(SETTING_TEXT(
             "accessibility.reduced_motion", "Enable Reduced Motion",
             "Reduce the amount and intensity of animations, hover effects, and other moving effects.",
             "animation reduce motion gif", ""),
         K::toggle, SettingBinding::reduced_motion),
    node(SETTING_TEXT("accessibility.smooth_scroll", "Smooth scrolling", "Animate mouse-wheel scrolling.",
                      "scroll wheel", ""),
         K::toggle, SettingBinding::smooth_scroll),
    node(SETTING_TEXT("accessibility.gif_autoplay", "Play GIFs when MaiCord is focused", "",
                      "gif autoplay animation video", ""),
         K::toggle, SettingBinding::gif_autoplay, C::none, none, dynamic),
    node(SETTING_TEXT("accessibility.animate_emoji", "Play animated emoji", "", "emoji animation animated",
                      ""),
         K::toggle, SettingBinding::animate_emoji, C::none, none, dynamic),
    node(SETTING_TEXT("accessibility.animate_stickers", "Play sticker animations", "", "stickers animation",
                      ""),
         K::radio, SettingBinding::animate_stickers, C::sticker_animations, none, dynamic),
};

constexpr PackedNode advanced[]{
    node(SETTING_TEXT(
             "advanced.developer_mode", "Developer Mode",
             "Developer Mode exposes context menu items helpful for people writing apps using the Discord "
             "API, such as Copy ID.",
             "copy id snowflake debug bot", ""),
         K::toggle, SettingBinding::developer_mode),
    node(SETTING_TEXT("advanced.preferences_retry", "Couldn't save settings",
                      "Your latest changes are applied but not saved yet.", "", "Try again"),
         K::notice, SettingBinding::preferences_retry, C::none, none, dynamic | hidden, 2),
};

constexpr PackedNode updates[]{
    node(SETTING_TEXT("updates.status", "Check for updates", "",
                      "update upgrade version new release download restart", "Check now"),
         K::action, SettingBinding::update_status, C::none, none, dynamic),
    node(SETTING_TEXT("updates.problem", "Update problem", "", "", ""), K::notice,
         SettingBinding::update_problem, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("updates.available", "Available update", "",
                      "download size release date verified signature", ""),
         K::action, SettingBinding::update_available, C::none, none, dynamic),
    node(SETTING_TEXT("updates.channel", "Update channel",
                      "Preview builds get new features first and may be less stable.",
                      "beta preview stable release channel master nightly", ""),
         K::radio, SettingBinding::update_channel, C::update_channels, none, dynamic),
    node(SETTING_TEXT(
             "updates.interval", "Check for updates every", "MaiCord also checks shortly after it starts.",
             "frequency interval schedule automatic background hourly daily hour day often how often", ""),
         K::radio, SettingBinding::update_interval, C::update_intervals, none, dynamic),
    node(SETTING_TEXT("updates.rollback", "Roll back to previous version", "",
                      "downgrade revert undo previous version", "Roll back"),
         K::action, SettingBinding::update_rollback, C::none, none, dynamic),
    node(SETTING_TEXT("updates.assets", "What updates include",
                      "Updates replace the app and its account module. Download a fresh copy to get new "
                      "fonts and licenses.",
                      "", ""),
         K::notice, SettingBinding::update_notice, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("updates.changelog", "What's new", "",
                      "changelog release notes changes commits what's new", ""),
         K::category, SettingBinding::update_changelog, C::none, none, dynamic),
};

constexpr PackedNode voice_nodes[]{
    node(SETTING_TEXT("voice.devices", "Voice", "", "", ""), K::category),
    node(SETTING_TEXT("voice.input_device", "Microphone", "The microphone used in voice channels.",
                      "microphone mic input headset", ""),
         K::choice, SettingBinding::voice_input_device, C::none, none, dynamic),
    node(SETTING_TEXT("voice.output_device", "Speaker", "Speakers or headphones for voice channels.",
                      "speakers headphones output audio", ""),
         K::choice, SettingBinding::voice_output_device, C::none, none, dynamic),
    node(SETTING_TEXT("voice.input_volume", "Microphone Volume", "", "microphone gain loudness input", ""),
         K::slider, SettingBinding::voice_input_volume, C::percent_200, 100),
    node(SETTING_TEXT("voice.output_volume", "Speaker Volume",
                      "Volume of everyone you hear in voice channels.", "speakers loudness output", ""),
         K::slider, SettingBinding::voice_output_volume, C::percent_200, 100),
    node(SETTING_TEXT("voice.input", "Input", "", "", ""), K::category),
    node(SETTING_TEXT("voice.input_mode", "Push to Talk",
                      "Activates your mic only when you hold down a designated key.",
                      "push to talk ptt voice activity vad input mode", ""),
         K::toggle, SettingBinding::voice_input_mode),
    node(SETTING_TEXT("voice.auto_sensitivity", "Automatically Adjust Input Sensitivity",
                      "Follow your background noise to tell when you're speaking.",
                      "vad threshold automatic", ""),
         K::toggle, SettingBinding::voice_auto_sensitivity, C::none, none, dynamic),
    node(SETTING_TEXT("voice.sensitivity", "Input Sensitivity",
                      "Sound above the marker is sent. The bar shows your microphone level.",
                      "vad threshold level meter mic test", ""),
         K::slider, SettingBinding::voice_sensitivity, C::decibels, none, dynamic),
    node(SETTING_TEXT("voice.push_to_talk_key", "Push to Talk Keybind",
                      "Hold this key to talk, even while MaiCord is in the background.",
                      "ptt keybind hotkey key shortcut", "Record keybind"),
         K::action, SettingBinding::voice_push_to_talk_key, C::none, none, dynamic),
    node(SETTING_TEXT("voice.push_to_talk_release", "Push to Talk Release Delay",
                      "Keep sending briefly after you let go.", "ptt delay", ""),
         K::slider, SettingBinding::voice_push_to_talk_release, C::milliseconds, 2, dynamic),
    node(SETTING_TEXT("voice.processing_category", "Voice processing", "", "", ""), K::category),
    node(SETTING_TEXT("voice.processing", "Echo and noise reduction",
                      "Let Windows cancel echo, suppress background noise and even out your volume.",
                      "echo cancellation noise suppression automatic gain control agc", ""),
         K::toggle, SettingBinding::voice_processing),
    node(SETTING_TEXT("voice.soundboard_category", "Soundboard", "", "", ""), K::category),
    node(SETTING_TEXT("voice.soundboard_volume", "Soundboard Volume",
                      "Control how loud sounds are for you personally.",
                      "soundboard sounds effects loudness", ""),
         K::slider, SettingBinding::voice_soundboard_volume, C::percent_100, 100),
    node(SETTING_TEXT("voice.soundboard_muted", "Mute soundboard",
                      "Don't play sounds other people send. You can still use the soundboard.",
                      "soundboard sounds silence mute", ""),
         K::toggle, SettingBinding::voice_soundboard_muted),
};

constexpr PackedNode account_nodes[]{
    node(SETTING_TEXT("account.card", "Account", "", "avatar profile", "Edit profile"), K::action,
         SettingBinding::account_card, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("account.notice", "Account details", "", "", "Try again"), K::notice,
         SettingBinding::account_notice, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("account.details", "Account Info", "", "", ""), K::category),
    node(SETTING_TEXT("account.username", "Username", "", "handle name", "Edit"), K::action,
         SettingBinding::account_username, C::none, none, dynamic),
    node(SETTING_TEXT("account.email", "Email", "", "mail address", "Reveal"), K::action,
         SettingBinding::account_email, C::none, none, dynamic),
    node(SETTING_TEXT("account.phone", "Phone Number", "", "sms mobile", "Reveal"), K::action,
         SettingBinding::account_phone, C::none, none, dynamic),
    node(SETTING_TEXT("account.security", "Password & Security", "", "", ""), K::category),
    node(SETTING_TEXT("account.password", "Password", "Changes open Discord in your browser.",
                      "change password security", "Change"),
         K::action, SettingBinding::account_password, C::none, none, dynamic),
    node(SETTING_TEXT("account.mfa", "Multi-Factor Authentication", "",
                      "2fa mfa authenticator security two-factor", ""),
         K::notice, SettingBinding::account_mfa, C::none, none, dynamic),
};
constexpr PackedNode profile_nodes[]{
    node(SETTING_TEXT("profiles.notice", "Profile", "", "", "Try again"), K::notice,
         SettingBinding::profile_notice, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("profiles.display_name", "Display Name", "", "global name nickname", "Edit"),
         K::action, SettingBinding::profile_display_name, C::none, none, dynamic),
    node(SETTING_TEXT("profiles.pronouns", "Pronouns", "", "", "Edit"), K::action,
         SettingBinding::profile_pronouns, C::none, none, dynamic),
    node(SETTING_TEXT("profiles.bio", "Bio", "", "about me description", "Edit"), K::action,
         SettingBinding::profile_bio, C::none, none, dynamic),
};
constexpr PackedNode device_nodes[]{
    node(SETTING_TEXT("devices.notice", "Devices", "", "", "Try again"), K::notice,
         SettingBinding::devices_notice, C::none, none, dynamic | hidden),
    node(SETTING_TEXT(
             "devices.list", "Logged-in Devices",
             "These are all the devices that are currently logged-in with your Discord account. Log out of "
             "devices that you don't recognize.",
             "sessions logged in log out", ""),
         K::category, SettingBinding::devices_list),
};
constexpr PackedNode content_nodes[]{
    node(SETTING_TEXT("content.notice", "Synced settings", "", "", ""), K::notice,
         SettingBinding::user_settings_notice, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("content.media", "Content Filters",
                      "Show, blur or block media that may be sexually explicit.", "", ""),
         K::category),
    node(SETTING_TEXT("content.explicit_friends", "Direct messages from friends", "",
                      "explicit nsfw blur filter sensitive media", ""),
         K::choice, SettingBinding::explicit_friends, C::explicit_content, none, dynamic),
    node(SETTING_TEXT("content.explicit_others", "Direct messages from others", "",
                      "explicit nsfw blur filter sensitive media", ""),
         K::choice, SettingBinding::explicit_others, C::explicit_content, none, dynamic),
    node(SETTING_TEXT("content.explicit_guilds", "Messages in server channels", "",
                      "explicit nsfw blur filter sensitive media", ""),
         K::choice, SettingBinding::explicit_guilds, C::explicit_guilds, none, dynamic),
    node(SETTING_TEXT(
             "content.nsfw_commands", "Allow access to age-restricted commands from apps in DMs",
             "Allows people 18+ to access commands marked as age-restricted in DMs. Applies to all apps.",
             "nsfw apps commands", ""),
         K::toggle, SettingBinding::nsfw_commands, C::none, none, dynamic),
    node(SETTING_TEXT("content.nsfw_guilds", "Allow access to age-restricted servers on iOS",
                      "Access age-restricted servers (18+) on iOS devices, after joining them on desktop.",
                      "nsfw servers ios", ""),
         K::toggle, SettingBinding::nsfw_guilds, C::none, none, dynamic),
    node(SETTING_TEXT("content.spam_filters", "Spam Filters", "", "", ""), K::category),
    node(
        SETTING_TEXT(
            "content.spam", "Automatically filter suspected spam messages",
            "Discord can filter out some messages that contain spam. These messages go to your Spam inbox.",
            "junk filter dm spam", ""),
        K::radio, SettingBinding::dm_spam_filter, C::spam, none, dynamic),
    node(SETTING_TEXT("content.dms", "Direct Message (DM) Permissions", "", "", ""), K::category),
    node(SETTING_TEXT("content.dm_from_members", "Direct Messages",
                      "Allow DMs from other server members. Applies to servers you join from now on.",
                      "direct messages privacy dms", ""),
         K::toggle, SettingBinding::dm_from_members, C::none, none, dynamic),
    node(SETTING_TEXT(
             "content.message_requests", "Message requests",
             "Filter messages from server members you may not know. Applies to servers you join from now "
             "on.",
             "dm requests", ""),
         K::toggle, SettingBinding::message_requests, C::none, none, dynamic),
    node(SETTING_TEXT("content.friend_requests", "Friend Request Permissions",
                      "Allow Friend Requests from...", "", ""),
         K::category),
    node(SETTING_TEXT("content.friend_everyone", "Everyone", "", "friend requests anyone", ""), K::toggle,
         SettingBinding::friend_everyone, C::none, none, dynamic),
    node(SETTING_TEXT("content.friend_mutual_friends", "Friend of friends", "", "friend requests mutual",
                      ""),
         K::toggle, SettingBinding::friend_mutual_friends, C::none, none, dynamic),
    node(SETTING_TEXT(
             "content.friend_mutual_guilds", "Server members",
             "Server members can only send you friend requests from servers where you also allow Direct "
             "Messages.",
             "friend requests mutual servers", ""),
         K::toggle, SettingBinding::friend_mutual_guilds, C::none, none, dynamic),
    node(
        SETTING_TEXT(
            "content.friend_request_notes", "Show personalized messages",
            "Show personalized messages on incoming friend requests. If you accept, the message will still "
            "appear in your DMs.",
            "friend request notes message", ""),
        K::toggle, SettingBinding::friend_request_notes, C::none, none, dynamic),
};
constexpr PackedNode privacy_nodes[]{
    node(SETTING_TEXT("privacy.notice", "Synced settings", "", "", ""), K::notice,
         SettingBinding::privacy_notice, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("privacy.data", "How Discord Uses My Data", "", "", ""), K::category),
    node(SETTING_TEXT(
             "privacy.sponsored_activity", "Use my Discord activity to personalize Sponsored Content",
             "Allows Discord to personalize Sponsored Content, like Quests, using your Discord activity, "
             "such "
             "as the games you play. If you opt out you may still see Quests, but they won\xE2\x80\x99t be "
             "personalized.",
             "quests sponsored personalize data", ""),
         K::toggle, SettingBinding::sponsored_activity, C::none, none, dynamic),
    node(SETTING_TEXT("privacy.sponsored_third_party",
                      "Use third-party data to personalize Sponsored Content",
                      "Allows Discord to personalize Sponsored Content, like Quests, using data Discord "
                      "receives from "
                      "advertisers and third-party data providers. If you opt out, you may still see "
                      "Quests, but they "
                      "won\xE2\x80\x99t be personalized.",
                      "quests sponsored advertisers third party data", ""),
         K::toggle, SettingBinding::sponsored_third_party, C::none, none, dynamic),
    node(SETTING_TEXT(
             "privacy.profile", "Profile Privacy",
             "Control who can see your profile info \xE2\x80\x94 like your bio and connected accounts.", "",
             ""),
         K::category),
    node(SETTING_TEXT("privacy.profile_visibility", "Share my full profile with",
                      "Small servers have 200 or fewer members. Everyone else sees a limited version.",
                      "privacy profile visibility", ""),
         K::radio, SettingBinding::profile_visibility, C::visibility, none, dynamic),
    node(SETTING_TEXT("privacy.profile_updates", "Share when I update my profile",
                      "Allow friends to receive a push notification when you update your profile.",
                      "notify friends profile updates", ""),
         K::toggle, SettingBinding::profile_updates, C::none, none, dynamic),
};
constexpr PackedNode app_nodes[]{
    node(SETTING_TEXT("apps.notice", "Authorized Apps", "", "", "Try again"), K::notice,
         SettingBinding::apps_notice, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("apps.list", "Authorized Apps",
                      "These include third-party services you've given permission to access your Discord "
                      "account. You "
                      "can review what each app can do and revoke access anytime.",
                      "oauth applications tokens connected", ""),
         K::category, SettingBinding::apps_list),
};
constexpr PackedNode language_nodes[]{
    node(SETTING_TEXT("language.notice", "Synced settings", "", "", ""), K::notice,
         SettingBinding::language_notice, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("language.time", "Time", "", "", ""), K::category),
    node(SETTING_TEXT("language.time_format", "Time format", "Auto follows your Windows region settings.",
                      "clock 12 24 hour am pm", ""),
         K::radio, SettingBinding::time_format, C::time_formats, 0, dynamic),
};

constexpr PackedNode registered_game_nodes[]{
    node(
        SETTING_TEXT("games.detection", "Detect games",
                     "Check running programs every 20 seconds. Program names stay on this device; only the "
                     "game you're playing is shared.",
                     "game detection running programs processes registered games", ""),
        K::toggle, SettingBinding::game_detection),
    node(SETTING_TEXT("games.added", "Added Games", "Games MaiCord recognizes while they run.",
                      "registered games added detected", ""),
         K::category, SettingBinding::registered_games),
    node(SETTING_TEXT("games.add", "Add a game",
                      "Pick a running program to show as the game you're playing.",
                      "add game not detected program", ""),
         K::choice, SettingBinding::add_game, C::none, none, dynamic),
};

constexpr PackedNode keybind_nodes[]{
    node(SETTING_TEXT(
             "keybinds.custom", "Custom Keybinds",
             "Keybinds are custom keyboard shortcuts that let you perform specific actions in MaiCord "
             "without needing to click through menus. Voice keybinds also work in the background during a "
             "call.",
             "hotkeys global keyboard", ""),
         K::category, SettingBinding::keybinds_custom),
    node(SETTING_TEXT("keybinds.add", "Add a keybind", "Choose a voice action, then press the keys to use.",
                      "new hotkey shortcut push to talk ptt mute deafen", "Add keybind"),
         K::action, SettingBinding::keybinds_add, C::none, none, dynamic,
         static_cast<u8>(UiSettingsVariant::primary)),
    node(SETTING_TEXT("keybinds.defaults", "Default Keybinds",
                      "Built-in shortcuts can't be changed. Press Ctrl+/ anywhere to see them.",
                      "keyboard shortcuts hotkeys combos", ""),
         K::notice, SettingBinding::keybinds_defaults, C::none, none, dynamic),
};

constexpr PackedNode system_nodes[]{
    node(SETTING_TEXT("system.startup", "General", "", "", ""), K::category),
    node(SETTING_TEXT("system.open_on_startup", "Automatically open MaiCord when your computer starts up",
                      "", "boot login autostart run startup", ""),
         K::toggle, SettingBinding::open_on_startup, C::none, none, dynamic),
    node(SETTING_TEXT("system.start_minimized", "Start MaiCord minimized",
                      "Start MaiCord in the background, out of your way.", "boot background hidden tray",
                      ""),
         K::toggle, SettingBinding::start_minimized, C::none, none, dynamic),
    node(SETTING_TEXT("system.minimize_to_tray", "Minimize MaiCord to System Tray",
                      "Clicking X minimizes MaiCord instead of closing it completely.",
                      "system tray notification area close exit background", ""),
         K::toggle, SettingBinding::minimize_to_tray),
    node(SETTING_TEXT("system.links", "Links", "", "", ""), K::category),
    node(SETTING_TEXT("system.protocol_handler", "Open discord:// links in MaiCord",
                      "Invite and message links from your browser and other apps open here.",
                      "protocol url handler invite browser default", ""),
         K::toggle, SettingBinding::protocol_handler, C::none, none, dynamic),
};

constexpr PackedNode activity_nodes[]{
    node(SETTING_TEXT("activity.notice", "Synced settings", "", "", ""), K::notice,
         SettingBinding::activity_notice, C::none, none, dynamic | hidden),
    node(SETTING_TEXT("activity.status", "What Activity I Share", "", "", ""), K::category),
    node(SETTING_TEXT(
             "activity.share", "Share my activity",
             "Share activity information from games and connected apps I use, including when and how I "
             "engage.",
             "activity status playing game rich presence privacy", ""),
         K::toggle, SettingBinding::share_activity, C::none, none, dynamic),
    node(SETTING_TEXT("activity.share_online", "Share when I come online",
                      "Allow friends to receive a push notification when you come online.",
                      "online notify friends", ""),
         K::toggle, SettingBinding::share_online, C::none, none, dynamic),
    node(SETTING_TEXT(
             "activity.share_streaming", "Share when I start streaming",
             "Allow members that you interact with the most to get a notification when you start streaming "
             "in small servers.",
             "go live stream notify", ""),
         K::toggle, SettingBinding::share_streaming, C::none, none, dynamic),
    node(SETTING_TEXT("activity.servers_category", "Where I Share Activity", "", "", ""), K::category),
    node(SETTING_TEXT("activity.servers", "Automatically share my activity when joining a server",
                      "Your activity is always shared with your friends.",
                      "activity servers status privacy", ""),
         K::radio, SettingBinding::activity_servers, C::activity_servers, none, dynamic),
    node(SETTING_TEXT("activity.joining", "Who Can Join My Games",
                      "Some games allow users to join the game you're playing when viewing your activity.",
                      "", ""),
         K::category),
    node(SETTING_TEXT("activity.join_friends", "Allow friends to join my game",
                      "Allow friends to join my game without sending a request.",
                      "join game party invite friends", ""),
         K::toggle, SettingBinding::join_friends, C::none, none, dynamic),
    node(SETTING_TEXT("activity.join_voice", "Allow voice channel participants to join my game",
                      "Allow anyone in a shared voice channel to join my game without sending a request. "
                      "This feature "
                      "only works in non-Community enabled servers.",
                      "join game party voice channel", ""),
         K::toggle, SettingBinding::join_voice, C::none, none, dynamic),
};

#undef SETTING_TEXT

// Packed nodes per panel, in SettingsPanel order, and their expanded storage.
template <u32 Count> constexpr ConstSlice<PackedNode> packed(const PackedNode (&nodes)[Count]) noexcept {
    return {nodes, Count};
}
constexpr ConstSlice<PackedNode> packed_panels[]{
    packed(notifications), packed(appearance),     packed(accessibility),         {},
    packed(advanced),      packed(updates),        packed(voice_nodes),           packed(account_nodes),
    packed(profile_nodes), packed(device_nodes),   packed(content_nodes),         packed(privacy_nodes),
    packed(app_nodes),     packed(language_nodes), packed(registered_game_nodes), packed(keybind_nodes),
    packed(system_nodes),  packed(activity_nodes),
};
static_assert(sizeof(packed_panels) / sizeof(packed_panels[0]) == kSettingsPanelCount);
constexpr u32 node_offset(u32 panel) noexcept {
    u32 offset = 0;
    for (u32 i = 0; i < panel; ++i)
        offset += packed_panels[i].count;
    return offset;
}
UiSettingsSchemaNode expanded_nodes[node_offset(kSettingsPanelCount)];
bool expanded_ready;
constexpr ConstSlice<UiSettingsSchemaNode> expanded(SettingsPanel panel) noexcept {
    const u32 index = static_cast<u32>(panel);
    return {expanded_nodes + node_offset(index), packed_panels[index].count};
}
void expand_node(const PackedNode& source, UiSettingsSchemaNode* output) noexcept {
    Utf8View text[5];
    const char* cursor = source.text;
    for (auto& field : text) {
        u32 bytes = 0;
        while (cursor[bytes])
            ++bytes;
        field = bytes ? Utf8View{cursor, bytes} : Utf8View{};
        cursor += bytes + 1;
    }
    *output = {text[0],
               text[1],
               text[2],
               text[3],
               source.kind,
               source.variant,
               source.flags,
               choice_sets[static_cast<u32>(source.choices)],
               source.default_choice == kPackedNoChoice ? none : source.default_choice,
               source.binding,
               text[4]};
}
// Slider labels and expanded nodes are built on first use of the catalog.
void catalog_initialize() noexcept {
    if (expanded_ready)
        return;
    number_labels_initialize();
    UiSettingsSchemaNode* output = expanded_nodes;
    for (const auto& panel : packed_panels)
        for (u32 i = 0; i < panel.count; ++i)
            expand_node(panel[i], output++);
    expanded_ready = true;
}

constexpr UiSettingsSchemaSection sections[]{{utf8_literal("GENERAL")},
                                             {utf8_literal("EXPERIENCE")},
                                             {utf8_literal("GAMES & APPS")},
                                             {utf8_literal("MAICORD")},
                                             {{}}};
constexpr UiSettingsSchemaPanel panels[]{
    {utf8_literal("notifications"), utf8_literal("Notifications"), utf8_literal("alerts sounds"),
     UiIcon::bell, 0, expanded(SettingsPanel::notifications)},
    {utf8_literal("appearance"), utf8_literal("Appearance"),
     utf8_literal("theme colors look messages chat images embeds reactions spoilers emoji"),
     UiIcon::palette, 1, expanded(SettingsPanel::appearance)},
    {utf8_literal("accessibility"), utf8_literal("Accessibility"),
     utf8_literal("motion a11y density animation"), UiIcon::accessibility, 1,
     expanded(SettingsPanel::accessibility)},
    {utf8_literal("plugins"), utf8_literal("Plugins"), utf8_literal("extensions add-ons modules install"),
     UiIcon::puzzle, 3, {}},
    {utf8_literal("advanced"), utf8_literal("Developer"), utf8_literal("developer advanced"), UiIcon::code,
     4, expanded(SettingsPanel::advanced)},
    {utf8_literal("updates"), utf8_literal("Updates"), utf8_literal("update version upgrade release"),
     UiIcon::download, 3, expanded(SettingsPanel::updates)},
    {utf8_literal("voice"), utf8_literal("Voice & Video"),
     utf8_literal("microphone speakers headphones audio calls video"), UiIcon::headphones, 1,
     expanded(SettingsPanel::voice)},
    {utf8_literal("account"), utf8_literal("Account"),
     utf8_literal("email phone username password my account"), UiIcon::user_circle, 0,
     expanded(SettingsPanel::account)},
    {utf8_literal("profiles"), utf8_literal("Profiles"), utf8_literal("profile pronouns about me bio"),
     UiIcon::user_edit, 0, expanded(SettingsPanel::profiles)},
    {utf8_literal("devices"), utf8_literal("Logged-in Devices"),
     utf8_literal("sessions logged in log out devices"), UiIcon::smartphone, 0,
     expanded(SettingsPanel::devices)},
    {utf8_literal("content_social"), utf8_literal("Messaging Permissions"),
     utf8_literal("privacy safety content social direct messages friend requests spam"),
     UiIcon::message_text, 0, expanded(SettingsPanel::content_social)},
    {utf8_literal("data_privacy"), utf8_literal("Data & Privacy"), utf8_literal("privacy profile data"),
     UiIcon::shield_check, 0, expanded(SettingsPanel::data_privacy)},
    {utf8_literal("authorized_apps"), utf8_literal("Connected Apps"),
     utf8_literal("authorized apps connections oauth"), UiIcon::key, 2,
     expanded(SettingsPanel::authorized_apps)},
    {utf8_literal("language_time"), utf8_literal("Language & Time"), utf8_literal("time format clock"),
     UiIcon::languages, 1, expanded(SettingsPanel::language_time)},
    {utf8_literal("registered_games"), utf8_literal("Registered Games"),
     utf8_literal("games detection playing added"), UiIcon::play, 2,
     expanded(SettingsPanel::registered_games)},
    {utf8_literal("keybinds"), utf8_literal("Keybinds"), utf8_literal("keyboard shortcuts hotkeys keys"),
     UiIcon::keyboard, 1, expanded(SettingsPanel::keybinds)},
    {utf8_literal("system"), utf8_literal("System"), utf8_literal("windows startup tray links protocol"),
     UiIcon::monitor, 1, expanded(SettingsPanel::system)},
    {utf8_literal("activity_privacy"), utf8_literal("Activity Privacy"),
     utf8_literal("activity status games playing sharing joining"), UiIcon::activity, 2,
     expanded(SettingsPanel::activity_privacy)},
};
static_assert(sizeof(panels) / sizeof(panels[0]) == kSettingsPanelCount);
constexpr UiSettingsSchema catalog{{sections, 5}, {panels, kSettingsPanelCount}};

constexpr UiSettingsNavEntry nav_header(u32 section) noexcept {
    return {UiSettingsNavKind::header, false, UiIcon::count, sections[section].header, {}, 0};
}
constexpr UiSettingsNavEntry nav_page(SettingsPanel panel) noexcept {
    const auto& p = panels[static_cast<u32>(panel)];
    return {UiSettingsNavKind::page, false, p.icon, p.title, {kSettingsNativeOwner, static_cast<u64>(panel) + 1}, 0};
}
constexpr UiSettingsNavEntry nav_separator{UiSettingsNavKind::separator, false, UiIcon::count, {}, {}, 0};
// The official grouping: Profiles first, the account pages, Experience and
// Games & Apps; MaiCord's own pages follow, then Developer and Log out.
constexpr UiSettingsNavEntry navigation[]{
    nav_page(SettingsPanel::profiles),
    nav_separator,
    nav_page(SettingsPanel::account),
    nav_page(SettingsPanel::devices),
    nav_page(SettingsPanel::data_privacy),
    nav_page(SettingsPanel::content_social),
    nav_page(SettingsPanel::notifications),
    nav_separator,
    nav_header(1),
    nav_page(SettingsPanel::voice),
    nav_page(SettingsPanel::appearance),
    nav_page(SettingsPanel::accessibility),
    nav_page(SettingsPanel::system),
    nav_page(SettingsPanel::keybinds),
    nav_page(SettingsPanel::language_time),
    nav_separator,
    nav_header(2),
    nav_page(SettingsPanel::registered_games),
    nav_page(SettingsPanel::activity_privacy),
    nav_page(SettingsPanel::authorized_apps),
    nav_separator,
    nav_header(3),
    nav_page(SettingsPanel::updates),
    nav_page(SettingsPanel::plugins),
    {UiSettingsNavKind::contributed, false, UiIcon::count, {}, {}, 0},
    nav_separator,
    nav_page(SettingsPanel::advanced),
    {UiSettingsNavKind::action, true, UiIcon::log_out, utf8_literal("Log Out"), {}, kSettingsLogOutAction},
};
constexpr u32 kNavigationCount = sizeof(navigation) / sizeof(navigation[0]);

struct LocalToggle {
    SettingBinding binding;
    bool ClientPreferences::* field;
};
constexpr LocalToggle toggles[]{
    {SettingBinding::member_list, &ClientPreferences::member_pane_visible},
    {SettingBinding::reduced_motion, &ClientPreferences::reduced_motion},
    {SettingBinding::smooth_scroll, &ClientPreferences::smooth_scroll},
    {SettingBinding::desktop_notifications, &ClientPreferences::desktop_enabled},
    {SettingBinding::message_sounds, &ClientPreferences::message_sounds_enabled},
    {SettingBinding::selected_channel_sounds, &ClientPreferences::selected_message_sounds_enabled},
    {SettingBinding::developer_mode, &ClientPreferences::developer_mode},
    {SettingBinding::unread_badge, &ClientPreferences::unread_badge},
    {SettingBinding::taskbar_flash, &ClientPreferences::taskbar_flash},
    {SettingBinding::start_minimized, &ClientPreferences::start_minimized},
    {SettingBinding::minimize_to_tray, &ClientPreferences::minimize_to_tray},
    {SettingBinding::spellcheck, &ClientPreferences::spellcheck},
    {SettingBinding::game_detection, &ClientPreferences::game_detection},
};
bool ClientPreferences::* toggle_field(SettingBinding binding) noexcept {
    for (const auto& toggle : toggles)
        if (toggle.binding == binding)
            return toggle.field;
    return nullptr;
}

u32 theme_index(const ClientTheme* values, u32 count, ClientTheme theme) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (values[i] == theme)
            return i;
    return none;
}
} // namespace

SettingBackend settings_binding_backend(SettingBinding binding) noexcept {
    switch (binding) {
    case SettingBinding::theme_base:
    case SettingBinding::theme_color:
    case SettingBinding::background_effect:
    case SettingBinding::gradient_direction:
    case SettingBinding::gradient_intensity:
    case SettingBinding::member_list:
    case SettingBinding::reduced_motion:
    case SettingBinding::smooth_scroll:
    case SettingBinding::desktop_notifications:
    case SettingBinding::message_sounds:
    case SettingBinding::selected_channel_sounds:
    case SettingBinding::output_volume:
    case SettingBinding::developer_mode:
    case SettingBinding::group_spacing:
    case SettingBinding::unread_badge:
    case SettingBinding::taskbar_flash:
    case SettingBinding::start_minimized:
    case SettingBinding::minimize_to_tray:
    case SettingBinding::spellcheck:
    case SettingBinding::game_detection:
        return SettingBackend::local;
    case SettingBinding::open_on_startup:
    case SettingBinding::protocol_handler:
        return SettingBackend::os;
    case SettingBinding::dm_from_members:
    case SettingBinding::message_requests:
    case SettingBinding::nsfw_commands:
    case SettingBinding::dm_spam_filter:
    case SettingBinding::explicit_friends:
    case SettingBinding::explicit_others:
    case SettingBinding::explicit_guilds:
    case SettingBinding::friend_everyone:
    case SettingBinding::friend_mutual_friends:
    case SettingBinding::friend_mutual_guilds:
    case SettingBinding::profile_visibility:
    case SettingBinding::profile_updates:
    case SettingBinding::message_display:
    case SettingBinding::time_format:
    case SettingBinding::user_settings_notice:
    case SettingBinding::privacy_notice:
    case SettingBinding::language_notice:
    case SettingBinding::chat_notice:
    case SettingBinding::inline_attachment_media:
    case SettingBinding::inline_embed_media:
    case SettingBinding::render_embeds:
    case SettingBinding::render_reactions:
    case SettingBinding::gif_autoplay:
    case SettingBinding::animate_emoji:
    case SettingBinding::animate_stickers:
    case SettingBinding::render_spoilers:
    case SettingBinding::convert_emoticons:
    case SettingBinding::nsfw_guilds:
    case SettingBinding::friend_request_notes:
    case SettingBinding::sponsored_activity:
    case SettingBinding::sponsored_third_party:
    case SettingBinding::share_online:
    case SettingBinding::share_streaming:
    case SettingBinding::activity_servers:
    case SettingBinding::join_friends:
    case SettingBinding::join_voice:
    case SettingBinding::activity_notice:
    case SettingBinding::accessibility_notice:
        return SettingBackend::proto1;
    case SettingBinding::account_card:
    case SettingBinding::account_display_name:
    case SettingBinding::account_username:
    case SettingBinding::account_email:
    case SettingBinding::account_phone:
    case SettingBinding::account_password:
    case SettingBinding::account_mfa:
    case SettingBinding::account_notice:
    case SettingBinding::profile_notice:
    case SettingBinding::devices_notice:
    case SettingBinding::apps_notice:
    case SettingBinding::profile_display_name:
    case SettingBinding::profile_pronouns:
    case SettingBinding::profile_bio:
    case SettingBinding::devices_list:
    case SettingBinding::apps_list:
        return SettingBackend::account;
    case SettingBinding::voice_input_device:
    case SettingBinding::voice_output_device:
    case SettingBinding::voice_input_volume:
    case SettingBinding::voice_output_volume:
    case SettingBinding::voice_input_mode:
    case SettingBinding::voice_auto_sensitivity:
    case SettingBinding::voice_sensitivity:
    case SettingBinding::voice_push_to_talk_key:
    case SettingBinding::voice_push_to_talk_release:
    case SettingBinding::voice_processing:
    case SettingBinding::voice_soundboard_volume:
    case SettingBinding::voice_soundboard_muted:
        return SettingBackend::voice;
    case SettingBinding::keybinds_custom:
    case SettingBinding::keybinds_add:
    case SettingBinding::keybinds_defaults:
        return SettingBackend::keybinds;
    default:
        return SettingBackend::action;
    }
}

const UiSettingsSchema& settings_catalog() noexcept {
    catalog_initialize();
    return catalog;
}
const UiSettingsSchemaPanel& settings_catalog_panel(SettingsPanel panel) noexcept {
    catalog_initialize();
    return panels[static_cast<u32>(panel) < kSettingsPanelCount ? static_cast<u32>(panel) : 0];
}

ConstSlice<UiSettingsNavEntry> settings_catalog_navigation(bool log_out) noexcept {
    return {navigation, log_out ? kNavigationCount : kNavigationCount - 1};
}
// Before sign-in: the account-independent pages in the same order.
constexpr UiSettingsNavEntry signed_out_navigation[]{
    nav_header(1),
    nav_page(SettingsPanel::voice),
    nav_page(SettingsPanel::appearance),
    nav_page(SettingsPanel::accessibility),
    nav_page(SettingsPanel::system),
    nav_page(SettingsPanel::keybinds),
    nav_page(SettingsPanel::language_time),
    nav_separator,
    nav_header(3),
    nav_page(SettingsPanel::updates),
    nav_separator,
    nav_page(SettingsPanel::advanced),
};
ConstSlice<UiSettingsNavEntry> settings_catalog_navigation_signed_out() noexcept {
    return {signed_out_navigation, sizeof(signed_out_navigation) / sizeof(signed_out_navigation[0])};
}

bool settings_control_location(u64 id, UiSettingsSchemaLocation* location) noexcept {
    const u64 panel = (id >> 16) - 1, node = (id & 0xffff) - 1;
    if (!location || !(id >> 16) || !(id & 0xffff) || (id >> 32) || panel >= kSettingsPanelCount ||
        node >= panels[panel].nodes.count)
        return false;
    *location = {static_cast<u32>(panel), static_cast<u32>(node)};
    return true;
}
const UiSettingsSchemaNode* settings_catalog_node(UiSettingsSchemaLocation location) noexcept {
    catalog_initialize();
    return location.panel < kSettingsPanelCount && location.node < panels[location.panel].nodes.count
               ? &panels[location.panel].nodes[location.node]
               : nullptr;
}
const UiSettingsSchemaNode* settings_catalog_binding(SettingBinding binding,
                                                     UiSettingsSchemaLocation* location) noexcept {
    catalog_initialize();
    for (u32 p = 0; p < kSettingsPanelCount; ++p)
        for (u32 n = 0; n < panels[p].nodes.count; ++n)
            if (panels[p].nodes[n].binding == bind(binding)) {
                if (location)
                    *location = {p, n};
                return &panels[p].nodes[n];
            }
    return nullptr;
}

ClientTheme settings_theme_choice(SettingBinding palette, u32 choice) noexcept {
    if (palette == SettingBinding::theme_base && choice < 3)
        return base_theme_values[choice];
    if (palette == SettingBinding::theme_color && choice < 5)
        return color_theme_values[choice];
    return ClientTheme::dark;
}

bool settings_theme_swatch(const ClientPreferences& preferences, SettingBinding palette, u32 choice,
                           BackgroundTheme* output) noexcept {
    if (!output || (palette != SettingBinding::theme_base && palette != SettingBinding::theme_color) ||
        choice >= (palette == SettingBinding::theme_base ? 3u : 5u))
        return false;
    auto swatch = preferences;
    swatch.theme = settings_theme_choice(palette, choice);
    const auto theme = client_theme_projection(swatch);
    *output = theme.background;
    if (!background_theme_valid(*output)) {
        *output = background_theme_defaults();
        const auto base = theme.base_low;
        const u32 rgb = static_cast<u32>(base.red * 255 + .5f) << 16 | static_cast<u32>(base.green * 255 + .5f) << 8 |
                        static_cast<u32>(base.blue * 255 + .5f);
        output->colors[0] = output->colors[1] = rgb;
        output->colors[2] = output->colors[3] = output->colors[4] = 0;
        output->color_count = 2;
        output->intensity = 100;
        output->effect = BackgroundEffect::none;
    }
    return true;
}

bool settings_store_read(const ClientPreferences& p, SettingBinding binding, SettingValue* value) noexcept {
    if (!value)
        return false;
    *value = {};
    if (const auto field = toggle_field(binding)) {
        value->checked = p.*field;
        return true;
    }
    switch (binding) {
    case SettingBinding::theme_base:
        value->choice = theme_index(base_theme_values, 3, p.theme);
        return true;
    case SettingBinding::theme_color:
        value->choice = theme_index(color_theme_values, 5, p.theme);
        return true;
    case SettingBinding::background_effect:
        value->choice = static_cast<u32>(p.gradient.effect);
        return true;
    case SettingBinding::gradient_direction:
        value->choice = p.gradient.angle_degrees;
        return true;
    case SettingBinding::gradient_intensity:
        value->choice = p.gradient.intensity;
        return true;
    case SettingBinding::output_volume:
        value->choice = p.output_volume;
        return true;
    case SettingBinding::group_spacing:
        value->choice = p.message_group_spacing ? p.message_group_spacing - 1u : kUiSettingsNoChoice;
        return true;
    default:
        return false;
    }
}

bool settings_store_write(ClientPreferences* p, SettingBinding binding, SettingValue value) noexcept {
    if (!p)
        return false;
    if (const auto field = toggle_field(binding)) {
        p->*field = value.checked;
        return true;
    }
    switch (binding) {
    case SettingBinding::theme_base:
    case SettingBinding::theme_color:
        if (value.choice >= (binding == SettingBinding::theme_base ? 3u : 5u))
            return false;
        p->theme = settings_theme_choice(binding, value.choice);
        return true;
    case SettingBinding::background_effect: {
        if (value.choice >= kBackgroundEffectCount)
            return false;
        // Each effect keeps its own speed, strength and density profile.
        const u32 previous = background_effect_profile_index(p->gradient.effect);
        if (previous < kBackgroundEffectProfileCount)
            p->effect_profiles[previous] = background_effect_settings(p->gradient);
        p->gradient.effect = static_cast<BackgroundEffect>(value.choice);
        const u32 index = background_effect_profile_index(p->gradient.effect);
        const auto effect = index < kBackgroundEffectProfileCount
                                ? p->effect_profiles[index]
                                : background_effect_settings_defaults(BackgroundEffect::none);
        p->gradient.speed = effect.speed;
        p->gradient.strength = effect.strength;
        p->gradient.density = effect.density;
        return true;
    }
    case SettingBinding::gradient_direction:
    case SettingBinding::gradient_intensity: {
        if (value.choice > (binding == SettingBinding::gradient_direction ? 360u : 100u))
            return false;
        if (binding == SettingBinding::gradient_direction)
            p->gradient.angle_degrees = static_cast<u16>(value.choice);
        else
            p->gradient.intensity = static_cast<u8>(value.choice);
        // Editing the gradient selects the custom gradient of the same appearance.
        p->theme = p->theme == ClientTheme::light || p->theme == ClientTheme::custom_light_gradient
                       ? ClientTheme::custom_light_gradient
                       : ClientTheme::custom_gradient;
        return true;
    }
    case SettingBinding::output_volume:
        if (value.choice > 200)
            return false;
        p->output_volume = static_cast<u8>(value.choice);
        return true;
    case SettingBinding::group_spacing:
        if (value.choice >= kClientPreferencesGroupSpacingCount - 1u)
            return false;
        p->message_group_spacing = static_cast<u8>(value.choice + 1);
        return true;
    default:
        return false;
    }
}

} // namespace maicord
