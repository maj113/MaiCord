#include "catalog.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/settings/client_theme.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/packed_reader.hpp"
#include "maicord/media/compression/inflated_block.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/theme/background_color_math.hpp"

namespace maicord {
namespace {
constexpr ClientTheme base_theme_values[]{ClientTheme::light, ClientTheme::dark, ClientTheme::amoled};
constexpr ClientTheme color_theme_values[]{ClientTheme::aurora, ClientTheme::sunset, ClientTheme::ocean,
                                           ClientTheme::custom_gradient, ClientTheme::custom_light_gradient};

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
// Image zoom sliders: "1x".."50x" in half steps and "50 px".."1000 px" in tens.
NumberLabels<99, 6> zoom_labels;
NumberLabels<96, 8> lens_labels;
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
            static constexpr char minus[] = "\xE2\x88\x92";
            for (u32 i = 0; i < 3; ++i)
                text[at++] = minus[i];
        }
        append_suffix(decibel_labels, value, at + decimal_write(text + at, db), " dB", 3);
    }
    for (u32 value = 0; value <= 200; ++value)
        append_suffix(millisecond_labels, value, decimal_write(millisecond_labels.text[value], value * 10),
                      " ms", 3);
    for (u32 half = 2; half <= 100; ++half) {
        char* text = zoom_labels.text[half - 2];
        u32 at = decimal_write(text, half / 2);
        if (half & 1) {
            text[at++] = '.';
            text[at++] = '5';
        }
        append_suffix(zoom_labels, half - 2, at, "x", 1);
    }
    for (u32 tens = 5; tens <= 100; ++tens)
        append_suffix(lens_labels, tens - 5, decimal_write(lens_labels.text[tens - 5], tens * 10), " px", 3);
    number_labels_ready = true;
}

constexpr u32 none = kUiSettingsNoChoice;
constexpr u32 bind(SettingBinding binding) noexcept {
    return static_cast<u32>(binding);
}

// The catalog's text and tables (src/app/settings/catalog_data.inc) ship as one zlib stream that
// tools/pack_settings_catalog.py writes: the first use inflates it and fills the zeroed tables below, instead of
// the image holding the strings, the node and panel tables and their relocations as constants.
#include <settings_catalog_pack.inc>
static_assert(kSettingsCatalogPanels == kSettingsPanelCount);

// The sidebar's order: a group header (kNavigationHeader | section), a panel (kNavigationPinned pins it to the
// foot) or the Log Out action; both lists are in the same order.
constexpr u8 kNavigationHeader = 0x80, kNavigationPinned = 0x40, kNavigationLogOut = 0xff;
constexpr u8 header(u32 section) noexcept {
    return static_cast<u8>(kNavigationHeader | section);
}
constexpr u8 page(SettingsPanel panel, bool pinned = false) noexcept {
    return static_cast<u8>(static_cast<u32>(panel) | (pinned ? kNavigationPinned : 0));
}
// The official client's order: the profile entry and the account pages under no title, then titled groups.
constexpr u8 kNavigation[]{
    page(SettingsPanel::profiles),
    page(SettingsPanel::account),
    // Devices is nested in My Account, as in the official client: Account's entry stays selected on it.
    page(SettingsPanel::data_privacy),
    page(SettingsPanel::content_social),
    page(SettingsPanel::notifications),
    header(1),
    page(SettingsPanel::voice),
    page(SettingsPanel::appearance),
    page(SettingsPanel::accessibility),
    page(SettingsPanel::system),
    page(SettingsPanel::keybinds),
    page(SettingsPanel::language_time),
    header(2),
    page(SettingsPanel::registered_games),
    page(SettingsPanel::activity_privacy),
    page(SettingsPanel::authorized_apps),
    header(3),
    page(SettingsPanel::updates),
    page(SettingsPanel::plugins),
    page(SettingsPanel::themes),
    page(SettingsPanel::effects),
    page(SettingsPanel::advanced, true),
    kNavigationLogOut,
};
// Before sign-in: the account-independent pages in the same order.
constexpr u8 kSignedOutNavigation[]{
    header(1),
    page(SettingsPanel::voice),
    page(SettingsPanel::appearance),
    page(SettingsPanel::accessibility),
    page(SettingsPanel::system),
    page(SettingsPanel::keybinds),
    page(SettingsPanel::language_time),
    header(3),
    page(SettingsPanel::updates),
    page(SettingsPanel::advanced, true),
};
constexpr u32 kNavigationCount = sizeof(kNavigation), kSignedOutNavigationCount = sizeof(kSignedOutNavigation);

