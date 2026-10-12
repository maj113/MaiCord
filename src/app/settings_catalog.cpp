#include "settings_catalog.hpp"
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
bool number_labels_ready;

template <u32 Count, u32 Width>
void append_suffix(NumberLabels<Count, Width>& labels, u32 value, u32 at, const char* suffix,
                   u32 suffix_bytes) noexcept {
    for (u32 i = 0; i < suffix_bytes; ++i)
        labels.text[value][at++] = suffix[i];
    labels.values[value] = {labels.text[value], at};
}
void number_labels_initialize() noexcept {
    if (number_labels_ready)
        return;
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
    number_labels_ready = true;
}

using K = UiSettingsControlKind;
constexpr u32 none = kUiSettingsNoChoice;
constexpr u16 dynamic = ui_settings_schema_dynamic, hidden = ui_settings_schema_search_hidden;
constexpr u32 bind(SettingBinding binding) noexcept {
    return static_cast<u32>(binding);
}
constexpr UiSettingsSchemaNode node(Utf8View key, K kind, Utf8View label, Utf8View description, Utf8View keywords,
                                    SettingBinding binding = SettingBinding::none, ConstSlice<Utf8View> choices = {},
                                    u32 default_choice = none, Utf8View action = {}, u16 flags = 0,
                                    u8 variant = 0) noexcept {
    return {key, label, description, keywords, kind, variant, flags, choices, default_choice, bind(binding), action};
}

constexpr Utf8View message_displays[]{utf8_literal("Cozy"), utf8_literal("Compact")};
constexpr Utf8View group_spacings[]{utf8_literal("0px"), utf8_literal("4px"), utf8_literal("8px"),
                                    utf8_literal("16px"), utf8_literal("24px")};

constexpr UiSettingsSchemaNode notifications[]{
    node(utf8_literal("notifications.desktop_category"), K::category, utf8_literal("Desktop notifications"), {}, {}),
    node(utf8_literal("notifications.desktop"), K::toggle, utf8_literal("Enable desktop notifications"),
         utf8_literal("Show notifications when MaiCord is in the background."),
         utf8_literal("alerts toast popup banner"), SettingBinding::desktop_notifications),
    node(utf8_literal("notifications.setup"), K::action, utf8_literal("Set up desktop notifications"), {},
         utf8_literal("permission enable windows"), SettingBinding::notification_setup, {}, none,
         utf8_literal("Set up")),
    node(utf8_literal("notifications.windows"), K::action, utf8_literal("Windows notification settings"),
         utf8_literal("Manage notification permissions in Windows."), utf8_literal("system permission focus"),
         SettingBinding::windows_notification_settings, {}, none, utf8_literal("Open settings")),
    node(utf8_literal("notifications.sounds"), K::category, utf8_literal("Sounds"), {}, {}),
    node(utf8_literal("notifications.message_sounds"), K::toggle, utf8_literal("Message sounds"),
         utf8_literal("Play a sound for new messages."), utf8_literal("audio ding chime"),
         SettingBinding::message_sounds),
    node(utf8_literal("notifications.selected_channel_sounds"), K::toggle, utf8_literal("Current channel sounds"),
         utf8_literal("Also play sounds for messages in the channel you're viewing."), utf8_literal("audio focused"),
         SettingBinding::selected_channel_sounds),
    node(utf8_literal("notifications.volume"), K::slider, utf8_literal("Sound volume"),
         utf8_literal("Volume of notification sounds."), utf8_literal("loudness audio output"),
         SettingBinding::output_volume, {percent_labels.values, 201}, 100),
    node(utf8_literal("notifications.badges"), K::category, utf8_literal("Badges"), {}, {}),
    node(utf8_literal("notifications.unread_badge"), K::toggle, utf8_literal("Unread message badge"),
         utf8_literal("Show mentions and unread messages on the taskbar icon."),
         utf8_literal("taskbar overlay count dot"), SettingBinding::unread_badge),
    node(utf8_literal("notifications.taskbar_flash"), K::toggle, utf8_literal("Taskbar flashing"),
         utf8_literal("Flash the taskbar button when a message notifies you."),
         utf8_literal("taskbar flash attention blink"), SettingBinding::taskbar_flash),
};

