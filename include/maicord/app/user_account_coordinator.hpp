#pragma once

#include "maicord/app/account_profile.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/time.hpp"

namespace maicord {

inline constexpr u32 kUserAccountAuthorizedResponseBytes = 256 * 1024;

enum class UserAccountCoordinatorPhase : u8 {
    empty,
    gateway_connecting,
    running,
    failed,
    stopping,
    stopped,
};

enum class UserAccountCoordinatorError : u8 {
    none,
    invalid_input,
    credential,
    properties,
    rest_start,
    gateway_start,
    gateway,
    stop,
    authentication_rejected,
};

struct UserAccountCoordinatorResult {
    u64 profile_id;
    u32 native_error;
    UserAccountCoordinatorError error;
    UserAccountCoordinatorPhase phase;
    RestClientError rest_error;
    GatewayClientError gateway_error;
    // Recoverable Gateway worker failures restart with backoff. Rejected
    // credentials and terminal configuration close codes never restart.
    // gateway_error keeps the last such failure for diagnostics.
    u64 gateway_restart_at_ms;
    u32 gateway_restarts;
    u32 gateway_restart_attempt;

    constexpr bool ok() const noexcept {
        return error == UserAccountCoordinatorError::none;
    }
};

// Delay before restarting a failed Gateway worker: 2 s doubling to a 5 min cap.
u32 user_account_coordinator_restart_delay(u32 attempt) noexcept;

// Separate message/history and profile/command REST owners share one rate
// authority. Gateway events never queue behind either REST or media work. No application
// credential, intents, sharding, or server-side application quota enters this
// type.
struct UserAccountCoordinator {
    RestClient rest;
    RestClient details;
    RestSharedState rest_shared;
    GatewayClient gateway;
    AccountProfileMetadata metadata;
    UserAccountCoordinatorResult state;
};

UserAccountCoordinatorResult user_account_coordinator_start(
    UserAccountCoordinator* coordinator, AccountProfile* profile,
    const GatewayUserClientConfiguration* configuration, const GatewayUserSessionSnapshot* session,
    Utf8View timezone = {}) noexcept;
// Rejected credentials (Gateway close 4004 or REST 401) and terminal Gateway
// configuration close codes 4010-4014 fail the coordinator. Configuration
// failures preserve credentials; recoverable failures schedule a restart.
UserAccountCoordinatorResult user_account_coordinator_pump(UserAccountCoordinator* coordinator,
                                                           u64 now_ms) noexcept;
inline UserAccountCoordinatorResult user_account_coordinator_pump(UserAccountCoordinator* coordinator) noexcept {
    return user_account_coordinator_pump(coordinator, GetTickCount64());
}
bool user_account_coordinator_gateway_restart_due(const UserAccountCoordinator* coordinator,
                                                  u64 now_ms) noexcept;
// Stops the failed worker (recovering its credential) and starts a fresh
// IDENTIFY session with the given configuration. Call only when due, from the
// thread that owns Gateway observers.
UserAccountCoordinatorResult user_account_coordinator_restart_gateway(
    UserAccountCoordinator* coordinator, const GatewayUserClientConfiguration* configuration,
    u64 now_ms) noexcept;
UserAccountCoordinatorResult user_account_coordinator_status(
    const UserAccountCoordinator* coordinator) noexcept;
bool user_account_coordinator_snapshot_session(const UserAccountCoordinator* coordinator,
                                               GatewayUserSessionSnapshot* snapshot) noexcept;
UserAccountCoordinatorResult user_account_coordinator_stop(UserAccountCoordinator* coordinator,
                                                           u32 timeout_ms,
                                                           GatewayUserSessionSnapshot* snapshot) noexcept;
u32 user_account_coordinator_notification_handles(const UserAccountCoordinator* coordinator, HANDLE* output,
                                                  u32 capacity) noexcept;
RestClient* user_account_coordinator_rest(UserAccountCoordinator* coordinator,
                                          bool details = false) noexcept;
RestClientTakeResult user_account_coordinator_take(UserAccountCoordinator* coordinator,
                                                   RestClientResponse* response,
                                                   Slice<u8> body) noexcept;
GatewayClient* user_account_coordinator_gateway(UserAccountCoordinator* coordinator) noexcept;

} // namespace maicord