struct Tables {
    UiSettingsSchemaSection sections[kSettingsCatalogSections];
    UiSettingsSchemaPanel panels[kSettingsCatalogPanels];
    UiSettingsSchemaNode nodes[kSettingsCatalogNodes];
    Utf8View choices[kSettingsCatalogChoices];
    UiSettingsSchema schema;
    UiSettingsNavEntry navigation[kNavigationCount], signed_out_navigation[kSignedOutNavigationCount];
};
Tables g{}; // value-initialised: constant-initialised zeros, so no dynamic initializer (.CRT) and no .data
InflatedBlock block;
// The inflated stream's format is in tools/pack_settings_catalog.py.
// The six numbered label sets, in the order the packer numbers them.
const Utf8View* label_set(u32 set) noexcept {
    return set == 0   ? degree_labels.values
           : set == 1 ? percent_labels.values
           : set == 2 ? decibel_labels.values
           : set == 3 ? millisecond_labels.values
           : set == 4 ? zoom_labels.values
                      : lens_labels.values;
}
void build_navigation(UiSettingsNavEntry* out, const u8* steps, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i) {
        const u32 step = steps[i];
        if (step == kNavigationLogOut) {
            out[i] = {UiSettingsNavKind::action, true, UiIcon::log_out, utf8_literal("Log Out"), {}, kSettingsLogOutAction,
                      true};
        } else if (step & kNavigationHeader) {
            out[i] = {UiSettingsNavKind::header, false, UiIcon::count, g.sections[step & ~u32{kNavigationHeader}].header,
                      {}, 0};
        } else {
            const u32 panel = step & ~u32{kNavigationPinned};
            out[i] = {UiSettingsNavKind::page,
                      false,
                      g.panels[panel].icon,
                      g.panels[panel].title,
                      {kSettingsNativeOwner, static_cast<u64>(panel) + 1},
                      0,
                      (step & kNavigationPinned) != 0};
            if (panel == static_cast<u32>(SettingsPanel::account))
                out[i].subpage = {kSettingsNativeOwner, static_cast<u64>(SettingsPanel::devices) + 1};
        }
    }
}

#if !defined(_WIN32)
// The Linux wording of what catalog_data.inc says in Windows terms. The data file and its shipped stream stay the
// Windows text (the Windows image and tests/app/settings/catalog_pack.cpp read them as they are); the Linux build
// replaces these fields once after inflating. An entry is four names (key, label, description, keywords); an empty
// field keeps the data's.
struct LinuxText {
    Utf8View key, label, description, keywords;
};
constexpr char kLinuxNodeText[] =
    "notifications.setup\0" "\0" "\0" "permission enable notification service desktop\0"
    "notifications.taskbar_flash\0" "Enable App Icon Flashing\0"
    "Flashes the app's icon in your dock or panel when you have new notifications.\0"
    "dock launcher panel icon flash attention blink urgent\0"
    "notifications.unread_badge\0" "\0" "\0" "dock launcher icon overlay badge count dot\0"
    "language.time_format\0" "\0" "Auto follows your system locale.\0" "\0"
    "voice.push_to_talk_key\0" "\0"
    "Hold this key to talk. Where your desktop allows global shortcuts it works in the background too.\0" "\0"
    "keybinds.custom\0" "\0"
    "Keybinds are custom keyboard shortcuts that let you perform specific actions in MaiCord without needing to "
    "click through menus. Voice keybinds also work in the background during a call if your desktop allows "
    "global shortcuts.\0" "\0"
    // A footnote's text is its label.
    "updates.assets\0" "Updates replace MaiCord, its fonts and its licenses.\0" "\0" "\0";