constexpr UiSettingsSchemaNode appearance[]{
    node(utf8_literal("appearance.theme"), K::palette, utf8_literal("Theme"),
         utf8_literal("Choose a light or dark look for MaiCord."), utf8_literal("dark mode light mode amoled black"),
         SettingBinding::theme_base, {base_themes, 3}),
    node(utf8_literal("appearance.color_theme"), K::palette, utf8_literal("Color themes"),
         utf8_literal("Gradient themes tint the background behind every surface."),
         utf8_literal("gradient colour color aurora sunset ocean"), SettingBinding::theme_color, {color_themes, 5}),
    node(utf8_literal("appearance.customize"), K::action, utf8_literal("Customize theme"),
         utf8_literal("Edit colors, the gradient and background effects."), utf8_literal("editor colors accent"),
         SettingBinding::customize_theme, {}, none, utf8_literal("Customize")),
    node(utf8_literal("appearance.background"), K::category, utf8_literal("Background"), {}, {}),
    node(utf8_literal("appearance.background_effect"), K::choice, utf8_literal("Background effect"),
         utf8_literal("Animated effect drawn behind gradient themes."),
         utf8_literal("stars comets animation"), SettingBinding::background_effect,
         {background_effects, kBackgroundEffectCount}),
    node(utf8_literal("appearance.gradient_direction"), K::slider, utf8_literal("Gradient direction"),
         utf8_literal("Angle of the background gradient."), utf8_literal("angle rotation degrees"),
         SettingBinding::gradient_direction, {degree_labels.values, 361}),
    node(utf8_literal("appearance.gradient_intensity"), K::slider, utf8_literal("Gradient intensity"),
         utf8_literal("How strongly the gradient shows behind content."), utf8_literal("strength opacity"),
         SettingBinding::gradient_intensity, {percent_labels.values, 101}),
    node(utf8_literal("appearance.imported"), K::category, utf8_literal("Imported theme"), {},
         utf8_literal("custom theme file import"), SettingBinding::none, {}, none, {}, dynamic),
    node(utf8_literal("appearance.imported.import"), K::action, utf8_literal("Current theme"), {},
         utf8_literal("custom theme file import"), SettingBinding::custom_import, {}, none,
         utf8_literal("Import theme…"), dynamic),
    node(utf8_literal("appearance.imported.installed"), K::choice, utf8_literal("Installed themes"),
         utf8_literal("Theme files in the MaiCord themes folder. Choose one to preview it."),
         utf8_literal("custom theme catalog installed folder json"), SettingBinding::custom_installed, {}, none, {},
         dynamic),
    node(utf8_literal("appearance.imported.preview"), K::action, utf8_literal("Imported theme"), {}, {},
         SettingBinding::custom_preview, {}, none, utf8_literal("Preview theme"), dynamic | hidden),
    node(utf8_literal("appearance.imported.apply"), K::action, utf8_literal("Imported theme"), {}, {},
         SettingBinding::custom_apply, {}, none, utf8_literal("Apply theme"), dynamic | hidden, 1),
    node(utf8_literal("appearance.imported.cancel"), K::action, utf8_literal("Discard preview"), {}, {},
         SettingBinding::custom_cancel, {}, none, utf8_literal("Cancel"), dynamic | hidden),
    node(utf8_literal("appearance.imported.builtin"), K::action, utf8_literal("Built-in appearance"),
         utf8_literal("Keep the imported theme available for later."), {}, SettingBinding::custom_builtin, {}, none,
         utf8_literal("Use built-in"), dynamic | hidden),
    node(utf8_literal("appearance.imported.restore"), K::action, utf8_literal("Saved imported theme"), {}, {},
         SettingBinding::custom_restore, {}, none, utf8_literal("Restore imported"), dynamic | hidden),
    node(utf8_literal("appearance.imported.retry_read"), K::action, utf8_literal("Couldn't read theme file"), {}, {},
         SettingBinding::custom_retry_read, {}, none, utf8_literal("Try again"), dynamic | hidden),
    node(utf8_literal("appearance.imported.retry_save"), K::action, utf8_literal("Couldn't save theme"), {}, {},
         SettingBinding::custom_retry_save, {}, none, utf8_literal("Try again"), dynamic | hidden),
    node(utf8_literal("appearance.messages"), K::category, utf8_literal("Messages"), {}, {}),
    node(utf8_literal("appearance.message_display"), K::radio, utf8_literal("Message display"),
         utf8_literal("Compact fits more messages on screen."), utf8_literal("compact cozy density chat"),
         SettingBinding::message_display, {message_displays, 2}, 0),
    node(utf8_literal("appearance.group_spacing"), K::slider, utf8_literal("Space between message groups"), {},
         utf8_literal("spacing gap padding density"), SettingBinding::group_spacing, {group_spacings, 5}, none, {},
         ui_settings_schema_marks),
    node(utf8_literal("appearance.layout"), K::category, utf8_literal("Layout"), {}, {}),
    node(utf8_literal("appearance.member_list"), K::toggle, utf8_literal("Show member list"),
         utf8_literal("Show the member list beside server channels."), utf8_literal("members users sidebar"),
         SettingBinding::member_list),
};

constexpr UiSettingsSchemaNode accessibility[]{
    node(utf8_literal("accessibility.motion"), K::category, utf8_literal("Motion"), {}, {}),
    node(utf8_literal("accessibility.reduced_motion"), K::toggle, utf8_literal("Reduced motion"),
         utf8_literal("Pause animated media and profile effects, and scroll immediately."),
         utf8_literal("animation reduce motion gif"), SettingBinding::reduced_motion),
    node(utf8_literal("accessibility.smooth_scroll"), K::toggle, utf8_literal("Smooth scrolling"),
         utf8_literal("Animate mouse-wheel scrolling."), utf8_literal("scroll wheel"), SettingBinding::smooth_scroll),
};

constexpr UiSettingsSchemaNode advanced[]{
    node(utf8_literal("advanced.developer_mode"), K::toggle, utf8_literal("Developer mode"),
         utf8_literal("Add Copy ID to context menus for messages, users, channels and servers."),
         utf8_literal("copy id snowflake debug bot"), SettingBinding::developer_mode),
    node(utf8_literal("advanced.preferences_retry"), K::notice, utf8_literal("Couldn't save settings"),
         utf8_literal("Your latest changes are applied but not saved yet."), {}, SettingBinding::preferences_retry,
         {}, none, utf8_literal("Try again"), dynamic | hidden, 2),
};

