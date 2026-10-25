#pragma once

#include "maicord/app/gateway_client.hpp"

namespace maicord {

struct ClientHeartbeatSession;
struct CookieJar;

inline constexpr u32 kUserAccountSessionStoreVersion = 4;
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

// Stores the profile-bound normal-user Resume state, authenticated user ID, a
// valid analytics heartbeat session and the jar's unexpired persistent
// cookies, if any (session cookies stay in memory). Version 1 loads with
// unknown user identity, versions before 3 without a heartbeat session and
// before 4 without cookies; all are upgraded on the next save. Load restores
// saved cookies into the jar, replacing its contents.
AccountSessionStoreResult account_user_session_store_save(
    const wchar_t* path, const GatewayUserSessionSnapshot& snapshot,
    const ClientHeartbeatSession* heartbeat = nullptr, CookieJar* cookies = nullptr) noexcept;
AccountSessionStoreResult account_user_session_store_load(
    const wchar_t* path, u64 expected_profile_id, GatewayUserSessionSnapshot* snapshot,
    ClientHeartbeatSession* heartbeat = nullptr, CookieJar* cookies = nullptr) noexcept;

// What the session file last held, for skipping saves that would write the
// same bytes. The Resume sequence advances after nearly every Gateway dispatch
// and a persisted resume is never replayed, so a periodic save ignores it
// (include_sequence false) and only the final save at shutdown records it.
struct AccountSessionSaveState {
    u64 fingerprint;
    u64 sequence;
    bool known;
};
bool account_session_save_state_current(const AccountSessionSaveState& state,
                                        const GatewayUserSessionSnapshot& snapshot,
                                        const ClientHeartbeatSession* heartbeat, u32 cookie_revision,
                                        bool include_sequence) noexcept;
void account_session_save_state_record(AccountSessionSaveState* state,
                                       const GatewayUserSessionSnapshot& snapshot,
                                       const ClientHeartbeatSession* heartbeat, u32 cookie_revision) noexcept;

} // namespace maicord
