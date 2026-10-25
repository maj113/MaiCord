#pragma once

#include "maicord/app/account_profile.hpp"

namespace maicord {

struct AccountPaths {
    wchar_t application_directory[kAccountProfileMaximumPathCharacters];
    wchar_t account_directory[kAccountProfileMaximumPathCharacters];
    wchar_t profile[kAccountProfileMaximumPathCharacters];
    wchar_t session[kAccountProfileMaximumPathCharacters];
    wchar_t drafts[kAccountProfileMaximumPathCharacters];
    // The encrypted startup shell (startup_shell.hpp).
    wchar_t shell[kAccountProfileMaximumPathCharacters];
};

enum class AccountPathsError : u8 {
    none,
    invalid_input,
    environment,
    capacity,
    directory,
};

struct AccountPathsResult {
    u32 native_error;
    AccountPathsError error;

    constexpr bool ok() const noexcept {
        return error == AccountPathsError::none;
    }
};

// Saved accounts live in slots. Slot 0 is the original single-account layout
// (accounts\active.*), so an existing profile keeps working unmoved; slot N
// is accounts\slot-N\active.*. Every slot has the same canonical file names.
inline constexpr u32 kAccountPathsSlotCount = 8;

// Builds the canonical per-user paths for slot without touching the filesystem.
AccountPathsResult account_paths_build_slot(const wchar_t* local_application_data, u32 slot,
                                            AccountPaths* paths) noexcept;
// Resolves the process's selected slot (0 until the account inventory selects one).
AccountPathsResult account_paths_resolve(AccountPaths* paths) noexcept;
AccountPathsResult account_paths_resolve_slot(u32 slot, AccountPaths* paths) noexcept;
// Main-thread selection; resolve() follows it. False for an invalid slot.
bool account_paths_select_slot(u32 slot) noexcept;
u32 account_paths_selected_slot() noexcept;

// Login/logout owns directory creation. Startup does not create account state
// before the user reaches the in-process login flow.
AccountPathsResult account_paths_ensure_directories(const AccountPaths& paths) noexcept;

} // namespace maicord
