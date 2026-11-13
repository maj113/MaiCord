#pragma once

#include "maicord/app/account/account_paths.hpp"
#include "maicord/core/slice.hpp"

namespace maicord {

enum class AccountCredentialForgetError : u8 {
    none,
    invalid_input,
    probe,
    marker_write,
    credential_delete,
    marker_delete,
};

struct AccountCredentialForgetResult {
    AccountCredentialForgetError error;
    bool pending;

    constexpr bool ok() const noexcept {
        return error == AccountCredentialForgetError::none;
    }
};

enum class AccountCredentialForgetPresence : u8 {
    missing,
    present,
    error
};

// The file operations, in account_credential_forget_io.cpp (tests link a replacement). Write replaces the file
// atomically; remove succeeds when the file is gone, missing included.
AccountCredentialForgetPresence account_credential_forget_stat(const wchar_t* path) noexcept;
bool account_credential_forget_write(const wchar_t* path, u32 length, ConstSlice<u8> bytes) noexcept;
bool account_credential_forget_remove(const wchar_t* path) noexcept;

// AccountPaths must contain the canonical active.profile and active.session
// children of account_directory. Only those files, the account's startup
// shell (active.shell) and their .new companions are touched. The nonsecret active.logout marker contains a fixed magic/v1.
// Any marker (including incomplete .new, a directory, or inaccessible path)
// blocks startup; probe does not trust marker contents or decrypt credentials.
AccountCredentialForgetResult account_credential_forget_probe(const AccountPaths& paths) noexcept;
AccountCredentialForgetResult account_credential_forget_begin(const AccountPaths& paths) noexcept;

// Caller owns all coordinator/login joins and promotion serialization. Preserves existing intent (including
// interrupted .new) or commits new intent before deleting four credentials and the shell, confirms absence,
// then deletes marker.new and marker last. Any error leaves pending=true. Missing files are successful; no
// drafts/preferences are read.
AccountCredentialForgetResult account_credential_forget_complete_after_join(const AccountPaths& paths) noexcept;

} // namespace maicord
