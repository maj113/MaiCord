// The sign-in screen's observation of the saved accounts (application_account_switch.cpp): how often it reads a
// slot's profile file while the "session expired" screen stays up. The account paths are this file's fakes, so each
// read of a slot's files is one call to account_paths_resolve_slot.
#include "account/application_account_switch.hpp"
#include "account/application_account_logout.hpp"
#include "account/application_account_module.hpp"
#include "account/login/auth.hpp"
#include "account/login/captcha_auth.hpp"
#include "account/login/ui.hpp"
#include "chat/composer/text_form.hpp"
#include "shell/state.hpp"
#include "shell/ui_runtime.hpp"
#include "maicord/app/account/account_inventory.hpp"
#include "maicord/app/account/account_logout_stop.hpp"
#include "maicord/app/account/account_paths.hpp"
#include "maicord/app/account/account_switch.hpp"
#include "maicord/app/account/login/account_credential_forget.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/os/file.hpp"
#include "maicord/ui/text/secure_edit.hpp"
#include "../../support/test_output.hpp"

// The owners the observation does not reach: the logout, the account switch machine, the host and the login form.
namespace maicord {
bool account_logout_stop_owned(const AccountLogoutStopOwner&) noexcept { return false; }
Waitable account_logout_stop_notification(const AccountLogoutStopOwner&) noexcept { return nullptr; }
AccountLogoutStopStartResult account_logout_stop_start(AccountLogoutStopOwner*, AccountHost*, u32, u64, u64,
                                                       AccountLogoutStopFunction) noexcept {
    return {};
}
bool account_logout_stop_take(AccountLogoutStopOwner*, AccountHostStopResult*) noexcept { return false; }
bool account_logout_stop_join_for_shutdown(AccountLogoutStopOwner*, u32, AccountHostStopResult*) noexcept { return true; }
AccountHostStopResult account_host_stop(AccountHost*, u32, u64, u64) noexcept { return {}; }
bool account_switch_busy(const AccountSwitch&) noexcept { return false; }
bool account_switch_pump(AccountSwitch*) noexcept { return false; }
bool account_switch_login_finished(AccountSwitch*, bool) noexcept { return false; }
bool account_switch_begin(AccountSwitch*, AccountSwitchKind, u32, u32) noexcept { return false; }
AccountLoginStatus account_host_login_stop(AccountHost*, u32) noexcept { return {}; }
void account_host_move(AccountHost*, AccountHost*) noexcept {}
AccountCredentialForgetResult account_credential_forget_complete_after_join(const AccountPaths&) noexcept { return {}; }
AccountCredentialForgetResult account_credential_forget_probe(const AccountPaths&) noexcept { return {}; }
SecureEditResult secure_edit_clear(SecureEdit*) noexcept { return {}; }
namespace app_detail {
bool application_account_attach_if_present() noexcept { return false; }
bool application_account_logout_blocked() noexcept { return false; }
bool application_account_logout_host_owned() noexcept { return false; }
bool application_account_logout_open() noexcept { return false; }
bool application_account_publish_logged_out(bool) noexcept { return false; }
bool application_account_return_to_login(bool) noexcept { return false; }
void application_account_module_release() noexcept {}
void application_captcha_auth_reset() noexcept {}
void application_login_reset_remote_session() noexcept {}
void application_login_retain_auth_retry() noexcept {}
void application_prepare_account_stop() noexcept {}
void application_account_stop_observed(const AccountHostStopResult&, bool) noexcept {}
bool application_hide_account_scenes() noexcept { return true; }
bool application_refresh_login_view() noexcept { return true; }
void application_release_account_ui(Application&) noexcept {}
bool application_text_form_open(const ApplicationTextFormRequest&) noexcept { return false; }
} // namespace app_detail
} // namespace maicord

