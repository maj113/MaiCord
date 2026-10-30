#include "maicord/app/client_theme_storage.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
struct StorageFixture {
    u8 record[kClientThemeStorageMaximumBytes], saved[kClientThemeStorageMaximumBytes];
    u32 structurals[kClientThemeImportMaximumBytes];
    ClientThemeStorageSnapshot output;
} fixture{};
bool equal(const ClientThemeStorageSnapshot& a, const ClientThemeStorageSnapshot& b) noexcept {
    if (a.custom_present != b.custom_present || a.custom_active != b.custom_active ||
        a.builtin_theme != b.builtin_theme || !background_theme_equal(a.gradient, b.gradient) ||
        (a.custom_present && !client_theme_document_equal(a.custom, b.custom)))
        return false;
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i) {
        const auto x = a.effect_profiles[i], y = b.effect_profiles[i];
        if (x.speed != y.speed || x.strength != y.strength || x.density != y.density)
            return false;
    }
    return true;
}
ClientThemeStorageSnapshot snapshot(bool active = true) noexcept {
    auto preferences = client_preferences_defaults();
    preferences.theme = ClientTheme::ocean;
    preferences.gradient.angle_degrees = 270;
    preferences.gradient.effect = BackgroundEffect::comets;
    preferences.effect_profiles[1] = {7, 8, 9};
    auto document = client_theme_document_defaults(ClientThemeBase::light);
    copy_bytes(document.name, "Retained", 8);
    document.name_bytes = 8;
    document.name[document.name_bytes] = 0;
    ClientThemeStorageSnapshot value{};
    client_theme_storage_capture(&value, preferences, document, true, active);
    return value;
}
void fix_checksum(u32 bytes) noexcept {
    byte_store_le64(fixture.record + 80, hash64_bytes(hash64_bytes(kHash64Seed, fixture.record, 80),
                                                      fixture.record + 88, bytes - 88));
}
ClientThemeStorageResult decode(u32 bytes) noexcept {
    return client_theme_storage_decode(
        {fixture.record, bytes}, {fixture.structurals, kClientThemeImportMaximumBytes}, &fixture.output);
}
bool await(ClientThemeIo* io, u64 generation, ClientThemeIoCompletion* output) noexcept {
    const auto deadline = GetTickCount64() + 1500;
    for (;;) {
        const auto now = GetTickCount64();
        if (now >= deadline || WaitForSingleObject(client_theme_io_completion_event(io),
                                                   static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0)
            return false;
        bool ready = false;
        if (!client_theme_io_poll(io, output, &ready))
            return false;
        if (ready && output->generation >= generation)
            return output->generation == generation;
    }
}
u32 path_units(const wchar_t* path) noexcept {
    return wide_length(path, kClientThemeIoMaximumPathUnits);
}
} // namespace
void test_client_theme_storage(void (*check)(bool, const char*) noexcept) noexcept {
    const auto initial = snapshot();
    u32 bytes = 99;
    check(client_theme_storage_valid(initial) &&
              client_theme_storage_encode(initial, {fixture.record, sizeof(fixture.record)}, &bytes).ok() &&
              bytes > kClientThemeStorageHeaderBytes && byte_load_le32(fixture.record + 8) == 1 &&
              byte_load_le32(fixture.record + 12) == bytes && fixture.record[16] == 3 &&
              byte_load_le32(fixture.record + 68) == bytes - kClientThemeStorageHeaderBytes,
          "theme storage explicitly encodes custom document active selector and builtin settings in one "
          "record");
    copy_bytes(fixture.saved, fixture.record, bytes);
    fixture.output = {};
    check(decode(bytes).ok() && equal(initial, fixture.output),
          "theme storage roundtrip retains inactive builtin profile and complete owning custom theme");
    auto preferences = client_preferences_defaults();
    preferences.desktop_enabled = false;
    preferences.message_sounds_enabled = false;
    preferences.selected_message_sounds_enabled = true;
    preferences.reduced_motion = true;
    preferences.smooth_scroll = false;
    preferences.member_pane_visible = false;
    preferences.output_volume = 173;
    preferences.soundpack = NotificationSoundpack::classic;
    const auto other_preferences = preferences;
    check(client_theme_storage_apply_theme_fields(fixture.output, &preferences) &&
              preferences.theme == ClientTheme::ocean &&
              preferences.output_volume == other_preferences.output_volume &&
              preferences.desktop_enabled == other_preferences.desktop_enabled &&
              preferences.message_sounds_enabled == other_preferences.message_sounds_enabled &&
              preferences.selected_message_sounds_enabled ==
                  other_preferences.selected_message_sounds_enabled &&
              preferences.reduced_motion == other_preferences.reduced_motion &&
              preferences.smooth_scroll == other_preferences.smooth_scroll &&
              preferences.member_pane_visible == other_preferences.member_pane_visible &&
              preferences.soundpack == other_preferences.soundpack &&
              preferences.effect_profiles[1].density == 9,
          "authoritative theme store applies only theme gradient and profiles preserving every other "
          "legacy preference");
    const auto sentinel = fixture.output;
    fixture.record[bytes - 1] ^= 1;
    check(decode(bytes).error == ClientThemeStorageError::checksum && equal(fixture.output, sentinel),
          "theme checksum failure preserves complete caller state");
    constexpr u32 reserved[]{18, 19, 66, 67, 72, 73, 74, 75, 76, 77,
                             78, 79, 88, 89, 90, 91, 92, 93, 94, 95};
    bool rejected = true;
    for (const auto index : reserved) {
        copy_bytes(fixture.record, fixture.saved, bytes);
        fixture.record[index] = 1;
        fix_checksum(bytes);
        rejected &=
            decode(bytes).error == ClientThemeStorageError::format && equal(fixture.output, sentinel);
    }
    check(rejected, "theme checksum-valid reserved envelope bytes are all rejected without publication");
    copy_bytes(fixture.record, fixture.saved, bytes);
    byte_store_le32(fixture.record + 8, 2);
    fix_checksum(bytes);
    check(decode(bytes).error == ClientThemeStorageError::version && equal(fixture.output, sentinel),
          "theme unsupported storage version preserves state");
    copy_bytes(fixture.record, fixture.saved, bytes);
    fixture.record[16] = 2;
    fix_checksum(bytes);
    check(!decode(bytes).ok() && equal(fixture.output, sentinel),
          "theme active custom selector without retained custom document is rejected");
    copy_bytes(fixture.record, fixture.saved, bytes);
    fixture.record[17] = 255;
    fix_checksum(bytes);
    check(decode(bytes).error == ClientThemeStorageError::format && equal(fixture.output, sentinel),
          "theme invalid builtin enum cannot overwrite caller preferences");
    copy_bytes(fixture.record, fixture.saved, bytes);
    fixture.record[48] = 101;
    fix_checksum(bytes);
    check(decode(bytes).error == ClientThemeStorageError::format && equal(fixture.output, sentinel),
          "theme retained effect profile limits are strict");
    copy_bytes(fixture.record, fixture.saved, bytes);
    fixture.record[44] = 6;
    fixture.record[45] = 77;
    fixture.record[46] = 66;
    fixture.record[47] = 55;
    for (u32 index = 57; index < 66; ++index)
        fixture.record[index] = static_cast<u8>(0x40 + index);
    fix_checksum(bytes);
    const auto none_settings = background_effect_settings_defaults(BackgroundEffect::none);
    check(decode(bytes).ok() && fixture.output.gradient.effect == BackgroundEffect::none &&
              fixture.output.gradient.speed == none_settings.speed &&
              fixture.output.gradient.strength == none_settings.strength &&
              fixture.output.gradient.density == none_settings.density &&
              fixture.output.builtin_theme == sentinel.builtin_theme &&
              fixture.output.effect_profiles[1].density == 9,
          "theme store: a saved removed effect restores None with default tuning and keeps the theme");
    fixture.output = sentinel;
    copy_bytes(fixture.record, fixture.saved, bytes);
    fixture.record[96] = '!';
    fix_checksum(bytes);
    check(decode(bytes).error == ClientThemeStorageError::document && equal(fixture.output, sentinel),
          "theme checksum-valid malformed JSON cannot partially replace stored state");
    copy_bytes(fixture.record, fixture.saved, bytes);
    check(!client_theme_storage_decode({fixture.record, bytes}, {fixture.structurals, 1}, &fixture.output)
                  .ok() &&
              equal(fixture.output, sentinel),
          "theme parser scratch capacity cannot truncate a stored document");
    const u8 before = fixture.record[0];
    u32 rejected_bytes = 123;
    check(client_theme_storage_encode(initial, {fixture.record, bytes - 1}, &rejected_bytes).error ==
                  ClientThemeStorageError::size &&
              rejected_bytes == 123 && fixture.record[0] == before &&
              bytes_equal(fixture.record, fixture.saved, bytes),
          "theme insufficient output capacity preserves record bytes and length");
    auto inactive = initial;
    inactive.custom_active = false;
    check(client_theme_storage_encode(inactive, {fixture.record, sizeof(fixture.record)}, &bytes).ok() &&
              decode(bytes).ok() && !fixture.output.custom_active && fixture.output.custom_present &&
              equal(inactive, fixture.output),
          "theme builtin switch retains imported inactive descriptor in the same atomic snapshot");
    auto absent = inactive;
    absent.custom_present = false;
    check(client_theme_storage_encode(absent, {fixture.record, sizeof(fixture.record)}, &bytes).ok() &&
              bytes == 96 && decode(bytes).ok() && !fixture.output.custom_present &&
              fixture.output.builtin_theme == initial.builtin_theme,
          "theme builtin-only authoritative store has a bounded document-free envelope");

    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{}, import_path[MAX_PATH]{};
    const u32 directory_length = GetTempPathW(MAX_PATH, directory);
    const bool paths = directory_length && directory_length < MAX_PATH &&
                       GetTempFileNameW(directory, L"MTS", 0, path) &&
                       GetTempFileNameW(directory, L"MTI", 0, import_path);
    check(paths, "theme real-file fixtures use OS temporary files outside user settings");
    if (!paths) {
        if (path[0])
            DeleteFileW(path);
        return;
    }
    ClientThemeIo* io = nullptr;
    struct Cleanup {
        ClientThemeIo** io;
        const wchar_t* path;
        const wchar_t* input;
        ~Cleanup() noexcept {
            client_theme_io_stop(io, INFINITE);
            DeleteFileW(path);
            DeleteFileW(input);
        }
    } cleanup{&io, path, import_path};
    check(client_theme_storage_load_file(path, &fixture.output).error == ClientThemeStorageError::size &&
              equal(absent, fixture.output),
          "theme invalid empty file preserves caller state");
    check(client_theme_storage_save_file(path, initial).ok() &&
              client_theme_storage_load_file(path, &fixture.output).ok() && equal(initial, fixture.output),
          "theme actual atomic file save and load retain custom selection and builtin fields together");
    const HANDLE held =
        CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const auto failed_save = client_theme_storage_save_file(path, inactive);
    if (held != INVALID_HANDLE_VALUE)
        CloseHandle(held);
    check(held != INVALID_HANDLE_VALUE && !failed_save.ok() &&
              client_theme_storage_load_file(path, &fixture.output).ok() && equal(initial, fixture.output),
          "theme failed atomic replacement retains the previous complete selector and document snapshot");
    wchar_t copied_path[MAX_PATH]{};
    copy_bytes(copied_path, path, sizeof(path));
    check(client_theme_io_create(&io, copied_path) && io,
          "theme IO owner starts lazily with a copied destination path");
    if (!io)
        return;
    copied_path[0] = 0;
    ClientThemeIoCompletion completion{};
    completion.generation = 99;
    bool ready = true;
    check(client_theme_io_poll(io, &completion, &ready) && !ready && completion.generation == 99 &&
              !client_theme_io_completion_pending(io) &&
              WaitForSingleObject(client_theme_io_completion_event(io), 0) == WAIT_TIMEOUT,
          "theme empty poll performs no file IO or snapshot publication");
    u64 generation = 0;
    check(client_theme_io_submit_load(io, &generation) && await(io, generation, &completion) &&
              completion.result.ok() && equal(initial, completion.snapshot) &&
              completion.operation == ClientThemeIoOperation::load,
          "theme lazy worker loads one owning authoritative snapshot");
    u32 json_bytes = 0;
    const auto written = client_theme_document_write(
        initial.custom, {reinterpret_cast<char*>(fixture.record), kClientThemeImportMaximumBytes},
        &json_bytes);
    const auto persisted =
        file_persistence_write_atomic(import_path, path_units(import_path), {fixture.record, json_bytes});
    FileSnapshot imported{};
    u32 native = 0;
    HANDLE input =
        CreateFileW(import_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const bool captured = input != INVALID_HANDLE_VALUE && file_snapshot_read(input, &imported, &native);
    if (input != INVALID_HANDLE_VALUE)
        CloseHandle(input);
    wchar_t copied_import[MAX_PATH]{};
    copy_bytes(copied_import, import_path, sizeof(import_path));
    const bool accepted = written.ok() && persisted.ok() && captured &&
                          client_theme_io_submit_import(io, {copied_import, path_units(copied_import)},
                                                        imported, inactive, &generation);
    copied_import[0] = 0;
    check(accepted && await(io, generation, &completion) && completion.result.ok() &&
              completion.operation == ClientThemeIoOperation::import_document &&
              completion.snapshot.custom_present && !completion.snapshot.custom_active &&
              client_theme_document_equal(initial.custom, completion.snapshot.custom) &&
              completion.snapshot.builtin_theme == inactive.builtin_theme,
          "theme import worker copies picker path validates exact file identity and returns inactive "
          "owning candidate");
    auto wrong_identity = imported;
    ++wrong_identity.identity.last_write;
    check(client_theme_io_submit_import(io, {import_path, path_units(import_path)}, wrong_identity,
                                        inactive, &generation) &&
              await(io, generation, &completion) &&
              completion.result.error == ClientThemeStorageError::changed,
          "theme changed selected file is rejected by exact worker reopen identity fence");
    check(client_theme_io_submit_save(io, inactive, &generation) &&
              WaitForSingleObject(client_theme_io_completion_event(io), 1500) == WAIT_OBJECT_0 &&
              client_theme_io_completion_pending(io),
          "theme accepted save publishes completion for exact cancellation test");
    client_theme_io_cancel(io, generation);
    completion.generation = 77;
    ready = true;
    check(client_theme_io_poll(io, &completion, &ready) && !ready && completion.generation == 77 &&
              WaitForSingleObject(client_theme_io_completion_event(io), 0) == WAIT_TIMEOUT,
          "theme exact cancellation drains completed generation without late UI publication");
    auto latest = initial;
    bool submissions = true;
    for (u32 i = 0; i < 64; ++i) {
        latest.custom_active = (i & 1) != 0;
        latest.gradient.angle_degrees = static_cast<u16>(i);
        submissions &= client_theme_io_submit_save(io, latest, &generation);
    }
    check(submissions && await(io, generation, &completion) && completion.result.ok() &&
              equal(latest, completion.snapshot) &&
              client_theme_storage_load_file(path, &fixture.output).ok() && equal(latest, fixture.output),
          "theme bounded64-save coalescing retains and persists newest complete snapshot without growing "
          "queue");
    bool stable = true;
    for (u32 i = 0; i < 64; ++i) {
        ready = true;
        stable &= client_theme_io_poll(io, &completion, &ready) && !ready;
    }
    check(stable, "theme64 idle polls publish no work or stale completion");
    check(client_theme_io_stop(&io, 1500) && !io && client_theme_io_stop(&io, 0),
          "theme IO joins before freeing worker storage and null stop remains idempotent");
    DeleteFileW(path);
    fixture.output = initial;
    check(client_theme_storage_load_file(path, &fixture.output).error == ClientThemeStorageError::missing &&
              equal(initial, fixture.output),
          "theme missing authoritative file preserves caller for80-byte legacy fallback");
}
} // namespace maicord
