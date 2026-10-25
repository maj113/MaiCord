#include "maicord/app/user_account_coordinator.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

UserAccountCoordinatorResult invalid_result() noexcept {
    UserAccountCoordinatorResult result{};
    result.error = UserAccountCoordinatorError::invalid_input;
    return result;
}

bool duplicate_credential(const SecureBuffer& source, SecureBuffer* destination) noexcept {
    if (destination->data || !source.data || !source.locked || source.bytes == 0 ||
        source.bytes > kAccountProfileMaximumCredentialBytes) {
        return false;
    }
    const SecureBufferResult initialized = secure_buffer_initialize(destination, source.bytes);
    if (!initialized.ok()) {
        return false;
    }
    if (!secure_buffer_assign(destination, secure_buffer_view(source)).ok()) {
        secure_buffer_release(destination);
        return false;
    }
    return true;
}

void cleanup_failed_start(UserAccountCoordinator* coordinator, SecureBuffer* rest_credential) noexcept {
    if (coordinator->details.worker.thread)
        rest_client_stop(&coordinator->details, 5'000);
    if (coordinator->rest.worker.thread) {
        rest_client_stop(&coordinator->rest, 5'000);
    }
    secure_buffer_release(rest_credential);
    secure_clear_bytes(coordinator, sizeof(*coordinator));
}

void set_failure(UserAccountCoordinator* coordinator, UserAccountCoordinatorError error,
                 u32 native_error = 0) noexcept {
    coordinator->state.error = error;
    coordinator->state.native_error = native_error;
    coordinator->state.phase = UserAccountCoordinatorPhase::failed;
    coordinator->state.gateway_restart_at_ms = 0;
}

} // namespace

UserAccountCoordinatorResult user_account_coordinator_start(
    UserAccountCoordinator* coordinator, AccountProfile* profile,
    const GatewayUserClientConfiguration* configuration, const GatewayUserSessionSnapshot* session,
    Utf8View timezone, Utf8View accept_language) noexcept {
    if (!coordinator || !profile || !configuration ||
        coordinator->state.phase != UserAccountCoordinatorPhase::empty ||
        profile->metadata.profile_id == 0 || !profile->credential.data || !profile->credential.locked ||
        profile->credential.bytes == 0 || !gateway_user_client_configuration_validate(*configuration) ||
        (session && (session->profile_id != profile->metadata.profile_id ||
                     (session->has_resume && !gateway_resume_validate(session->resume))))) {
        return invalid_result();
    }

    u8 json_scratch[kRestUserSuperPropertiesMaximumBytes];
    u8 super_properties[kRestUserSuperPropertiesMaximumBytes];
    const RestUserPropertiesResult properties = write_rest_user_super_properties(
        configuration->properties, Slice<u8>{json_scratch, sizeof(json_scratch)},
        Slice<u8>{super_properties, sizeof(super_properties)});
    if (!properties.ok()) {
        secure_clear_bytes(json_scratch, sizeof(json_scratch));
        secure_clear_bytes(super_properties, sizeof(super_properties));
        UserAccountCoordinatorResult result{};
        result.error = UserAccountCoordinatorError::properties;
        return result;
    }

    SecureBuffer rest_credential{};
    if (!duplicate_credential(profile->credential, &rest_credential)) {
        secure_clear_bytes(super_properties, sizeof(super_properties));
        UserAccountCoordinatorResult result{};
        result.error = UserAccountCoordinatorError::credential;
        return result;
    }
    // Live metadata replaces the locale with the Discord interface locale per request.
    const RestUserConfiguration rest_configuration{ConstSlice<u8>{super_properties, properties.bytes},
                                                   configuration->properties.system_locale, timezone,
                                                   configuration->metadata, accept_language};
    const RestClientResult rest_started =
        rest_client_start_user(&coordinator->rest, &rest_credential, &rest_configuration,
                               kUserAccountAuthorizedResponseBytes, &coordinator->rest_shared);
    if (!rest_started.ok()) {
        secure_clear_bytes(super_properties, sizeof(super_properties));
        cleanup_failed_start(coordinator, &rest_credential);
        UserAccountCoordinatorResult result{};
        result.error = UserAccountCoordinatorError::rest_start;
        result.rest_error = rest_started.error;
        result.native_error = rest_started.native_error;
        return result;
    }

    const bool copied = duplicate_credential(profile->credential, &rest_credential);
    const auto details_started =
        copied ? rest_client_start_user(&coordinator->details, &rest_credential, &rest_configuration,
                                        kUserAccountAuthorizedResponseBytes, &coordinator->rest_shared)
               : RestClientResult{.error = RestClientError::memory};
    secure_clear_bytes(super_properties, sizeof(super_properties));
    if (!details_started.ok()) {
        cleanup_failed_start(coordinator, &rest_credential);
        UserAccountCoordinatorResult result{};
        result.error = UserAccountCoordinatorError::rest_start;
        result.rest_error = details_started.error;
        result.native_error = details_started.native_error;
        return result;
    }

    coordinator->metadata = profile->metadata;
    const GatewayClientResult gateway_started =
        gateway_client_start_user(&coordinator->gateway, profile, configuration, nullptr, session);
    if (!gateway_started.ok()) {
        cleanup_failed_start(coordinator, &rest_credential);
        UserAccountCoordinatorResult result{};
        result.error = UserAccountCoordinatorError::gateway_start;
        result.gateway_error = gateway_started.error;
        result.native_error = gateway_started.native_error;
        return result;
    }
    coordinator->state.profile_id = coordinator->metadata.profile_id;
    coordinator->state.phase = UserAccountCoordinatorPhase::gateway_connecting;
    return coordinator->state;
}

u32 user_account_coordinator_restart_delay(u32 attempt) noexcept {
    const u32 shift = attempt < 8 ? attempt : 8;
    const u32 delay = 2'000u << shift;
    return delay < 300'000u ? delay : 300'000u;
}

UserAccountCoordinatorResult user_account_coordinator_pump(UserAccountCoordinator* coordinator,
                                                           u64 now_ms) noexcept {
    if (!coordinator) {
        return invalid_result();
    }
    if (coordinator->state.phase == UserAccountCoordinatorPhase::gateway_connecting ||
        coordinator->state.phase == UserAccountCoordinatorPhase::running) {
        const AccountRequirement required = gateway_client_required_action(&coordinator->gateway);
        rest_client_set_required_action(&coordinator->rest, required);
        rest_client_set_required_action(&coordinator->details, required);
        const GatewayClientResult gateway = gateway_client_status(&coordinator->gateway);
        coordinator->state.gateway_error = gateway.error;
        coordinator->state.native_error = gateway.native_error;
        if (gateway.close_code == 4004 || rest_client_status(&coordinator->rest).authentication_rejected ||
            rest_client_status(&coordinator->details).authentication_rejected) {
            // Only rejected credentials end the session (and its saved token).
            set_failure(coordinator, UserAccountCoordinatorError::authentication_rejected, 0);
        } else if (gateway.close_code >= 4010 && gateway.close_code <= 4014) {
            // Invalid shard/version/intents configuration cannot recover by
            // repeating IDENTIFY. Keep the credentials for an explicit restart
            // after the configuration is corrected.
            set_failure(coordinator, UserAccountCoordinatorError::gateway, gateway.native_error);
        } else if (!gateway.ok()) {
            // Decode, protocol, transport and worker failures are not a
            // logout: keep the session and UI, restart the worker later.
            if (!coordinator->state.gateway_restart_at_ms) {
                const u32 delay =
                    user_account_coordinator_restart_delay(coordinator->state.gateway_restart_attempt);
                if (coordinator->state.gateway_restart_attempt < 0xFFFF'FFFFu)
                    ++coordinator->state.gateway_restart_attempt;
                coordinator->state.gateway_restart_at_ms = now_ms + delay;
            }
        } else if (gateway.phase == GatewayClientPhase::ready) {
            coordinator->state.phase = UserAccountCoordinatorPhase::running;
            coordinator->state.gateway_restart_attempt = 0;
        }
    }
    return coordinator->state;
}

bool user_account_coordinator_gateway_restart_due(const UserAccountCoordinator* coordinator,
                                                  u64 now_ms) noexcept {
    return coordinator && coordinator->state.gateway_restart_at_ms &&
           now_ms >= coordinator->state.gateway_restart_at_ms &&
           (coordinator->state.phase == UserAccountCoordinatorPhase::gateway_connecting ||
            coordinator->state.phase == UserAccountCoordinatorPhase::running);
}

UserAccountCoordinatorResult user_account_coordinator_restart_gateway(
    UserAccountCoordinator* coordinator, const GatewayUserClientConfiguration* configuration,
    u64 now_ms) noexcept {
    if (!coordinator || !configuration ||
        !user_account_coordinator_gateway_restart_due(coordinator, now_ms))
        return coordinator ? coordinator->state : invalid_result();
    AccountProfile profile{};
    const GatewayClientResult stopped =
        gateway_client_stop_return_user_profile(&coordinator->gateway, 5'000, &profile);
    if (!profile.credential.data || coordinator->gateway.runtime || coordinator->gateway.worker.thread) {
        // Without the credential or a joined worker there is nothing safe to
        // restart; report a terminal Gateway failure (not a rejection).
        account_profile_release(&profile);
        set_failure(coordinator, UserAccountCoordinatorError::gateway, stopped.native_error);
        return coordinator->state;
    }
    // The previous session's resume state described a model this worker no
    // longer serves; a restart is always a fresh IDENTIFY.
    const GatewayClientResult started =
        gateway_client_start_user(&coordinator->gateway, &profile, configuration, nullptr, nullptr);
    account_profile_release(&profile);
    if (!started.ok() || !coordinator->gateway.runtime) {
        coordinator->state.gateway_error = started.error;
        set_failure(coordinator, UserAccountCoordinatorError::gateway, started.native_error);
        return coordinator->state;
    }
    coordinator->state.gateway_restart_at_ms = 0;
    if (coordinator->state.phase != UserAccountCoordinatorPhase::running)
        coordinator->state.phase = UserAccountCoordinatorPhase::gateway_connecting;
    return coordinator->state;
}

UserAccountCoordinatorResult user_account_coordinator_status(
    const UserAccountCoordinator* coordinator) noexcept {
    return coordinator ? coordinator->state : invalid_result();
}

bool user_account_coordinator_snapshot_session(const UserAccountCoordinator* coordinator,
                                               GatewayUserSessionSnapshot* snapshot) noexcept {
    if (!coordinator || !snapshot || coordinator->metadata.profile_id == 0 ||
        coordinator->state.phase == UserAccountCoordinatorPhase::empty ||
        coordinator->state.phase == UserAccountCoordinatorPhase::stopped) {
        return false;
    }
    return gateway_client_snapshot_user_session(&coordinator->gateway, snapshot);
}

UserAccountCoordinatorResult user_account_coordinator_stop(UserAccountCoordinator* coordinator,
                                                           u32 timeout_ms,
                                                           GatewayUserSessionSnapshot* snapshot) noexcept {
    if (!coordinator || timeout_ms == 0 || coordinator->state.phase == UserAccountCoordinatorPhase::empty ||
        coordinator->state.phase == UserAccountCoordinatorPhase::stopped) {
        return invalid_result();
    }
    coordinator->state.phase = UserAccountCoordinatorPhase::stopping;
    if (snapshot) {
        gateway_client_snapshot_user_session(&coordinator->gateway, snapshot);
    }
    AccountProfile returned{};
    const GatewayClientResult gateway_stopped =
        gateway_client_stop_return_user_profile(&coordinator->gateway, timeout_ms, &returned);
    const RestClientResult rest_stopped = coordinator->rest.worker.thread
                                              ? rest_client_stop(&coordinator->rest, timeout_ms)
                                              : RestClientResult{};
    if (coordinator->details.worker.thread)
        rest_client_stop(&coordinator->details, timeout_ms);
    account_profile_release(&returned);
    // Stop results retain the original transport failure even after successful
    // teardown. Ownership, rather than the old error, decides whether we can release.
    const bool stopped = !coordinator->gateway.runtime && !coordinator->gateway.worker.thread &&
                         !coordinator->rest.runtime && !coordinator->rest.worker.thread &&
                         !coordinator->details.runtime && !coordinator->details.worker.thread;
    if (!stopped) {
        coordinator->state.gateway_error = gateway_stopped.error;
        coordinator->state.rest_error = rest_stopped.error;
        set_failure(coordinator, UserAccountCoordinatorError::stop,
                    gateway_stopped.native_error != 0 ? gateway_stopped.native_error
                                                      : rest_stopped.native_error);
        return coordinator->state;
    }
    const u64 profile_id = coordinator->metadata.profile_id;
    secure_clear_bytes(coordinator, sizeof(*coordinator));
    coordinator->state.profile_id = profile_id;
    coordinator->state.phase = UserAccountCoordinatorPhase::stopped;
    return coordinator->state;
}

u32 user_account_coordinator_notification_handles(const UserAccountCoordinator* coordinator, HANDLE* output,
                                                  u32 capacity) noexcept {
    if (!coordinator || !output || capacity == 0) {
        return 0;
    }
    u32 count = 0;
    const HANDLE rest = rest_client_notification_handle(&coordinator->rest);
    if (rest) {
        output[count++] = rest;
    }
    const HANDLE details = rest_client_notification_handle(&coordinator->details);
    if (details && count < capacity)
        output[count++] = details;
    const HANDLE gateway = gateway_client_notification_handle(&coordinator->gateway);
    if (gateway && count < capacity) {
        output[count++] = gateway;
    }
    return count;
}

RestClient* user_account_coordinator_rest(UserAccountCoordinator* coordinator, bool details) noexcept {
    return coordinator && coordinator->rest.runtime &&
                   coordinator->state.phase == UserAccountCoordinatorPhase::running
               ? (details ? &coordinator->details : &coordinator->rest)
               : nullptr;
}

RestClient* user_account_coordinator_startup_rest(UserAccountCoordinator* coordinator) noexcept {
    return coordinator && coordinator->rest.runtime &&
                   (coordinator->state.phase == UserAccountCoordinatorPhase::gateway_connecting ||
                    coordinator->state.phase == UserAccountCoordinatorPhase::running)
               ? &coordinator->rest
               : nullptr;
}

RestClientTakeResult user_account_coordinator_take(UserAccountCoordinator* coordinator,
                                                   RestClientResponse* response, Slice<u8> body) noexcept {
    auto* rest = user_account_coordinator_startup_rest(coordinator);
    if (!rest)
        return {0, RestClientTakeError::invalid_input};
    auto result = rest_client_take_response(rest, response, body);
    return result.error == RestClientTakeError::empty && user_account_coordinator_rest(coordinator, true)
               ? rest_client_take_response(&coordinator->details, response, body)
               : result;
}

GatewayClient* user_account_coordinator_gateway(UserAccountCoordinator* coordinator) noexcept {
    return coordinator && coordinator->gateway.runtime ? &coordinator->gateway : nullptr;
}

} // namespace maicord
