#pragma once

#include "maicord/app/gateway_client.hpp"

namespace maicord {

struct ClientHeartbeatSession;

inline constexpr u32 kUserAccountSessionStoreVersion = 3;
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

// Stores the profile-bound normal-user Resume state, authenticated user ID and
// a valid analytics heartbeat session, if any. Version 1 loads with unknown
// user identity and versions before 3 without a heartbeat session; both are
// upgraded on the next save.
AccountSessionStoreResult account_user_session_store_save(
    const wchar_t* path, const GatewayUserSessionSnapshot& snapshot,
    const ClientHeartbeatSession* heartbeat = nullptr) noexcept;
AccountSessionStoreResult account_user_session_store_load(
    const wchar_t* path, u64 expected_profile_id, GatewayUserSessionSnapshot* snapshot,
    ClientHeartbeatSession* heartbeat = nullptr) noexcept;

} // namespace maicord