constexpr Utf8View update_channels[]{utf8_literal("Stable"), utf8_literal("Preview")};
constexpr UiSettingsSchemaNode updates[]{
    node(utf8_literal("updates.status"), K::action, utf8_literal("Check for updates"), {},
         utf8_literal("update upgrade version new release download restart"), SettingBinding::update_status, {}, none,
         utf8_literal("Check now"), dynamic),
    node(utf8_literal("updates.channel"), K::radio, utf8_literal("Update channel"),
         utf8_literal("Preview builds get new features first and may be less stable."),
         utf8_literal("beta preview stable release channel"), SettingBinding::update_channel, {update_channels, 2}, none, {},
         dynamic),
    node(utf8_literal("updates.rollback"), K::action, utf8_literal("Roll back to previous version"), {},
         utf8_literal("downgrade revert undo previous version"), SettingBinding::update_rollback, {}, none,
         utf8_literal("Roll back"), dynamic),
    node(utf8_literal("updates.assets"), K::notice, utf8_literal("What updates include"),
         utf8_literal("Updates replace the app and its account module. Download a fresh copy to get new fonts and licenses."),
         {}, SettingBinding::update_notice, {}, none, {}, dynamic | hidden),
};

constexpr Utf8View input_modes[]{utf8_literal("Voice activity"), utf8_literal("Push to talk")};
constexpr UiSettingsSchemaNode voice_nodes[]{
    node(utf8_literal("voice.devices"), K::category, utf8_literal("Devices"), {}, {}),
    node(utf8_literal("voice.input_device"), K::choice, utf8_literal("Input device"),
         utf8_literal("The microphone used in voice channels."), utf8_literal("microphone mic input headset"),
         SettingBinding::voice_input_device, {}, none, {}, dynamic),
    node(utf8_literal("voice.output_device"), K::choice, utf8_literal("Output device"),
         utf8_literal("Speakers or headphones for voice channels."), utf8_literal("speakers headphones output audio"),
         SettingBinding::voice_output_device, {}, none, {}, dynamic),
    node(utf8_literal("voice.input_volume"), K::slider, utf8_literal("Input volume"), {},
         utf8_literal("microphone gain loudness"), SettingBinding::voice_input_volume, {percent_labels.values, 201},
         100),
    node(utf8_literal("voice.output_volume"), K::slider, utf8_literal("Output volume"),
         utf8_literal("Volume of everyone you hear in voice channels."), utf8_literal("speakers loudness"),
         SettingBinding::voice_output_volume, {percent_labels.values, 201}, 100),
    node(utf8_literal("voice.input"), K::category, utf8_literal("Input"), {}, {}),
    node(utf8_literal("voice.input_mode"), K::radio, utf8_literal("Input mode"), {},
         utf8_literal("push to talk ptt voice activity vad"), SettingBinding::voice_input_mode, {input_modes, 2}),
    node(utf8_literal("voice.auto_sensitivity"), K::toggle, utf8_literal("Set input sensitivity automatically"),
         utf8_literal("Follow your background noise to tell when you're speaking."),
         utf8_literal("vad threshold automatic"), SettingBinding::voice_auto_sensitivity, {}, none, {}, dynamic),
    node(utf8_literal("voice.sensitivity"), K::slider, utf8_literal("Input sensitivity"),
         utf8_literal("Sound above the marker is sent. The bar shows your microphone level."),
         utf8_literal("vad threshold level meter mic test"), SettingBinding::voice_sensitivity,
         {decibel_labels.values, 101}, none, {}, dynamic),
    node(utf8_literal("voice.push_to_talk_key"), K::action, utf8_literal("Push to talk shortcut"),
         utf8_literal("Hold this key to talk, even while MaiCord is in the background."),
         utf8_literal("ptt keybind hotkey key"), SettingBinding::voice_push_to_talk_key, {}, none,
         utf8_literal("Record shortcut"), dynamic),
    node(utf8_literal("voice.push_to_talk_release"), K::slider, utf8_literal("Release delay"),
         utf8_literal("Keep sending briefly after you let go."), utf8_literal("ptt delay"),
         SettingBinding::voice_push_to_talk_release, {millisecond_labels.values, 201}, 2, {}, dynamic),
    node(utf8_literal("voice.processing_category"), K::category, utf8_literal("Voice processing"), {}, {}),
    node(utf8_literal("voice.processing"), K::toggle, utf8_literal("Echo and noise reduction"),
         utf8_literal("Let Windows cancel echo, suppress background noise and even out your volume."),
         utf8_literal("echo cancellation noise suppression automatic gain control agc"),
         SettingBinding::voice_processing),
    node(utf8_literal("voice.soundboard_category"), K::category, utf8_literal("Soundboard"), {}, {}),
    node(utf8_literal("voice.soundboard_volume"), K::slider, utf8_literal("Soundboard volume"),
         utf8_literal("Volume of sounds people play in voice channels."),
         utf8_literal("soundboard sounds effects loudness"), SettingBinding::voice_soundboard_volume,
         {percent_labels.values, 101}, 100),
    node(utf8_literal("voice.soundboard_muted"), K::toggle, utf8_literal("Mute soundboard"),
         utf8_literal("Don't play sounds other people send. You can still use the soundboard."),
         utf8_literal("soundboard sounds silence mute"), SettingBinding::voice_soundboard_muted),
};

