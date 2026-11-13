#include "maicord/app/account/login/account_credential_forget.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

constexpr const wchar_t* kNames[]{L"active.profile",     L"active.session", L"active.profile.new",
                                  L"active.session.new", L"active.logout",  L"active.logout.new",
                                  L"active.shell",       L"active.shell.new"};
// Everything logout removes: the credentials and the startup shell.
constexpr u32 kForgotten[]{0, 1, 2, 3, 6, 7};
constexpr u8 kMarkerBit = 1u << 4, kMarkerNewBit = 1u << 5;
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
            !account_paths_build_slot(base, 0, &paths).ok() || !account_paths_ensure_directories(paths).ok() ||
            !child(paths.application_directory, L"client.preferences", preferences))
            return false;
        ready = true;
        return write_file(paths.drafts) && write_file(preferences);
    }
    bool target(u32 index, wchar_t* path) const noexcept {
        return index < 8 && child(paths.account_directory, kNames[index], path);
    }
    bool credentials() const noexcept {
        wchar_t path[kAccountProfileMaximumPathCharacters];
        for (const u32 index : kForgotten)
            if (!target(index, path) || !write_file(path))
                return false;
        return true;
    }
    bool protected_files() const noexcept {
        return sentinel_unchanged(paths.drafts) && sentinel_unchanged(preferences);
    }
    bool all_credentials_absent() const noexcept {
        wchar_t path[kAccountProfileMaximumPathCharacters];
        for (const u32 index : kForgotten) {
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
        for (u32 index = 0; index < 8; ++index)
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

// The faults armed for one call; none outside faulted().
struct Faults {
    u32 calls;
    u32 removes;
    u32 sequence;
    u32 marker_new_removed_at;
    u32 marker_removed_at;
    bool remove_fails[8];
    u8 probe_errors;
    u8 probe_present;
    u8 probe_missing;
    bool write_fails;
};
Faults* g_faults;

u32 target_index(const wchar_t* path) noexcept {
    for (u32 i = 0; i < 8; ++i)
        if (suffix(path, kNames[i]))
            return i;
    return 8;
}

template <typename Call> auto faulted(Faults& faults, Call call) noexcept {
    g_faults = &faults;
    const auto result = call();
    g_faults = nullptr;
    return result;
}

} // namespace

// The file operations for every test in this executable; without armed faults they act as
// account_credential_forget_io.cpp does.
AccountCredentialForgetPresence account_credential_forget_stat(const wchar_t* path) noexcept {
    if (Faults* f = g_faults) {
        ++f->calls;
        const u32 i = target_index(path);
        if (i >= 8 || (f->probe_errors & (1u << i)))
            return AccountCredentialForgetPresence::error;
        if (f->probe_present & (1u << i))
            return AccountCredentialForgetPresence::present;
        if (f->probe_missing & (1u << i))
            return AccountCredentialForgetPresence::missing;
    }
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        return AccountCredentialForgetPresence::present;
    return win32_error_is_missing_file(thread_last_error()) ? AccountCredentialForgetPresence::missing
                                                             : AccountCredentialForgetPresence::error;
}

bool account_credential_forget_write(const wchar_t* path, u32 length, ConstSlice<u8> bytes) noexcept {
    if (Faults* f = g_faults) {
        ++f->calls;
        if (f->write_fails)
            return false;
    }
    return file_persistence_write_atomic(path, length, bytes).ok();
}

bool account_credential_forget_remove(const wchar_t* path) noexcept {
    if (Faults* f = g_faults) {
        ++f->calls;
        ++f->removes;
        const u32 i = target_index(path);
        if (i >= 8)
            return false;
        ++f->sequence;
        if (i == 5)
            f->marker_new_removed_at = f->sequence;
        if (i == 4)
            f->marker_removed_at = f->sequence;
        if (f->remove_fails[i])
            return false;
    }
    return DeleteFileW(path) || win32_error_is_missing_file(thread_last_error());
}

void test_account_credential_forget(void (*check)(bool, const char*) noexcept) noexcept {
    Fixture fixture{};
    const bool initialized = fixture.initialize();
    check(initialized, "credential forget creates an isolated real-file fixture");
    if (!initialized)
        return;
    const auto absent = account_credential_forget_probe(fixture.paths);
    check(absent.ok() && !absent.pending, "absent logout markers allow ordinary startup");

    Faults faults{};
    AccountPaths noncanonical = fixture.paths;
    noncanonical.profile[0] = L'!';
    const auto invalid = faulted(faults, [&]() noexcept { return account_credential_forget_begin(noncanonical); });
    check(invalid.error == AccountCredentialForgetError::invalid_input && invalid.pending && faults.calls == 0,
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
    raced.probe_missing = kMarkerBit;
    const auto commit_blocked =
        faulted(raced, [&]() noexcept { return account_credential_forget_begin(fixture.paths); });
    check(handle != INVALID_HANDLE_VALUE &&
              commit_blocked.error == AccountCredentialForgetError::marker_write &&
              commit_blocked.pending && sentinel_unchanged(fixture.paths.profile),
          "real atomic replacement failure after a simulated probe race leaves credentials untouched and "
          "intent pending");
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    const auto complete =
        faulted(faults, [&]() noexcept { return account_credential_forget_complete_after_join(fixture.paths); });
    check(complete.ok() && !complete.pending && fixture.all_credentials_absent() &&
              !account_credential_forget_probe(fixture.paths).pending &&
              faults.marker_new_removed_at == 7 && faults.marker_removed_at == 8 &&
              fixture.protected_files(),
          "joined cleanup deletes four exact credentials and the startup shell, then temporary and final "
          "marker, preserving drafts and preferences");
    const auto repeated = account_credential_forget_complete_after_join(fixture.paths);
    check(repeated.ok() && !repeated.pending && fixture.protected_files(),
          "missing credentials and markers permit idempotent cleanup");

    fixture.target(5, marker);
    check(write_file(marker) && account_credential_forget_probe(fixture.paths).pending,
          "a malformed interrupted marker.new blocks a fresh startup without parsing contents");
    faults = {};
    faults.write_fails = true;
    faults.remove_fails[0] = true;
    const auto interrupted_failure =
        faulted(faults, [&]() noexcept { return account_credential_forget_complete_after_join(fixture.paths); });
    check(interrupted_failure.error == AccountCredentialForgetError::credential_delete &&
              interrupted_failure.pending && sentinel_unchanged(marker) &&
              account_credential_forget_probe(fixture.paths).pending,
          "retry never rewrites or loses the sole interrupted marker.new even when writer and deletion "
          "would fail");
    check(account_credential_forget_complete_after_join(fixture.paths).ok() &&
              !account_credential_forget_probe(fixture.paths).pending,
          "disk-only interrupted intent can be reconstructed and completed");
    fixture.target(4, marker);
    check(CreateDirectoryW(marker, nullptr) && account_credential_forget_probe(fixture.paths).pending,
          "a directory occupying the marker path also blocks startup");
    RemoveDirectoryW(marker);

    faults = {};
    faults.probe_errors = kMarkerBit | kMarkerNewBit;
    const auto inaccessible =
        faulted(faults, [&]() noexcept { return account_credential_forget_probe(fixture.paths); });
    check(inaccessible.error == AccountCredentialForgetError::probe && inaccessible.pending && faults.calls == 2,
          "both marker probes fail closed");

    faults = {};
    faults.write_fails = true;
    const auto write_failed = fixture.credentials() && faulted(faults, [&]() noexcept {
        return account_credential_forget_complete_after_join(fixture.paths);
    }).error == AccountCredentialForgetError::marker_write;
    check(write_failed && faults.removes == 0 && sentinel_unchanged(fixture.paths.profile) &&
              !account_credential_forget_probe(fixture.paths).pending,
          "a marker write failure prevents every credential delete");

    check(account_credential_forget_begin(fixture.paths).ok(),
          "blocked-delete fixture commits intent first");
    handle = CreateFileW(fixture.paths.profile, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto blocked = account_credential_forget_complete_after_join(fixture.paths);
    check(handle != INVALID_HANDLE_VALUE &&
              blocked.error == AccountCredentialForgetError::credential_delete && blocked.pending &&
              account_credential_forget_probe(fixture.paths).pending &&
              GetFileAttributesW(fixture.paths.session) == INVALID_FILE_ATTRIBUTES &&
              sentinel_unchanged(fixture.paths.profile) && fixture.protected_files(),
          "a real deny-delete handle retains durable intent while other credentials are cleared");
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    check(account_credential_forget_complete_after_join(fixture.paths).ok() &&
              fixture.all_credentials_absent() && !account_credential_forget_probe(fixture.paths).pending,
          "fresh retry after releasing the real lock completes partial cleanup");

    faults = {};
    faults.remove_fails[0] = true;
    faults.remove_fails[1] = true;
    const auto multiple =
        faulted(faults, [&]() noexcept { return account_credential_forget_complete_after_join(fixture.paths); });
    check(multiple.error == AccountCredentialForgetError::credential_delete && faults.removes == 6 &&
              account_credential_forget_probe(fixture.paths).pending,
          "all exact credential deletions are attempted after one fails");

    faults = {};
    faults.probe_present = 1u << 3;
    const auto reappeared =
        faulted(faults, [&]() noexcept { return account_credential_forget_complete_after_join(fixture.paths); });
    check(reappeared.error == AccountCredentialForgetError::credential_delete && faults.removes == 6 &&
              account_credential_forget_probe(fixture.paths).pending,
          "post-delete absence verification prevents marker clearance if a credential remains");

    bool marker_failures = true;
    for (u32 target = 4; target < 6; ++target) {
        faults = {};
        faults.remove_fails[target] = true;
        const auto failed =
            faulted(faults, [&]() noexcept { return account_credential_forget_complete_after_join(fixture.paths); });
        marker_failures &= failed.error == AccountCredentialForgetError::marker_delete && failed.pending &&
                           fixture.all_credentials_absent() &&
                           account_credential_forget_probe(fixture.paths).pending &&
                           (target != 5 || faults.marker_removed_at == 0);
    }
    check(marker_failures,
          "marker deletion failures preserve restart blocking and temporary marker is removed first");
    check(account_credential_forget_complete_after_join(fixture.paths).ok() &&
              !account_credential_forget_probe(fixture.paths).pending && fixture.protected_files(),
          "marker-only retry finishes without modifying retained drafts or global preferences");
}

} // namespace maicord
