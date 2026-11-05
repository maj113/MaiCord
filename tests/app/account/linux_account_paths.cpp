#include "maicord/app/account/account_paths.hpp"
#include "maicord/app/account/login/account_credential_forget.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/os/file.hpp"
#include "../../support/test_output.hpp"

// The account's file locations on a Linux profile: data_path_build joins with '/', so what shared code appends to those
// paths and compares against them must too (kPathSeparator), and a fresh user's data root does not exist yet: the
// directories are made from the root down (XDG: as needed). The start-up probe for an unfinished logout answers "none"
// for an empty profile and "pending" for a profile with the marker, as on Windows; before this test a mismatch between
// the separators made every launch of a profile without a marker look like a logout in need of recovery.

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool is_directory(const wchar_t* path) noexcept {
    FileStat stat{};
    return file_path_stat(path, &stat) && stat.kind == FileKind::directory;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_account_paths_entry) {
    using namespace maicord;
    wchar_t root[1024];
    const u32 root_units = environment_variable(L"XDG_DATA_HOME", root, 1024);
    check(root_units != 0 && root_units < 1000, "the test profile names its data root");
    // directory_ensure makes the missing ancestors too (a profile directory is made below a data root nobody made).
    wchar_t nested[1100], middle[1100], first[1100];
    u32 length = root_units;
    copy_bytes(first, root, root_units * sizeof(wchar_t));
    first[root_units] = 0;
    wide_append(first, 1100, &length, L"/ensure-test");
    copy_bytes(middle, first, (length + 1) * sizeof(wchar_t));
    wide_append(middle, 1100, &length, L"/middle");
    copy_bytes(nested, middle, (length + 1) * sizeof(wchar_t));
    wide_append(nested, 1100, &length, L"/leaf");
    dir_remove(nested);
    dir_remove(middle);
    dir_remove(first);
    check(!is_directory(first) && directory_ensure(nested) && is_directory(nested) && is_directory(middle),
          "directory_ensure creates the missing ancestors");
    check(directory_ensure(nested), "directory_ensure finds an existing directory");
    dir_remove(nested);
    dir_remove(middle);
    dir_remove(first);

    AccountPaths paths{};
    check(account_paths_resolve(&paths).ok(), "the account paths resolve");
    const auto ensured = account_paths_ensure_directories(paths);
    check(ensured.ok(), "the account directories are made from a missing data root down");
    check(is_directory(paths.application_directory) && is_directory(paths.account_directory),
          "the application and account directories exist");

    // A second slot sits below accounts/: its parent is made first, found by the native separator.
    AccountPaths second{};
    check(account_paths_build_slot(root, 1, &second).ok() && account_paths_ensure_directories(second).ok() &&
              is_directory(second.account_directory),
          "a slot directory is made below the accounts directory");

    const auto probed = account_credential_forget_probe(paths);
    check(probed.ok() && !probed.pending, "an empty profile has no unfinished logout");
    const auto begun = account_credential_forget_begin(paths);
    check(begun.ok(), "a logout can record its intention");
    const auto pending = account_credential_forget_probe(paths);
    check(pending.ok() && pending.pending, "the recorded intention is found again");
    check(account_credential_forget_complete_after_join(paths, true).ok() && !account_credential_forget_probe(paths).pending,
          "finishing the logout clears it");
    test_exit(g_failures, "linux_account_paths");
}