constexpr UiSettingsSchemaNode account_nodes[]{
    node(utf8_literal("account.card"), K::action, utf8_literal("Account"), {}, utf8_literal("avatar profile"),
         SettingBinding::account_card, {}, none, utf8_literal("Edit profile"), dynamic | hidden),
    node(utf8_literal("account.notice"), K::notice, utf8_literal("Account details"), {}, {},
         SettingBinding::account_notice, {}, none, utf8_literal("Try again"), dynamic | hidden),
    node(utf8_literal("account.details"), K::category, utf8_literal("Account details"), {}, {}),
    node(utf8_literal("account.display_name"), K::action, utf8_literal("Display name"), {},
         utf8_literal("global name nickname"), SettingBinding::account_display_name, {}, none, utf8_literal("Edit"),
         dynamic),
    node(utf8_literal("account.username"), K::action, utf8_literal("Username"), {}, utf8_literal("handle name"),
         SettingBinding::account_username, {}, none, utf8_literal("Edit"), dynamic),
    node(utf8_literal("account.email"), K::action, utf8_literal("Email"), {}, utf8_literal("mail address"),
         SettingBinding::account_email, {}, none, utf8_literal("Reveal"), dynamic),
    node(utf8_literal("account.phone"), K::action, utf8_literal("Phone number"), {}, utf8_literal("sms mobile"),
         SettingBinding::account_phone, {}, none, utf8_literal("Reveal"), dynamic),
    node(utf8_literal("account.security"), K::category, utf8_literal("Password and authentication"), {}, {}),
    node(utf8_literal("account.password"), K::action, utf8_literal("Password"),
         utf8_literal("Changes open Discord in your browser."), utf8_literal("change password security"),
         SettingBinding::account_password, {}, none, utf8_literal("Change"), dynamic),
    node(utf8_literal("account.mfa"), K::notice, utf8_literal("Two-factor authentication"), {},
         utf8_literal("2fa mfa authenticator security"), SettingBinding::account_mfa, {}, none, {}, dynamic),
};
constexpr UiSettingsSchemaNode profile_nodes[]{
    node(utf8_literal("profiles.notice"), K::notice, utf8_literal("Profile"), {}, {}, SettingBinding::profile_notice,
         {}, none, utf8_literal("Try again"), dynamic | hidden),
    node(utf8_literal("profiles.display_name"), K::action, utf8_literal("Display name"), {},
         utf8_literal("global name nickname"), SettingBinding::profile_display_name, {}, none, utf8_literal("Edit"),
         dynamic),
    node(utf8_literal("profiles.pronouns"), K::action, utf8_literal("Pronouns"), {}, {},
         SettingBinding::profile_pronouns, {}, none, utf8_literal("Edit"), dynamic),
    node(utf8_literal("profiles.bio"), K::action, utf8_literal("About me"), {}, utf8_literal("bio description"),
         SettingBinding::profile_bio, {}, none, utf8_literal("Edit"), dynamic),
};
constexpr UiSettingsSchemaNode device_nodes[]{
    node(utf8_literal("devices.notice"), K::notice, utf8_literal("Devices"), {}, {}, SettingBinding::devices_notice,
         {}, none, utf8_literal("Try again"), dynamic | hidden),
    node(utf8_literal("devices.list"), K::category, utf8_literal("Signed-in devices"),
         utf8_literal("Log out of any device you don't recognize."), utf8_literal("sessions logged in"),
         SettingBinding::devices_list),
};
constexpr Utf8View explicit_choices[]{utf8_literal("Show"), utf8_literal("Blur"), utf8_literal("Block")};
constexpr Utf8View spam_choices[]{utf8_literal("Filter all"), utf8_literal("Non-friends only"), utf8_literal("Off")};
constexpr UiSettingsSchemaNode content_nodes[]{
    node(utf8_literal("content.notice"), K::notice, utf8_literal("Synced settings"), {}, {},
         SettingBinding::user_settings_notice, {}, none, {}, dynamic | hidden),
    node(utf8_literal("content.dms"), K::category, utf8_literal("Direct messages"), {}, {}),
    node(utf8_literal("content.dm_from_members"), K::toggle, utf8_literal("Allow DMs from server members"),
         utf8_literal("Applies to servers you join from now on."), utf8_literal("direct messages privacy"),
         SettingBinding::dm_from_members, {}, none, {}, dynamic),
    node(utf8_literal("content.message_requests"), K::toggle, utf8_literal("Message requests"),
         utf8_literal("Enable requests from server members you may not know. Applies to servers you join "
                      "from now on."), utf8_literal("dm requests"),
         SettingBinding::message_requests, {}, none, {}, dynamic),
    node(utf8_literal("content.nsfw_commands"), K::toggle, utf8_literal("Age-restricted app commands in DMs"),
         utf8_literal("Allow apps to offer age-restricted commands in your DMs."), utf8_literal("nsfw apps"),
         SettingBinding::nsfw_commands, {}, none, {}, dynamic),
    node(utf8_literal("content.spam"), K::radio, utf8_literal("DM spam filter"),
         utf8_literal("Messages that look like spam go to a separate inbox."), utf8_literal("junk filter"),
         SettingBinding::dm_spam_filter, {spam_choices, 3}, none, {}, dynamic),
    node(utf8_literal("content.media"), K::category, utf8_literal("Sensitive media"),
         utf8_literal("Media that may be sexually explicit."), {}),
    node(utf8_literal("content.explicit_friends"), K::choice, utf8_literal("DMs from friends"), {},
         utf8_literal("explicit nsfw blur filter"), SettingBinding::explicit_friends, {explicit_choices, 3}, none,
         {}, dynamic),
    node(utf8_literal("content.explicit_others"), K::choice, utf8_literal("DMs from others"), {},
         utf8_literal("explicit nsfw blur filter"), SettingBinding::explicit_others, {explicit_choices, 3}, none,
         {}, dynamic),
    node(utf8_literal("content.explicit_guilds"), K::choice, utf8_literal("Server channels"), {},
         utf8_literal("explicit nsfw blur filter"), SettingBinding::explicit_guilds, {explicit_choices, 3}, none,
         {}, dynamic),
    node(utf8_literal("content.friend_requests"), K::category, utf8_literal("Friend requests"),
         utf8_literal("Who can send you a friend request."), {}),
    node(utf8_literal("content.friend_everyone"), K::toggle, utf8_literal("Everyone"), {},
         utf8_literal("friend requests anyone"), SettingBinding::friend_everyone, {}, none, {}, dynamic),
    node(utf8_literal("content.friend_mutual_friends"), K::toggle, utf8_literal("Friends of friends"), {},
         utf8_literal("friend requests mutual"), SettingBinding::friend_mutual_friends, {}, none, {}, dynamic),
    node(utf8_literal("content.friend_mutual_guilds"), K::toggle, utf8_literal("Server members"), {},
         utf8_literal("friend requests mutual servers"), SettingBinding::friend_mutual_guilds, {}, none, {}, dynamic),
};
constexpr Utf8View visibility_choices[]{utf8_literal("Friends and all servers"),
                                        utf8_literal("Friends and small servers"), utf8_literal("Friends only")};