namespace maicord {
namespace switch_observe_test {
u32 failures{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
wchar_t g_root[kAccountProfileMaximumPathCharacters];
u32 g_root_length;
u32 g_selected;
u32 g_resolves[kAccountPathsSlotCount];

void join(wchar_t* output, const wchar_t* leaf) noexcept {
    u32 length = 0;
    wide_append(output, kAccountProfileMaximumPathCharacters, &length, g_root);
    wide_append(output, kAccountProfileMaximumPathCharacters, &length, leaf);
}
} // namespace switch_observe_test

// Slot 0's files in the test directory; the other slots have none.
AccountPathsResult account_paths_resolve_slot(u32 slot, AccountPaths* paths) noexcept {
    using namespace switch_observe_test;
    if (!paths || slot >= kAccountPathsSlotCount)
        return {0, AccountPathsError::invalid_input};
    ++g_resolves[slot];
    clear_bytes(paths, sizeof(*paths));
    join(paths->application_directory, L"");
    join(paths->account_directory, slot ? L"\\accounts\\slot" : L"\\accounts");
    join(paths->profile, slot ? L"\\accounts\\slot\\active.profile" : L"\\accounts\\active.profile");
    join(paths->session, L"\\accounts\\active.session");
    join(paths->drafts, L"\\accounts\\active.drafts");
    join(paths->shell, L"\\accounts\\active.shell");
    return {};
}
AccountPathsResult account_paths_resolve(AccountPaths* paths) noexcept {
    return account_paths_resolve_slot(switch_observe_test::g_selected, paths);
}
bool account_paths_select_slot(u32 slot) noexcept {
    if (slot >= kAccountPathsSlotCount)
        return false;
    switch_observe_test::g_selected = slot;
    return true;
}
u32 account_paths_selected_slot() noexcept {
    return switch_observe_test::g_selected;
}
AccountPathsResult account_paths_ensure_directories(const AccountPaths&) noexcept {
    return {};
}

namespace switch_observe_test {
using namespace app_detail;

bool write_file(const wchar_t* leaf, ConstSlice<u8> bytes) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    join(path, leaf);
    const FileHandle file = file_open(path, {FileAccess::write, FileCreate::create_always, 0, 0});
    u32 written = 0;
    const bool ok = file != INVALID_HANDLE_VALUE && file_write(file, bytes.data, bytes.count, &written) &&
                    written == bytes.count;
    if (file != INVALID_HANDLE_VALUE)
        file_close(file);
    return ok;
}

bool saved_needs_login() noexcept {
    AccountPaths root{};
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    AccountInventory inventory{};
    const auto* entry = account_paths_resolve_slot(0, &root).ok() &&
                                account_inventory_path(root, path, kAccountProfileMaximumPathCharacters) &&
                                account_inventory_load(path, &inventory).ok()
                            ? account_inventory_find_slot(inventory, 0)
                            : nullptr;
    return entry && (entry->flags & account_inventory_needs_login);
}

void run() noexcept {
    g_root_length = environment_variable(L"TEMP", g_root, 400);
    check(g_root_length && g_root_length < 400, "the test has a TEMP directory");
    wchar_t directory[kAccountProfileMaximumPathCharacters]{};
    join(directory, L"\\accounts");
    dir_create(directory);
    // One saved account in slot 0 whose profile file is present.
    AccountPaths root{};
    wchar_t inventory_path[kAccountProfileMaximumPathCharacters]{};
    AccountInventory inventory{};
    AccountInventoryEntry entry{};
    entry.user_id = 1;
    entry.last_used_unix_ms = 1;
    check(account_paths_resolve_slot(0, &root).ok() &&
              account_inventory_path(root, inventory_path, kAccountProfileMaximumPathCharacters) &&
              account_inventory_upsert(&inventory, entry) == AccountInventoryError::none &&
              account_inventory_save(inventory_path, inventory).ok(),
          "the saved account's inventory is written");
    const u8 profile[]{1};
    check(write_file(L"\\accounts\\active.profile", {profile, 1}), "the saved account's profile file is written");
    application_account_switch_initialize();

    // The session expired screen of slot 0 stays up for 100 loop turns.
    g_app.account.instance = nullptr;
    g_app.login.visible = true;
    g_app.login.status = LoginStatus::session_expired;
    clear_bytes(g_resolves, sizeof(g_resolves));
    constexpr u32 kTurns = 100;
    for (u32 turn = 0; turn < kTurns; ++turn)
        application_account_switch_pump();
    test_write_metric("slot 0 file reads over 100 loop turns", g_resolves[0]);
    check(g_resolves[0] == 1, "the screen reads the profile file once, not on each loop turn");
    check(!saved_needs_login(), "a present profile leaves the saved account signed in");

    // A session in between: the next expiry reads the file again, once.
    g_app.account.instance = &g_resolves;
    application_account_switch_pump();
    g_app.account.instance = nullptr;
    clear_bytes(g_resolves, sizeof(g_resolves));
    for (u32 turn = 0; turn < kTurns; ++turn)
        application_account_switch_pump();
    check(g_resolves[0] == 1 && !saved_needs_login(), "after a session the expired screen reads the file once again");

    // The screen shown again with the profile gone: one read finds it missing and the inventory save marks the account
    // (that save resolves slot 0's paths once more).
    wchar_t profile_path[kAccountProfileMaximumPathCharacters]{};
    join(profile_path, L"\\accounts\\active.profile");
    check(file_remove(profile_path), "the profile file is removed");
    g_app.login.status = LoginStatus::idle;
    application_account_switch_pump();
    g_app.login.status = LoginStatus::session_expired;
    clear_bytes(g_resolves, sizeof(g_resolves));
    for (u32 turn = 0; turn < kTurns; ++turn)
        application_account_switch_pump();
    check(g_resolves[0] == 2 && saved_needs_login(),
          "the screen shown again reads the file once and marks the account whose profile is gone");
}
} // namespace switch_observe_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_account_switch_observe_entry() noexcept {
    maicord::switch_observe_test::run();
    maicord::test_exit(maicord::switch_observe_test::failures, "account switch observation");
}