constexpr char kLinuxPanelText[] =
    "system\0" "\0" "Startup, the tray and how links open on your desktop.\0" "startup autostart tray links protocol\0"
    "updates\0" "\0" "Updates are signed. Nothing installs until you restart.\0" "\0";
LinuxText next_text(NameList& list) noexcept {
    LinuxText text{};
    Utf8View* const fields[]{&text.key, &text.label, &text.description, &text.keywords};
    for (Utf8View* field : fields) {
        *field = list.name();
        list.next();
    }
    return text;
}
void apply_linux_text() noexcept {
    for (NameList list{kLinuxNodeText}; list;) {
        const LinuxText text = next_text(list);
        for (auto& node : g.nodes) {
            if (!utf8_equal(node.key(), text.key))
                continue;
            if (text.label.bytes) {
                node.label_data = text.label.data;
                node.label_bytes = static_cast<u8>(text.label.bytes);
            }
            if (text.description.bytes) {
                node.description_data = text.description.data;
                node.description_bytes = static_cast<u8>(text.description.bytes);
            }
            if (text.keywords.bytes) {
                node.keywords_data = text.keywords.data;
                node.keywords_bytes = static_cast<u8>(text.keywords.bytes);
            }
            break;
        }
    }
    for (NameList list{kLinuxPanelText}; list;) {
        const LinuxText text = next_text(list);
        for (auto& panel : g.panels) {
            if (!utf8_equal(panel.key, text.key))
                continue;
            if (text.description.bytes)
                panel.summary = text.description;
            if (text.keywords.bytes)
                panel.keywords = text.keywords;
            break;
        }
    }
}
#endif
// Runs once on the inflated bytes, before any thread sees them.
void publish(const u8* data) noexcept {
    PackedReader in{data};
    in.byte();
    for (auto& section : g.sections)
        section.header = in.text();
    in.byte();
    u32 first = 0;
    for (auto& panel : g.panels) {
        panel.key = in.text();
        panel.title = in.text();
        panel.keywords = in.text();
        panel.summary = in.text();
        panel.icon = static_cast<UiIcon>(in.byte());
        panel.section = static_cast<u8>(in.byte());
        const u32 count = in.word();
        panel.nodes = count ? ConstSlice<UiSettingsSchemaNode>{g.nodes + first, count} : ConstSlice<UiSettingsSchemaNode>{};
        first += count;
    }
    u32 choices = 0;
    for (auto& node : g.nodes) {
        const Utf8View key = in.text(), label = in.text(), description = in.text(), keywords = in.text(),
                       action = in.text();
        node.key_data = key.data;
        node.key_bytes = static_cast<u8>(key.bytes);
        node.label_data = label.data;
        node.label_bytes = static_cast<u8>(label.bytes);
        node.description_data = description.data;
        node.description_bytes = static_cast<u8>(description.bytes);
        node.keywords_data = keywords.data;
        node.keywords_bytes = static_cast<u8>(keywords.bytes);
        node.action_data = action.data;
        node.action_bytes = static_cast<u8>(action.bytes);
        node.kind = static_cast<UiSettingsControlKind>(in.byte());
        node.variant = static_cast<u8>(in.byte());
        node.flags = static_cast<u16>(in.word());
        node.binding_slot = static_cast<u16>(in.word());
        node.default_choice_slot = static_cast<u16>(in.word());
        node.choice_data = nullptr;
        node.choice_count = 0;
        if (const u32 mode = in.byte(); mode == 1) {
            node.choice_count = static_cast<u16>(in.byte());
            node.choice_data = g.choices + choices;
            for (u32 i = 0; i < node.choice_count; ++i)
                g.choices[choices++] = in.text();
        } else if (mode == 2) {
            const u32 set = in.byte();
            node.choice_count = static_cast<u16>(in.word());
            node.choice_data = label_set(set);
        }
    }
#if !defined(_WIN32)
    apply_linux_text();
#endif
    g.schema = {{g.sections, kSettingsCatalogSections}, {g.panels, kSettingsCatalogPanels}};
    build_navigation(g.navigation, kNavigation, kNavigationCount);
    build_navigation(g.signed_out_navigation, kSignedOutNavigation, kSignedOutNavigationCount);
}
// False only when the pages for the tables could not be allocated or the stream failed to inflate.
bool ready() noexcept {
    number_labels_initialize();
    return block.state == 2 || inflated_block_load(block, kSettingsCatalogPacked, sizeof(kSettingsCatalogPacked),
                                                   kSettingsCatalogBytes, publish) != nullptr;
}
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
    {SettingBinding::role_colors_everywhere, &ClientPreferences::role_colors_everywhere},
    {SettingBinding::reply_timestamps, &ClientPreferences::reply_timestamps},
    {SettingBinding::folders_close_others, &ClientPreferences::folders_close_others},
    {SettingBinding::folders_close_on_home, &ClientPreferences::folders_close_on_home},
    {SettingBinding::folders_follow_selection, &ClientPreferences::folders_follow_selection},
    {SettingBinding::show_hidden_channels, &ClientPreferences::show_hidden_channels},
    {SettingBinding::tabs, &ClientPreferences::tabs_enabled},
    {SettingBinding::image_zoom, &ClientPreferences::image_zoom},
    {SettingBinding::image_zoom_square, &ClientPreferences::image_zoom_square},
    {SettingBinding::game_detection, &ClientPreferences::game_detection},
    {SettingBinding::underline_links, &ClientPreferences::underline_links},
    {SettingBinding::desaturate_user_colors, &ClientPreferences::desaturate_user_colors},
    {SettingBinding::reduced_motion_sync, &ClientPreferences::reduced_motion_sync},
    {SettingBinding::streamer_mode, &ClientPreferences::streamer_mode},
    {SettingBinding::streamer_auto, &ClientPreferences::streamer_auto},
    {SettingBinding::streamer_hide_personal, &ClientPreferences::streamer_hide_personal},
    {SettingBinding::streamer_hide_invites, &ClientPreferences::streamer_hide_invites},
    {SettingBinding::streamer_disable_sounds, &ClientPreferences::streamer_disable_sounds},
    {SettingBinding::streamer_disable_notifications, &ClientPreferences::streamer_disable_notifications},
    {SettingBinding::streamer_hide_window, &ClientPreferences::streamer_hide_window},
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
    case SettingBinding::role_colors_everywhere:
    case SettingBinding::reply_timestamps:
    case SettingBinding::folders_close_others:
    case SettingBinding::folders_close_on_home:
    case SettingBinding::folders_follow_selection:
    case SettingBinding::show_hidden_channels:
    case SettingBinding::tabs:
    case SettingBinding::image_zoom:
    case SettingBinding::image_zoom_square:
    case SettingBinding::image_zoom_factor:
    case SettingBinding::image_zoom_size:
    case SettingBinding::game_detection:
    case SettingBinding::chat_font_size:
    case SettingBinding::underline_links:
    case SettingBinding::zoom:
    case SettingBinding::saturation:
    case SettingBinding::desaturate_user_colors:
    case SettingBinding::role_style:
    case SettingBinding::reduced_motion_sync:
    case SettingBinding::speech_rate:
    case SettingBinding::streamer_mode:
    case SettingBinding::streamer_auto:
    case SettingBinding::streamer_hide_personal:
    case SettingBinding::streamer_hide_invites:
    case SettingBinding::streamer_disable_sounds:
    case SettingBinding::streamer_disable_notifications:
    case SettingBinding::streamer_hide_window:
    case SettingBinding::speak_messages:
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
    case SettingBinding::language:
    case SettingBinding::notifications_notice:
    case SettingBinding::notify_go_live:
    case SettingBinding::notify_friend_online:
    case SettingBinding::notify_profile_updates:
    case SettingBinding::notify_anniversaries:
    case SettingBinding::notify_reactions:
    case SettingBinding::mobile_push_delay:
    case SettingBinding::game_mentions_autocomplete:
    case SettingBinding::ui_density:
    case SettingBinding::compact_avatars:
    case SettingBinding::image_descriptions:
    case SettingBinding::sticker_autocomplete:
    case SettingBinding::tts_command:
        return SettingBackend::proto1;
    case SettingBinding::account_card:
    case SettingBinding::account_devices:
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
    case SettingBinding::account_totp:
    case SettingBinding::account_sms:
    case SettingBinding::account_backup_codes:
    case SettingBinding::security_keys_list:
    case SettingBinding::account_key_add:
    case SettingBinding::account_disable:
    case SettingBinding::account_delete:
    case SettingBinding::devices_logout_all:
    case SettingBinding::connections_list:
    case SettingBinding::data_notice:
    case SettingBinding::data_usage_statistics:
    case SettingBinding::data_activity_link:
    case SettingBinding::data_request:
    case SettingBinding::harvest_account:
    case SettingBinding::harvest_analytics:
    case SettingBinding::harvest_activities:
    case SettingBinding::harvest_ads:
    case SettingBinding::harvest_messages:
    case SettingBinding::harvest_servers:
    case SettingBinding::harvest_zendesk:
    case SettingBinding::blocked_list:
    case SettingBinding::ignored_list:
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

