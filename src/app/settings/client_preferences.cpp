#include "maicord/app/settings/client_preferences.hpp"
#include "client_gradient_record.hpp"
#include "maicord/app/account/account_profile.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"

namespace maicord {
namespace {
constexpr u8 kMagic[8] = {'M', 'A', 'I', 'C', 'P', 'R', 'E', 'F'};
constexpr u32 kHeaderBytes = 16;
constexpr u32 kChecksumOffset = 88, kVersion9Bytes = 80, kVersion9ChecksumOffset = 72;
constexpr u8 kKnownFlags = 127;
constexpr u32 kBuiltInFlags = 63;

ClientPreferencesResult failure(ClientPreferencesError error, u32 native_error = 0) noexcept {
    return {native_error, error};
}

bool valid_path(const wchar_t* path, u32* length) noexcept {
    *length = wide_length(path, kAccountProfileMaximumPathCharacters);
    return *length != 0 && *length + 4 < kAccountProfileMaximumPathCharacters;
}

ClientPreferencesResult user_path(wchar_t* path, bool create_directory) noexcept {
    // Leaves valid_path()'s four spare characters.
    const DataPathResult built =
        data_path(path, kAccountProfileMaximumPathCharacters - 4, L"preferences.bin", create_directory);
    if (!built.ok())
        return failure(built.error == DataPathError::directory ? ClientPreferencesError::directory
                                                               : ClientPreferencesError::path,
                       built.native_error);
    return {};
}
constexpr bool pane_width_valid(u16 width) noexcept {
    return width == 0 ||
           (width >= kClientPreferencesPaneWidthMinimum && width <= kClientPreferencesPaneWidthMaximum);
}
// A record byte's switches are consecutive bools in bit order (ClientPreferences), so one multiplication moves them
// between the byte's bits and the bools' bytes: no two of its partial products share a bit, so no carry reaches
// another switch.
#define AT(member) __builtin_offsetof(ClientPreferences, member)
template <typename... Offsets> constexpr bool in_bit_order(usize first, Offsets... next) noexcept {
    return ((next == ++first) && ...);
}
static_assert(in_bit_order(AT(reduced_motion), AT(smooth_scroll), AT(member_pane_visible), AT(desktop_enabled),
                           AT(message_sounds_enabled), AT(selected_message_sounds_enabled), AT(developer_mode)) &&
              in_bit_order(AT(image_zoom), AT(image_zoom_square)) &&
              in_bit_order(AT(role_colors_everywhere), AT(reply_timestamps), AT(folders_close_others),
                           AT(folders_close_on_home), AT(folders_follow_selection), AT(show_hidden_channels)) &&
              in_bit_order(AT(taskbar_flash), AT(unread_badge), AT(minimize_to_tray), AT(start_minimized),
                           AT(spellcheck), AT(game_detection), AT(dm_profile_visible)) &&
              in_bit_order(AT(reduced_motion_sync), AT(desaturate_user_colors), AT(underline_links)) &&
              in_bit_order(AT(streamer_mode), AT(streamer_auto), AT(streamer_hide_personal), AT(streamer_hide_invites),
                           AT(streamer_disable_sounds), AT(streamer_disable_notifications), AT(streamer_hide_window)));
// The byte of `count` switches from offset `first` (each bool 0 or 1).
inline u8 gather_switches(const ClientPreferences& preferences, usize first, u32 count) noexcept {
    u64 bools;
    copy_bytes(&bools, reinterpret_cast<const u8*>(&preferences) + first, sizeof(bools));
    bools &= 0x0101010101010101ull >> (64 - 8 * count);
    return static_cast<u8>(bools * 0x0102040810204080ull >> 56);
}
// Bits 0..6 of a byte (bit 7 is zero) as the eight bools from offset `first`. The ones past the byte's switches are
// other members: the decoder writes them after this.
inline void spread_switches(ClientPreferences& preferences, usize first, u32 bits) noexcept {
    const u64 bools = bits * 0x0002040810204081ull & 0x0101010101010101ull;
    copy_bytes(reinterpret_cast<u8*>(&preferences) + first, &bools, sizeof(bools));
}
// Switches that are on by default are stored inverted, so a zero bit is their default: image zoom (byte 60); role
// colours everywhere (65); taskbar flash, unread badge, minimize to tray, spellcheck and the DM profile panel (66).
constexpr u8 kInverted60 = 0x01, kInverted65 = 0x01, kInverted66 = 0x57;
// Bytes 72..75 (chat text size, desaturation, zoom, speech rate) are four consecutive members.
static_assert(AT(desaturation) == AT(chat_font_size) + 1 && AT(zoom) == AT(chat_font_size) + 2 &&
              AT(speech_rate) == AT(chat_font_size) + 3 && AT(image_zoom_size) == AT(image_zoom_factor) + 1);
// Bytes 72..87 as the defaults encode them: a version 9 record (which ends at byte 72) reads with them.
constexpr u8 kVersion10Defaults[16]{0, 0, 100, kClientPreferencesSpeechRateDefault, 1, 0x3C};
constexpr ClientPreferences kDefaults = client_preferences_defaults();
static_assert(!kDefaults.chat_font_size && !kDefaults.desaturation && kDefaults.zoom == 100 &&
              kDefaults.reduced_motion_sync && !kDefaults.desaturate_user_colors && !kDefaults.underline_links &&
              !kDefaults.role_style && kDefaults.display_name_styles && !kDefaults.streamer_mode &&
              !kDefaults.streamer_auto &&
              kDefaults.streamer_hide_personal && kDefaults.streamer_hide_invites && kDefaults.streamer_disable_sounds &&
              kDefaults.streamer_disable_notifications && !kDefaults.streamer_hide_window && !kDefaults.speak_messages &&
              !kDefaults.tabs_enabled && !kDefaults.test_mode_application);
} // namespace

bool client_preferences_valid(const ClientPreferences& preferences) noexcept {
    for (const auto profile : preferences.effect_profiles)
        if (!background_effect_settings_valid(profile))
            return false;
    return static_cast<u8>(preferences.theme) <= static_cast<u8>(ClientTheme::custom_light_gradient) &&
           static_cast<u8>(preferences.builtin_theme) <= static_cast<u8>(ClientTheme::custom_light_gradient) &&
           preferences.output_volume <= 200 &&
           preferences.message_group_spacing < kClientPreferencesGroupSpacingCount &&
           pane_width_valid(preferences.channel_pane_width) && pane_width_valid(preferences.member_pane_width) &&
           preferences.image_zoom_factor >= 2 && preferences.image_zoom_factor <= 100 &&
           preferences.image_zoom_size >= 5 && preferences.image_zoom_size <= 100 &&
           (!preferences.chat_font_size || (preferences.chat_font_size >= 12 && preferences.chat_font_size <= 24)) &&
           preferences.desaturation <= 100 && preferences.zoom >= kClientPreferencesZoomMinimum &&
           preferences.zoom <= kClientPreferencesZoomMaximum &&
           preferences.speech_rate < kClientPreferencesSpeechRateCount &&
           preferences.role_style <= client_role_style_hidden &&
           preferences.speak_messages <= client_speak_messages_all && background_theme_valid(preferences.gradient);
}

ClientPreferencesResult client_preferences_encode(const ClientPreferences& preferences,
                                                  Slice<u8> record) noexcept {
    if (!record.data || record.count != kClientPreferencesRecordBytes ||
        !client_preferences_valid(preferences))
        return failure(ClientPreferencesError::invalid_input);
    clear_bytes(record.data, record.count);
    copy_bytes(record.data, kMagic, sizeof(kMagic));
    byte_store_le32(record.data + 8, kClientPreferencesVersion);
    byte_store_le32(record.data + 12, kClientPreferencesRecordBytes);
    u8* out = record.data;
    out[16] = static_cast<u8>(preferences.theme);
    out[17] = gather_switches(preferences, AT(reduced_motion), 7);
    out[18] = preferences.output_volume;
    gradient_record_store(out, preferences.gradient, preferences.effect_profiles);
    out[57] = static_cast<u8>(static_cast<u8>(preferences.builtin_theme) + 1);
    byte_store_le16(out + 58, preferences.expression_picker_width);
    // Image zoom: flags (bit 0 off, bit 1 square lens), zoom in halves, size in tens of DIPs.
    out[60] = static_cast<u8>(gather_switches(preferences, AT(image_zoom), 2) ^ kInverted60);
    copy_bytes(out + 61, &preferences.image_zoom_factor, 2);
    out[65] = static_cast<u8>(gather_switches(preferences, AT(role_colors_everywhere), 6) ^ kInverted65);
    out[66] = static_cast<u8>(gather_switches(preferences, AT(taskbar_flash), 7) ^ kInverted66);
    out[67] = preferences.message_group_spacing;
    byte_store_le16(out + 68, preferences.channel_pane_width);
    byte_store_le16(out + 70, preferences.member_pane_width);
    copy_bytes(out + 72, &preferences.chat_font_size, 4);
    out[76] = static_cast<u8>(gather_switches(preferences, AT(reduced_motion_sync), 3) | preferences.role_style << 3 |
                              !preferences.display_name_styles << 5);
    out[77] = gather_switches(preferences, AT(streamer_mode), 7);
    out[78] = preferences.speak_messages;
    out[79] = preferences.tabs_enabled;
    byte_store_le64(out + 80, preferences.test_mode_application);
    byte_store_le64(record.data + kChecksumOffset, hash64_bytes(kHash64Seed, record.data, kChecksumOffset));
    return {};
}

ClientPreferencesResult client_preferences_decode(ConstSlice<u8> record,
                                                  ClientPreferences* preferences) noexcept {
    if (!record.data || !preferences)
        return failure(ClientPreferencesError::invalid_input);
    if (record.count < kHeaderBytes || record.count > kClientPreferencesRecordBytes)
        return failure(ClientPreferencesError::size);
    if (!bytes_equal(record.data, kMagic, sizeof(kMagic)))
        return failure(ClientPreferencesError::format);
    // Version 9 records decode with the fields added since at their defaults; older ones are not migrated and
    // callers treat them like a missing file.
    const u32 version = byte_load_le32(record.data + 8);
    if (version != kClientPreferencesVersion && version != 9)
        return failure(ClientPreferencesError::version);
    const bool current = version == kClientPreferencesVersion;
    const u32 bytes = current ? kClientPreferencesRecordBytes : kVersion9Bytes;
    const u32 checksum = current ? kChecksumOffset : kVersion9ChecksumOffset;
    if (record.count != bytes)
        return failure(ClientPreferencesError::size);
    if (byte_load_le32(record.data + 12) != bytes)
        return failure(ClientPreferencesError::format);
    // A version 9 record reads as a current one with the fields added since at their defaults.
    u8 data[kChecksumOffset];
    copy_bytes(data, record.data, kVersion9ChecksumOffset);
    copy_bytes(data + kVersion9ChecksumOffset, current ? record.data + kVersion9ChecksumOffset : kVersion10Defaults,
               sizeof(kVersion10Defaults));
    if (data[16] > static_cast<u8>(ClientTheme::custom_light_gradient) || (data[17] & ~kKnownFlags) != 0 ||
        data[18] > 200 || (data[66] & ~127u) || (data[65] & ~kBuiltInFlags) ||
        data[67] >= kClientPreferencesGroupSpacingCount || (data[60] & ~3u) ||
        (data[61] && (data[61] < 2 || data[61] > 100)) || (data[62] && (data[62] < 5 || data[62] > 100)) ||
        (data[76] & ~63u) || (data[77] & ~127u) || data[78] > client_speak_messages_all || (data[79] & ~1u))
        return failure(ClientPreferencesError::format);
    if (byte_load_le64(record.data + checksum) != hash64_bytes(kHash64Seed, record.data, checksum))
        return failure(ClientPreferencesError::checksum);
    ClientPreferences decoded;
    // In member order: a switch group's eight-byte store reaches into the members after it, written after it.
    decoded.theme = static_cast<ClientTheme>(data[16]);
    spread_switches(decoded, AT(reduced_motion), data[17]);
    decoded.output_volume = data[18];
    copy_bytes(&decoded.image_zoom_factor, data + 61, 2);
    decoded.message_group_spacing = data[67];
    gradient_record_load(data, &decoded.gradient, decoded.effect_profiles);
    spread_switches(decoded, AT(taskbar_flash), data[66] ^ kInverted66);
    spread_switches(decoded, AT(role_colors_everywhere), data[65] ^ kInverted65);
    spread_switches(decoded, AT(image_zoom), data[60] ^ kInverted60);
    decoded.channel_pane_width = byte_load_le16(data + 68);
    decoded.member_pane_width = byte_load_le16(data + 70);
    decoded.expression_picker_width = byte_load_le16(data + 58);
    // Records written before the last built-in theme existed restore the record's own theme.
    decoded.builtin_theme = data[57] && data[57] <= static_cast<u8>(ClientTheme::custom_light_gradient) + 1
                                ? static_cast<ClientTheme>(data[57] - 1)
                                : decoded.theme;
    copy_bytes(&decoded.chat_font_size, data + 72, 4);
    spread_switches(decoded, AT(reduced_motion_sync), data[76]);
    decoded.role_style = static_cast<u8>(data[76] >> 3 & 3);
    decoded.display_name_styles = !(data[76] & 32);
    spread_switches(decoded, AT(streamer_mode), data[77]);
    decoded.speak_messages = data[78];
    decoded.tabs_enabled = data[79] != 0;
    decoded.test_mode_application = byte_load_le64(data + 80);
    // Image zoom and lens size: zero restores the default.
    if (!decoded.image_zoom_factor)
        decoded.image_zoom_factor = kClientPreferencesDefaultZoomFactor;
    if (!decoded.image_zoom_size)
        decoded.image_zoom_size = kClientPreferencesDefaultZoomSize;
    if (!client_preferences_valid(decoded))
        return failure(ClientPreferencesError::format);
    *preferences = decoded;
    return {};
}

ClientPreferencesResult client_preferences_load_file(const wchar_t* path,
                                                     ClientPreferences* preferences) noexcept {
    u32 length = 0;
    if (!preferences || !valid_path(path, &length))
        return failure(ClientPreferencesError::invalid_input);
    u8 record[kClientPreferencesRecordBytes];
    const FileReadResult read = file_persistence_read_file(path, {record, sizeof(record)}, kHeaderBytes);
    switch (read.error) {
    case FileReadError::none:
        return client_preferences_decode({record, read.bytes}, preferences);
    case FileReadError::missing:
        return failure(ClientPreferencesError::missing, read.native_error);
    case FileReadError::open:
        return failure(ClientPreferencesError::open, read.native_error);
    case FileReadError::size:
        return failure(ClientPreferencesError::size, read.native_error);
    case FileReadError::read:
        break;
    }
    return failure(ClientPreferencesError::read, read.native_error);
}

ClientPreferencesResult client_preferences_save_file(const wchar_t* path,
                                                     const ClientPreferences& preferences) noexcept {
    u32 length = 0;
    if (!valid_path(path, &length))
        return failure(ClientPreferencesError::invalid_input);
    u8 record[kClientPreferencesRecordBytes];
    const auto encoded = client_preferences_encode(preferences, {record, sizeof(record)});
    if (!encoded.ok())
        return encoded;
    const auto stored = file_persistence_write_atomic(path, length, {record, sizeof(record)});
    switch (stored.error) {
    case FilePersistenceWriteError::none:
        return {};
    case FilePersistenceWriteError::open:
        return failure(ClientPreferencesError::open, stored.native_error);
    case FilePersistenceWriteError::write:
        return failure(ClientPreferencesError::write, stored.native_error);
    case FilePersistenceWriteError::flush:
        return failure(ClientPreferencesError::flush, stored.native_error);
    case FilePersistenceWriteError::commit:
        return failure(ClientPreferencesError::commit, stored.native_error);
    }
    return failure(ClientPreferencesError::write, stored.native_error);
}

ClientPreferencesResult client_preferences_load(ClientPreferences* preferences) noexcept {
    if (!preferences)
        return failure(ClientPreferencesError::invalid_input);
    wchar_t path[kAccountProfileMaximumPathCharacters];
    const auto result = user_path(path, false);
    return result.ok() ? client_preferences_load_file(path, preferences) : result;
}

ClientPreferencesResult client_preferences_save(const ClientPreferences& preferences) noexcept {
    if (!client_preferences_valid(preferences))
        return failure(ClientPreferencesError::invalid_input);
    wchar_t path[kAccountProfileMaximumPathCharacters];
    const auto result = user_path(path, true);
    return result.ok() ? client_preferences_save_file(path, preferences) : result;
}

#undef AT
} // namespace maicord