constexpr UiSettingsSchemaNode privacy_nodes[]{
    node(utf8_literal("privacy.notice"), K::notice, utf8_literal("Synced settings"), {}, {},
         SettingBinding::privacy_notice, {}, none, {}, dynamic | hidden),
    node(utf8_literal("privacy.profile"), K::category, utf8_literal("Profile"), {}, {}),
    node(utf8_literal("privacy.profile_visibility"), K::radio, utf8_literal("Who can see your full profile"),
         utf8_literal("Small servers have up to 200 members."), utf8_literal("privacy profile visibility"),
         SettingBinding::profile_visibility, {visibility_choices, 3}, none, {}, dynamic),
    node(utf8_literal("privacy.profile_updates"), K::toggle, utf8_literal("Share profile updates"),
         utf8_literal("Let friends know when you update your profile."), utf8_literal("notify friends"),
         SettingBinding::profile_updates, {}, none, {}, dynamic),
};
constexpr UiSettingsSchemaNode app_nodes[]{
    node(utf8_literal("apps.notice"), K::notice, utf8_literal("Authorized apps"), {}, {}, SettingBinding::apps_notice,
         {}, none, utf8_literal("Try again"), dynamic | hidden),
    node(utf8_literal("apps.list"), K::category, utf8_literal("Apps with access"),
         utf8_literal("Apps you have connected to your Discord account."), utf8_literal("oauth applications tokens"),
         SettingBinding::apps_list),
};
constexpr Utf8View time_formats[]{utf8_literal("Auto"), utf8_literal("12-hour"), utf8_literal("24-hour")};
constexpr UiSettingsSchemaNode language_nodes[]{
    node(utf8_literal("language.notice"), K::notice, utf8_literal("Synced settings"), {}, {},
         SettingBinding::language_notice, {}, none, {}, dynamic | hidden),
    node(utf8_literal("language.time"), K::category, utf8_literal("Time"), {}, {}),
    node(utf8_literal("language.time_format"), K::radio, utf8_literal("Time format"),
         utf8_literal("Auto follows your Windows region settings."), utf8_literal("clock 12 24 hour am pm"),
         SettingBinding::time_format, {time_formats, 3}, 0, {}, dynamic),
};

constexpr Utf8View sticker_animations[]{utf8_literal("Always animate"), utf8_literal("Animate on hover"),
                                        utf8_literal("Never animate")};
constexpr Utf8View spoiler_choices[]{utf8_literal("On click"), utf8_literal("On servers I moderate"),
                                     utf8_literal("Always")};
