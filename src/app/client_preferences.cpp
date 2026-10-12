#include "maicord/app/client_preferences.hpp"
#include "maicord/app/account_profile.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/win32_process.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u8 kMagic[8] = {'M', 'A', 'I', 'C', 'P', 'R', 'E', 'F'};
constexpr u32 kHeaderBytes = 16;
constexpr u32 kChecksumOffset = 72;
constexpr u8 kKnownFlags = 127;

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
} // namespace

bool client_preferences_valid(const ClientPreferences& preferences) noexcept {
    for (const auto profile : preferences.effect_profiles)
        if (!background_effect_settings_valid(profile))
            return false;
    return static_cast<u8>(preferences.theme) <= static_cast<u8>(ClientTheme::custom_light_gradient) &&
           preferences.output_volume <= 200 && notification_soundpack_valid(preferences.soundpack) &&
           preferences.message_group_spacing < kClientPreferencesGroupSpacingCount &&
           pane_width_valid(preferences.channel_pane_width) && pane_width_valid(preferences.member_pane_width) &&
           background_theme_valid(preferences.gradient);
}

bool client_preferences_equal(const ClientPreferences& a, const ClientPreferences& b) noexcept {
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i) {
        const auto x = a.effect_profiles[i], y = b.effect_profiles[i];
        if (x.speed != y.speed || x.strength != y.strength || x.density != y.density)
            return false;
    }
    return a.theme == b.theme && a.reduced_motion == b.reduced_motion &&
           a.smooth_scroll == b.smooth_scroll && a.member_pane_visible == b.member_pane_visible &&
           a.desktop_enabled == b.desktop_enabled && a.message_sounds_enabled == b.message_sounds_enabled &&
           a.selected_message_sounds_enabled == b.selected_message_sounds_enabled &&
           a.output_volume == b.output_volume && a.soundpack == b.soundpack &&
           a.developer_mode == b.developer_mode && a.taskbar_flash == b.taskbar_flash &&
           a.unread_badge == b.unread_badge && a.minimize_to_tray == b.minimize_to_tray &&
           a.start_minimized == b.start_minimized && a.spellcheck == b.spellcheck &&
           a.game_detection == b.game_detection &&
           a.message_group_spacing == b.message_group_spacing &&
           a.channel_pane_width == b.channel_pane_width && a.member_pane_width == b.member_pane_width &&
           background_theme_equal(a.gradient, b.gradient);
}

ClientPreferencesResult client_preferences_encode(const ClientPreferences& preferences,
                                                  Slice<u8> record) noexcept {
    if (!record.data || record.count != kClientPreferencesRecordBytes ||
        !client_preferences_valid(preferences))
        return failure(ClientPreferencesError::invalid_input);
    for (u32 i = 0; i < record.count; ++i)
        record.data[i] = 0;
    for (u32 i = 0; i < sizeof(kMagic); ++i)
        record.data[i] = kMagic[i];
    byte_store_le32(record.data + 8, kClientPreferencesVersion);
    byte_store_le32(record.data + 12, kClientPreferencesRecordBytes);
    record.data[16] = static_cast<u8>(preferences.theme);
    record.data[17] = static_cast<u8>(
        (preferences.reduced_motion ? 1u : 0u) | (preferences.smooth_scroll ? 2u : 0u) |
        (preferences.member_pane_visible ? 4u : 0u) | (preferences.desktop_enabled ? 8u : 0u) |
        (preferences.message_sounds_enabled ? 16u : 0u) |
        (preferences.selected_message_sounds_enabled ? 32u : 0u) | (preferences.developer_mode ? 64u : 0u));
    record.data[18] = preferences.output_volume;
    record.data[19] = static_cast<u8>(preferences.soundpack);
    for (u32 i = 0; i < 5; ++i)
        byte_store_le32(record.data + 20 + i * 4, preferences.gradient.colors[i]);
    record.data[40] = static_cast<u8>(preferences.gradient.angle_degrees);
    record.data[41] = static_cast<u8>(preferences.gradient.angle_degrees >> 8);
    record.data[42] = preferences.gradient.color_count;
    record.data[43] = preferences.gradient.intensity;
    record.data[44] = static_cast<u8>(preferences.gradient.effect);
    record.data[45] = preferences.gradient.speed;
    record.data[46] = preferences.gradient.strength;
    record.data[47] = preferences.gradient.density;
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i) {
        record.data[48 + i * 3] = preferences.effect_profiles[i].speed;
        record.data[49 + i * 3] = preferences.effect_profiles[i].strength;
        record.data[50 + i * 3] = preferences.effect_profiles[i].density;
    }
    record.data[66] = static_cast<u8>((preferences.taskbar_flash ? 0u : 1u) | (preferences.unread_badge ? 0u : 2u) |
                                      (preferences.minimize_to_tray ? 0u : 4u) |
                                      (preferences.start_minimized ? 8u : 0u) | (preferences.spellcheck ? 0u : 16u) |
                                      (preferences.game_detection ? 32u : 0u));
    record.data[67] = preferences.message_group_spacing;
    record.data[68] = static_cast<u8>(preferences.channel_pane_width);
    record.data[69] = static_cast<u8>(preferences.channel_pane_width >> 8);
    record.data[70] = static_cast<u8>(preferences.member_pane_width);
    record.data[71] = static_cast<u8>(preferences.member_pane_width >> 8);
    byte_store_le64(record.data + kChecksumOffset, hash64_bytes(kHash64Seed, record.data, kChecksumOffset));
    return {};
}

