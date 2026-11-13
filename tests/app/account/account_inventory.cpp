#include "maicord/app/account/account_inventory.hpp"
#include "maicord/app/account/account_switch.hpp"
#include "maicord/app/account/login/account_credential_forget.hpp"
#include "maicord/app/account/account_profile.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/winsock.hpp"

namespace maicord {
namespace {

using Check = void (*)(bool, const char*) noexcept;

// A recognizable synthetic credential; it must never appear outside the
// DPAPI-protected profile, and never in plaintext anywhere.
constexpr u8 kToken[] = {'m', 'c', 'i', 'n', 'v', '.', 's', 'y', 'n', 't', 'h', 'e', 't', 'i', 'c',
                         '.', 't', 'o', 'k', 'e', 'n', '.', '7', '3', '1', '9'};

u32 path_length(const wchar_t* text) noexcept {
    return wide_length(text, kAccountProfileMaximumPathCharacters);
}

bool join(const wchar_t* directory, const wchar_t* name, wchar_t* output) noexcept {
    const u32 first = path_length(directory), second = path_length(name);
    if (!first || first + second + 2 > kAccountProfileMaximumPathCharacters)
        return false;
    copy_bytes(output, directory, first * 2);
    output[first] = L'\\';
    copy_bytes(output + first + 1, name, (second + 1) * 2);
    return true;
}

bool ends_with(const wchar_t* path, const wchar_t* suffix) noexcept {
    const u32 a = path_length(path), b = path_length(suffix);
    return a >= b && bytes_equal(path + a - b, suffix, b * 2);
}

bool exists(const wchar_t* path) noexcept {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

u32 read_file(const wchar_t* path, u8* output, u32 capacity) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    DWORD read = 0;
    const bool ok = ReadFile(file, output, capacity, &read, nullptr);
    CloseHandle(file);
    return ok ? read : 0;
}

bool contains(const u8* haystack, u32 count, const u8* needle, u32 needle_bytes) noexcept {
    for (u32 index = 0; index + needle_bytes <= count; ++index)
        if (bytes_equal(haystack + index, needle, needle_bytes))
            return true;
    return false;
}

void remove_tree(const wchar_t* directory) noexcept {
    wchar_t pattern[kAccountProfileMaximumPathCharacters];
    if (!join(directory, L"*", pattern))
        return;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((data.cFileName[0] == L'.' && data.cFileName[1] == 0) ||
                (data.cFileName[0] == L'.' && data.cFileName[1] == L'.' && data.cFileName[2] == 0))
                continue;
            wchar_t child[kAccountProfileMaximumPathCharacters];
            if (!join(directory, data.cFileName, child))
                continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(child);
            else
                DeleteFileW(child);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(directory);
}

// Every file below the isolated tree: the credential bytes may appear only
// as ciphertext inside the profile, never in plaintext anywhere.
bool tree_free_of_token(const wchar_t* directory) noexcept {
    wchar_t pattern[kAccountProfileMaximumPathCharacters];
    if (!join(directory, L"*", pattern))
        return false;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE)
        return true;
    bool clean = true;
    do {
        if (data.cFileName[0] == L'.')
            continue;
        wchar_t child[kAccountProfileMaximumPathCharacters];
        if (!join(directory, data.cFileName, child))
            continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            clean = tree_free_of_token(child) && clean;
            continue;
        }
        u8 bytes[8192];
        const u32 count = read_file(child, bytes, sizeof(bytes));
        clean = !contains(bytes, count, kToken, sizeof(kToken)) && clean;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return clean;
}

struct Profile {
    wchar_t base[MAX_PATH]{};
    AccountPaths root{};
    bool ready{};