constexpr UiSettingsSchemaNode chat_nodes[]{
    node(utf8_literal("chat.notice"), K::notice, utf8_literal("Synced settings"), {}, {},
         SettingBinding::chat_notice, {}, none, {}, dynamic | hidden),
    node(utf8_literal("chat.media"), K::category, utf8_literal("Images, videos and GIFs"), {}, {}),
    node(utf8_literal("chat.inline_embed_media"), K::toggle, utf8_literal("When posted as links to chat"), {},
         utf8_literal("images videos links preview inline media"), SettingBinding::inline_embed_media, {}, none,
         {}, dynamic),
    node(utf8_literal("chat.inline_attachment_media"), K::toggle, utf8_literal("When uploaded directly to Discord"),
         utf8_literal("Off shows uploads as file cards."), utf8_literal("images videos attachments uploads inline"),
         SettingBinding::inline_attachment_media, {}, none, {}, dynamic),
    node(utf8_literal("chat.embeds"), K::category, utf8_literal("Embeds and link previews"), {}, {}),
    node(utf8_literal("chat.render_embeds"), K::toggle, utf8_literal("Show embeds and link previews"), {},
         utf8_literal("embeds link previews websites cards"), SettingBinding::render_embeds, {}, none, {},
         dynamic),
    node(utf8_literal("chat.emoji"), K::category, utf8_literal("Emoji"), {}, {}),
    node(utf8_literal("chat.render_reactions"), K::toggle, utf8_literal("Show emoji reactions"), {},
         utf8_literal("reactions emoji"), SettingBinding::render_reactions, {}, none, {}, dynamic),
    node(utf8_literal("chat.convert_emoticons"), K::toggle, utf8_literal("Convert emoticons to emoji"),
         utf8_literal("For example, :) becomes 🙂 when you send it."),
         utf8_literal("emoticons smiley emoji convert"), SettingBinding::convert_emoticons, {}, none, {}, dynamic),
    node(utf8_literal("chat.animation"), K::category, utf8_literal("Animation"),
         utf8_literal("Off keeps media still until you hover or focus its message."), {}),
    node(utf8_literal("chat.gif_autoplay"), K::toggle, utf8_literal("Play GIFs automatically"), {},
         utf8_literal("gif autoplay animation video"), SettingBinding::gif_autoplay, {}, none, {}, dynamic),
    node(utf8_literal("chat.animate_emoji"), K::toggle, utf8_literal("Play animated emoji"), {},
         utf8_literal("emoji animation animated"), SettingBinding::animate_emoji, {}, none, {}, dynamic),
    node(utf8_literal("chat.animate_stickers"), K::radio, utf8_literal("Sticker animation"), {},
         utf8_literal("stickers animation"), SettingBinding::animate_stickers, {sticker_animations, 3}, none, {},
         dynamic),
    node(utf8_literal("chat.input"), K::category, utf8_literal("Message box"), {}, {}),
    node(utf8_literal("chat.spellcheck"), K::toggle, utf8_literal("Spellcheck"),
         utf8_literal("Underline misspelled words as you type."), utf8_literal("spelling dictionary typo"),
         SettingBinding::spellcheck),
    node(utf8_literal("chat.spoilers"), K::category, utf8_literal("Spoilers"), {}, {}),
    node(utf8_literal("chat.render_spoilers"), K::radio, utf8_literal("Show spoiler content"), {},
         utf8_literal("spoilers hidden reveal"), SettingBinding::render_spoilers, {spoiler_choices, 3}, none, {},
         dynamic),
};

constexpr UiSettingsSchemaNode keybind_nodes[]{
    node(utf8_literal("keybinds.custom"), K::category, utf8_literal("Custom keybinds"),
         utf8_literal("Voice keybinds also work while MaiCord is in the background during a call."),
         utf8_literal("hotkeys global keyboard"), SettingBinding::keybinds_custom),
    node(utf8_literal("keybinds.add"), K::action, utf8_literal("Add a keybind"),
         utf8_literal("Choose a voice action, then press the keys to use."),
         utf8_literal("new hotkey shortcut push to talk ptt mute deafen"), SettingBinding::keybinds_add, {}, none,
         utf8_literal("Add keybind"), dynamic, static_cast<u8>(UiSettingsVariant::primary)),
    node(utf8_literal("keybinds.defaults"), K::notice, utf8_literal("Default shortcuts"),
         utf8_literal("Built-in shortcuts can't be changed. Press Ctrl+/ anywhere to see them."),
         utf8_literal("keyboard shortcuts hotkeys combos"), SettingBinding::keybinds_defaults, {}, none, {}, dynamic),
};

constexpr UiSettingsSchemaNode system_nodes[]{
    node(utf8_literal("system.startup"), K::category, utf8_literal("Startup"), {}, {}),
    node(utf8_literal("system.open_on_startup"), K::toggle, utf8_literal("Open MaiCord on startup"),
         utf8_literal("Start MaiCord when you sign in to Windows."), utf8_literal("boot login autostart run"),
         SettingBinding::open_on_startup, {}, none, {}, dynamic),
    node(utf8_literal("system.start_minimized"), K::toggle, utf8_literal("Start minimized"),
         utf8_literal("MaiCord starts in the background and stays out of your way."),
         utf8_literal("boot background hidden tray"), SettingBinding::start_minimized, {}, none, {}, dynamic),
    node(utf8_literal("system.window"), K::category, utf8_literal("Close button"), {}, {}),
    node(utf8_literal("system.minimize_to_tray"), K::toggle, utf8_literal("Minimize to tray"),
         utf8_literal("Closing the window keeps MaiCord running in the notification area."),
         utf8_literal("system tray notification area close exit background"), SettingBinding::minimize_to_tray),
    node(utf8_literal("system.links"), K::category, utf8_literal("Links"), {}, {}),
    node(utf8_literal("system.protocol_handler"), K::toggle, utf8_literal("Open discord:// links in MaiCord"),
         utf8_literal("Invite and message links from your browser and other apps open here."),
         utf8_literal("protocol url handler invite browser default"), SettingBinding::protocol_handler, {}, none, {},
         dynamic),
};

constexpr UiSettingsSchemaNode activity_nodes[]{
    node(utf8_literal("activity.status"), K::category, utf8_literal("Activity status"), {}, {}),
    node(utf8_literal("activity.share"), K::toggle, utf8_literal("Share my activity"),
         utf8_literal("Show the game you're playing on your profile and in member lists."),
         utf8_literal("activity status playing game rich presence privacy"), SettingBinding::share_activity, {}, none,
         {}, dynamic),
    node(utf8_literal("activity.detection"), K::toggle, utf8_literal("Detect games"),
         utf8_literal("Check running programs every 20 seconds. Program names stay on this device; only the "
                      "game you're playing is shared."),
         utf8_literal("game detection running programs processes registered games"), SettingBinding::game_detection),
    node(utf8_literal("activity.games"), K::category, utf8_literal("Registered games"),
         utf8_literal("Games MaiCord recognizes while they run."), utf8_literal("registered games added detected"),
         SettingBinding::registered_games),
    node(utf8_literal("activity.add_game"), K::choice, utf8_literal("Add a game"),
         utf8_literal("Pick a running program to show as the game you're playing."),
         utf8_literal("add game not detected program"), SettingBinding::add_game, {}, none, {}, dynamic),
};

