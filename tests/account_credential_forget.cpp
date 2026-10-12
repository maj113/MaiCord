#include "maicord/app/account_credential_forget.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_process.hpp"

namespace maicord {
namespace {

constexpr const wchar_t* kNames[]{L"active.profile",     L"active.session", L"active.profile.new",
                                  L"active.session.new", L"active.logout",  L"active.logout.new"};
constexpr u8 kSentinel[]{'s', 'y', 'n', 't', 'h', 'e', 't', 'i', 'c',
                         '-', 'f', 'i', 'x', 't', 'u', 'r', 'e'};

bool child(const wchar_t* directory, const wchar_t* name, wchar_t* output) noexcept {
    const u32 first = wide_length(directory, kAccountProfileMaximumPathCharacters);
    const u32 last = wide_length(name, kAccountProfileMaximumPathCharacters);
    if (!first || first + last + 2 > kAccountProfileMaximumPathCharacters)
        return false;
    for (u32 i = 0; i < first; ++i)
        output[i] = directory[i];
    output[first] = L'\\';
    for (u32 i = 0; i <= last; ++i)
        output[first + 1 + i] = name[i];
    return true;
}

bool write_file(const wchar_t* path) noexcept {
    HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, kSentinel, sizeof(kSentinel), &written, nullptr) &&
                    written == sizeof(kSentinel) && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

bool sentinel_unchanged(const wchar_t* path) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    u8 bytes[sizeof(kSentinel) + 1]{};
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes, sizeof(bytes), &read, nullptr) && read == sizeof(kSentinel) &&
                    bytes_equal(bytes, kSentinel, sizeof(kSentinel));
    CloseHandle(file);
    return ok;
}

bool marker_format(const wchar_t* path) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    constexpr u8 expected[]{'M', 'A', 'I', 'C', 'L', 'O', 'G', 'O', 1, 0, 0, 0};
    u8 bytes[sizeof(expected) + 1]{};
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes, sizeof(bytes), &read, nullptr) && read == sizeof(expected) &&
                    bytes_equal(bytes, expected, sizeof(expected));
    CloseHandle(file);
    return ok;
}

struct Fixture {
    AccountPaths paths{};
    wchar_t base[kAccountProfileMaximumPathCharacters]{};
    wchar_t preferences[kAccountProfileMaximumPathCharacters]{};
    bool ready{};

    bool initialize() noexcept {
        wchar_t temporary[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temporary);
        if (!length || length >= MAX_PATH || !GetTempFileNameW(temporary, L"MCF", 0, base) ||
            !DeleteFileW(base) || !CreateDirectoryW(base, nullptr) ||
            !account_paths_build(base, &paths).ok() || !account_paths_ensure_directories(paths).ok() ||
            !child(paths.application_directory, L"client.preferences", preferences))
            return false;
        ready = true;
        return write_file(paths.drafts) && write_file(preferences);
    }
    bool target(u32 index, wchar_t* path) const noexcept {
        return index < 6 && child(paths.account_directory, kNames[index], path);
    }
    bool credentials() const noexcept {
        wchar_t path[kAccountProfileMaximumPathCharacters];
        for (u32 index = 0; index < 4; ++index)
            if (!target(index, path) || !write_file(path))
                return false;
        return true;
    }
    bool protected_files() const noexcept {
        return sentinel_unchanged(paths.drafts) && sentinel_unchanged(preferences);
    }
    bool all_credentials_absent() const noexcept {
        wchar_t path[kAccountProfileMaximumPathCharacters];
        for (u32 index = 0; index < 4; ++index) {
            if (!target(index, path) || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES ||
                !win32_error_is_missing_file(thread_last_error()))
                return false;
        }
        return true;
    }
    ~Fixture() noexcept {
        if (!ready)
            return;
        wchar_t path[kAccountProfileMaximumPathCharacters];
        for (u32 index = 0; index < 6; ++index)
            if (target(index, path))
                DeleteFileW(path);
        DeleteFileW(paths.drafts);
        DeleteFileW(preferences);
        RemoveDirectoryW(paths.account_directory);
        RemoveDirectoryW(paths.application_directory);
        RemoveDirectoryW(base);
    }
};