    bool initialize() noexcept {
        wchar_t temporary[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temporary);
        ready = length && length < MAX_PATH && GetTempFileNameW(temporary, L"MCI", 0, base) &&
                DeleteFileW(base) && CreateDirectoryW(base, nullptr);
        return ready && account_paths_build_slot(base, 0, &root).ok();
    }
    bool slot(u32 index, AccountPaths* paths) const noexcept {
        return account_paths_build_slot(base, index, paths).ok();
    }
    ~Profile() noexcept {
        if (ready)
            remove_tree(base);
    }
};

bool save_profile(const wchar_t* path, u64 user_id) noexcept {
    SecureBuffer token{};
    const bool ok = secure_buffer_initialize(&token, 64).ok() &&
                    secure_buffer_assign(&token, ConstSlice<u8>{kToken, sizeof(kToken)}).ok() &&
                    account_profile_save(path, AccountProfileMetadata{user_id}, token).ok();
    secure_buffer_release(&token);
    return ok;
}

AccountInventoryEntry entry(u64 user, u32 slot, u64 used, const char* name) noexcept {
    AccountInventoryEntry value{};
    value.user_id = user;
    value.slot = static_cast<u8>(slot);
    value.last_used_unix_ms = used;
    const u32 bytes = c_string_length(name);
    account_inventory_set_identity(&value, {name, bytes}, {name, bytes}, utf8_literal("a_0123456789abcdef"));
    return value;
}

void test_paths(Check check, const Profile& profile) noexcept {
    AccountPaths zero{}, three{};
    check(profile.slot(0, &zero) && bytes_equal(&zero, &profile.root, sizeof(zero)),
          "account_inventory_slot0_is_single_profile_layout");
    check(profile.slot(3, &three) && ends_with(three.profile, L"\\accounts\\slot-3\\active.profile") &&
              ends_with(three.session, L"\\accounts\\slot-3\\active.session") &&
              ends_with(three.drafts, L"\\accounts\\slot-3\\active.drafts") &&
              ends_with(three.account_directory, L"\\accounts\\slot-3"),
          "account_inventory_slot_paths_are_per_profile");
    AccountPaths invalid{};
    check(!account_paths_build_slot(profile.base, kAccountPathsSlotCount, &invalid).ok(),
          "account_inventory_slot_paths_reject_out_of_range");
    check(account_paths_ensure_directories(three).ok() && exists(three.account_directory),
          "account_inventory_slot_directories_nest_under_accounts");
    const u32 previous = account_paths_selected_slot();
    AccountPaths resolved{};
    check(account_paths_select_slot(3) && account_paths_selected_slot() == 3 &&
              !account_paths_select_slot(kAccountPathsSlotCount) && account_paths_selected_slot() == 3,
          "account_inventory_slot_selection_is_bounded");
    check(account_paths_resolve(&resolved).ok() && ends_with(resolved.profile, L"\\slot-3\\active.profile"),
          "account_inventory_resolve_follows_selected_slot");
    account_paths_select_slot(previous);
}

// Leaves a pattern in the stack below the caller, so the next call's frame starts dirty.
__declspec(noinline) void dirty_stack(u8 pattern) noexcept {
    volatile u8 scratch[16 * 1024];
    for (u32 index = 0; index < sizeof(scratch); ++index)
        scratch[index] = pattern;
}

void test_persistence(Check check, const Profile& profile) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters];
    check(account_inventory_path(profile.root, path, kAccountProfileMaximumPathCharacters) &&
              ends_with(path, L"\\MaiCord\\accounts\\accounts.inventory"),
          "account_inventory_path_is_beside_slots");
    AccountInventory loaded{};
    check(account_inventory_load(path, &loaded).error == AccountInventoryError::missing,
          "account_inventory_missing_file_is_distinct");
    AccountInventory saved{};
    check(account_inventory_upsert(&saved, entry(111, 0, 10, "First")) == AccountInventoryError::none &&
              account_inventory_upsert(&saved, entry(222, 2, 20, "Second \xc3\xa9")) == AccountInventoryError::none &&
              account_inventory_touch(&saved, 2, 30) && saved.active_slot == 2,
          "account_inventory_builds_entries");
    check(account_paths_ensure_directories(profile.root).ok() && account_inventory_save(path, saved).ok() &&
              account_inventory_load(path, &loaded).ok() && loaded.count == 2 && loaded.active_slot == 2 &&
              bytes_equal(&loaded.entries[0], &saved.entries[0], sizeof(AccountInventoryEntry)) &&
              bytes_equal(&loaded.entries[1], &saved.entries[1], sizeof(AccountInventoryEntry)),
          "account_inventory_round_trips_exactly");
    // The file never carries stack bytes: two saves from differently dirtied stacks are byte-identical.
    u8 first[kAccountInventoryMaximumFileBytes], again[kAccountInventoryMaximumFileBytes];
    dirty_stack(0xA5);
    const bool first_saved = account_inventory_save(path, saved).ok();
    const u32 first_bytes = read_file(path, first, sizeof(first));
    dirty_stack(0x5A);
    const bool again_saved = account_inventory_save(path, saved).ok();
    const u32 again_bytes = read_file(path, again, sizeof(again));
    check(first_saved && again_saved && first_bytes == 32 + 2 * 256 && first_bytes == again_bytes &&
              bytes_equal(first, again, first_bytes),
          "account_inventory_save_is_independent_of_stack_contents");
    const auto* second = account_inventory_find_user(loaded, 222);
    check(second && second->slot == 2 &&
              utf8_equal(account_inventory_display_name(*second), utf8_literal("Second \xc3\xa9")) &&
              utf8_equal(account_inventory_avatar_hash(*second), utf8_literal("a_0123456789abcdef")),
          "account_inventory_keeps_public_identity");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool tampered = file != INVALID_HANDLE_VALUE;
    if (tampered) {
        LARGE_INTEGER offset{};
        offset.QuadPart = 32 + 16; // first entry's slot byte
        const u8 value = 0x7f;
        DWORD written = 0;
        tampered = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
                   WriteFile(file, &value, 1, &written, nullptr) && written == 1;
        CloseHandle(file);
    }
    AccountInventory rejected{};
    check(tampered && account_inventory_load(path, &rejected).error == AccountInventoryError::format &&
              rejected.count == 0,
          "account_inventory_rejects_corrupt_slot");
    DeleteFileW(path);
}

