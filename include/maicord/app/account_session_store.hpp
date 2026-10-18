#pragma once

#include "maicord/app/gateway_client.hpp"

namespace maicord {

inline constexpr u32 kUserAccountSessionStoreVersion = 2;
inline constexpr u32 kAccountSessionStoreMaximumFileBytes = 64 * 1024;

enum class AccountSessionStoreError : u8 {
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

struct AccountSessionStoreResult {
    u32 bytes;
    u32 native_error;
    AccountSessionStoreError error;

    constexpr bool ok() const noexcept {
        return error == AccountSessionStoreError::none;
    }
};

// Stores the profile-bound normal-user Resume state and authenticated user ID.
// Version 1 loads with unknown user identity and is upgraded on the next save.
AccountSessionStoreResult account_user_session_store_save(
    const wchar_t* path, const GatewayUserSessionSnapshot& snapshot) noexcept;
AccountSessionStoreResult account_user_session_store_load(
    const wchar_t* path, u64 expected_profile_id,
    GatewayUserSessionSnapshot* snapshot) noexcept;

} // namespace maicord