bool suffix(const wchar_t* path, const wchar_t* name) noexcept {
    const u32 a = wide_length(path, kAccountProfileMaximumPathCharacters);
    const u32 b = wide_length(name, kAccountProfileMaximumPathCharacters);
    if (a < b + 1 || path[a - b - 1] != L'\\')
        return false;
    for (u32 i = 0; i < b; ++i)
        if (path[a - b + i] != name[i])
            return false;
    return true;
}

struct Faults {
    u32 calls{};
    u32 removes{};
    u32 sequence{};
    u32 marker_new_removed_at{};
    u32 marker_removed_at{};
    u32 remove_errors[6]{};
    u8 probe_errors{};
    u8 probe_present{};
    u8 probe_missing{};
    FilePersistenceWriteError write_error{};

    static u32 index(const wchar_t* path) noexcept {
        for (u32 i = 0; i < 6; ++i)
            if (suffix(path, kNames[i]))
                return i;
        return 6;
    }
    static AccountCredentialForgetProbeResult probe(void* context, const wchar_t* path) noexcept {
        auto& f = *static_cast<Faults*>(context);
        ++f.calls;
        const u32 i = index(path);
        if (i >= 6)
            return {ERROR_INVALID_PARAMETER, AccountCredentialForgetPresence::error};
        if (f.probe_errors & (1u << i))
            return {ERROR_ACCESS_DENIED, AccountCredentialForgetPresence::error};
        if (f.probe_present & (1u << i))
            return {0, AccountCredentialForgetPresence::present};
        if (f.probe_missing & (1u << i))
            return {0, AccountCredentialForgetPresence::missing};
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            return {0, AccountCredentialForgetPresence::present};
        const u32 error = thread_last_error();
        return {error, win32_error_is_missing_file(error) ? AccountCredentialForgetPresence::missing
                                                          : AccountCredentialForgetPresence::error};
    }
    static FilePersistenceWriteResult write(void* context, const wchar_t* path, u32 length,
                                            ConstSlice<u8> bytes) noexcept {
        auto& f = *static_cast<Faults*>(context);
        ++f.calls;
        if (f.write_error != FilePersistenceWriteError::none)
            return {ERROR_WRITE_FAULT, f.write_error};
        return file_persistence_write_atomic(path, length, bytes);
    }
    static u32 remove(void* context, const wchar_t* path) noexcept {
        auto& f = *static_cast<Faults*>(context);
        ++f.calls;
        ++f.removes;
        const u32 i = index(path);
        if (i >= 6)
            return ERROR_INVALID_PARAMETER;
        ++f.sequence;
        if (i == 5)
            f.marker_new_removed_at = f.sequence;
        if (i == 4)
            f.marker_removed_at = f.sequence;
        if (f.remove_errors[i])
            return f.remove_errors[i];
        return DeleteFileW(path) ? 0u : thread_last_error();
    }
    AccountCredentialForgetIo io() noexcept {
        return {this, probe, write, remove};
    }
};

} // namespace