void test_migration(Check check, const Profile& profile) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters];
    AccountInventory inventory{};
    const bool built = account_inventory_path(profile.root, path, kAccountProfileMaximumPathCharacters);
    const auto empty = account_inventory_open(profile.root, &inventory, 5);
    check(built && empty.ok() && !empty.migrated && inventory.count == 0 && !exists(path),
          "account_inventory_no_profile_writes_nothing");
    constexpr u64 kUser = 0x0123456789abcdefull;
    u8 before[4096]{}, after[4096]{};
    const bool legacy = account_paths_ensure_directories(profile.root).ok() && save_profile(profile.root.profile, kUser);
    const u32 before_bytes = legacy ? read_file(profile.root.profile, before, sizeof(before)) : 0;
    u64 header_user = 0;
    check(legacy && account_inventory_profile_user(profile.root.profile, &header_user) && header_user == kUser,
          "account_inventory_reads_profile_header_without_decrypting");
    const auto migrated = account_inventory_open(profile.root, &inventory, 77);
    const u32 after_bytes = read_file(profile.root.profile, after, sizeof(after));
    check(migrated.ok() && migrated.migrated && inventory.count == 1 && inventory.entries[0].slot == 0 &&
              inventory.entries[0].user_id == kUser && inventory.active_slot == 0 && exists(path),
          "account_inventory_migrates_single_profile_in_place");
    check(before_bytes && before_bytes == after_bytes && bytes_equal(before, after, before_bytes),
          "account_inventory_migration_leaves_profile_untouched");
    AccountInventory again{};
    const auto reopened = account_inventory_open(profile.root, &again, 99);
    check(reopened.ok() && !reopened.migrated && again.count == 1 &&
              again.entries[0].last_used_unix_ms == 77,
          "account_inventory_migration_runs_once");
    AccountProfile restored{};
    check(account_profile_load(profile.root.profile, &restored).ok() && restored.credential.bytes == sizeof(kToken) &&
              bytes_equal(restored.credential.data, kToken, sizeof(kToken)),
          "account_inventory_migrated_session_still_restores");
    account_profile_release(&restored);
}

