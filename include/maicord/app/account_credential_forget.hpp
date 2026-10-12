#pragma once

#include "maicord/app/account_paths.hpp"
#include "maicord/app/file_persistence.hpp"

namespace maicord {

enum class AccountCredentialForgetError : u8 {
    none,
    invalid_input,
    not_joined,
    probe,
    marker_write,
    credential_delete,
    marker_delete,
};

enum AccountCredentialForgetTarget : u8 {
    account_forget_profile = 1u << 0,
    account_forget_session = 1u << 1,
    account_forget_profile_new = 1u << 2,
    account_forget_session_new = 1u << 3,
    account_forget_marker = 1u << 4,
    account_forget_marker_new = 1u << 5,
};

struct AccountCredentialForgetResult {
    u32 native_error{};
    AccountCredentialForgetError error{};
    u8 failed_targets{};
    bool pending{};

    constexpr bool ok() const noexcept {
        return error == AccountCredentialForgetError::none;
    }
};

enum class AccountCredentialForgetPresence : u8 {
    missing,
    present,
    error
};
struct AccountCredentialForgetProbeResult {
    u32 native_error{};
    AccountCredentialForgetPresence presence{};
};

// Optional operation adapter is borrowed for this call only, for focused fault
// fixtures. Null uses real Win32 operations; an adapter must supply all three.
struct AccountCredentialForgetIo {
    void* context{};
    AccountCredentialForgetProbeResult (*probe)(void*, const wchar_t*) noexcept {};
    FilePersistenceWriteResult (*write_atomic)(void*, const wchar_t*, u32, ConstSlice<u8>) noexcept {};
    u32 (*remove)(void*, const wchar_t*) noexcept {};
};

// AccountPaths must contain the canonical active.profile and active.session
// children of account_directory. Only those files and their .new companions
// are touched. The nonsecret active.logout marker contains a fixed magic/v1.
// Any marker (including incomplete .new, a directory, or inaccessible path)
// blocks startup; probe does not trust marker contents or decrypt credentials.
AccountCredentialForgetResult account_credential_forget_probe(
    const AccountPaths& paths, const AccountCredentialForgetIo* io = nullptr) noexcept;
AccountCredentialForgetResult account_credential_forget_begin(
    const AccountPaths& paths, const AccountCredentialForgetIo* io = nullptr) noexcept;

// Caller owns all coordinator/login joins and promotion serialization. False
// performs zero IO. True preserves existing intent (including interrupted
// .new) or commits new intent before deleting four credentials,
// confirms absence, then deletes marker.new and marker last. Any error leaves
// pending=true. Missing files are successful; no drafts/preferences are read.
AccountCredentialForgetResult account_credential_forget_complete_after_join(
    const AccountPaths& paths, bool workers_joined, const AccountCredentialForgetIo* io = nullptr) noexcept;

} // namespace maicord
