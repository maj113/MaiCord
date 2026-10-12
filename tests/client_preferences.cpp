#include "maicord/app/client_preferences.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"

namespace maicord {
namespace {
bool fixture_write(const wchar_t* path, const u8* bytes, u32 count) noexcept {
    // Atomic persistence commits its hidden temporary sibling to this path.
    // CREATE_ALWAYS must repeat FILE_ATTRIBUTE_HIDDEN for that existing file,
    // otherwise Windows denies the fixture rewrite before load is exercised.
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes, count, &written, nullptr) && written == count;
    CloseHandle(file);
    return ok;
}
} // namespace

void test_client_preferences(void (*check)(bool, const char*) noexcept) noexcept {
    const auto defaults = client_preferences_defaults();
    check(defaults.theme == ClientTheme::dark && !defaults.reduced_motion && defaults.smooth_scroll &&
              defaults.member_pane_visible && defaults.desktop_enabled && defaults.message_sounds_enabled &&
              !defaults.selected_message_sounds_enabled && defaults.output_volume == 100 &&
              defaults.soundpack == NotificationSoundpack::classic &&
              background_theme_equal(defaults.gradient, background_theme_defaults()),
          "preferences: default choices and gradient initialize together");
    ClientPreferences decoded = defaults;
    u8 record[81]{};
    // Records written by older builds are not migrated: any other version is
    // reported as `version`, which startup treats like a missing file.
    auto stale = [&](u32 version) noexcept {
        if (!client_preferences_encode(defaults, {record, 80}).ok())
            return false;
        byte_store_le32(record + 8, version);
        byte_store_le64(record + 72, hash64_bytes(kHash64Seed, record, 72));
        return true;
    };
    bool round_trip = true;
    for (u32 theme = 0; theme < 8; ++theme)
        for (u32 volume = 0; volume <= 200; ++volume) {
            auto value = defaults;
            value.theme = static_cast<ClientTheme>(theme);
            value.output_volume = static_cast<u8>(volume);
            value.soundpack = static_cast<NotificationSoundpack>(volume % 9);
            value.selected_message_sounds_enabled = (volume & 1) != 0;
            value.gradient = {{0xff0000, 0x00ff00, 0x0000ff, 0x102030, 0xffffff},
                              static_cast<u16>(volume % 361),
                              5,
                              static_cast<u8>(volume % 101),
                              static_cast<BackgroundEffect>(volume % kBackgroundEffectCount),
                              static_cast<u8>(volume % 101),
                              25,
                              35};
            for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i)
                value.effect_profiles[i] = {static_cast<u8>((volume + i * 7) % 101),
                                            static_cast<u8>((volume + i * 11) % 101),
                                            static_cast<u8>((volume + i * 13) % 101)};
            round_trip = round_trip && client_preferences_encode(value, {record, 80}).ok() &&
                         client_preferences_decode({record, 80}, &decoded).ok() &&
                         client_preferences_equal(value, decoded);
        }
    check(round_trip,
          "preferences: all eight themes, exact volumes, five gradient stops and effects round trip");
    auto solid = defaults;
    solid.theme = ClientTheme::custom_gradient;
    solid.gradient = {{0x00e0ff, 0, 0, 0, 0}, 271, 1, 100, BackgroundEffect::comets, 20, 22, 30};
    check(client_preferences_encode(solid, {record, 80}).ok() &&
              client_preferences_decode({record, 80}, &decoded).ok() &&
              client_preferences_equal(solid, decoded),
          "preferences: v7 single-color appearance preserves color, angle and independent effect settings");
    auto sentinel = defaults;
    sentinel.theme = ClientTheme::custom_gradient;
    sentinel.gradient = {{0x112233, 0xfedcba, 0, 0, 0},      360, 2,  100,
                         BackgroundEffect::stars_and_comets, 100, 25, 35};
    check(client_preferences_encode(sentinel, {record, 80}).ok() &&
              client_preferences_decode({record, 80}, &decoded).ok() &&
              client_preferences_equal(decoded, sentinel),
          "preferences: angle360 and maximum intensity/speed round trip");
    auto both = solid;
    both.gradient.effect = BackgroundEffect::stars_and_comets;
    both.effect_profiles[2] = {0, 100, 37};
    check(client_preferences_encode(both, {record, 80}).ok() && record[44] == 3 && record[54] == 0 &&
              record[55] == 100 && record[56] == 37 && record[57] == 0 && record[65] == 0 &&
              client_preferences_decode({record, 80}, &decoded).ok() &&
              client_preferences_equal(decoded, both),
          "preferences: exact third profile and Stars and comets mode share the existing80-byte record");
    // A saved effect that no longer exists restores None with its default tuning; the
    // rest of the record (theme, palette, other profiles) is kept.
    bool removed_effects = true;
    for (u32 stored = 4; stored < 8; ++stored) {
        removed_effects = removed_effects && client_preferences_encode(both, {record, 80}).ok();
        record[44] = static_cast<u8>(stored);
        record[45] = 77;
        record[46] = 66;
        record[47] = 55;
        for (u32 i = 57; i < 66; ++i)
            record[i] = static_cast<u8>(0x40 + i);
        byte_store_le64(record + 72, hash64_bytes(kHash64Seed, record, 72));
        const auto none_settings = background_effect_settings_defaults(BackgroundEffect::none);
        removed_effects = removed_effects && client_preferences_decode({record, 80}, &decoded).ok() &&
                          decoded.gradient.effect == BackgroundEffect::none &&
                          decoded.gradient.speed == none_settings.speed &&
                          decoded.gradient.strength == none_settings.strength &&
                          decoded.gradient.density == none_settings.density &&
                          decoded.theme == both.theme && decoded.gradient.colors[0] == both.gradient.colors[0] &&
                          decoded.effect_profiles[2].density == 37 && background_theme_valid(decoded.gradient);
    }
    check(removed_effects, "preferences: a saved removed effect falls back to None with default tuning "
                           "instead of rejecting the record");
    decoded = sentinel;
    auto reset = [&]() noexcept { return client_preferences_encode(defaults, {record, 80}).ok(); };
    auto checksum = [&]() noexcept { byte_store_le64(record + 72, hash64_bytes(kHash64Seed, record, 72)); };
    auto reject = [&](ClientPreferencesError error, u32 bytes = 80) noexcept {
        return client_preferences_decode({record, bytes}, &decoded).error == error &&
               client_preferences_equal(decoded, sentinel);
    };
    check(reset() && record[8] == 9 && record[12] == 80 && record[17] == 30 && record[18] == 100 &&
              byte_load_le32(record + 20) == 0x6935b8 && byte_load_le16(record + 40) == 120 &&
              record[42] == 3 && record[43] == 40,
          "preferences: v8 portable fields use checksum72 without struct serialization");
    auto developer = defaults;
    developer.developer_mode = true;
    check(client_preferences_encode(developer, {record, 80}).ok() && record[17] == (30 | 64) &&
              client_preferences_decode({record, 80}, &decoded).ok() && decoded.developer_mode &&
              client_preferences_equal(decoded, developer),
          "preferences: v8 developer mode roundtrips as flag 64");
    decoded = sentinel;
    bool old_versions = true;
    for (u32 version = 1; version < kClientPreferencesVersion; ++version)
        old_versions = old_versions && stale(version) && reject(ClientPreferencesError::version);
    // Older layouts were shorter; the header alone identifies them as stale.
    old_versions = old_versions && stale(4) && reject(ClientPreferencesError::version, 32) &&
                   stale(5) && reject(ClientPreferencesError::version, 64);
    check(old_versions, "preferences: every older version is rejected without migration");
    reset();
    record[0] ^= 1;
    check(reject(ClientPreferencesError::format), "preferences: magic failure preserves output");
    reset();
    record[8] = 10;
    check(reject(ClientPreferencesError::version), "preferences: future version rejected");
    reset();
    record[12] = 32;
    check(reject(ClientPreferencesError::format), "preferences: header length must match the record");
    reset();
    record[72] ^= 1;
    check(reject(ClientPreferencesError::checksum), "preferences: checksum corruption rejected");
    reset();
    record[17] ^= 1;
    check(reject(ClientPreferencesError::checksum), "preferences: damaged supported value fails integrity");
    auto attention = defaults;
    attention.taskbar_flash = false;
    attention.unread_badge = false;
    attention.message_group_spacing = 5;
    check(client_preferences_encode(attention, {record, 80}).ok() && record[66] == 3 && record[67] == 5 &&
              client_preferences_decode({record, 80}, &decoded).ok() && client_preferences_equal(decoded, attention) &&
              defaults.taskbar_flash && defaults.unread_badge && !defaults.message_group_spacing,
          "preferences: v9 taskbar flash, unread badge and group spacing roundtrip at bytes 66/67");
    decoded = sentinel;
    reset();
    record[67] = kClientPreferencesGroupSpacingCount;
    checksum();
    const bool spacing_bound = reject(ClientPreferencesError::format);
    reset();
    record[66] = 64;
    checksum();
    check(spacing_bound && reject(ClientPreferencesError::format),
          "preferences: v9 unknown attention flags and spacing codes are rejected");
    auto desktop = defaults;
    desktop.minimize_to_tray = false;
    desktop.start_minimized = true;
    desktop.spellcheck = false;
    check(defaults.minimize_to_tray && !defaults.start_minimized && defaults.spellcheck &&
              client_preferences_encode(desktop, {record, 80}).ok() && record[66] == 28 &&
              client_preferences_decode({record, 80}, &decoded).ok() && client_preferences_equal(decoded, desktop) &&
              !client_preferences_equal(decoded, defaults),
          "preferences: tray, start minimized and spellcheck bits roundtrip at byte 66");
    auto games = defaults;
    games.game_detection = true;
    check(!defaults.game_detection && client_preferences_encode(games, {record, 80}).ok() && record[66] == 32 &&
              client_preferences_decode({record, 80}, &decoded).ok() && decoded.game_detection &&
              !client_preferences_equal(decoded, defaults),
          "preferences: opt-in game detection bit roundtrips at byte 66 and defaults off");
    auto panes = defaults;
    panes.channel_pane_width = 312;
    panes.member_pane_width = kClientPreferencesPaneWidthMaximum;
    check(reset() && byte_load_le16(record + 68) == 0 && byte_load_le16(record + 70) == 0 &&
              client_preferences_encode(panes, {record, 80}).ok() && byte_load_le16(record + 68) == 312 &&
              byte_load_le16(record + 70) == kClientPreferencesPaneWidthMaximum &&
              client_preferences_decode({record, 80}, &decoded).ok() && client_preferences_equal(decoded, panes) &&
              !client_preferences_equal(decoded, defaults),
          "preferences: v9 splitter pane widths roundtrip at bytes 68..71; zero keeps theme defaults");
    decoded = sentinel;
    bool pane_bounds = true;
    constexpr u16 invalid_widths[]{1, kClientPreferencesPaneWidthMinimum - 1, kClientPreferencesPaneWidthMaximum + 1,
                                   0xFFFF};
    for (u32 offset = 68; offset < 72; offset += 2)
        for (const u16 width : invalid_widths) {
            reset();
            record[offset] = static_cast<u8>(width);
            record[offset + 1] = static_cast<u8>(width >> 8);
            checksum();
            pane_bounds = pane_bounds && reject(ClientPreferencesError::format);
        }
    panes.channel_pane_width = 1;
    check(pane_bounds && !client_preferences_encode(panes, {record, 80}).ok(),
          "preferences: checksum-valid out-of-range pane widths are rejected on decode and encode");
    bool profile_bounds = true;
    for (u32 offset = 46; offset <= 56; ++offset) {
        reset();
        record[offset] = 101;
        checksum();
        profile_bounds = profile_bounds && reject(ClientPreferencesError::format);
    }
    check(profile_bounds, "preferences: every active and retained effect setting rejects values above100");
    constexpr u32 offsets[]{16, 17, 18, 19, 23, 40, 42, 43, 45};
    constexpr u8 values[]{8, 128, 201, 9, 1, 255, 6, 101, 101};
    bool invalid = true;
    for (u32 i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        reset();
        record[offsets[i]] = values[i];
        if (offsets[i] == 40)
            record[41] = 1;
        checksum();
        invalid = invalid && reject(ClientPreferencesError::format);
    }
    reset();
    record[32] = 1;
    checksum();
    invalid = invalid && reject(ClientPreferencesError::format);
    check(invalid,
          "preferences: unknown enums, flags, bounds, RGB high bits and unused stops cannot publish");
    reset();
    check(reject(ClientPreferencesError::size, 15) && reject(ClientPreferencesError::size, 79) &&
              reject(ClientPreferencesError::size, 81),
          "preferences: only the exact current length can be decoded");
    u8 snapshot[80]{};
    copy_bytes(snapshot, record, 80);
    auto bad = defaults;
    bad.gradient.color_count = 0;
    check(!client_preferences_valid(bad) &&
              client_preferences_encode(bad, {record, 80}).error == ClientPreferencesError::invalid_input &&
              bytes_equal(snapshot, record, 80),
          "preferences: invalid gradient cannot partly overwrite encoded bytes");
    check(client_preferences_encode(defaults, {nullptr, 80}).error ==
                  ClientPreferencesError::invalid_input &&
              client_preferences_encode(defaults, {record, 32}).error ==
                  ClientPreferencesError::invalid_input &&
              client_preferences_decode({nullptr, 80}, &decoded).error ==
                  ClientPreferencesError::invalid_input &&
              client_preferences_decode({record, 80}, nullptr).error ==
                  ClientPreferencesError::invalid_input &&
              client_preferences_load(nullptr).error == ClientPreferencesError::invalid_input,
          "preferences: invalid buffers fail before file IO");
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    const bool fixture = length && length < MAX_PATH && GetTempFileNameW(directory, L"MCP", 0, path);
    check(fixture, "preferences: external temporary fixture initialized");
    if (!fixture)
        return;
    struct Cleanup {
        const wchar_t* path;
        ~Cleanup() noexcept {
            DeleteFileW(path);
        }
    } cleanup{path};
    DeleteFileW(path);
    check(client_preferences_load_file(path, &decoded).error == ClientPreferencesError::missing &&
              client_preferences_equal(decoded, sentinel),
          "preferences: missing file retains current choices");
    const bool stale_written = stale(8) && fixture_write(path, record, 80);
    check(stale_written && client_preferences_load_file(path, &decoded).error == ClientPreferencesError::version &&
              client_preferences_equal(decoded, sentinel) && stale(4) && fixture_write(path, record, 32) &&
              client_preferences_load_file(path, &decoded).error == ClientPreferencesError::version &&
              client_preferences_equal(decoded, sentinel),
          "preferences: older files load as absent and retain current choices");
    check(client_preferences_save_file(path, sentinel).ok() &&
              client_preferences_load_file(path, &decoded).ok() &&
              client_preferences_equal(decoded, sentinel),
          "preferences: custom gradient persists through atomic file round trip");
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(file != INVALID_HANDLE_VALUE, "preferences: destination locked against atomic replacement");
    if (file != INVALID_HANDLE_VALUE) {
        check(client_preferences_save_file(path, defaults).error == ClientPreferencesError::commit,
              "preferences: replacement failure categorized");
        CloseHandle(file);
        check(client_preferences_load_file(path, &decoded).ok() &&
                  client_preferences_equal(decoded, sentinel),
              "preferences: failed replacement retains complete custom theme");
    }
    reset();
    record[72] ^= 1;
    check(fixture_write(path, record, 80) &&
              client_preferences_load_file(path, &decoded).error == ClientPreferencesError::checksum &&
              client_preferences_equal(decoded, sentinel),
          "preferences: corrupt new file preserves current theme");
    check(fixture_write(path, record, 63) &&
              client_preferences_load_file(path, &decoded).error == ClientPreferencesError::size &&
              client_preferences_equal(decoded, sentinel),
          "preferences: truncated new file cannot publish partial gradient");
    check(client_preferences_save_file(path, defaults).ok() &&
              client_preferences_load_file(path, &decoded).ok() &&
              client_preferences_equal(decoded, defaults),
          "preferences: explicit save repairs corrupt record");
}
} // namespace maicord