void test_limits(Check check) noexcept {
    AccountInventory inventory{};
    bool added = true;
    for (u32 index = 0; index < kAccountInventoryMaximumAccounts; ++index)
        added = account_inventory_upsert(&inventory, entry(1000 + index, index, 100 + index, "A")) ==
                    AccountInventoryError::none &&
                added;
    u32 slot = 99;
    check(added && inventory.count == 5 && !account_inventory_free_slot(inventory, &slot) && slot == 99,
          "account_inventory_limit_is_five");
    check(account_inventory_upsert(&inventory, entry(2000, 6, 1, "B")) == AccountInventoryError::capacity,
          "account_inventory_rejects_sixth_account");
    check(account_inventory_upsert(&inventory, entry(1003, 6, 1, "B")) == AccountInventoryError::invalid_input,
          "account_inventory_rejects_same_user_in_two_slots");
    check(account_inventory_upsert(&inventory, entry(1002, 2, 500, "Renamed")) == AccountInventoryError::none &&
              inventory.count == 5,
          "account_inventory_upsert_replaces_by_slot");
    u8 order[kAccountInventoryMaximumAccounts]{};
    const u32 count = account_inventory_order(inventory, order);
    check(count == 5 && inventory.entries[order[0]].slot == 2 && inventory.entries[order[1]].slot == 4 &&
              inventory.entries[order[4]].slot == 0,
          "account_inventory_orders_by_last_used");
    check(account_inventory_touch(&inventory, 0, 1) && account_inventory_order(inventory, order) == 5 &&
              inventory.entries[order[0]].slot == 0 && inventory.active_slot == 0,
          "account_inventory_touch_moves_to_front_even_with_old_clock");
    check(account_inventory_remove_slot(&inventory, 1) && inventory.count == 4 &&
              !account_inventory_find_slot(inventory, 1) && account_inventory_free_slot(inventory, &slot) && slot == 1,
          "account_inventory_remove_frees_lowest_slot");
    AccountInventoryEntry long_name{};
    long_name.user_id = 1;
    char text[200];
    for (u32 index = 0; index < 200; index += 2) {
        text[index] = static_cast<char>(0xc3);
        text[index + 1] = static_cast<char>(0xa9);
    }
    check(account_inventory_set_identity(&long_name, {text, 200}, {text, 200}, {}) &&
              long_name.display_name_bytes == 128 && long_name.username_bytes == 64 &&
              !account_inventory_set_identity(&long_name, {text, 200}, {text, 200}, {}),
          "account_inventory_identity_is_bounded_on_code_points");
}

// Logging out of a saved, non-active account reuses the logout contract
// (marker, delete, confirm) on that slot alone; no worker runs there.
void test_inactive_logout(Check check, const Profile& profile) noexcept {
    AccountPaths active{}, other{};
    const bool ready = profile.slot(0, &active) && profile.slot(4, &other) &&
                       account_paths_ensure_directories(active).ok() && account_paths_ensure_directories(other).ok() &&
                       save_profile(active.profile, 11) && save_profile(other.profile, 44);
    const auto begun = ready ? account_credential_forget_begin(other) : AccountCredentialForgetResult{};
    const auto completed = begun.ok() ? account_credential_forget_complete_after_join(other)
                                      : AccountCredentialForgetResult{};
    const auto probe = account_credential_forget_probe(other);
    check(ready && begun.ok() && completed.ok() && !completed.pending && !exists(other.profile) &&
              probe.ok() && !probe.pending,
          "account_inventory_inactive_logout_forgets_only_its_slot");
    u64 user = 0;
    check(account_inventory_profile_user(active.profile, &user) && user == 11 &&
              account_credential_forget_probe(active).ok(),
          "account_inventory_inactive_logout_keeps_active_session");
    check(tree_free_of_token(profile.base), "account_inventory_no_plaintext_token_on_disk");
}

struct FakeIo {
    char trace[64];
    u32 used;
    u32 selected;
    u32 committed_slot;
    u32 polls_pending;
    bool stop_ok = true, stop_fails, attach_target = true, attach_source = true, login_ok = true;
    void put(char c) noexcept {
        if (used < sizeof(trace) - 1)
            trace[used++] = c;
    }
    bool is(const char* expected) const noexcept {
        const u32 bytes = c_string_length(expected);
        return bytes == used && bytes_equal(trace, expected, bytes);
    }
};
FakeIo* g_fake;

} // namespace

bool account_switch_stop_begin() noexcept {
    g_fake->put('S');
    return g_fake->stop_ok;
}
AccountSwitchStop account_switch_stop_poll() noexcept {
    g_fake->put('p');
    if (g_fake->polls_pending) {
        --g_fake->polls_pending;
        return AccountSwitchStop::pending;
    }
    return g_fake->stop_fails ? AccountSwitchStop::failed : AccountSwitchStop::stopped;
}
void account_switch_reset() noexcept {
    g_fake->put('R');
}
bool account_switch_select(u32 slot) noexcept {
    g_fake->put(static_cast<char>('0' + slot));
    g_fake->selected = slot;
    return true;
}
bool account_switch_attach() noexcept {
    g_fake->put('A');
    return g_fake->selected == 0 ? g_fake->attach_source : g_fake->attach_target;
}
bool account_switch_open_login() noexcept {
    g_fake->put('L');
    return g_fake->login_ok;
}
void account_switch_commit(u32 slot) noexcept {
    g_fake->put('C');
    g_fake->committed_slot = slot;
}

