#pragma once

#include "maicord/platform/secure_buffer.hpp"

namespace maicord {

inline constexpr u32 kAccountProfileVersion = 2;
inline constexpr u32 kAccountProfileMaximumCredentialBytes = 2 * 1024;
inline constexpr u32 kAccountProfileMaximumFileBytes = 64 * 1024;
inline constexpr u32 kAccountProfileMaximumPathCharacters = 1024;

struct AccountProfileMetadata {
    u64 profile_id;
};

struct AccountProfile {
    SecureBuffer credential;
    AccountProfileMetadata metadata;
};

enum class AccountProfileError : u8 {
    none,
    invalid_input,
    memory,
    open,
    size,
    read,
    format,
    version,
    protect,
    unprotect,
    write,
    flush,
    commit,
};

struct AccountProfileResult {
    u32 bytes;
    u32 native_error;
    AccountProfileError error;

    constexpr bool ok() const noexcept {
        return error == AccountProfileError::none;
    }
};

// The caller-supplied path is the only persistence target. Save writes
// an encrypted same-directory `.new` file, flushes it, and commits with an
// atomic replace. Version and profile ID are DPAPI-bound entropy. Version 2
// is normal-user-only and rejects the application-token envelope.
AccountProfileResult account_profile_save(const wchar_t* path, const AccountProfileMetadata& metadata,
                                          const SecureBuffer& credential) noexcept;
AccountProfileResult account_profile_load(const wchar_t* path, AccountProfile* profile) noexcept;
void account_profile_release(AccountProfile* profile) noexcept;

} // namespace maicord