#if !defined(_WIN32)
bool settings_binding_unavailable(SettingBinding binding) noexcept {
    return binding == SettingBinding::windows_notification_settings || binding == SettingBinding::voice_processing ||
           binding == SettingBinding::streamer_hide_window;
}
#endif

const UiSettingsSchema& settings_catalog() noexcept {
    ready();
    return g.schema;
}
const UiSettingsSchemaPanel& settings_catalog_panel(SettingsPanel panel) noexcept {
    ready();
    return g.panels[static_cast<u32>(panel)];
}

ConstSlice<UiSettingsNavEntry> settings_catalog_navigation(bool log_out) noexcept {
    return ready() ? ConstSlice<UiSettingsNavEntry>{g.navigation, log_out ? kNavigationCount : kNavigationCount - 1}
                   : ConstSlice<UiSettingsNavEntry>{};
}
ConstSlice<UiSettingsNavEntry> settings_catalog_navigation_signed_out() noexcept {
    return ready() ? ConstSlice<UiSettingsNavEntry>{g.signed_out_navigation, kSignedOutNavigationCount}
                   : ConstSlice<UiSettingsNavEntry>{};
}
ConstSlice<UiSettingsNavEntry> settings_catalog_navigation_dotted(ConstSlice<UiSettingsNavEntry> navigation,
                                                                   UiSettingsNavEntry* storage) noexcept {
    if (!storage || navigation.count > kUiSettingsNavigationMaximum)
        return {};
    copy_bytes(storage, navigation.data, navigation.count * sizeof(UiSettingsNavEntry));
    for (u32 i = 0; i < navigation.count; ++i)
        if (storage[i].kind == UiSettingsNavKind::page &&
            ui_page_key_equal(storage[i].page, {kSettingsNativeOwner, static_cast<u64>(SettingsPanel::updates) + 1}))
            storage[i].badge = kUiSettingsNavDot;
    return {storage, navigation.count};
}