namespace {

void test_switch_machine(Check check) noexcept {
    {
        FakeIo fake{};
        g_fake = &fake;
        fake.polls_pending = 2;
        AccountSwitch machine{};
        const bool begun = account_switch_begin(&machine, AccountSwitchKind::switch_account, 0, 3);
        const bool waiting = !account_switch_pump(&machine) && !account_switch_pump(&machine) &&
                             machine.phase == AccountSwitchPhase::stopping;
        const bool done = account_switch_pump(&machine);
        check(begun && waiting && done && fake.is("SRppp3AC") && machine.phase == AccountSwitchPhase::idle &&
                  machine.outcome == AccountSwitchOutcome::switched && fake.committed_slot == 3,
              "account_switch_stops_resets_then_attaches_and_commits");
    }
    {
        FakeIo fake{};
        g_fake = &fake;
        AccountSwitch machine{};
        account_switch_begin(&machine, AccountSwitchKind::switch_account, 0, 2);
        check(!account_switch_begin(&machine, AccountSwitchKind::switch_account, 0, 4) && fake.is("SR"),
              "account_switch_rejects_second_switch_while_busy");
        fake.attach_target = false;
        account_switch_pump(&machine);
        check(fake.is("SRp2A0A") && machine.outcome == AccountSwitchOutcome::reverted && fake.committed_slot == 0 &&
                  machine.phase == AccountSwitchPhase::idle,
              "account_switch_failed_attach_reverts_without_commit");
    }
    {
        FakeIo fake{};
        g_fake = &fake;
        fake.stop_fails = true;
        AccountSwitch machine{};
        account_switch_begin(&machine, AccountSwitchKind::switch_account, 0, 1);
        for (u32 i = 0; i < 5; ++i)
            account_switch_pump(&machine);
        check(fake.is("SRpSpSp") && machine.outcome == AccountSwitchOutcome::failed &&
                  machine.phase == AccountSwitchPhase::idle,
              "account_switch_bounded_stop_retries");
    }
    {
        FakeIo fake{};
        g_fake = &fake;
        fake.stop_ok = false;
        AccountSwitch machine{};
        check(!account_switch_begin(&machine, AccountSwitchKind::switch_account, 0, 1) && fake.is("S") &&
                  machine.phase == AccountSwitchPhase::idle,
              "account_switch_unstarted_stop_keeps_current_account");
        check(!account_switch_begin(&machine, AccountSwitchKind::switch_account, 2, 2) &&
                  !account_switch_begin(&machine, AccountSwitchKind::add_account, 0, kAccountPathsSlotCount) &&
                  !account_switch_begin(&machine, AccountSwitchKind::none, 0, 1) && fake.is("S"),
              "account_switch_rejects_invalid_requests_before_stopping");
    }
    {
        FakeIo fake{};
        g_fake = &fake;
        AccountSwitch machine{};
        account_switch_begin(&machine, AccountSwitchKind::add_account, 0, 5);
        account_switch_pump(&machine);
        const bool login = machine.phase == AccountSwitchPhase::login && fake.is("SRp5L") && !fake.committed_slot;
        check(login && account_switch_login_finished(&machine, true) && fake.is("SRp5LC") &&
                  fake.committed_slot == 5 && machine.outcome == AccountSwitchOutcome::added,
              "account_switch_add_mode_commits_only_after_login");
    }
    {
        FakeIo fake{};
        g_fake = &fake;
        AccountSwitch machine{};
        account_switch_begin(&machine, AccountSwitchKind::add_account, 0, 5);
        account_switch_pump(&machine);
        check(account_switch_login_finished(&machine, false) && fake.is("SRp5L0A") && !fake.committed_slot &&
                  machine.outcome == AccountSwitchOutcome::cancelled &&
                  !account_switch_login_finished(&machine, true),
              "account_switch_add_cancel_restores_previous_account");
    }
    g_fake = nullptr;
}

} // namespace

void test_account_inventory(Check check) noexcept {
    {
        Profile profile;
        check(profile.initialize(), "account_inventory_isolated_profile_directory");
        if (profile.ready) {
            test_paths(check, profile);
            test_persistence(check, profile);
            test_migration(check, profile);
            test_inactive_logout(check, profile);
        }
    }
    test_limits(check);
    test_switch_machine(check);
}

} // namespace maicord