ClientPreferencesResult client_preferences_decode(ConstSlice<u8> record,
                                                  ClientPreferences* preferences) noexcept {
    if (!record.data || !preferences)
        return failure(ClientPreferencesError::invalid_input);
    if (record.count < kHeaderBytes || record.count > kClientPreferencesRecordBytes)
        return failure(ClientPreferencesError::size);
    for (u32 i = 0; i < sizeof(kMagic); ++i)
        if (record.data[i] != kMagic[i])
            return failure(ClientPreferencesError::format);
    // Older records are not migrated; callers treat them like a missing file.
    if (byte_load_le32(record.data + 8) != kClientPreferencesVersion)
        return failure(ClientPreferencesError::version);
    if (record.count != kClientPreferencesRecordBytes)
        return failure(ClientPreferencesError::size);
    if (byte_load_le32(record.data + 12) != kClientPreferencesRecordBytes)
        return failure(ClientPreferencesError::format);
    for (u32 i = 72; i < kChecksumOffset; ++i)
        if (record.data[i] != 0)
            return failure(ClientPreferencesError::format);
    if (record.data[16] > static_cast<u8>(ClientTheme::custom_light_gradient) ||
        (record.data[17] & ~kKnownFlags) != 0 || record.data[18] > 200 ||
        !notification_soundpack_valid(static_cast<NotificationSoundpack>(record.data[19])) ||
        (record.data[66] & ~63u) || record.data[67] >= kClientPreferencesGroupSpacingCount)
        return failure(ClientPreferencesError::format);
    if (byte_load_le64(record.data + kChecksumOffset) !=
        hash64_bytes(kHash64Seed, record.data, kChecksumOffset))
        return failure(ClientPreferencesError::checksum);
    ClientPreferences decoded = {static_cast<ClientTheme>(record.data[16]),
                                 (record.data[17] & 1) != 0,
                                 (record.data[17] & 2) != 0,
                                 (record.data[17] & 4) != 0,
                                 (record.data[17] & 8) != 0,
                                 (record.data[17] & 16) != 0,
                                 (record.data[17] & 32) != 0,
                                 record.data[18],
                                 static_cast<NotificationSoundpack>(record.data[19])};
    for (u32 i = 0; i < 5; ++i)
        decoded.gradient.colors[i] = byte_load_le32(record.data + 20 + i * 4);
    decoded.gradient.angle_degrees = byte_load_le16(record.data + 40);
    decoded.gradient.color_count = record.data[42];
    decoded.gradient.intensity = record.data[43];
    if (background_effect_known(record.data[44])) {
        decoded.gradient.effect = static_cast<BackgroundEffect>(record.data[44]);
        decoded.gradient.speed = record.data[45];
        decoded.gradient.strength = record.data[46];
        decoded.gradient.density = record.data[47];
    } else {
        const auto defaults = background_effect_settings_defaults(BackgroundEffect::none);
        decoded.gradient.effect = BackgroundEffect::none;
        decoded.gradient.speed = defaults.speed;
        decoded.gradient.strength = defaults.strength;
        decoded.gradient.density = defaults.density;
    }
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i)
        decoded.effect_profiles[i] = {record.data[48 + i * 3], record.data[49 + i * 3],
                                      record.data[50 + i * 3]};
    if (!background_theme_valid(decoded.gradient))
        return failure(ClientPreferencesError::format);
    decoded.developer_mode = (record.data[17] & 64) != 0;
    decoded.taskbar_flash = !(record.data[66] & 1);
    decoded.unread_badge = !(record.data[66] & 2);
    decoded.minimize_to_tray = !(record.data[66] & 4);
    decoded.start_minimized = (record.data[66] & 8) != 0;
    decoded.spellcheck = !(record.data[66] & 16);
    decoded.game_detection = (record.data[66] & 32) != 0;
    decoded.message_group_spacing = record.data[67];
    decoded.channel_pane_width = byte_load_le16(record.data + 68);
    decoded.member_pane_width = byte_load_le16(record.data + 70);
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
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const u32 native_error = thread_last_error();
        return failure(native_error == ERROR_FILE_NOT_FOUND || native_error == ERROR_PATH_NOT_FOUND
                           ? ClientPreferencesError::missing
                           : ClientPreferencesError::open,
                       native_error);
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        const u32 native_error = thread_last_error();
        CloseHandle(file);
        return failure(ClientPreferencesError::size, native_error);
    }
    if (size.QuadPart < kHeaderBytes || size.QuadPart > kClientPreferencesRecordBytes) {
        CloseHandle(file);
        return failure(ClientPreferencesError::size);
    }
    u8 record[kClientPreferencesRecordBytes]{};
    u32 native_error = 0;
    const u32 record_bytes = static_cast<u32>(size.QuadPart);
    const bool read = file_persistence_read_complete(file, {record, record_bytes}, &native_error);
    CloseHandle(file);
    return read ? client_preferences_decode({record, record_bytes}, preferences)
                : failure(ClientPreferencesError::read, native_error);
}

ClientPreferencesResult client_preferences_save_file(const wchar_t* path,
                                                     const ClientPreferences& preferences) noexcept {
    u32 length = 0;
    if (!valid_path(path, &length))
        return failure(ClientPreferencesError::invalid_input);
    u8 record[kClientPreferencesRecordBytes]{};
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
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    const auto result = user_path(path, false);
    return result.ok() ? client_preferences_load_file(path, preferences) : result;
}

ClientPreferencesResult client_preferences_save(const ClientPreferences& preferences) noexcept {
    if (!client_preferences_valid(preferences))
        return failure(ClientPreferencesError::invalid_input);
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    const auto result = user_path(path, true);
    return result.ok() ? client_preferences_save_file(path, preferences) : result;
}

} // namespace maicord
