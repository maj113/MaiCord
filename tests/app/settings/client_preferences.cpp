#include "maicord/app/settings/client_preferences.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/win32_kernel.hpp"

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
    ClientPreferences decoded = defaults;
    constexpr u32 kBytes = kClientPreferencesRecordBytes, kSum = kBytes - 8;
    u8 record[kBytes + 1]{};
    // Records older than version 9 are not migrated: they are reported as
    // `version`, which startup treats like a missing file.
    auto stale = [&](u32 version) noexcept {
        if (!client_preferences_encode(defaults, {record, kBytes}).ok())
            return false;
        byte_store_le32(record + 8, version);
        byte_store_le64(record + kSum, hash64_bytes(kHash64Seed, record, kSum));
        return true;
    };
    bool round_trip = true;
    constexpr u32 volumes[]{0, 1, 2, 3, 4, 5, 6, 7, 8, 100, 199, 200};
    for (u32 theme = 0; theme < 8; ++theme)
        for (const u32 volume : volumes) {
            auto value = defaults;
            value.theme = static_cast<ClientTheme>(theme);
            value.output_volume = static_cast<u8>(volume);
            value.soundpack = static_cast<NotificationSoundpack>(volume % 9);
            value.selected_message_sounds_enabled = (volume & 1) != 0;
            value.speak_messages = static_cast<u8>(volume % 3);
            value.test_mode_application = volume * 0x0123456789ull;
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
            round_trip = round_trip && client_preferences_encode(value, {record, kBytes}).ok() &&
                         client_preferences_decode({record, kBytes}, &decoded).ok() &&
                         client_preferences_equal(value, decoded);
        }
    check(round_trip,
          "preferences: all eight themes, exact volumes, five gradient stops and effects round trip");
    auto solid = defaults;
    solid.theme = ClientTheme::custom_gradient;
    solid.gradient = {{0x00e0ff, 0, 0, 0, 0}, 271, 1, 100, BackgroundEffect::comets, 20, 22, 30};
    check(client_preferences_encode(solid, {record, kBytes}).ok() &&
              client_preferences_decode({record, kBytes}, &decoded).ok() &&
              client_preferences_equal(solid, decoded),
          "preferences: v7 single-color appearance preserves color, angle and independent effect settings");
    auto sentinel = defaults;
    sentinel.theme = ClientTheme::custom_gradient;
    sentinel.gradient = {{0x112233, 0xfedcba, 0, 0, 0},      360, 2,  100,
                         BackgroundEffect::stars_and_comets, 100, 25, 35};
    check(client_preferences_encode(sentinel, {record, kBytes}).ok() &&
              client_preferences_decode({record, kBytes}, &decoded).ok() &&
              client_preferences_equal(decoded, sentinel),
          "preferences: angle360 and maximum intensity/speed round trip");
    auto both = solid;
    both.gradient.effect = BackgroundEffect::stars_and_comets;
    both.effect_profiles[2] = {0, 100, 37};
    check(client_preferences_encode(both, {record, kBytes}).ok() && record[44] == 3 && record[54] == 0 &&
              record[55] == 100 && record[56] == 37 && record[57] == 1 && record[65] == 0 &&
              client_preferences_decode({record, kBytes}, &decoded).ok() &&
              client_preferences_equal(decoded, both),
          "preferences: exact third profile and Stars and comets mode share the record");
    // The last built-in theme survives a custom theme's base (byte 57 = theme + 1); a record written before it
    // existed (zero) takes the record's own theme.
    auto remembered = defaults;
    remembered.theme = ClientTheme::dark;
    remembered.builtin_theme = ClientTheme::sunset;
    bool builtin_kept = client_preferences_encode(remembered, {record, kBytes}).ok() && record[57] == 4 &&
                        client_preferences_decode({record, kBytes}, &decoded).ok() &&
                        decoded.builtin_theme == ClientTheme::sunset && decoded.theme == ClientTheme::dark;
    remembered.theme = ClientTheme::ocean;
    builtin_kept = builtin_kept && client_preferences_encode(remembered, {record, kBytes}).ok();
    record[57] = 0;
    byte_store_le64(record + kSum, hash64_bytes(kHash64Seed, record, kSum));
    builtin_kept = builtin_kept && client_preferences_decode({record, kBytes}, &decoded).ok() &&
                   decoded.builtin_theme == ClientTheme::ocean;
    check(builtin_kept, "preferences: the last built-in theme round trips and older records migrate to their theme");
    // A saved effect that no longer exists restores None with its default tuning; the
    // rest of the record (theme, palette, other profiles) is kept.
    bool removed_effects = true;
    for (u32 stored = 4; stored < 8; ++stored) {
        removed_effects = removed_effects && client_preferences_encode(both, {record, kBytes}).ok();
        record[44] = static_cast<u8>(stored);
        record[45] = 77;
        record[46] = 66;
        record[47] = 55;
        for (u32 i = 57; i < 65; ++i)
            if (i != 58 && i != 59 && (i < 60 || i > 62))
                record[i] = static_cast<u8>(0x40 + i);
        byte_store_le64(record + kSum, hash64_bytes(kHash64Seed, record, kSum));
        const auto none_settings = background_effect_settings_defaults(BackgroundEffect::none);
        removed_effects =
            removed_effects && client_preferences_decode({record, kBytes}, &decoded).ok() &&
            decoded.gradient.effect == BackgroundEffect::none &&
            decoded.gradient.speed == none_settings.speed &&
            decoded.gradient.strength == none_settings.strength &&
            decoded.gradient.density == none_settings.density && decoded.theme == both.theme &&
            decoded.gradient.colors[0] == both.gradient.colors[0] &&
            decoded.effect_profiles[2].density == 37 && background_theme_valid(decoded.gradient);
    }
    check(removed_effects, "preferences: a saved removed effect falls back to None with default tuning "
                           "instead of rejecting the record");
    decoded = sentinel;
    auto reset = [&]() noexcept { return client_preferences_encode(defaults, {record, kBytes}).ok(); };
    auto checksum = [&]() noexcept { byte_store_le64(record + kSum, hash64_bytes(kHash64Seed, record, kSum)); };
    auto reject = [&](ClientPreferencesError error, u32 bytes = kBytes) noexcept {
        return client_preferences_decode({record, bytes}, &decoded).error == error &&
               client_preferences_equal(decoded, sentinel);
    };
    check(reset() && record[8] == 10 && record[12] == kBytes && record[17] == 30 && record[18] == 100 &&
              byte_load_le32(record + 20) == 0x6935b8 && byte_load_le16(record + 40) == 120 &&
              record[42] == 3 && record[43] == 40,
          "preferences: portable fields use a trailing checksum without struct serialization");
    auto developer = defaults;
    developer.developer_mode = true;
    check(client_preferences_encode(developer, {record, kBytes}).ok() && record[17] == (30 | 64) &&
              client_preferences_decode({record, kBytes}, &decoded).ok() && decoded.developer_mode &&
              client_preferences_equal(decoded, developer),
          "preferences: v8 developer mode roundtrips as flag 64");
    decoded = sentinel;
    bool old_versions = true;
    for (u32 version = 1; version < 9; ++version)
        old_versions = old_versions && stale(version) && reject(ClientPreferencesError::version);
    // Older layouts were shorter; the header alone identifies them as stale.
    old_versions = old_versions && stale(4) && reject(ClientPreferencesError::version, 32) &&
                   stale(5) && reject(ClientPreferencesError::version, 64);
    check(old_versions, "preferences: every version before 9 is rejected without migration");
    // A version 9 record (80 bytes, checksum at 72) keeps its fields and takes the newer ones' defaults.
    auto migrated = defaults;
    migrated.taskbar_flash = false;
    migrated.message_group_spacing = 2;
    bool version9 = client_preferences_encode(migrated, {record, kBytes}).ok();
    byte_store_le32(record + 8, 9);
    byte_store_le32(record + 12, 80);
    clear_bytes(record + 72, kBytes - 72);
    byte_store_le64(record + 72, hash64_bytes(kHash64Seed, record, 72));
    version9 = version9 && client_preferences_decode({record, 80}, &decoded).ok() &&
               client_preferences_equal(decoded, migrated);
    decoded = sentinel;
    version9 = version9 && reject(ClientPreferencesError::size, kBytes);
    record[72] ^= 1;
    check(version9 && reject(ClientPreferencesError::checksum, 80),
          "preferences: a version 9 record migrates with the newer fields at their defaults");
    reset();
    record[0] ^= 1;
    check(reject(ClientPreferencesError::format), "preferences: magic failure preserves output");
    reset();
    record[8] = 11;
    check(reject(ClientPreferencesError::version), "preferences: future version rejected");
    reset();
    record[12] = 32;
    check(reject(ClientPreferencesError::format), "preferences: header length must match the record");
    reset();
    record[kSum] ^= 1;
    check(reject(ClientPreferencesError::checksum), "preferences: checksum corruption rejected");
    reset();
    record[17] ^= 1;
    check(reject(ClientPreferencesError::checksum), "preferences: damaged supported value fails integrity");
    auto attention = defaults;
    attention.taskbar_flash = false;
    attention.unread_badge = false;
    attention.message_group_spacing = 5;
    check(client_preferences_encode(attention, {record, kBytes}).ok() && record[66] == 3 && record[67] == 5 &&
              client_preferences_decode({record, kBytes}, &decoded).ok() && client_preferences_equal(decoded, attention),
          "preferences: v9 taskbar flash, unread badge and group spacing roundtrip at bytes 66/67");
    decoded = sentinel;
    reset();
    record[67] = kClientPreferencesGroupSpacingCount;
    checksum();
    const bool spacing_bound = reject(ClientPreferencesError::format);
    reset();
    record[66] = 128;
    checksum();
    check(spacing_bound && reject(ClientPreferencesError::format),
          "preferences: v9 unknown attention flags and spacing codes are rejected");
    auto desktop = defaults;
    desktop.minimize_to_tray = false;
    desktop.start_minimized = true;
    desktop.spellcheck = false;
    check(client_preferences_encode(desktop, {record, kBytes}).ok() && record[66] == 28 &&
              client_preferences_decode({record, kBytes}, &decoded).ok() && client_preferences_equal(decoded, desktop) &&
              !client_preferences_equal(decoded, defaults),
          "preferences: tray, start minimized and spellcheck bits roundtrip at byte 66");
    auto games = defaults;
    games.game_detection = true;
    check(!defaults.game_detection && client_preferences_encode(games, {record, kBytes}).ok() &&
              record[66] == 32 && client_preferences_decode({record, kBytes}, &decoded).ok() &&
              decoded.game_detection && !client_preferences_equal(decoded, defaults),
          "preferences: opt-in game detection bit roundtrips at byte 66 and defaults off");
    auto panel = defaults;
    panel.dm_profile_visible = false;
    check(defaults.dm_profile_visible && client_preferences_encode(panel, {record, kBytes}).ok() && record[66] == 64 &&
              client_preferences_decode({record, kBytes}, &decoded).ok() && !decoded.dm_profile_visible &&
              client_preferences_equal(decoded, panel) && !client_preferences_equal(decoded, defaults),
          "preferences: the DM profile panel defaults shown and its hidden bit roundtrips at byte 66");
    auto extras = defaults;
    bool extras_roundtrip = defaults.role_colors_everywhere && !defaults.reply_timestamps &&
                            !defaults.folders_close_others && !defaults.folders_close_on_home &&
                            !defaults.folders_follow_selection && !defaults.show_hidden_channels && reset() && record[65] == 0;
    constexpr u8 extra_bits[]{1, 2, 4, 8, 16, 32};
    for (u32 bit = 0; bit < 6; ++bit) {
        extras = defaults;
        (bit == 0   ? extras.role_colors_everywhere
         : bit == 1 ? extras.reply_timestamps
         : bit == 2 ? extras.folders_close_others
         : bit == 3 ? extras.folders_close_on_home
         : bit == 4 ? extras.folders_follow_selection
                    : extras.show_hidden_channels) = bit != 0;
        extras_roundtrip = extras_roundtrip && client_preferences_encode(extras, {record, kBytes}).ok() &&
                           record[65] == extra_bits[bit] && client_preferences_decode({record, kBytes}, &decoded).ok() &&
                           client_preferences_equal(decoded, extras) && !client_preferences_equal(decoded, defaults);
    }
    check(extras_roundtrip, "preferences: the built-in extras roundtrip bit by bit at byte 65, zero bits keep defaults");
    decoded = sentinel;
    reset();
    record[65] = 64;
    checksum();
    check(reject(ClientPreferencesError::format), "preferences: unknown byte 65 bits cannot publish");
    auto picker = defaults;
    picker.expression_picker_width = 750;
    check(reset() && byte_load_le16(record + 58) == 0 &&
              client_preferences_encode(picker, {record, kBytes}).ok() && byte_load_le16(record + 58) == 750 &&
              client_preferences_decode({record, kBytes}, &decoded).ok() &&
              client_preferences_equal(decoded, picker) && !client_preferences_equal(decoded, defaults),
          "preferences: expression drawer width uses reserved bytes without changing the record version or "
          "size");
    auto panes = defaults;
    panes.channel_pane_width = 312;
    panes.member_pane_width = kClientPreferencesPaneWidthMaximum;
    check(reset() && byte_load_le16(record + 68) == 0 && byte_load_le16(record + 70) == 0 &&
              client_preferences_encode(panes, {record, kBytes}).ok() && byte_load_le16(record + 68) == 312 &&
              byte_load_le16(record + 70) == kClientPreferencesPaneWidthMaximum &&
              client_preferences_decode({record, kBytes}, &decoded).ok() &&
              client_preferences_equal(decoded, panes) && !client_preferences_equal(decoded, defaults),
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
    check(pane_bounds && !client_preferences_encode(panes, {record, kBytes}).ok(),
          "preferences: checksum-valid out-of-range pane widths are rejected on decode and encode");
    auto zoom = defaults;
    zoom.image_zoom = false;
    zoom.image_zoom_square = true;
    zoom.image_zoom_factor = 9;
    zoom.image_zoom_size = 55;
    check(reset() && record[60] == 0 && record[61] == 4 && record[62] == 20 &&
              client_preferences_encode(zoom, {record, kBytes}).ok() && record[60] == 3 && record[61] == 9 &&
              record[62] == 55 && client_preferences_decode({record, kBytes}, &decoded).ok() &&
              client_preferences_equal(decoded, zoom) && !client_preferences_equal(decoded, defaults),
          "preferences: image zoom roundtrips at bytes 60..62 (off, square, half-unit zoom, size in tens)");
    check(reset() && (record[61] = 0, record[62] = 0, checksum(), true) &&
              client_preferences_decode({record, kBytes}, &decoded).ok() && decoded.image_zoom &&
              decoded.image_zoom_factor == 4 && decoded.image_zoom_size == 20,
          "preferences: a record from before image zoom (zero bytes) restores its defaults");
    decoded = sentinel;
    bool zoom_bounds = true;
    constexpr u8 invalid_zoom[][2]{{60, 4}, {60, 0x80}, {61, 1}, {61, 101}, {61, 255}, {62, 4}, {62, 101}, {62, 255}};
    for (const auto& entry : invalid_zoom) {
        reset();
        record[entry[0]] = entry[1];
        checksum();
        zoom_bounds = zoom_bounds && reject(ClientPreferencesError::format);
    }
    zoom.image_zoom_factor = 1;
    check(zoom_bounds && !client_preferences_encode(zoom, {record, kBytes}).ok(),
          "preferences: checksum-valid out-of-range image zoom bytes are rejected on decode and encode");
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
    check(reject(ClientPreferencesError::size, 15) && reject(ClientPreferencesError::size, kBytes - 1) &&
              reject(ClientPreferencesError::size, kBytes + 1),
          "preferences: only the exact current length can be decoded");
    // Accessibility and Streamer Mode fields round trip at bytes 72..77; out-of-range values and unknown flag bits
    // cannot publish.
    auto access = defaults;
    access.chat_font_size = 20;
    access.desaturation = 65;
    access.zoom = 125;
    access.speech_rate = 40;
    access.reduced_motion_sync = false;
    access.desaturate_user_colors = access.underline_links = true;
    access.role_style = client_role_style_dot;
    access.streamer_mode = access.streamer_hide_window = true;
    access.streamer_auto = access.streamer_disable_sounds = false;
    access.speak_messages = client_speak_messages_selected;
    access.test_mode_application = 0x1122334455667788ull;
    check(defaults.desaturation == 0 && defaults.zoom == 100 && defaults.speech_rate == 4 &&
              defaults.reduced_motion_sync && defaults.streamer_auto && defaults.streamer_hide_personal &&
              defaults.streamer_hide_invites && defaults.streamer_disable_sounds &&
              defaults.streamer_disable_notifications && !defaults.streamer_mode && !defaults.streamer_hide_window &&
              defaults.speak_messages == client_speak_messages_never && !defaults.test_mode_application &&
              client_preferences_encode(access, {record, kBytes}).ok() && record[72] == 20 && record[73] == 65 &&
              record[74] == 125 && record[75] == 40 && record[76] == (2 | 4 | 8) &&
              record[77] == (1 | 4 | 8 | 32 | 64) && record[78] == 1 && record[79] == 0 &&
              byte_load_le64(record + 80) == 0x1122334455667788ull &&
              client_preferences_decode({record, kBytes}, &decoded).ok() &&
              client_preferences_equal(decoded, access) && !client_preferences_equal(decoded, defaults),
          "preferences: accessibility, Streamer Mode, speech and test mode fields roundtrip at bytes 72..87");
    decoded = sentinel;
    bool access_bounds = true;
    constexpr u8 invalid_access[][2]{{72, 11}, {72, 25}, {73, 101}, {74, 49}, {74, 201}, {75, 41},
                                     {76, 3 << 3}, {76, 32}, {77, 128}, {78, 3}, {79, 1}};
    for (const auto& entry : invalid_access) {
        reset();
        record[entry[0]] = entry[1];
        checksum();
        access_bounds = access_bounds && reject(ClientPreferencesError::format);
    }
    access.zoom = 40;
    check(access_bounds && !client_preferences_encode(access, {record, kBytes}).ok(),
          "preferences: out-of-range accessibility values are rejected on decode and encode");
    reset();
    u8 snapshot[kBytes]{};
    copy_bytes(snapshot, record, kBytes);
    auto bad = defaults;
    bad.gradient.color_count = 0;
    check(!client_preferences_valid(bad) &&
              client_preferences_encode(bad, {record, kBytes}).error == ClientPreferencesError::invalid_input &&
              bytes_equal(snapshot, record, kBytes),
          "preferences: invalid gradient cannot partly overwrite encoded bytes");
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
    record[kSum] ^= 1;
    check(fixture_write(path, record, kBytes) &&
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