constexpr UiSettingsSchemaSection sections[]{{utf8_literal("USER SETTINGS")},
                                             {utf8_literal("APP SETTINGS")},
                                             {utf8_literal("MAICORD")},
                                             {{}}};
constexpr UiSettingsSchemaPanel panels[]{
    {utf8_literal("notifications"), utf8_literal("Notifications"), utf8_literal("alerts sounds"), UiIcon::bell, 0,
     {notifications, sizeof(notifications) / sizeof(notifications[0])}},
    {utf8_literal("appearance"), utf8_literal("Appearance"), utf8_literal("theme colors look"), UiIcon::palette, 1,
     {appearance, sizeof(appearance) / sizeof(appearance[0])}},
    {utf8_literal("accessibility"), utf8_literal("Accessibility"), utf8_literal("motion a11y"), UiIcon::accessibility,
     1, {accessibility, sizeof(accessibility) / sizeof(accessibility[0])}},
    {utf8_literal("plugins"), utf8_literal("Plugins"), utf8_literal("extensions add-ons modules install"),
     UiIcon::puzzle, 2, {}},
    {utf8_literal("advanced"), utf8_literal("Advanced"), utf8_literal("developer"), UiIcon::code, 3,
     {advanced, sizeof(advanced) / sizeof(advanced[0])}},
    {utf8_literal("updates"), utf8_literal("Updates"), utf8_literal("update version upgrade release"), UiIcon::download, 2,
     {updates, sizeof(updates) / sizeof(updates[0])}},
    {utf8_literal("voice"), utf8_literal("Voice"), utf8_literal("microphone speakers headphones audio calls video"),
     UiIcon::headphones, 1, {voice_nodes, sizeof(voice_nodes) / sizeof(voice_nodes[0])}},
    {utf8_literal("account"), utf8_literal("My account"), utf8_literal("email phone username password"),
     UiIcon::user_circle, 0, {account_nodes, sizeof(account_nodes) / sizeof(account_nodes[0])}},
    {utf8_literal("profiles"), utf8_literal("Profiles"), utf8_literal("profile pronouns about me bio"),
     UiIcon::user_edit, 0, {profile_nodes, sizeof(profile_nodes) / sizeof(profile_nodes[0])}},
    {utf8_literal("devices"), utf8_literal("Devices"), utf8_literal("sessions logged in log out"), UiIcon::smartphone,
     0, {device_nodes, sizeof(device_nodes) / sizeof(device_nodes[0])}},
    {utf8_literal("content_social"), utf8_literal("Content & social"),
     utf8_literal("privacy safety direct messages friend requests"), UiIcon::message_text, 0,
     {content_nodes, sizeof(content_nodes) / sizeof(content_nodes[0])}},
    {utf8_literal("data_privacy"), utf8_literal("Data & privacy"), utf8_literal("privacy profile"),
     UiIcon::shield_check, 0, {privacy_nodes, sizeof(privacy_nodes) / sizeof(privacy_nodes[0])}},
    {utf8_literal("authorized_apps"), utf8_literal("Authorized apps"), utf8_literal("connected apps oauth"),
     UiIcon::key, 0, {app_nodes, sizeof(app_nodes) / sizeof(app_nodes[0])}},
    {utf8_literal("language_time"), utf8_literal("Language & time"), utf8_literal("time format clock"),
     UiIcon::languages, 1, {language_nodes, sizeof(language_nodes) / sizeof(language_nodes[0])}},
    {utf8_literal("chat"), utf8_literal("Chat"), utf8_literal("text images embeds reactions spoilers emoji gifs"),
     UiIcon::direct_message, 1, {chat_nodes, sizeof(chat_nodes) / sizeof(chat_nodes[0])}},
    {utf8_literal("keybinds"), utf8_literal("Keybinds"), utf8_literal("keyboard shortcuts hotkeys keys"),
     UiIcon::keyboard, 1, {keybind_nodes, sizeof(keybind_nodes) / sizeof(keybind_nodes[0])}},
    {utf8_literal("system"), utf8_literal("System"), utf8_literal("windows startup tray links protocol"),
     UiIcon::monitor, 1, {system_nodes, sizeof(system_nodes) / sizeof(system_nodes[0])}},
    {utf8_literal("activity_privacy"), utf8_literal("Activity privacy"),
     utf8_literal("activity status games playing registered detection"), UiIcon::activity, 0,
     {activity_nodes, sizeof(activity_nodes) / sizeof(activity_nodes[0])}},
};
static_assert(sizeof(panels) / sizeof(panels[0]) == kSettingsPanelCount);
constexpr UiSettingsSchema catalog{{sections, 4}, {panels, kSettingsPanelCount}};