bool settings_control_location(u64 id, UiSettingsSchemaLocation* location) noexcept {
    const u64 panel = (id >> 16) - 1, node = (id & 0xffff) - 1;
    if (!location || !(id >> 16) || !(id & 0xffff) || (id >> 32) || panel >= kSettingsPanelCount || !ready() ||
        node >= g.panels[panel].nodes.count)
        return false;
    *location = {static_cast<u32>(panel), static_cast<u32>(node)};
    return true;
}
const UiSettingsSchemaNode* settings_catalog_node(UiSettingsSchemaLocation location) noexcept {
    return ready() && location.panel < kSettingsPanelCount && location.node < g.panels[location.panel].nodes.count
               ? &g.panels[location.panel].nodes[location.node]
               : nullptr;
}
const UiSettingsSchemaNode* settings_catalog_binding(SettingBinding binding,
                                                     UiSettingsSchemaLocation* location) noexcept {
    if (!ready())
        return nullptr;
    for (u32 p = 0; p < kSettingsPanelCount; ++p)
        for (u32 n = 0; n < g.panels[p].nodes.count; ++n)
            if (g.panels[p].nodes[n].binding() == bind(binding)) {
                if (location)
                    *location = {p, n};
                return &g.panels[p].nodes[n];
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
        const u32 rgb = background_color_pack(theme.base_low);
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
    reset_value(value);
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
    case SettingBinding::image_zoom_factor:
        value->choice = p.image_zoom_factor - 2u;
        return true;
    case SettingBinding::image_zoom_size:
        value->choice = p.image_zoom_size - 5u;
        return true;
    case SettingBinding::chat_font_size:
        // Unset follows the theme's body size; the projection marks the nearest size.
        value->choice = p.chat_font_size ? client_preferences_nearest_mark(kClientPreferencesFontSizes, p.chat_font_size)
                                         : none;
        return true;
    case SettingBinding::zoom:
        value->choice = client_preferences_nearest_mark(kClientPreferencesZoomScales, p.zoom);
        return true;
    case SettingBinding::saturation:
        value->choice = 100u - p.desaturation;
        return true;
    case SettingBinding::role_style:
        value->choice = p.role_style;
        return true;
    case SettingBinding::speech_rate:
        value->choice = p.speech_rate;
        return true;
    case SettingBinding::speak_messages:
        // Choices in the official order: all channels, the selected channel, never.
        value->choice = client_speak_messages_all - u32{p.speak_messages};
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
    case SettingBinding::image_zoom_factor:
        if (value.choice > 98)
            return false;
        p->image_zoom_factor = static_cast<u8>(value.choice + 2);
        return true;
    case SettingBinding::image_zoom_size:
        if (value.choice > 95)
            return false;
        p->image_zoom_size = static_cast<u8>(value.choice + 5);
        return true;
    case SettingBinding::chat_font_size:
        if (value.choice >= sizeof(kClientPreferencesFontSizes))
            return false;
        p->chat_font_size = kClientPreferencesFontSizes[value.choice];
        return true;
    case SettingBinding::zoom:
        if (value.choice >= sizeof(kClientPreferencesZoomScales))
            return false;
        p->zoom = kClientPreferencesZoomScales[value.choice];
        return true;
    case SettingBinding::saturation:
        if (value.choice > 100)
            return false;
        p->desaturation = static_cast<u8>(100 - value.choice);
        return true;
    case SettingBinding::role_style:
    case SettingBinding::speech_rate:
        if (value.choice > (binding == SettingBinding::role_style ? u32{client_role_style_hidden}
                                                                  : kClientPreferencesSpeechRateCount - 1u))
            return false;
        (binding == SettingBinding::role_style ? p->role_style : p->speech_rate) = static_cast<u8>(value.choice);
        return true;
    case SettingBinding::speak_messages:
        if (value.choice > client_speak_messages_all)
            return false;
        p->speak_messages = static_cast<u8>(client_speak_messages_all - value.choice);
        return true;
    default:
        return false;
    }
}

} // namespace maicord