void test_account_credential_forget(void (*check)(bool, const char*) noexcept) noexcept {
    Fixture fixture{};
    const bool initialized = fixture.initialize();
    check(initialized, "credential forget creates an isolated real-file fixture");
    if (!initialized)
        return;
    const auto absent = account_credential_forget_probe(fixture.paths);
    check(absent.ok() && !absent.pending, "absent logout markers allow ordinary startup");
    Faults faults{};
    auto io = faults.io();
    const auto not_joined = account_credential_forget_complete_after_join(fixture.paths, false, &io);
    check(not_joined.error == AccountCredentialForgetError::not_joined && not_joined.pending &&
              faults.calls == 0,
          "unjoined shutdown performs zero filesystem operations");

    AccountPaths noncanonical = fixture.paths;
    noncanonical.profile[0] = L'!';
    const auto invalid = account_credential_forget_begin(noncanonical, &io);
    check(invalid.error == AccountCredentialForgetError::invalid_input && invalid.pending &&
              faults.calls == 0,
          "noncanonical credential paths fail closed before IO");
    check(fixture.credentials() && account_credential_forget_begin(fixture.paths).ok() &&
              account_credential_forget_probe(fixture.paths).pending &&
              sentinel_unchanged(fixture.paths.profile),
          "durable logout intent precedes credential deletion");
    wchar_t marker[kAccountProfileMaximumPathCharacters];
    fixture.target(4, marker);
    check(marker_format(marker),
          "committed logout marker is exactly the nonsecret twelve-byte magic and version");
    HANDLE handle = CreateFileW(marker, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Faults raced{};
    raced.probe_missing = account_forget_marker;
    auto raced_io = raced.io();
    const auto commit_blocked = account_credential_forget_begin(fixture.paths, &raced_io);
    check(handle != INVALID_HANDLE_VALUE &&
              commit_blocked.error == AccountCredentialForgetError::marker_write &&
              commit_blocked.pending && sentinel_unchanged(fixture.paths.profile),
          "real atomic replacement failure after a simulated probe race leaves credentials untouched and "
          "intent pending");
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    const auto complete = account_credential_forget_complete_after_join(fixture.paths, true, &io);
    check(complete.ok() && !complete.pending && fixture.all_credentials_absent() &&
              !account_credential_forget_probe(fixture.paths).pending &&
              faults.marker_new_removed_at == 5 && faults.marker_removed_at == 6 &&
              fixture.protected_files(),
          "joined cleanup deletes four exact credentials then temporary and final marker, preserving "
          "drafts and preferences");
    const auto repeated = account_credential_forget_complete_after_join(fixture.paths, true);
    check(repeated.ok() && !repeated.pending && fixture.protected_files(),
          "missing credentials and markers permit idempotent cleanup");

    fixture.target(5, marker);
    check(write_file(marker) && account_credential_forget_probe(fixture.paths).pending,
          "a malformed interrupted marker.new blocks a fresh startup without parsing contents");
    faults = {};
    faults.write_error = FilePersistenceWriteError::flush;
    faults.remove_errors[0] = ERROR_ACCESS_DENIED;
    io = faults.io();
    const auto interrupted_failure =
        account_credential_forget_complete_after_join(fixture.paths, true, &io);
    check(interrupted_failure.error == AccountCredentialForgetError::credential_delete &&
              interrupted_failure.pending && sentinel_unchanged(marker) &&
              account_credential_forget_probe(fixture.paths).pending,
          "retry never rewrites or loses the sole interrupted marker.new even when writer and deletion "
          "would fail");
    check(account_credential_forget_complete_after_join(fixture.paths, true).ok() &&
              !account_credential_forget_probe(fixture.paths).pending,
          "disk-only interrupted intent can be reconstructed and completed");
    fixture.target(4, marker);
    check(CreateDirectoryW(marker, nullptr) && account_credential_forget_probe(fixture.paths).pending,
          "a directory occupying the marker path also blocks startup");
    RemoveDirectoryW(marker);

    faults = {};
    faults.probe_errors = account_forget_marker | account_forget_marker_new;
    io = faults.io();
    const auto inaccessible = account_credential_forget_probe(fixture.paths, &io);
    check(inaccessible.error == AccountCredentialForgetError::probe && inaccessible.pending &&
              inaccessible.native_error == ERROR_ACCESS_DENIED &&
              inaccessible.failed_targets == (account_forget_marker | account_forget_marker_new) &&
              faults.calls == 2,
          "both marker probes retain truthful native failures and fail closed");

    constexpr FilePersistenceWriteError stages[]{
        FilePersistenceWriteError::open, FilePersistenceWriteError::write, FilePersistenceWriteError::flush,
        FilePersistenceWriteError::commit};
    bool write_failures = fixture.credentials();
    for (auto stage : stages) {
        faults = {};
        faults.write_error = stage;
        io = faults.io();
        const auto failed = account_credential_forget_complete_after_join(fixture.paths, true, &io);
        write_failures &= failed.error == AccountCredentialForgetError::marker_write && failed.pending &&
                          failed.native_error == ERROR_WRITE_FAULT && faults.removes == 0 &&
                          sentinel_unchanged(fixture.paths.profile);
    }
    check(write_failures, "marker open write flush and commit failures prevent every credential delete");

    check(account_credential_forget_begin(fixture.paths).ok(),
          "blocked-delete fixture commits intent first");
    handle = CreateFileW(fixture.paths.profile, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto blocked = account_credential_forget_complete_after_join(fixture.paths, true);
    check(handle != INVALID_HANDLE_VALUE &&
              blocked.error == AccountCredentialForgetError::credential_delete &&
              blocked.failed_targets == account_forget_profile && blocked.native_error != 0 &&
              blocked.pending && account_credential_forget_probe(fixture.paths).pending &&
              sentinel_unchanged(fixture.paths.profile) && fixture.protected_files(),
          "a real deny-delete handle retains durable intent while other credentials are cleared");
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    check(account_credential_forget_complete_after_join(fixture.paths, true).ok() &&
              fixture.all_credentials_absent() && !account_credential_forget_probe(fixture.paths).pending,
          "fresh retry after releasing the real lock completes partial cleanup");

    faults = {};
    faults.remove_errors[0] = ERROR_ACCESS_DENIED;
    faults.remove_errors[1] = ERROR_SHARING_VIOLATION;
    io = faults.io();
    const auto multiple = account_credential_forget_complete_after_join(fixture.paths, true, &io);
    check(multiple.error == AccountCredentialForgetError::credential_delete &&
              multiple.native_error == ERROR_ACCESS_DENIED &&
              multiple.failed_targets == (account_forget_profile | account_forget_session) &&
              faults.removes == 4 && account_credential_forget_probe(fixture.paths).pending,
          "all exact credential deletions are attempted with first error and complete failed-target mask");

    faults = {};
    faults.probe_present = account_forget_session_new;
    io = faults.io();
    const auto reappeared = account_credential_forget_complete_after_join(fixture.paths, true, &io);
    check(reappeared.error == AccountCredentialForgetError::credential_delete &&
              reappeared.native_error == ERROR_BUSY &&
              reappeared.failed_targets == account_forget_session_new && faults.removes == 4 &&
              account_credential_forget_probe(fixture.paths).pending,
          "post-delete absence verification prevents marker clearance if a credential remains");

    bool marker_failures = true;
    for (u32 target = 4; target < 6; ++target) {
        faults = {};
        faults.remove_errors[target] = ERROR_ACCESS_DENIED;
        io = faults.io();
        const auto failed = account_credential_forget_complete_after_join(fixture.paths, true, &io);
        marker_failures &= failed.error == AccountCredentialForgetError::marker_delete && failed.pending &&
                           failed.failed_targets == (1u << target) && fixture.all_credentials_absent() &&
                           account_credential_forget_probe(fixture.paths).pending &&
                           (target != 5 || faults.marker_removed_at == 0);
    }
    check(marker_failures,
          "marker deletion failures preserve restart blocking and temporary marker is removed first");
    check(account_credential_forget_complete_after_join(fixture.paths, true).ok() &&
              !account_credential_forget_probe(fixture.paths).pending && fixture.protected_files(),
          "marker-only retry finishes without modifying retained drafts or global preferences");
}

} // namespace maicord