constexpr UiSettingsNavEntry nav_header(u32 section) noexcept {
    return {UiSettingsNavKind::header, false, UiIcon::count, sections[section].header, {}, 0};
}
constexpr UiSettingsNavEntry nav_page(SettingsPanel panel) noexcept {
    const auto& p = panels[static_cast<u32>(panel)];
    return {UiSettingsNavKind::page, false, p.icon, p.title, {kSettingsNativeOwner, static_cast<u64>(panel) + 1}, 0};
}
constexpr UiSettingsNavEntry nav_separator{UiSettingsNavKind::separator, false, UiIcon::count, {}, {}, 0};
constexpr UiSettingsNavEntry navigation[]{
    nav_header(0),
    nav_page(SettingsPanel::account),
    nav_page(SettingsPanel::profiles),
    nav_page(SettingsPanel::devices),
    nav_page(SettingsPanel::content_social),
    nav_page(SettingsPanel::data_privacy),
    nav_page(SettingsPanel::authorized_apps),
    nav_page(SettingsPanel::activity_privacy),
    nav_page(SettingsPanel::notifications),
    nav_separator,
    nav_header(1),
    nav_page(SettingsPanel::appearance),
    nav_page(SettingsPanel::chat),
    nav_page(SettingsPanel::accessibility),
    nav_page(SettingsPanel::voice),
    nav_page(SettingsPanel::keybinds),
    nav_page(SettingsPanel::language_time),
    nav_page(SettingsPanel::system),
    nav_separator,
    nav_header(2),
    nav_page(SettingsPanel::updates),
    nav_page(SettingsPanel::plugins),
    {UiSettingsNavKind::contributed, false, UiIcon::count, {}, {}, 0},
    nav_separator,
    nav_page(SettingsPanel::advanced),
    {UiSettingsNavKind::action, true, UiIcon::log_out, utf8_literal("Log out"), {}, kSettingsLogOutAction},
};
constexpr u32 kNavigationCount = sizeof(navigation) / sizeof(navigation[0]);

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
    number_labels_initialize();
    return catalog;
}
const UiSettingsSchemaPanel& settings_catalog_panel(SettingsPanel panel) noexcept {
    number_labels_initialize();
    return panels[static_cast<u32>(panel) < kSettingsPanelCount ? static_cast<u32>(panel) : 0];
}

ConstSlice<UiSettingsNavEntry> settings_catalog_navigation(bool log_out) noexcept {
    return {navigation, log_out ? kNavigationCount : kNavigationCount - 1};
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
    number_labels_initialize();
    return location.panel < kSettingsPanelCount && location.node < panels[location.panel].nodes.count
               ? &panels[location.panel].nodes[location.node]
               : nullptr;
}
const UiSettingsSchemaNode* settings_catalog_binding(SettingBinding binding,
                                                     UiSettingsSchemaLocation* location) noexcept {
    number_labels_initialize();
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

bool settings_store_read(const ClientPreferences& p, SettingBinding binding, SettingValue* value) noexcept {
    if (!value)
        return false;
    *value = {};
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
    case SettingBinding::member_list:
        value->checked = p.member_pane_visible;
        return true;
    case SettingBinding::reduced_motion:
        value->checked = p.reduced_motion;
        return true;
    case SettingBinding::smooth_scroll:
        value->checked = p.smooth_scroll;
        return true;
    case SettingBinding::desktop_notifications:
        value->checked = p.desktop_enabled;
        return true;
    case SettingBinding::message_sounds:
        value->checked = p.message_sounds_enabled;
        return true;
    case SettingBinding::selected_channel_sounds:
        value->checked = p.selected_message_sounds_enabled;
        return true;
    case SettingBinding::output_volume:
        value->choice = p.output_volume;
        return true;
    case SettingBinding::developer_mode:
        value->checked = p.developer_mode;
        return true;
    case SettingBinding::unread_badge:
        value->checked = p.unread_badge;
        return true;
    case SettingBinding::taskbar_flash:
        value->checked = p.taskbar_flash;
        return true;
    case SettingBinding::start_minimized:
        value->checked = p.start_minimized;
        return true;
    case SettingBinding::minimize_to_tray:
        value->checked = p.minimize_to_tray;
        return true;
    case SettingBinding::spellcheck:
        value->checked = p.spellcheck;
        return true;
    case SettingBinding::game_detection:
        value->checked = p.game_detection;
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
    case SettingBinding::member_list:
        p->member_pane_visible = value.checked;
        return true;
    case SettingBinding::reduced_motion:
        p->reduced_motion = value.checked;
        return true;
    case SettingBinding::smooth_scroll:
        p->smooth_scroll = value.checked;
        return true;
    case SettingBinding::desktop_notifications:
        p->desktop_enabled = value.checked;
        return true;
    case SettingBinding::message_sounds:
        p->message_sounds_enabled = value.checked;
        return true;
    case SettingBinding::selected_channel_sounds:
        p->selected_message_sounds_enabled = value.checked;
        return true;
    case SettingBinding::output_volume:
        if (value.choice > 200)
            return false;
        p->output_volume = static_cast<u8>(value.choice);
        return true;
    case SettingBinding::developer_mode:
        p->developer_mode = value.checked;
        return true;
    case SettingBinding::unread_badge:
        p->unread_badge = value.checked;
        return true;
    case SettingBinding::taskbar_flash:
        p->taskbar_flash = value.checked;
        return true;
    case SettingBinding::start_minimized:
        p->start_minimized = value.checked;
        return true;
    case SettingBinding::minimize_to_tray:
        p->minimize_to_tray = value.checked;
        return true;
    case SettingBinding::spellcheck:
        p->spellcheck = value.checked;
        return true;
    case SettingBinding::game_detection:
        p->game_detection = value.checked;
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
