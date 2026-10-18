#include "maicord/app/client_preferences_writer.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

bool await_revision(ClientPreferencesWriter* writer, u64 requested,
                    u64* completed, ClientPreferencesResult* result) noexcept {
    const u64 deadline = GetTickCount64() + 5'000;
    const HANDLE event = client_preferences_writer_completion_event(writer);
    for (;;) {
        const u64 now = GetTickCount64();
        if (now >= deadline || WaitForSingleObject(event, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0)
            return false;
        bool ready = false;
        if (!client_preferences_writer_poll(writer, completed, result, &ready)) return false;
        if (ready && *completed >= requested) return *completed == requested;
    }
}

ClientPreferences snapshot(u32 value) noexcept {
    return {value & 8 ? ClientTheme::amoled : ClientTheme::dark,
            (value & 1) != 0, (value & 2) != 0, (value & 4) != 0,
            (value & 16) != 0, (value & 32) != 0, (value & 64) != 0,
            static_cast<u8>(value % 201),static_cast<NotificationSoundpack>(value % 9)};
}

} // namespace

void test_client_preferences_writer(void (*check)(bool, const char*) noexcept) noexcept {
    ClientPreferencesWriter* writer = nullptr;
    u64 revision = 91, completed = 92;
    ClientPreferencesResult result{93, ClientPreferencesError::checksum};
    bool ready = true;
    const auto defaults = client_preferences_defaults();
    check(!client_preferences_writer_create(&writer, L"") && !writer &&
          !client_preferences_writer_completion_event(nullptr) &&
          client_preferences_writer_stop(&writer, 0) && !writer,
          "preferences writer: invalid arguments preserve outputs and null stop is idempotent");

    // Only an external temporary fixture is ever supplied to the writer.
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{}, copied_path[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    const bool fixture = length > 0 && length < MAX_PATH &&
                          GetTempFileNameW(directory, L"MPW", 0, path) != 0;
    check(fixture, "preferences writer: fixture is outside the workspace and actual preference directory");
    if (!fixture) return;
    struct Cleanup {
        ClientPreferencesWriter** writer;
        const wchar_t* path;
        ~Cleanup() noexcept {
            // Test owner storage cannot disappear while a worker is live.
            client_preferences_writer_stop(writer, INFINITE);
            DeleteFileW(path);
        }
    } cleanup{&writer, path};
    for (u32 i = 0; i < MAX_PATH; ++i) {
        copied_path[i] = path[i];
        if (!path[i]) break;
    }
    check(client_preferences_writer_create(&writer, copied_path) && writer,
          "preferences writer: lazy creation starts one bounded worker for the exact copied path");
    if (!writer) return;
    copied_path[0] = 0;
    const HANDLE event = client_preferences_writer_completion_event(writer);
    auto* original = writer;
    check(event && WaitForSingleObject(event, 0) == WAIT_TIMEOUT &&
          !client_preferences_writer_create(&writer, path) && writer == original,
          "preferences writer: creation has no completion and cannot overwrite live ownership");
    ClientPreferences untouched = defaults;
    check(client_preferences_load_file(path, &untouched).error == ClientPreferencesError::size &&
          client_preferences_equal(untouched, defaults),
          "preferences writer: creation alone leaves the empty fixture unmodified");
    check(client_preferences_writer_poll(writer, &completed, &result, &ready) && !ready &&
          completed == 92 && result.native_error == 93 && result.error == ClientPreferencesError::checksum,
          "preferences writer: empty poll preserves revision/result and clears ready");
    const ClientPreferences invalid{static_cast<ClientTheme>(255), false, false, false};
    check(!client_preferences_writer_submit(writer, invalid, &revision) && revision == 91,
          "preferences writer: malformed requests cannot consume revisions or enqueue disk IO");

    auto invalid_pack=defaults;invalid_pack.soundpack=static_cast<NotificationSoundpack>(9);
    const u64 pack_rejected_revision=revision;
    check(!client_preferences_writer_submit(writer,invalid_pack,&revision) && revision==pack_rejected_revision,
        "preferences writer: invalid pack cannot consume a revision or enqueue a malformed snapshot");

    const auto first = snapshot(9);
    check(client_preferences_writer_submit(writer, first, &revision) && revision == 1 &&
          await_revision(writer, revision, &completed, &result) && result.ok(),
          "preferences writer: accepted request completes with its monotonic revision");
    ClientPreferences loaded{};
    check(client_preferences_load_file(path, &loaded).ok() && client_preferences_equal(first, loaded),
          "preferences writer: private path copy survives caller buffer reuse");
    check(WaitForSingleObject(event, 0) == WAIT_TIMEOUT &&
          client_preferences_writer_poll(writer, &completed, &result, &ready) && !ready &&
          completed == 1 && result.ok(),
          "preferences writer: consuming completion resets the manual event without losing the result");

    bool accepted = true;
    u64 previous = revision;
    for (u32 i = 0; i < 512; ++i) {
        const bool submitted = client_preferences_writer_submit(writer, snapshot(i), &revision);
        accepted = accepted && submitted && revision == previous + 1;
        previous = revision;
    }
    const auto last = snapshot(511);
    check(accepted && await_revision(writer, revision, &completed, &result) && result.ok() &&
          client_preferences_load_file(path, &loaded).ok() && client_preferences_equal(last, loaded),
          "preferences writer: a burst coalesces copied pack flags and volume snapshots and commits the newest accepted value");

    // Alternate publication and consumption; a producer may publish before or
    // during poll's reset. The subsequent event wait must still see its revision.
    bool wakeups = true;
    for (u32 i = 0; i < 8; ++i) {
        wakeups = wakeups && client_preferences_writer_submit(writer, snapshot(i), &revision) &&
                  await_revision(writer, revision, &completed, &result) && result.ok();
    }
    check(wakeups, "preferences writer: repeated completion/reset cycles never lose a wakeup");

    // Existing destination denied FILE_SHARE_DELETE deterministically injects
    // atomic commit failure while retaining the previous complete file.
    const auto before_failure = snapshot(7);
    HANDLE locked = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "preferences writer: atomic destination replacement failure is injected locally");
    if (locked != INVALID_HANDLE_VALUE) {
        const auto retry = snapshot(14);
        check(client_preferences_writer_submit(writer, retry, &revision) &&
              await_revision(writer, revision, &completed, &result) &&
              result.error == ClientPreferencesError::commit && result.native_error != 0,
              "preferences writer: worker publishes native commit failure with the matching revision");
        CloseHandle(locked);
        check(client_preferences_load_file(path, &loaded).ok() && client_preferences_equal(before_failure, loaded),
              "preferences writer: failed write preserves the previous atomic record");
        const u64 failed_revision = revision;
        check(client_preferences_writer_submit(writer, retry, &revision) && revision == failed_revision + 1 &&
              await_revision(writer, revision, &completed, &result) && result.ok() &&
              client_preferences_load_file(path, &loaded).ok() && client_preferences_equal(retry, loaded),
              "preferences writer: explicit retry accepts the same snapshot and supersedes the failure");
    }

    // Stop immediately after a burst: it must persist the final pending value,
    // even when its bounded wait times out while persistence is in progress.
    for (u32 i = 0; i < 512; ++i)
        accepted = client_preferences_writer_submit(writer, snapshot(i), &revision) && accepted;
    const auto final = snapshot(5);
    accepted = client_preferences_writer_submit(writer, final, &revision) && accepted;
    original = writer;
    const bool joined = client_preferences_writer_stop(&writer, 0);
    if (!joined) {
        const DWORD stop_error = GetLastError();
        const u64 preserved_revision = revision;
        check(stop_error == WAIT_TIMEOUT && writer == original &&
              client_preferences_writer_completion_event(writer) == event &&
              !client_preferences_writer_submit(writer, defaults, &revision) && revision == preserved_revision,
              "preferences writer: timed out stop retains storage/handles and refuses further submissions");
        check(client_preferences_writer_stop(&writer, 5'000),
              "preferences writer: second bounded stop joins the preserved worker");
    }
    check(accepted && !writer && client_preferences_load_file(path, &loaded).ok() &&
          client_preferences_equal(final, loaded) && client_preferences_writer_stop(&writer, 0),
          "preferences writer: stop drains newest accepted snapshot before releasing ownership");
}

} // namespace maicord
