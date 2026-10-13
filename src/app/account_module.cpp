#include "maicord/app/account_user_social.hpp"
#include "maicord/app/account_guild_community.hpp"
#include "maicord/app/account_guild_moderation.hpp"
#include "maicord/app/account_soundboard.hpp"
#include "maicord/app/account_voice_message.hpp"
#include "maicord/app/voice_log.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/app/account_module_api.hpp"
#include "maicord/app/account_relationships.hpp"
#include "maicord/app/account_message_ack.hpp"
#include "maicord/app/account_bulk_message_ack.hpp"
#include "maicord/app/account_self_status.hpp"
#include "maicord/app/account_thread_actions.hpp"
#include "maicord/app/account_thread_browser.hpp"
#include "maicord/app/account_message_poll.hpp"
#include "maicord/app/account_poll_create.hpp"
#include "maicord/app/account_notification_settings.hpp"
#include "maicord/app/account_private_channel.hpp"
#include "maicord/app/account_guild_invite.hpp"
#include "maicord/app/account_settings_rest.hpp"

#include "maicord/app/account_profile.hpp"
#include "maicord/app/account_profile_promotion.hpp"
#include "maicord/app/browser_login.hpp"
#include "maicord/app/account_session_store.hpp"
#include "maicord/app/account_media_worker.hpp"
#include "maicord/app/user_account_coordinator.hpp"
#include "maicord/app/user_client_configuration.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/protocol/attachment_refresh.hpp"
#include "application_command_request.hpp"
#include "account_login_handoff.hpp"
#include "account_login_captcha.hpp"

namespace maicord {
namespace {

inline constexpr u64 kSessionAutosaveIntervalMs = 30'000;
inline constexpr u64 kSessionAutosaveRetryMs = 5'000;
inline constexpr u64 kMessageHistoryRouteId = 0x4D53474849535452ull;

struct AccountDownloadOwner {
    HttpsFileDownload transfer;
    u64 id;
};
struct AccountModuleRuntime {
    UserAccountCoordinator coordinator;
    AccountMediaWorker media;
    HttpsFileUpload upload;
    u64 upload_id;
    AccountDownloadOwner* download;
    wchar_t session_path[kAccountProfileMaximumPathCharacters];
    u64 next_autosave_at_ms;
    u64 persisted_fingerprint;
    u32 session_saves;
    u32 session_save_skips;
    u32 session_save_failures;
    u32 session_native_error;
    AccountSessionStoreError session_error;
    bool has_persisted_fingerprint;
    u64 logout_request_id;
    bool logout_submitted;
    AccountActionCaptcha action_captcha;
    // Call video decoders; installed into the current gateway runtime.
    VideoStreamDecoders video;
    // Soundboard fetch/decode/cache and the effect mixer calls render.
    AccountSoundboard soundboard;
    // Voice message recording and preview.
    AccountVoiceMessage voice_message;
};

bool copy_path(const wchar_t* source, wchar_t* destination) noexcept;

GatewayClientSelfStatusSnapshot WINAPI module_self_status_snapshot(void* instance) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? account_self_status_snapshot(&runtime->coordinator.gateway)
                   : GatewayClientSelfStatusSnapshot{};
}

RestClientSubmitResult WINAPI module_self_status_submit(void* instance, u64 profile_id,
                                                        u64 self_user_id,
                                                        SelfStatusSettingsOperation operation,
                                                        GatewayUserStatus status, u64 request_id,
                                                        u64 unix_now_ms) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? account_self_status_submit(&runtime->coordinator.gateway, &runtime->coordinator.rest,
                                                profile_id, self_user_id, operation, status, request_id,
                                                unix_now_ms)
                   : RestClientSubmitResult{RestClientSubmitError::invalid_input};
}

bool WINAPI module_self_status_complete(void* instance, u64 profile_id, u64 self_user_id, u64 request_id,
                                        const RestClientResponse* response, Utf8View body) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && account_self_status_complete(&runtime->coordinator.gateway, profile_id, self_user_id,
                                                   request_id, response, body);
}

AccountModuleStatus module_failure(AccountModuleError error, u32 native_error = 0) noexcept {
    AccountModuleStatus status{};
    status.error = error;
    status.native_error = native_error;
    status.failed = true;
    return status;
}

AccountLoginStatus module_login_start_mode(void** instance, const wchar_t* profile_path,
                                           bool reset) noexcept {
    if (!instance || *instance || !profile_path) {
        AccountLoginStatus status{};
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    auto* runtime = static_cast<AccountLoginRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountLoginRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime) {
        AccountLoginStatus status{};
        status.error = AccountLoginError::memory;
        status.native_error = thread_last_error();
        return status;
    }
    if (!copy_path(profile_path, runtime->profile_path)) {
        SecureZeroMemory(runtime, sizeof(*runtime));
        VirtualFree(runtime, 0, MEM_RELEASE);
        AccountLoginStatus status{};
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    runtime->reset_mode = reset;
    *instance = runtime;
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_start(void** instance, const wchar_t* profile_path) noexcept {
    return module_login_start_mode(instance, profile_path, false);
}

AccountLoginStatus WINAPI module_login_start_reset(void** instance, const wchar_t* profile_path) noexcept {
    return module_login_start_mode(instance, profile_path, true);
}

bool ensure_user_auth_coordinator(AccountLoginRuntime* runtime) noexcept {
    if (runtime->coordinator_started) {
        return true;
    }
    const UserAuthCoordinatorResult started = runtime->reset_mode
                                                  ? user_auth_coordinator_start_reset(&runtime->coordinator)
                                                  : user_auth_coordinator_start(&runtime->coordinator);
    if (!started.ok()) {
        runtime->error = AccountLoginError::coordinator;
        return false;
    }
    runtime->coordinator_started = true;
    return true;
}

AccountLoginStatus WINAPI module_login_submit_password(void* instance, const SecureBuffer* login,
                                                       const SecureBuffer* password) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !login || !password || runtime->remote_mode || runtime->reset_mode ||
        runtime->browser_mode) {
        AccountLoginStatus status{};
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    if (!ensure_user_auth_coordinator(runtime)) {
        return login_status(runtime);
    }
    runtime->last_event = UserAuthCoordinatorEventKind::none;
    runtime->error_code = 0;
    runtime->mfa_capabilities = 0;
    const UserAuthCoordinatorResult submitted = user_auth_coordinator_submit_password(
        &runtime->coordinator, UserAuthPasswordInput{login, password, nullptr, false});
    if (submitted.ok())
        runtime->recovery_mode = false;
    if (!submitted.ok()) {
        runtime->error = AccountLoginError::coordinator;
    }
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_submit_recovery(void* instance, const SecureBuffer* login) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !login || runtime->remote_mode || runtime->reset_mode || runtime->browser_mode ||
        runtime->profile_saved || account_profile_promotion_has_credential(runtime->promotion)) {
        AccountLoginStatus status{};
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    if (!ensure_user_auth_coordinator(runtime))
        return login_status(runtime);
    const auto submitted = user_auth_coordinator_submit_recovery(&runtime->coordinator, login);
    runtime->error = submitted.ok() ? AccountLoginError::none : AccountLoginError::coordinator;
    if (submitted.ok()) {
        runtime->recovery_mode = true;
        runtime->last_event = UserAuthCoordinatorEventKind::none;
        runtime->error_code = 0;
        runtime->mfa_capabilities = 0;
        runtime->user_id = 0;
    }
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_start_browser(void* instance, u64 generation,
                                                     u32 timeout_ms) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !generation || runtime->browser_mode || runtime->remote_mode || runtime->reset_mode ||
        runtime->coordinator_started || runtime->profile_saved || timeout_ms < 1000 ||
        timeout_ms > 300000) {
        AccountLoginStatus status{};
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    if (!ensure_user_auth_coordinator(runtime))
        return login_status(runtime);
    runtime->browser_mode = true;
    if (!browser_login_start(&runtime->browser, generation, timeout_ms).ok())
        runtime->error = AccountLoginError::browser_auth;
    return login_status(runtime);
}

SecureBufferResult WINAPI module_login_copy_browser_url(void* instance, u64 generation,
                                                        SecureBuffer* output) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !runtime->browser_mode || runtime->error != AccountLoginError::none)
        return {0, ERROR_INVALID_STATE, SecureBufferError::invalid_input};
    return browser_sign_in_receiver_copy_url(&runtime->browser.receiver, generation, output);
}

AccountLoginStatus WINAPI module_login_submit_verification(void* instance,
                                                           const SecureBuffer* link_token) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !link_token || runtime->remote_mode || runtime->reset_mode || runtime->browser_mode) {
        AccountLoginStatus status{};
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    if (!ensure_user_auth_coordinator(runtime))
        return login_status(runtime);
    const UserAuthCoordinatorResult submitted =
        user_auth_coordinator_submit_verification(&runtime->coordinator, link_token);
    if (!submitted.ok()) {
        runtime->error = AccountLoginError::coordinator;
    } else {
        runtime->recovery_mode = false;
        runtime->last_event = UserAuthCoordinatorEventKind::none;
        runtime->error_code = 0;
        runtime->mfa_capabilities = 0;
        runtime->error = AccountLoginError::none;
    }
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_submit_phone_verify(void* instance,
                                                           const SecureBuffer* code) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !runtime->coordinator_started || !runtime->recovery_mode || runtime->remote_mode ||
        runtime->reset_mode || runtime->profile_saved ||
        account_profile_promotion_has_credential(runtime->promotion)) {
        AccountLoginStatus result{};
        result.error = AccountLoginError::invalid_input;
        return result;
    }
    const auto submitted = user_auth_coordinator_submit_phone_verify(&runtime->coordinator, code);
    runtime->error = submitted.ok() ? AccountLoginError::none : AccountLoginError::coordinator;
    if (submitted.ok()) {
        runtime->last_event = UserAuthCoordinatorEventKind::none;
        runtime->error_code = 0;
    }
    return login_status(runtime);
}
AccountLoginStatus WINAPI module_login_submit_phone_resend(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !runtime->coordinator_started || !runtime->recovery_mode || runtime->remote_mode ||
        runtime->reset_mode || runtime->profile_saved ||
        account_profile_promotion_has_credential(runtime->promotion)) {
        AccountLoginStatus result{};
        result.error = AccountLoginError::invalid_input;
        return result;
    }
    const auto submitted = user_auth_coordinator_submit_phone_resend(&runtime->coordinator);
    runtime->error = submitted.ok() ? AccountLoginError::none : AccountLoginError::coordinator;
    if (submitted.ok()) {
        runtime->last_event = UserAuthCoordinatorEventKind::none;
        runtime->error_code = 0;
    }
    return login_status(runtime);
}
SecureBufferResult WINAPI module_login_take_phone_reset_token(void* instance, u64 generation,
                                                              SecureBuffer* output) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !runtime->coordinator_started || !runtime->recovery_mode || runtime->remote_mode ||
        runtime->reset_mode || runtime->profile_saved ||
        account_profile_promotion_has_credential(runtime->promotion) || !output ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(output), sizeof(*output)},
                            {reinterpret_cast<const u8*>(runtime), sizeof(*runtime)}))
        return {0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    if (!byte_range_valid({output->data, output->capacity}) ||
        byte_ranges_overlap({output->data, output->capacity},
                            {reinterpret_cast<const u8*>(runtime), sizeof(*runtime)}))
        return {0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    return user_auth_coordinator_take_phone_reset_token(&runtime->coordinator, generation, output);
}

AccountLoginStatus WINAPI module_login_submit_mfa(void* instance, const SecureBuffer* code,
                                                  UserAuthMfaType type) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !code || runtime->remote_mode || !runtime->coordinator_started) {
        AccountLoginStatus status{};
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    runtime->last_event = UserAuthCoordinatorEventKind::none;
    runtime->error_code = 0;
    const UserAuthCoordinatorResult submitted =
        user_auth_coordinator_submit_mfa(&runtime->coordinator, UserAuthMfaInput{code, nullptr, type});
    if (!submitted.ok()) {
        runtime->error = AccountLoginError::coordinator;
    }
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_submit_reset(void* instance, const SecureBuffer* link_token,
                                                    const SecureBuffer* password) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !password || !runtime->reset_mode || runtime->remote_mode || runtime->profile_saved ||
        account_profile_promotion_has_credential(runtime->promotion)) {
        AccountLoginStatus invalid{};
        invalid.error = AccountLoginError::invalid_input;
        return invalid;
    }
    if (!ensure_user_auth_coordinator(runtime))
        return login_status(runtime);
    const auto submitted = user_auth_coordinator_submit_reset(&runtime->coordinator, link_token, password);
    runtime->error = submitted.ok() ? AccountLoginError::none : AccountLoginError::coordinator;
    if (submitted.ok()) {
        runtime->last_event = UserAuthCoordinatorEventKind::none;
        runtime->error_code = 0;
        runtime->mfa_capabilities = 0;
        runtime->user_id = 0;
    }
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_send_sms(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || runtime->remote_mode || !runtime->coordinator_started) {
        AccountLoginStatus status{};
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    runtime->last_event = UserAuthCoordinatorEventKind::none;
    const UserAuthCoordinatorResult submitted =
        user_auth_coordinator_submit_sms_send(&runtime->coordinator);
    if (!submitted.ok()) {
        runtime->error = AccountLoginError::coordinator;
    }
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_start_remote_auth(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || runtime->remote_mode || runtime->reset_mode || runtime->browser_mode ||
        runtime->profile_saved ||
        (runtime->coordinator_started &&
         user_auth_coordinator_status(&runtime->coordinator).phase != UserAuthCoordinatorPhase::idle)) {
        AccountLoginStatus status{};
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    if (runtime->coordinator_started) {
        const UserAuthCoordinatorResult stopped = user_auth_coordinator_stop(&runtime->coordinator, 5'000);
        if (!stopped.ok()) {
            AccountLoginStatus status{};
            status.error = AccountLoginError::stop;
            status.coordinator_error = stopped.error;
            status.native_error = stopped.native_error;
            return status;
        }
        runtime->coordinator = UserAuthCoordinator{};
        runtime->coordinator_started = false;
    }
    runtime->last_event = UserAuthCoordinatorEventKind::none;
    runtime->last_remote_event = RemoteAuthClientEventKind::none;
    runtime->remote_status = remote_auth_client_start(&runtime->remote_auth);
    runtime->remote_mode = true;
    runtime->remote_active = runtime->remote_status.ok();
    if (!runtime->remote_status.ok()) {
        runtime->error = AccountLoginError::remote_auth;
    }
    return login_status(runtime);
}

bool WINAPI module_login_copy_remote_auth_qr(void* instance, Slice<char> output, u32* bytes) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    return runtime && runtime->remote_mode && runtime->remote_active &&
           remote_auth_client_copy_qr_url(runtime->remote_auth, output, bytes);
}

bool WINAPI module_login_copy_remote_auth_user(void* instance, Slice<char> output, u32* bytes) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    return runtime && runtime->remote_mode && runtime->remote_active &&
           remote_auth_client_copy_user(runtime->remote_auth, output, bytes);
}

AccountLoginStatus WINAPI module_login_cancel_remote_auth(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !runtime->remote_mode || !runtime->remote_active) {
        AccountLoginStatus status{};
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    runtime->remote_status = remote_auth_client_cancel(&runtime->remote_auth);
    return login_status(runtime);
}

SecureBufferResult WINAPI module_login_copy_webauthn_challenge(void* instance,
                                                               SecureBuffer* output) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !output) {
        return SecureBufferResult{0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    }
    return runtime->coordinator_started
               ? user_auth_coordinator_copy_secret(
                     &runtime->coordinator, UserAuthCoordinatorSecretKind::webauthn_challenge, output)
               : SecureBufferResult{0, ERROR_INVALID_STATE, SecureBufferError::invalid_input};
}

bool copy_path(const wchar_t* source, wchar_t* destination) noexcept {
    u32 length = 0;
    return source && source[0] && destination &&
           wide_append(destination, kAccountProfileMaximumPathCharacters, &length, source);
}

template <typename T> u64 hash_value(u64 hash, const T& value) noexcept {
    return hash64_bytes(hash, &value, static_cast<u32>(sizeof(value)));
}

u64 snapshot_fingerprint(const GatewayUserSessionSnapshot& snapshot) noexcept {
    u64 hash = kHash64Seed;
    hash = hash_value(hash, snapshot.profile_id);
    hash = hash_value(hash, snapshot.self_user_id);
    hash = hash_value(hash, snapshot.has_resume);
    if (snapshot.has_resume) {
        hash = hash_value(hash, snapshot.resume.sequence);
        hash = hash_value(hash, snapshot.resume.session_id_bytes);
        hash = hash64_bytes(hash, snapshot.resume.session_id, snapshot.resume.session_id_bytes);
        hash = hash_value(hash, snapshot.resume.resume_url_bytes);
        hash = hash64_bytes(hash, snapshot.resume.resume_url, snapshot.resume.resume_url_bytes);
    }
    return hash;
}

bool snapshot_is_persistable(const GatewayUserSessionSnapshot& snapshot) noexcept {
    return snapshot.profile_id != 0 && snapshot.has_resume && gateway_resume_validate(snapshot.resume);
}

bool save_snapshot(AccountModuleRuntime* runtime, const GatewayUserSessionSnapshot& snapshot) noexcept {
    if (runtime->coordinator.state.error == UserAccountCoordinatorError::authentication_rejected ||
        !snapshot_is_persistable(snapshot)) {
        return true;
    }
    const u64 fingerprint = snapshot_fingerprint(snapshot);
    if (runtime->has_persisted_fingerprint && runtime->persisted_fingerprint == fingerprint) {
        ++runtime->session_save_skips;
        return true;
    }
    const AccountSessionStoreResult saved =
        account_user_session_store_save(runtime->session_path, snapshot);
    runtime->session_error = saved.error;
    runtime->session_native_error = saved.native_error;
    if (!saved.ok()) {
        ++runtime->session_save_failures;
        return false;
    }
    runtime->persisted_fingerprint = fingerprint;
    runtime->has_persisted_fingerprint = true;
    ++runtime->session_saves;
    return true;
}

void autosave_if_due(AccountModuleRuntime* runtime, u64 monotonic_now_ms) noexcept {
    if (monotonic_now_ms < runtime->next_autosave_at_ms) {
        return;
    }
    GatewayUserSessionSnapshot snapshot{};
    const bool captured = user_account_coordinator_snapshot_session(&runtime->coordinator, &snapshot);
    const bool saved = !captured || save_snapshot(runtime, snapshot);
    runtime->next_autosave_at_ms =
        deadline_after(monotonic_now_ms, saved ? kSessionAutosaveIntervalMs : kSessionAutosaveRetryMs);
}

AccountModuleStatus build_status(AccountModuleRuntime* runtime) noexcept {
    if (!runtime) {
        return module_failure(AccountModuleError::invalid_input);
    }
    const UserAccountCoordinatorResult coordinator = user_account_coordinator_status(&runtime->coordinator);
    AccountModuleStatus status{};
    status.profile_id = coordinator.profile_id;
    status.frecency_settings_revision =
        static_cast<u64>(ReadAcquire64(&runtime->coordinator.gateway.frecency_settings_revision));
    status.required_action = gateway_client_required_action(&runtime->coordinator.gateway);
    status.native_error = coordinator.native_error;
    status.gateway_sources = 1;
    status.gateway_ready = coordinator.phase == UserAccountCoordinatorPhase::running ? 1u : 0u;
    status.session_saves = runtime->session_saves;
    status.session_save_skips = runtime->session_save_skips;
    status.session_save_failures = runtime->session_save_failures;
    status.session_native_error = runtime->session_native_error;
    status.coordinator_phase = static_cast<u8>(coordinator.phase);
    status.coordinator_error = static_cast<u8>(coordinator.error);
    status.rest_error = static_cast<u8>(coordinator.rest_error);
    status.gateway_error = static_cast<u8>(coordinator.gateway_error);
    status.session_error = static_cast<u8>(runtime->session_error);
    status.running = coordinator.phase == UserAccountCoordinatorPhase::running;
    status.failed = coordinator.phase == UserAccountCoordinatorPhase::failed;
    // A failed Gateway worker restarts at this deadline (see the coordinator).
    if (coordinator.gateway_restart_at_ms)
        status.next_wake_at_ms = earliest_deadline(status.next_wake_at_ms, coordinator.gateway_restart_at_ms);
    status.error = status.failed ? AccountModuleError::coordinator : AccountModuleError::none;
    if (coordinator.phase != UserAccountCoordinatorPhase::failed &&
        coordinator.phase != UserAccountCoordinatorPhase::stopping &&
        coordinator.phase != UserAccountCoordinatorPhase::stopped) {
        status.next_wake_at_ms = earliest_deadline(status.next_wake_at_ms, runtime->next_autosave_at_ms);
        status.next_wake_at_ms = earliest_deadline(status.next_wake_at_ms,
                                                   account_media_worker_next_wake_at_ms(&runtime->media));
    }
    return status;
}

AccountModuleStartResult WINAPI module_start(const wchar_t* profile_path, const wchar_t* session_path,
                                             u64 monotonic_now_ms, u64 unix_now_ms) noexcept {
    AccountModuleStartResult result{};
    if (!profile_path || !session_path || unix_now_ms == 0) {
        result.error = AccountModuleError::invalid_input;
        return result;
    }
    auto* runtime = static_cast<AccountModuleRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountModuleRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime) {
        result.error = AccountModuleError::memory;
        result.native_error = thread_last_error();
        return result;
    }
    video_stream_decoders_initialize(&runtime->video);
    account_soundboard_initialize(&runtime->soundboard);
    account_voice_message_initialize(&runtime->voice_message);
    if (!copy_path(session_path, runtime->session_path)) {
        SecureZeroMemory(runtime, sizeof(*runtime));
        VirtualFree(runtime, 0, MEM_RELEASE);
        result.error = AccountModuleError::invalid_input;
        return result;
    }

    AccountProfile profile{};
    const AccountProfileResult loaded = account_profile_load(profile_path, &profile);
    if (!loaded.ok()) {
        SecureZeroMemory(runtime, sizeof(*runtime));
        VirtualFree(runtime, 0, MEM_RELEASE);
        result.error = AccountModuleError::profile;
        result.profile_error = static_cast<u8>(loaded.error);
        result.native_error = loaded.native_error;
        return result;
    }

    GatewayUserSessionSnapshot restored{};
    const AccountSessionStoreResult session =
        account_user_session_store_load(session_path, profile.metadata.profile_id, &restored);
    runtime->session_error = session.error;
    runtime->session_native_error = session.native_error;
    result.session_error = static_cast<u8>(session.error);
    result.session_loaded = session.ok();
    if (session.ok() && snapshot_is_persistable(restored)) {
        runtime->persisted_fingerprint = snapshot_fingerprint(restored);
        runtime->has_persisted_fingerprint = true;
    }

    const AccountMediaRequestResult media_started = account_media_worker_initialize(&runtime->media);
    if (!media_started.ok()) {
        account_profile_release(&profile);
        SecureZeroMemory(runtime, sizeof(*runtime));
        VirtualFree(runtime, 0, MEM_RELEASE);
        result.error = AccountModuleError::media_start;
        return result;
    }

    UserClientConfigurationStorage configuration{};
    const UserClientConfigurationResult configured = user_client_configuration_initialize(&configuration);
    if (!configured.ok()) {
        account_media_worker_stop(&runtime->media, 5'000);
        account_profile_release(&profile);
        SecureZeroMemory(runtime, sizeof(*runtime));
        VirtualFree(runtime, 0, MEM_RELEASE);
        result.error = AccountModuleError::coordinator_start;
        result.native_error = configured.native_error;
        return result;
    }
    // Saved desired status arrives with READY; avoid a transient Online publish
    // while an account's Invisible preference is still unknown to this process.
    configuration.configuration.status = GatewayUserStatus::invisible;
    const UserAccountCoordinatorResult started =
        user_account_coordinator_start(&runtime->coordinator, &profile, &configuration.configuration,
                                       // The persisted Gateway resume token is tied to its matching
                                       // guild/channel/message model. That model is not persisted, so a
                                       // cross-process RESUMED dispatch cannot reconstruct the UI. Keep
                                       // validating the session file, but identify as a fresh session.
                                       nullptr, {configuration.timezone, configuration.timezone_bytes});
    SecureZeroMemory(&configuration, sizeof(configuration));
    if (!started.ok()) {
        account_media_worker_stop(&runtime->media, 5'000);
        account_profile_release(&profile);
        SecureZeroMemory(runtime, sizeof(*runtime));
        VirtualFree(runtime, 0, MEM_RELEASE);
        result.error = AccountModuleError::coordinator_start;
        result.native_error = started.native_error;
        return result;
    }
    runtime->next_autosave_at_ms = deadline_after(monotonic_now_ms, kSessionAutosaveIntervalMs);
    result.instance = runtime;
    result.status = build_status(runtime);
    result.error = AccountModuleError::none;
    return result;
}

AccountModuleStatus WINAPI module_pump(void* instance, u64 monotonic_now_ms, u64 unix_now_ms) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || unix_now_ms == 0) {
        return module_failure(AccountModuleError::invalid_input);
    }
    user_account_coordinator_pump(&runtime->coordinator, monotonic_now_ms);
    if (user_account_coordinator_gateway_restart_due(&runtime->coordinator, monotonic_now_ms)) {
        // Same configuration as the first start; the saved status follows READY.
        UserClientConfigurationStorage configuration{};
        if (user_client_configuration_initialize(&configuration).ok()) {
            configuration.configuration.status = GatewayUserStatus::invisible;
            // Soundboard and video sinks re-attach to the new runtime on
            // their next pump/snapshot, as for any fresh session.
            user_account_coordinator_restart_gateway(&runtime->coordinator, &configuration.configuration,
                                                     monotonic_now_ms);
        } else {
            runtime->coordinator.state.gateway_restart_at_ms =
                monotonic_now_ms + user_account_coordinator_restart_delay(
                                       runtime->coordinator.state.gateway_restart_attempt);
        }
        SecureZeroMemory(&configuration, sizeof(configuration));
    }
    if (!account_media_worker_healthy(&runtime->media)) {
        return module_failure(AccountModuleError::media);
    }
    account_media_worker_expire_retries(&runtime->media, monotonic_now_ms);
    account_soundboard_pump(&runtime->soundboard, user_account_coordinator_gateway(&runtime->coordinator),
                            monotonic_now_ms);
    autosave_if_due(runtime, monotonic_now_ms);
    return build_status(runtime);
}

u32 WINAPI module_notification_handles(void* instance, HANDLE* output, u32 capacity) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || (!output && capacity != 0)) {
        return 0;
    }
    u32 count = user_account_coordinator_notification_handles(&runtime->coordinator, output, capacity);
    const HANDLE media = account_media_worker_notification_handle(&runtime->media);
    if (media && count < capacity) {
        output[count++] = media;
    }
    if (runtime->upload.notification && !runtime->upload.stop_requested && count < capacity)
        output[count++] = runtime->upload.notification;
    if (runtime->download && runtime->download->transfer.notification &&
        !runtime->download->transfer.stop_requested && count < capacity)
        output[count++] = runtime->download->transfer.notification;
    if (const HANDLE soundboard = account_soundboard_notification_handle(&runtime->soundboard);
        soundboard && count < capacity)
        output[count++] = soundboard;
    return count;
}

void* WINAPI module_gateway_source(void* instance, u32 source_index) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && source_index == 0 ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
}

GatewayClientPopResult WINAPI module_gateway_pop(void* source, GatewayClientEvent* event,
                                                 Slice<u8> payload_output) noexcept {
    return gateway_client_pop_event_payload(static_cast<GatewayClient*>(source), event, payload_output);
}

bool WINAPI module_guild_subscribe(void* instance, const GatewayGuildSubscription* subscription) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    GatewayClient* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    return gateway && subscription && gateway_client_subscribe_guild_members(gateway, *subscription);
}

GatewayStateRecoveryResult WINAPI module_gateway_refresh(void* instance) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return gateway_client_request_fresh_state(
        runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr);
}

RestClientSubmitResult WINAPI module_private_conversation(void* instance,
                                                          PrivateConversationOperation operation,
                                                          u64 id, u64 request_id) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    if (!rest || !request_id)
        return invalid;
    u8 target[96]{}, body[64]{};
    const auto written = private_conversation_write(operation, id, {target, 96}, {body, 64});
    if (!written.ok)
        return invalid;
    return rest_client_submit(rest, {target, written.target_bytes},
                              {body, written.body_bytes},
                              private_conversation_route(operation, id),
                              request_id,
                              operation == PrivateConversationOperation::open);
}

RestClientSubmitResult WINAPI module_relationships(void* instance,
                                                   RelationshipOperation operation,
                                                   u64 user_id, Utf8View username,
                                                   u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    return account_relationship_submit(rest, operation, user_id, username, request_id);
}

RestClientSubmitResult WINAPI module_history_submit(void* instance, u64 channel_id,
                                                    const MessageHistoryQuery* query,
                                                    u64 request_id) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    RestClient* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    if (!rest || !query || request_id == 0) {
        return invalid;
    }
    u8 target[128]{};
    const MessageHistoryTargetResult written =
        write_message_history_target(channel_id, *query, Slice<u8>{target, sizeof(target)});
    if (!written.ok()) {
        return invalid;
    }
    return rest_client_submit(rest, ConstSlice<u8>{target, written.bytes},
                              {},
                              RestRouteKey{kMessageHistoryRouteId, channel_id,
                                           RestMethod::get,
                                           RestMajorParameter::channel},
                              request_id,
                              false);
}

AccountUploadResult WINAPI module_upload(void* instance, AccountUploadOperation operation, u64 id,
                                         const AccountUploadRequest* request) noexcept {
    AccountUploadResult result{};
    result.id = id;
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !id) {
        result.submit_error = HttpsFileUploadError::invalid_input;
        result.released = !runtime || (!runtime->upload_id && !runtime->upload.runtime);
        return result;
    }
    result.owns_transfer = runtime->upload_id == id && runtime->upload.runtime;
    if (operation == AccountUploadOperation::start) {
        if (!request || runtime->upload_id || runtime->upload.runtime ||
            !user_account_coordinator_rest(&runtime->coordinator)) {
            result.submit_error = HttpsFileUploadError::invalid_input;
            result.released = !runtime->upload_id && !runtime->upload.runtime;
            result.owns_transfer = runtime->upload_id == id && runtime->upload.runtime;
            return result;
        }
        result.submit_error =
            https_file_upload_start(&runtime->upload, request->url, request->path, request->source.bytes,
                                    60 * 60 * 1000, &request->source.identity, request->content,
                                    request->content_type);
        if (result.submit_error == HttpsFileUploadError::none) {
            runtime->upload_id = id;
        } else {
            // A failed start can leave a partially started transport. Reap
            // without blocking, keeping its ID until release joins it.
            result.released = https_file_upload_release(&runtime->upload, 0);
            if (!result.released)
                runtime->upload_id = id;
        }
    } else {
        if (runtime->upload_id != id) {
            result.submit_error = HttpsFileUploadError::invalid_input;
            return result;
        }
        if (operation == AccountUploadOperation::cancel) {
            if (!https_file_upload_cancel(&runtime->upload))
                result.submit_error = HttpsFileUploadError::worker;
        } else if (operation == AccountUploadOperation::release) {
            result.released = https_file_upload_release(&runtime->upload, 0);
            if (result.released)
                runtime->upload_id = 0;
        } else if (operation != AccountUploadOperation::status) {
            result.submit_error = HttpsFileUploadError::invalid_input;
            return result;
        }
    }
    result.transfer = https_file_upload_status(&runtime->upload);
    result.owns_transfer = runtime->upload_id == id && runtime->upload.runtime;
    return result;
}

AccountDownloadResult WINAPI module_download(void* instance, AccountDownloadOperation operation, u64 id,
                                             const AccountDownloadRequest* request) noexcept {
    AccountDownloadResult result{};
    result.id = id;
    // A failed start either leaves nothing running or returns a handle that must be reaped.
    // Report that distinction even for failures before allocation.
    result.released = operation == AccountDownloadOperation::start;
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !id) {
        result.submit_error = HttpsFileDownloadError::invalid_input;
        return result;
    }
    if (operation == AccountDownloadOperation::start) {
        if (!request || runtime->download || !user_account_coordinator_rest(&runtime->coordinator)) {
            result.submit_error = HttpsFileDownloadError::invalid_input;
            return result;
        }
        auto* owner = static_cast<AccountDownloadOwner*>(
            VirtualAlloc(nullptr, sizeof(AccountDownloadOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!owner) {
            result.submit_error = HttpsFileDownloadError::memory;
            return result;
        }
        owner->id = id;
        runtime->download = owner;
        result.released = false;
        result.submit_error = https_file_download_start(&owner->transfer, *request);
        if (result.submit_error != HttpsFileDownloadError::none &&
            https_file_download_release(&owner->transfer, 0)) {
            runtime->download = nullptr;
            VirtualFree(owner, 0, MEM_RELEASE);
            result.released = true;
            return result;
        }
    } else {
        auto* owner = runtime->download;
        if (!owner || owner->id != id) {
            result.submit_error = HttpsFileDownloadError::invalid_input;
            return result;
        }
        if (operation == AccountDownloadOperation::cancel) {
            if (!https_file_download_cancel(&owner->transfer))
                result.submit_error = HttpsFileDownloadError::worker;
        } else if (operation == AccountDownloadOperation::release) {
            result.released = https_file_download_release(&owner->transfer, 0);
            if (result.released) {
                runtime->download = nullptr;
                VirtualFree(owner, 0, MEM_RELEASE);
                return result;
            }
        } else if (operation != AccountDownloadOperation::status) {
            result.submit_error = HttpsFileDownloadError::invalid_input;
            return result;
        }
    }
    auto& transfer = runtime->download->transfer;
    if (transfer.notification)
        ResetEvent(transfer.notification);
    result.transfer = https_file_download_status(&transfer);
    return result;
}

RestClientSubmitResult WINAPI module_attachment_upload(void* instance, u64 channel_id, u32 id,
                                                       Utf8View filename, u64 file_bytes,
                                                       u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    RestClient* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    if (!rest || !request_id)
        return {RestClientSubmitError::invalid_input};
    u8 target[96]{}, body[8 * 1024]{};
    const auto written = attachment_upload_write(channel_id, id, filename, file_bytes,
                                                 {target, sizeof(target)}, {body, sizeof(body)});
    if (!written.ok)
        return {RestClientSubmitError::invalid_input};
    const auto result = rest_client_submit(rest, {target, written.target_bytes},
                                           {body, written.body_bytes},
                                           attachment_upload_route(channel_id),
                                           request_id,
                                           true);
    SecureZeroMemory(body, sizeof(body));
    return result;
}

RestClientSubmitResult WINAPI
module_message_submit(void* instance, u64 channel_id, Utf8View content, u64 nonce, u64 reply_message_id,
                      ConstSlice<UploadedAttachment> attachments, ConstSlice<u64> sticker_ids,
                      u64 request_id, bool mention_reply_author, u32 flags) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    RestClient* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    if (!rest || channel_id == 0 || nonce == 0 || request_id == 0) {
        return invalid;
    }
    u8 target[96]{};
    u8 body[kMessageCreateMaximumBodyBytes]{};
    const MessageCreateWriteResult target_written =
        write_message_create_target(channel_id, Slice<u8>{target, sizeof(target)});
    const MessageCreateWriteResult body_written = write_message_create_body(
        content, nonce, reply_message_id ? channel_id : 0, reply_message_id, attachments, sticker_ids,
        {body, sizeof(body)}, mention_reply_author, flags);
    if (!target_written.ok() || !body_written.ok()) {
        SecureZeroMemory(body, sizeof(body));
        return invalid;
    }
    const RestClientSubmitResult submitted = rest_client_submit(
        rest, ConstSlice<u8>{target, target_written.bytes}, ConstSlice<u8>{body, body_written.bytes},
        RestRouteKey{kMessageCreateRouteId, channel_id, RestMethod::post,
                     RestMajorParameter::channel},
        request_id, true);
    SecureZeroMemory(body, sizeof(body));
    return submitted;
}

RestClientSubmitResult WINAPI module_message_forward(void* instance, u64 destination_channel_id,
                                                     Utf8View caption, u64 nonce,
                                                     MessageForwardReference source,
                                                     u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    RestClient* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    if (!rest || !destination_channel_id || !nonce || !request_id)
        return {RestClientSubmitError::invalid_input};
    u8 target[96]{};
    u8 body[kMessageCreateMaximumBodyBytes]{};
    const auto target_written =
        write_message_create_target(destination_channel_id, {target, sizeof(target)});
    const auto body_written =
        write_message_forward_body(caption, nonce, source, {body, sizeof(body)});
    if (!target_written.ok() || !body_written.ok()) {
        SecureZeroMemory(body, sizeof(body));
        return {RestClientSubmitError::invalid_input};
    }
    const auto submitted = rest_client_submit(
        rest, {target, target_written.bytes},
        {body, body_written.bytes},
        {kMessageCreateRouteId, destination_channel_id, RestMethod::post,
         RestMajorParameter::channel},
        request_id,
        true);
    SecureZeroMemory(body, sizeof(body));
    return submitted;
}

RestClientSubmitResult WINAPI module_message_delete(void* instance, u64 channel_id, u64 message_id,
                                                    u64 request_id) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    RestClient* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    if (!rest || channel_id == 0 || message_id == 0 || request_id == 0) {
        return invalid;
    }
    u8 target[128]{};
    const MessageDeleteWriteResult written =
        write_message_delete_target(channel_id, message_id, Slice<u8>{target, sizeof(target)});
    if (!written.ok()) {
        return invalid;
    }
    return rest_client_submit(rest, ConstSlice<u8>{target, written.bytes},
                              {},
                              RestRouteKey{kMessageDeleteRouteId, channel_id,
                                           RestMethod::delete_method,
                                           RestMajorParameter::channel},
                              request_id,
                              false);
}

RestClientSubmitResult WINAPI module_message_edit(void* instance, u64 channel_id, u64 message_id,
                                                  Utf8View content, u64 request_id) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    RestClient* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    if (!rest || channel_id == 0 || message_id == 0 || (!content.data && content.bytes != 0) ||
        request_id == 0) {
        return invalid;
    }
    u8 target[128]{};
    u8 body[kMessageEditMaximumBodyBytes]{};
    const MessageEditWriteResult target_written =
        write_message_edit_target(channel_id, message_id, Slice<u8>{target, sizeof(target)});
    const MessageEditWriteResult body_written =
        write_message_edit_body(content, Slice<u8>{body, sizeof(body)});
    if (!target_written.ok() || !body_written.ok()) {
        SecureZeroMemory(body, sizeof(body));
        return invalid;
    }
    const RestClientSubmitResult submitted = rest_client_submit(
        rest, ConstSlice<u8>{target, target_written.bytes}, ConstSlice<u8>{body, body_written.bytes},
        RestRouteKey{kMessageEditRouteId, channel_id, RestMethod::patch,
                     RestMajorParameter::channel},
        request_id, true);
    SecureZeroMemory(body, sizeof(body));
    return submitted;
}

RestClientSubmitResult WINAPI module_message_reaction(void* instance, u64 channel_id, u64 message_id,
                                                      u64 emoji_id, Utf8View emoji_name,
                                                      MessageReactionType type, bool add,
                                                      u64 request_id) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    RestClient* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    if (!rest || channel_id == 0 || message_id == 0 || request_id == 0) {
        return invalid;
    }
    u8 target[kMessageReactionMaximumTargetBytes]{};
    const MessageReactionWriteResult written = write_message_reaction_target(
        channel_id, message_id, emoji_id, emoji_name, type, add, Slice<u8>{target, sizeof(target)});
    if (!written.ok()) {
        return invalid;
    }
    return rest_client_submit(rest, ConstSlice<u8>{target, written.bytes},
                              {},
                              RestRouteKey{kMessageReactionRouteId, channel_id,
                                           add ? RestMethod::put
                                                              : RestMethod::delete_method,
                                           RestMajorParameter::channel},
                              request_id,
                              false);
}

RestClientSubmitResult WINAPI module_application_command(void* instance, u64 channel, u64 guild,
                                                         u64 application, u64 nonce, Utf8View data,
                                                         u64 request_id, u8 operation) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator, true) : nullptr;
    if (!rest || !request_id || operation > 3)
        return invalid;
    u8 target[160]{};
    ProtocolWriter path{{target, sizeof(target)}, 0};
    if (operation <= 1) {
        if (operation == 1) {
            if (!protocol_writer_append_literal(&path, "/api/v10/users/@me/application-command-index"))
                return invalid;
        } else if (!channel || !protocol_writer_append_literal(&path, "/api/v10/") ||
                   !protocol_writer_append_c_string(&path, guild ? "guilds/" : "channels/") ||
                   !protocol_writer_append_u64(&path, guild ? guild : channel) ||
                   !protocol_writer_append_literal(&path, "/application-command-index"))
            return invalid;
        return rest_client_submit(
            rest, {target, path.used},
            {},
            {0x434d4449, guild ? guild : channel, RestMethod::get,
             guild ? RestMajorParameter::guild : RestMajorParameter::channel},
            request_id,
            false);
    }
    if (!channel || !application || !nonce || !data.bytes || !utf8_validate(data).valid)
        return invalid;
    GatewayUserSessionSnapshot snapshot{};
    if (!user_account_coordinator_snapshot_session(&runtime->coordinator, &snapshot) ||
        !snapshot.has_resume)
        return invalid;
    Arena storage{};
    const u32 capacity = data.bytes <= ~0u - 1024 ? data.bytes + 1024 : 0;
    if (!capacity || !arena_initialize(&storage, capacity, 0, 4096))
        return invalid;
    u8* bytes = arena_allocate<u8>(&storage, capacity);
    ProtocolWriter body{{bytes, capacity}, 0};
    bool ok = bytes && protocol_writer_append_literal(&body, "{\"type\":") &&
              protocol_writer_append_u64(&body, operation == 3 ? 4 : 2) &&
              protocol_writer_append_literal(&body, ",\"application_id\":\"") &&
              protocol_writer_append_u64(&body, application) &&
              protocol_writer_append_literal(&body, "\",\"channel_id\":\"") &&
              protocol_writer_append_u64(&body, channel) &&
              protocol_writer_append_literal(&body, "\",\"nonce\":\"") &&
              protocol_writer_append_u64(&body, nonce) &&
              protocol_writer_append_literal(&body, "\",\"session_id\":") &&
              protocol_writer_append_json_string(
                  &body, {snapshot.resume.session_id, snapshot.resume.session_id_bytes});
    if (guild)
        ok = ok && protocol_writer_append_literal(&body, ",\"guild_id\":\"") &&
             protocol_writer_append_u64(&body, guild) && protocol_writer_append_byte(&body, '"');
    ok = ok && protocol_writer_append_literal(&body, ",\"data\":") &&
         protocol_writer_append_bytes(&body, {reinterpret_cast<const u8*>(data.data), data.bytes}) &&
         protocol_writer_append_byte(&body, '}');
    auto result = invalid;
    if (ok) {
        result = rest_client_submit(rest, app_detail::application_command_interaction_request(
                                                      {bytes, body.used}, request_id, operation == 3));
    }
    arena_release(&storage);
    return result;
}

RestClientSubmitResult WINAPI module_message_interaction(void* instance,
                                                         const MessageInteractionRequest* request,
                                                         u64 request_id) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator, true) : nullptr;
    if (!rest || !request || !request_id || !message_interaction_request_valid(*request))
        return invalid;
    GatewayUserSessionSnapshot snapshot{};
    if (!user_account_coordinator_snapshot_session(&runtime->coordinator, &snapshot) || !snapshot.has_resume)
        return invalid;
    Arena storage{};
    const u32 capacity = request->data_bytes + 1024u;
    if (!arena_initialize(&storage, capacity, 0, 4096))
        return invalid;
    u8* bytes = arena_allocate<u8>(&storage, capacity);
    ProtocolWriter body{{bytes, capacity}, 0};
    auto result = invalid;
    if (bytes && message_interaction_write_body(
                     *request, {snapshot.resume.session_id, snapshot.resume.session_id_bytes}, &body))
        result = rest_client_submit(
            rest, app_detail::application_command_interaction_request({bytes, body.used}, request_id, false));
    arena_release(&storage);
    return result;
}

RestClientSubmitResult WINAPI module_profile_get(void* instance, u64 user, u64 guild, u64 request_id,
                                                 u8 operation, Utf8View value) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator, true) : nullptr;
    if (!rest || !user || !request_id)
        return invalid;
    if (operation == 4) {
        u8 target[96];
        ProtocolWriter writer{{target, sizeof(target)}, 0};
        if (!protocol_writer_append_literal(&writer, "/api/v10/collectibles-products/") ||
            !protocol_writer_append_u64(&writer, user))
            return invalid;
        return rest_client_submit(
            rest, {target, writer.used},
            {},
            {0x50524546, 0, RestMethod::get, RestMajorParameter::none},
            request_id,
            false);
    }
    if (operation) {
        if (operation > 3 || !utf8_validate(value).valid || value.bytes > (operation == 1 ? 1024u : 128u))
            return invalid;
        u8 body[6200];
        ProtocolWriter writer{{body, sizeof(body)}, 0};
        const char* field = operation == 1   ? "{\"bio\":"
                            : operation == 2 ? "{\"pronouns\":"
                                             : "{\"global_name\":";
        if (!protocol_writer_append_c_string(&writer, field) ||
            !protocol_writer_append_json_string(&writer, value) ||
            !protocol_writer_append_byte(&writer, '}'))
            return invalid;
        const auto target = operation == 3 ? utf8_literal("/api/v10/users/@me")
                                           : utf8_literal("/api/v10/users/@me/profile");
        return rest_client_submit(
            rest, {reinterpret_cast<const u8*>(target.data), target.bytes},
            {body, writer.used},
            {0x5052464d, 0, RestMethod::patch, RestMajorParameter::none},
            request_id,
            false);
    }
    u8 target[256]{};
    ProtocolWriter writer{{target, sizeof(target)}, 0};
    if (!protocol_writer_append_literal(&writer, "/api/v10/users/") ||
        !protocol_writer_append_u64(&writer, user) ||
        !protocol_writer_append_literal(&writer, "/profile?with_mutual_guilds=true&with_mutual_friends=true"
                                                 "&with_mutual_friends_count=true"))
        return invalid;
    if (guild && (!protocol_writer_append_literal(&writer, "&guild_id=") ||
                  !protocol_writer_append_u64(&writer, guild)))
        return invalid;
    return rest_client_submit(
        rest, {target, writer.used},
        {},
        {0x5052464c, 0, RestMethod::get, RestMajorParameter::none},
        request_id,
        false);
}

RestClientSubmitResult WINAPI module_reaction_users(void* instance, u64 channel_id, u64 message_id,
                                                    u64 emoji_id, Utf8View emoji_name,
                                                    MessageReactionType type,
                                                    u64 after_user_id, u32 limit,
                                                    u64 request_id) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    RestClient* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    if (!rest || channel_id == 0 || message_id == 0 || request_id == 0) {
        return invalid;
    }
    u8 target[kReactionUsersMaximumTargetBytes]{};
    const ReactionUsersWriteResult written =
        write_reaction_users_target(channel_id, message_id, emoji_id, emoji_name, type,
                                    after_user_id, limit, Slice<u8>{target, sizeof(target)});
    if (!written.ok()) {
        return invalid;
    }
    return rest_client_submit(rest, ConstSlice<u8>{target, written.bytes},
                              {},
                              RestRouteKey{kReactionUsersRouteId, channel_id,
                                           RestMethod::get,
                                           RestMajorParameter::channel},
                              request_id,
                              false);
}

RestClientSubmitResult WINAPI module_message_typing(void* instance, u64 channel_id,
                                                    u64 request_id) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    RestClient* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    if (!rest || channel_id == 0 || request_id == 0) {
        return invalid;
    }
    u8 target[kMessageTypingMaximumTargetBytes]{};
    const MessageTypingWriteResult written =
        write_message_typing_target(channel_id, Slice<u8>{target, sizeof(target)});
    if (!written.ok()) {
        return invalid;
    }
    return rest_client_submit(rest, ConstSlice<u8>{target, written.bytes},
                              {},
                              RestRouteKey{kMessageTypingRouteId, channel_id,
                                           RestMethod::post,
                                           RestMajorParameter::channel},
                              request_id,
                              false);
}

RestClientSubmitResult WINAPI module_message_ack(void* instance, u64 channel_id, u64 message_id,
                                                 ConstSlice<u8> body, u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return account_message_ack_submit(runtime ? user_account_coordinator_rest(&runtime->coordinator)
                                              : nullptr,
                                      channel_id, message_id, body, request_id);
}

RestClientSubmitResult WINAPI module_bulk_message_ack(void* instance,
                                                      const BulkMessageAckEntry* entries,
                                                      u32 count, u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return account_bulk_message_ack_submit(runtime ? user_account_coordinator_rest(&runtime->coordinator)
                                                   : nullptr,
                                           entries, count, request_id);
}

RestClientSubmitResult WINAPI module_media_catalog(void* instance,
                                                   AccountMediaCatalogOperation operation,
                                                   const MediaCatalogQuery* query,
                                                   u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return account_media_catalog(runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr,
                                 operation, query, request_id);
}

bool WINAPI module_guild_members_request(void* instance,
                                         const GatewayGuildMemberRequest* request) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    if (!gateway)
        return false;
    if (!request) {
        gateway_client_cancel_guild_member_request(gateway);
        return true;
    }
    return gateway_client_request_guild_members(gateway, *request);
}
RestClientSubmitResult WINAPI module_gif_favorites(void* instance,
                                                   AccountGifFavoritesOperation operation,
                                                   ConstSlice<u8> patch_json,
                                                   u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return account_gif_favorites(runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr,
                                 operation, patch_json, request_id);
}
RestClientSubmitResult WINAPI module_emoji_preferences(void* instance,
                                                       AccountGifFavoritesOperation operation,
                                                       ConstSlice<u8> patch_json,
                                                       u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return account_emoji_preferences(runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr,
                                     operation, patch_json, request_id);
}
RestClientSubmitResult WINAPI module_account_settings(void* instance, const AccountSettingsRequest* request,
                                                            u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (request && !account_settings_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    // Settings share the PRIMARY lane with the self-status settings writer;
    // a cancel without a request tries both lanes.
    if (!request) {
        const auto primary = account_settings_cancel(user_account_coordinator_rest(&runtime->coordinator), request_id);
        return primary.error == RestClientSubmitError::none
                   ? primary
                   : account_settings_cancel(user_account_coordinator_rest(&runtime->coordinator, true), request_id);
    }
    return account_settings_submit(
        user_account_coordinator_rest(&runtime->coordinator, !account_settings_primary(request->operation)), *request,
        request_id);
}
RestClientSubmitResult WINAPI module_pinned_messages(void* instance,
                                                     const MessagePinsRequest* request,
                                                     u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator, true) : nullptr;
    return request ? account_pinned_messages_submit(rest, request, request_id)
                   : account_pinned_messages_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_channel_overview(void* instance,
                                                      ChannelOverviewOperation operation,
                                                      u64 channel_id,
                                                      const ChannelOverviewPatch* patch,
                                                      u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return account_channel_overview_submit(
        runtime ? user_account_coordinator_rest(&runtime->coordinator, true) : nullptr, operation,
        channel_id, patch, request_id);
}
RestClientSubmitResult WINAPI module_thread_action(void* instance,
                                                   const ThreadActionRequest* request,
                                                   u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime)
        return {RestClientSubmitError::invalid_input};
    if (request) {
        const auto identity = gateway_client_self_status_snapshot(&runtime->coordinator.gateway);
        if (!identity.self_user_id || request->self_user_id != identity.self_user_id ||
            identity.profile_id != runtime->coordinator.metadata.profile_id)
            return {RestClientSubmitError::invalid_input};
    }
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return request ? account_thread_action_submit(rest, request, request_id)
                   : account_thread_action_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_thread_browser(void* instance,
                                                    const ThreadBrowserRequest* request,
                                                    u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (request && !thread_browser_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return request ? account_thread_browser_submit(rest, request, request_id)
                   : account_thread_browser_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_thread_creation(void* instance,
                                                     const ThreadCreationRequest* request,
                                                     u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (request && !thread_creation_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    if (request) {
        const auto identity = gateway_client_self_status_snapshot(&runtime->coordinator.gateway);
        if (!identity.self_user_id || identity.profile_id != runtime->coordinator.metadata.profile_id)
            return {RestClientSubmitError::invalid_input};
    }
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return request ? account_thread_creation_submit(rest, request, request_id)
                   : account_thread_creation_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_poll_create(void* instance, u64 channel_id, u64 nonce,
                                                 const PollCreate* poll, u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (poll && !poll_create_valid(*poll)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return poll ? account_poll_create_submit(rest, channel_id, nonce, poll, request_id)
                : account_poll_create_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_private_channel(void* instance, const PrivateChannelActionRequest* request,
                                                     u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (request && !private_channel_action_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return request ? account_private_channel_submit(rest, request, request_id)
                   : account_private_channel_cancel(rest, request_id);
}
bool WINAPI module_local_presence(void* instance, const GatewayLocalPresence* input,
                                  GatewayActivitySharing* output) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime) {
        if (output)
            *output = {};
        return false;
    }
    return gateway_client_local_presence(&runtime->coordinator.gateway, input, output);
}
RestClientSubmitResult WINAPI module_activity_sharing(void* instance, u64 profile_id, u64 self_user_id,
                                                      bool share, u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? gateway_client_activity_sharing_submit(&runtime->coordinator.gateway, &runtime->coordinator.rest,
                                                            profile_id, self_user_id, share, request_id)
                   : RestClientSubmitResult{RestClientSubmitError::invalid_input};
}
bool WINAPI module_custom_status_snapshot(void* instance, SelfCustomStatus* output) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && gateway_client_custom_status_snapshot(&runtime->coordinator.gateway, output);
}
RestClientSubmitResult WINAPI module_custom_status_submit(void* instance, u64 profile_id, u64 self_user_id,
                                                          const SelfCustomStatus* custom,
                                                          u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? gateway_client_custom_status_submit(&runtime->coordinator.gateway, &runtime->coordinator.rest,
                                                         profile_id, self_user_id, custom, request_id)
                   : RestClientSubmitResult{RestClientSubmitError::invalid_input};
}
RestClientSubmitResult WINAPI module_guild_settings(void* instance, const UserGuildSettingsPatch* patch,
                                                    u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (patch && !user_guild_settings_patch_valid(*patch)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return patch ? account_guild_settings_submit(rest, patch, request_id)
                 : account_notification_request_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_mentions(void* instance, const MentionsQuery* query,
                                              u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (query && !mentions_query_valid(*query)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return query ? account_mentions_submit(rest, query, request_id)
                 : account_notification_request_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_forum_post(void* instance,
                                                const ForumPostRequest* request,
                                                u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (request && !forum_post_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    if (request) {
        const auto identity = gateway_client_self_status_snapshot(&runtime->coordinator.gateway);
        if (!identity.available || !identity.self_user_id || !identity.profile_id ||
            identity.profile_id != runtime->coordinator.metadata.profile_id)
            return {RestClientSubmitError::invalid_input};
    }
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return request ? account_forum_post_submit(rest, request, request_id)
                   : account_forum_post_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_poll(void* instance, const PollRequest* request,
                                          u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id)
        return {RestClientSubmitError::invalid_input};
    if (request) {
        const auto identity = gateway_client_self_status_snapshot(&runtime->coordinator.gateway);
        if (!identity.available || !identity.self_user_id || !identity.profile_id ||
            identity.profile_id != runtime->coordinator.metadata.profile_id)
            return {RestClientSubmitError::invalid_input};
    }
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return request ? account_message_poll_submit(rest, request, request_id)
                   : account_message_poll_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_guild_admin(void* instance, const GuildAdminRequest* request,
                                                 u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (request && !guild_admin_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return request ? account_guild_admin_submit(rest, request, request_id)
                   : account_guild_admin_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_guild_moderation(void* instance, const GuildModerationRequest* request,
                                                      u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (request && !guild_moderation_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return request ? account_guild_moderation_submit(rest, request, request_id)
                   : account_guild_moderation_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_guild_assets(void* instance, const GuildAssetRequest* request,
                                                  u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || (request && !guild_asset_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    if (request && request->operation == GuildAssetOperation::image_probe)
        return {account_guild_assets_probe(request) ? RestClientSubmitError::none
                                                    : RestClientSubmitError::invalid_input};
    if (!request_id)
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return request ? account_guild_assets_submit(rest, request, request_id)
                   : account_guild_assets_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_profile_edit(void* instance, const ProfileEditRequest* request,
                                                  u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (request && !profile_edit_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    return request ? account_profile_edit_submit(rest, request, request_id)
                   : account_profile_edit_cancel(rest, request_id);
}
bool WINAPI module_profile_image(void* instance, ProfileImageRequest* request) noexcept {
    return instance && account_profile_image(request);
}
void bind_video_keyframes(void* context, VideoKeyframeRequester requester) noexcept {
    video_stream_decoders_set_requester(static_cast<VideoStreamDecoders*>(context), requester);
}
// Installs the decoders into the gateway runtime that currently exists (a new
// session allocates a new runtime that starts without a sink).
void ensure_video_frames(AccountModuleRuntime* runtime, GatewayClient* gateway) noexcept {
    if (!gateway || gateway_client_has_video_frames(gateway, &runtime->video))
        return;
    gateway_client_set_video_frames(
        gateway, {video_stream_decoders_sink(&runtime->video), &runtime->video, bind_video_keyframes});
}
bool WINAPI module_voice_request(void* instance, const GatewayVoiceStateUpdate* state) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    return gateway && state && gateway_client_request_voice_state(gateway, *state);
}
bool WINAPI module_voice_snapshot(void* instance, GatewayClientVoiceSnapshot* output) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    if (gateway)
        ensure_video_frames(runtime, gateway);
    return gateway && gateway_client_voice_snapshot(gateway, output);
}
bool WINAPI module_voice_video_snapshot(void* instance, GatewayClientVoiceVideoSnapshot* output) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    if (gateway)
        ensure_video_frames(runtime, gateway);
    return gateway && gateway_client_voice_video_snapshot(gateway, output);
}
bool WINAPI module_voice_video_device(void* instance, ID3D11Device* device) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime)
        return false;
    video_stream_decoders_set_device(&runtime->video, device);
    ensure_video_frames(runtime, user_account_coordinator_gateway(&runtime->coordinator));
    return true;
}
bool WINAPI module_voice_video_frame(void* instance, u32 ssrc, VideoStreamFrame* output) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !output)
        return false;
    ensure_video_frames(runtime, user_account_coordinator_gateway(&runtime->coordinator));
    return video_stream_decoders_frame(&runtime->video, ssrc, output);
}
bool WINAPI module_voice_video_wanted(void* instance, u64 user_id, bool wanted) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    return gateway && gateway_client_voice_video_wanted(gateway, user_id, wanted);
}
bool WINAPI module_stream_watch(void* instance, const GatewayStreamKey* key) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    if (!gateway)
        return false;
    // Stream video goes to the same decoders as call video.
    ensure_video_frames(runtime, gateway);
    return gateway_client_stream_watch(gateway, key ? *key : GatewayStreamKey{});
}
bool WINAPI module_stream_snapshot(void* instance, GatewayClientStreamSnapshot* output) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    return gateway && gateway_client_stream_snapshot(gateway, output);
}
bool WINAPI module_stream_volume(void* instance, u32 percent) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    return gateway && gateway_client_stream_volume(gateway, percent);
}
RestClientSubmitResult WINAPI module_stream_preview(void* instance, const GatewayStreamKey* key,
                                                    u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (key && !gateway_stream_key_valid(*key)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    if (!rest)
        return {RestClientSubmitError::invalid_input};
    if (!key)
        return rest_client_cancel_result(rest, request_id);
    // version busts intermediate caches, as the official client's Date.now().
    u8 target[kGatewayStreamPreviewMaximumTargetBytes];
    const u32 bytes = gateway_stream_preview_target(*key, clock_unix_milliseconds(), {target, sizeof(target)});
    if (!bytes)
        return {RestClientSubmitError::invalid_input};
    return rest_client_submit(
        rest, {target, bytes}, {},
        {kGatewayStreamPreviewRouteId, 0, RestMethod::get, RestMajorParameter::none},
        request_id, false);
}
RestClientSubmitResult WINAPI module_stage(void* instance, const StageRequest* request,
                                           u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (request && !stage_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    if (!rest)
        return {RestClientSubmitError::invalid_input};
    return request ? account_stage_submit(rest, request, request_id) : account_stage_cancel(rest, request_id);
}
RestClientSubmitResult WINAPI module_guild_community(void* instance, const GuildCommunityRequest* request,
                                                     u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (request && !guild_community_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    if (!rest)
        return {RestClientSubmitError::invalid_input};
    if (!request)
        return account_guild_community_cancel(rest, request_id);
    // A lurking join names this Gateway session, like the official client.
    GatewayUserSessionSnapshot session{};
    const bool known = request->operation == GuildCommunityOperation::guild_join &&
                       user_account_coordinator_snapshot_session(&runtime->coordinator, &session) &&
                       session.has_resume;
    const auto result = account_guild_community_submit(
        rest, request, request_id,
        known ? Utf8View{session.resume.session_id, session.resume.session_id_bytes} : Utf8View{});
    SecureZeroMemory(&session, sizeof(session));
    return result;
}
RestClientSubmitResult WINAPI module_user_social(void* instance, const UserSocialRequest* request,
                                                 u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (request && !user_social_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    if (!rest)
        return {RestClientSubmitError::invalid_input};
    return request ? account_user_social_submit(rest, request, request_id)
                   : account_user_social_cancel(rest, request_id);
}
bool WINAPI module_voice_user_audio(void* instance, u64 user_id, u32 gain_percent, bool muted) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    return gateway && gateway_client_voice_user_audio(gateway, user_id, gain_percent, muted);
}
bool WINAPI module_voice_audio_configure(void* instance, const VoiceAudioSettings* settings) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    if (runtime && settings) {
        account_soundboard_output(&runtime->soundboard, *settings);
        account_voice_message_configure(&runtime->voice_message, *settings);
    }
    return gateway && settings && gateway_client_voice_audio_configure(gateway, *settings);
}
RestClientSubmitResult WINAPI module_soundboard(void* instance, const SoundboardRequest* request,
                                                u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || (request && !soundboard_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    if (request && request->operation == SoundboardOperation::probe)
        return {account_soundboard_probe(request) ? RestClientSubmitError::none
                                                  : RestClientSubmitError::invalid_input};
    if (!request_id)
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    if (!rest)
        return {RestClientSubmitError::invalid_input};
    return request ? account_soundboard_submit(rest, request, request_id) : account_soundboard_cancel(rest, request_id);
}
bool WINAPI module_soundboard_audio(void* instance, const SoundboardAudioCommand* command,
                                    SoundboardAudioStatus* status) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && account_soundboard_command(&runtime->soundboard,
                                                 user_account_coordinator_gateway(&runtime->coordinator), command,
                                                 status, GetTickCount64());
}
bool WINAPI module_voice_message(void* instance, const VoiceMessageCommand* command,
                                 VoiceMessageStatus* status) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && account_voice_message_command(&runtime->voice_message, command, status);
}
RestClientSubmitResult WINAPI module_guild_invite(void* instance, const GuildInviteRequest* request,
                                                  u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id || (request && !guild_invite_request_valid(*request)))
        return {RestClientSubmitError::invalid_input};
    auto* rest = user_account_coordinator_rest(&runtime->coordinator, true);
    if (!request)
        return account_guild_invite_cancel(rest, request_id);
    // Accept carries the Gateway session like the official client.
    GatewayUserSessionSnapshot session{};
    const bool known = request->operation == GuildInviteOperation::accept &&
                       user_account_coordinator_snapshot_session(&runtime->coordinator, &session) &&
                       session.has_resume;
    const auto result = account_guild_invite_submit(
        rest, request, request_id,
        known ? Utf8View{session.resume.session_id, session.resume.session_id_bytes} : Utf8View{});
    SecureZeroMemory(&session, sizeof(session));
    return result;
}
CaptchaBridgeStatus WINAPI module_action_captcha(void* instance, const AccountActionCaptchaRequest* request) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request)
        return {0, 0, 0, 0, CaptchaBridgePhase::empty, CaptchaBridgeError::invalid_input, false};
    return account_action_captcha(&runtime->action_captcha, user_account_coordinator_rest(&runtime->coordinator),
                                  user_account_coordinator_rest(&runtime->coordinator, true), *request);
}
bool WINAPI module_voice_audio_status(void* instance, VoiceAudioStatus* output) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    return gateway && gateway_client_voice_audio_status(gateway, output);
}
AccountMediaRequestResult WINAPI
module_catalog_preview_request(void* instance, const AccountMediaCatalogPreviewRequest* request, u64 now_ms,
                               u64 now_seconds) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request) {
        AccountMediaRequestResult invalid{};
        invalid.error = AccountMediaError::invalid_input;
        return invalid;
    }
    return account_media_worker_request_catalog_preview(&runtime->media, *request, now_ms, now_seconds);
}
AccountMediaRequestResult WINAPI module_media_local_preview(void* instance,
                                                            AccountLocalPreviewOperation operation,
                                                            const AccountMediaLocalPreviewRequest* request,
                                                            u64 now_ms, u64 now_seconds) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime ||
        (operation != AccountLocalPreviewOperation::request &&
         operation != AccountLocalPreviewOperation::retire &&
         operation != AccountLocalPreviewOperation::uploaded &&
         operation != AccountLocalPreviewOperation::presented) ||
        (operation == AccountLocalPreviewOperation::presented ? request != nullptr : request == nullptr) ||
        (operation != AccountLocalPreviewOperation::request && (now_ms || now_seconds))) {
        AccountMediaRequestResult invalid{};
        invalid.error = AccountMediaError::invalid_input;
        return invalid;
    }
    if (operation == AccountLocalPreviewOperation::presented)
        return account_media_worker_local_previews_presented(&runtime->media);
    if (operation == AccountLocalPreviewOperation::uploaded)
        return account_media_worker_local_preview_uploaded(&runtime->media, *request);
    return operation == AccountLocalPreviewOperation::request
               ? account_media_worker_request_local_preview(&runtime->media, *request, now_ms, now_seconds)
               : account_media_worker_retire_local_preview(&runtime->media, *request);
}
RestClientSubmitResult WINAPI module_message_search(void* instance,
                                                    AccountMessageSearchOperation operation,
                                                    const MessageSearchQuery* query,
                                                    u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    return account_message_search(rest, operation, query, request_id);
}

RestClientSubmitResult WINAPI module_media_url_refresh(void* instance, Utf8View source,
                                                       u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    RestClient* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    u8 body[kCdnMaximumTargetBytes * 2]{};
    const u32 bytes = attachment_refresh_body(source, {body, sizeof(body)});
    if (!rest || request_id == 0 || bytes == 0)
        return {RestClientSubmitError::invalid_input};
    constexpr auto target = utf8_literal("/api/v10/attachments/refresh-urls");
    return rest_client_submit(
        rest, {reinterpret_cast<const u8*>(target.data), target.bytes},
        {body, bytes},
        {0x4154545245465245ull, 0, RestMethod::post, RestMajorParameter::none},
        request_id,
        true);
}

RestClientTakeResult WINAPI module_rest_take(void* instance, RestClientResponse* response,
                                             Slice<u8> body_output) noexcept {
    RestClientTakeResult invalid{};
    invalid.error = RestClientTakeError::invalid_input;
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    RestClient* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator) : nullptr;
    if (rest && runtime->logout_request_id)
        return rest_client_take_response(&runtime->coordinator.details, response, body_output);
    return rest ? user_account_coordinator_take(&runtime->coordinator, response, body_output) : invalid;
}

AccountLogoutResult WINAPI module_logout(void* instance, AccountLogoutOperation operation, u64 request_id,
                                         AccountLogoutResponse* response) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id)
        return {RestClientSubmitError::invalid_input, RestClientTakeError::invalid_input,
                false, false};
    if (operation == AccountLogoutOperation::begin) {
        if ((runtime->logout_request_id && runtime->logout_request_id != request_id) ||
            runtime->logout_submitted)
            return {RestClientSubmitError::busy, RestClientTakeError::empty, false, false};
        runtime->logout_request_id = request_id;
    } else if (runtime->logout_request_id != request_id) {
        return {RestClientSubmitError::invalid_input, RestClientTakeError::empty, false,
                false};
    }
    // A 401 can fail the coordinator before the UI reads this response. The
    // PRIMARY worker stays alive until module_stop joins it.
    const auto result = account_logout(&runtime->coordinator.rest, operation, request_id, response);
    if (operation == AccountLogoutOperation::begin &&
        result.submit_error == RestClientSubmitError::none)
        runtime->logout_submitted = true;
    if (result.consumed) {
        runtime->logout_request_id = 0;
        runtime->logout_submitted = false;
    }
    return result;
}

AccountMediaRequestResult WINAPI module_media_request(void* instance, MediaAssetKey key,
                                                      u64 monotonic_now_ms, u64 unix_now_seconds) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime) {
        AccountMediaRequestResult invalid{};
        invalid.state = AccountMediaRequestState::invalid;
        invalid.error = AccountMediaError::invalid_input;
        return invalid;
    }
    return account_media_worker_request_static_asset(&runtime->media, key, monotonic_now_ms,
                                                     unix_now_seconds);
}

AccountMediaRequestResult WINAPI
module_media_guild_icon_request(void* instance, const AccountMediaGuildIconRequest* request,
                                u64 monotonic_now_ms, u64 unix_now_seconds) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request) {
        AccountMediaRequestResult invalid{};
        invalid.state = AccountMediaRequestState::invalid;
        invalid.error = AccountMediaError::invalid_input;
        return invalid;
    }
    return account_media_worker_request_guild_icon(&runtime->media, *request, monotonic_now_ms,
                                                   unix_now_seconds);
}

AccountMediaRequestResult WINAPI
module_media_attachment_request(void* instance, const AccountMediaAttachmentRequest* request,
                                u64 monotonic_now_ms, u64 unix_now_seconds) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request) {
        AccountMediaRequestResult invalid{};
        invalid.state = AccountMediaRequestState::invalid;
        invalid.error = AccountMediaError::invalid_input;
        return invalid;
    }
    return account_media_worker_request_attachment(&runtime->media, *request, monotonic_now_ms,
                                                   unix_now_seconds);
}

AccountMediaRequestResult WINAPI module_media_advance(void* instance, MediaAssetKey key,
                                                      u16 cache_generation, u32 frame_index,
                                                      u64 unix_now_seconds) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime) {
        AccountMediaRequestResult invalid{};
        invalid.state = AccountMediaRequestState::invalid;
        invalid.error = AccountMediaError::invalid_input;
        return invalid;
    }
    return account_media_worker_advance_animation(&runtime->media, key, cache_generation, frame_index,
                                                  unix_now_seconds);
}

bool WINAPI module_media_take(void* instance, AccountMediaFrame* frame) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && account_media_worker_take_frame(&runtime->media, frame);
}

u32 WINAPI module_runtime_bytes() noexcept {
    return sizeof(AccountModuleRuntime);
}

AccountVideoResult WINAPI module_video_select(void* instance,
                                              const AccountVideoSelection* selection) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && selection ? account_media_worker_video_select(&runtime->media, *selection)
                                : AccountVideoResult{AccountVideoError::invalid_input};
}
AccountVideoResult WINAPI module_video_control(void* instance,
                                               const AccountVideoControl* command) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && command ? account_media_worker_video_control(&runtime->media, *command)
                              : AccountVideoResult{AccountVideoError::invalid_input};
}
AccountVideoPlaybackStatus WINAPI module_video_status(void* instance) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? account_media_worker_video_status(&runtime->media) : AccountVideoPlaybackStatus{};
}
bool WINAPI module_video_take(void* instance, AccountVideoFrame* frame) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && account_media_worker_video_take(&runtime->media, frame);
}
bool WINAPI module_video_acknowledge(void* instance, const AccountVideoFrame* frame) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && frame && account_media_worker_video_acknowledge(&runtime->media, *frame);
}
AccountModuleStatus WINAPI module_stop(void** instance, u32 timeout_ms, u64 monotonic_now_ms,
                                       u64 unix_now_ms) noexcept {
    static_cast<void>(monotonic_now_ms);
    if (!instance || !*instance || timeout_ms == 0 || unix_now_ms == 0) {
        return module_failure(AccountModuleError::invalid_input);
    }
    auto* runtime = static_cast<AccountModuleRuntime*>(*instance);
    if (runtime->download) {
        if (!https_file_download_release(&runtime->download->transfer, timeout_ms)) {
            AccountModuleStatus status = build_status(runtime);
            status.error = AccountModuleError::download;
            status.failed = true;
            return status;
        }
        VirtualFree(runtime->download, 0, MEM_RELEASE);
        runtime->download = nullptr;
    }
    if (!https_file_upload_release(&runtime->upload, timeout_ms)) {
        AccountModuleStatus status = build_status(runtime);
        status.error = AccountModuleError::upload;
        status.failed = true;
        return status;
    }
    runtime->upload_id = 0;
    account_voice_message_release(&runtime->voice_message);
    if (!account_soundboard_stop(&runtime->soundboard, user_account_coordinator_gateway(&runtime->coordinator),
                                 timeout_ms)) {
        AccountModuleStatus status = build_status(runtime);
        status.error = AccountModuleError::media;
        status.failed = true;
        return status;
    }
    if (!account_media_worker_stop(&runtime->media, timeout_ms)) {
        AccountModuleStatus status = build_status(runtime);
        status.error = AccountModuleError::media;
        status.failed = true;
        return status;
    }
    account_action_captcha_release(&runtime->action_captcha, user_account_coordinator_rest(&runtime->coordinator),
                                   user_account_coordinator_rest(&runtime->coordinator, true));
    const bool rejected =
        runtime->coordinator.state.error == UserAccountCoordinatorError::authentication_rejected;
    GatewayUserSessionSnapshot snapshot{};
    const UserAccountCoordinatorResult stopped =
        user_account_coordinator_stop(&runtime->coordinator, timeout_ms, &snapshot);
    bool final_session_saved = true;
    if (!rejected && snapshot_is_persistable(snapshot)) {
        final_session_saved = save_snapshot(runtime, snapshot);
    }
    AccountModuleStatus status = build_status(runtime);
    if (!stopped.ok() || stopped.phase != UserAccountCoordinatorPhase::stopped) {
        // The gateway may still deliver frames: keep the decoders alive.
        status.error = AccountModuleError::coordinator;
        status.failed = true;
        return status;
    }
    status.profile_id = snapshot.profile_id;
    status.running = false;
    status.failed = false;
    status.error = final_session_saved ? AccountModuleError::none : AccountModuleError::session_store;
    status.session_saves = runtime->session_saves;
    status.session_save_skips = runtime->session_save_skips;
    status.session_save_failures = runtime->session_save_failures;
    status.session_native_error = runtime->session_native_error;
    status.session_error = static_cast<u8>(runtime->session_error);
    video_stream_decoders_release(&runtime->video);
    SecureZeroMemory(runtime, sizeof(*runtime));
    VirtualFree(runtime, 0, MEM_RELEASE);
    *instance = nullptr;
    return status;
}

// One host diagnostic sink for the module: REST traces and voice records
// ("voice:" prefixed, routed by the host into the voice log category).
void module_set_trace(RestTraceFunction trace) noexcept {
    rest_client_set_trace(trace);
    voice_log_set_sink(trace);
}

} // namespace
} // namespace maicord

extern "C" bool WINAPI maicord_account_get_api(maicord::u32 requested_version,
                                               maicord::AccountModuleApi* api) noexcept {
    if (!api || requested_version != maicord::kAccountModuleApiVersion) {
        return false;
    }
    *api = maicord::AccountModuleApi{
        maicord::kAccountModuleApiVersion,
        sizeof(maicord::AccountModuleApi),
        maicord::module_start,
        maicord::module_pump,
        maicord::module_notification_handles,
        maicord::module_gateway_source,
        maicord::module_gateway_pop,
        maicord::module_guild_subscribe,
        maicord::module_gateway_refresh,
        maicord::module_private_conversation,
        maicord::module_attachment_upload,
        maicord::module_upload,
        maicord::module_history_submit,
        maicord::module_message_submit,
        maicord::module_message_delete,
        maicord::module_message_edit,
        maicord::module_message_reaction,
        maicord::module_reaction_users,
        maicord::module_message_typing,
        maicord::module_media_url_refresh,
        maicord::module_rest_take,
        maicord::module_media_request,
        maicord::module_media_guild_icon_request,
        maicord::module_media_attachment_request,
        maicord::module_media_advance,
        maicord::module_media_take,
        maicord::module_runtime_bytes,
        maicord::module_stop,
        maicord::module_login_start,
        maicord::module_login_submit_password,
        maicord::module_login_submit_mfa,
        maicord::module_login_send_sms,
        maicord::module_login_start_remote_auth,
        maicord::module_login_copy_remote_auth_qr,
        maicord::module_login_copy_remote_auth_user,
        maicord::module_login_cancel_remote_auth,
        maicord::module_login_pump_with_captcha,
        maicord::module_login_copy_captcha_secret,
        maicord::module_login_submit_captcha,
        maicord::module_login_copy_webauthn_challenge,
        maicord::module_login_notification_handle_with_captcha,
        maicord::module_login_stop,
        maicord::module_profile_get,
        maicord::module_application_command,
        maicord::module_set_trace,
        maicord::module_login_submit_verification,
        maicord::module_relationships,
        maicord::module_message_ack,
        maicord::module_login_submit_recovery,
        maicord::module_login_start_reset,
        maicord::module_login_submit_reset,
        maicord::module_login_submit_phone_verify,
        maicord::module_login_submit_phone_resend,
        maicord::module_login_take_phone_reset_token,
        maicord::module_video_select,
        maicord::module_video_control,
        maicord::module_video_status,
        maicord::module_video_take,
        maicord::module_video_acknowledge,
        maicord::module_message_search,
        maicord::module_login_start_browser,
        maicord::module_login_copy_browser_url,
        maicord::module_self_status_snapshot,
        maicord::module_self_status_submit,
        maicord::module_self_status_complete,
        maicord::module_media_catalog,
        maicord::module_catalog_preview_request,
        maicord::module_gif_favorites,
        maicord::module_channel_overview,
        maicord::module_guild_members_request,
        maicord::module_pinned_messages,
        maicord::module_message_forward,
        maicord::module_download,
        maicord::module_thread_action,
        maicord::module_thread_browser,
        maicord::module_thread_creation,
        maicord::module_forum_post,
        maicord::module_media_local_preview,
        maicord::module_logout,
        maicord::module_bulk_message_ack,
        maicord::module_poll,
        maicord::module_guild_admin,
        maicord::module_voice_request,
        maicord::module_voice_snapshot,
        maicord::module_login_start_captcha_bridge,
        maicord::module_login_captcha_bridge_status,
        maicord::module_login_copy_captcha_bridge_url,
        maicord::module_login_cancel_captcha_bridge,
        maicord::module_voice_user_audio,
        maicord::module_voice_audio_configure,
        maicord::module_voice_audio_status,
        maicord::module_guild_invite,
        maicord::module_emoji_preferences,
        maicord::module_account_settings,
        maicord::module_action_captcha,
        maicord::module_poll_create,
        maicord::module_private_channel,
        maicord::module_custom_status_snapshot,
        maicord::module_custom_status_submit,
        maicord::module_guild_settings,
        maicord::module_mentions,
        maicord::module_guild_assets,
        maicord::module_voice_video_snapshot,
        maicord::module_voice_video_wanted,
        maicord::module_voice_video_device,
        maicord::module_voice_video_frame,
        maicord::module_stream_watch,
        maicord::module_stream_snapshot,
        maicord::module_stream_volume,
        maicord::module_stream_preview,
        maicord::module_stage,
        maicord::module_message_interaction,
        maicord::module_user_social,
        maicord::module_guild_community,
        maicord::module_local_presence,
        maicord::module_activity_sharing,
        maicord::module_soundboard,
        maicord::module_soundboard_audio,
        maicord::module_voice_message,
        maicord::module_profile_edit,
        maicord::module_profile_image,
        {
            maicord::gateway_decode_message_create,
            maicord::gateway_decode_message_create,
            maicord::gateway_decode_message_create,
            maicord::gateway_decode_message_create,
            maicord::gateway_decode_message_snapshot_core,
            maicord::gateway_extract_message_snapshot,
            maicord::gateway_decode_message_poll,
            maicord::gateway_client_route_channel_create,
            maicord::gateway_client_route_private_channel,
        },
        {
            maicord::parse_signed_attachment_url,
            maicord::parse_media_url,
            maicord::catalog_provider_target_valid,
            maicord::sticker_json_target_valid,
            maicord::soundboard_target_valid,
            maicord::parse_catalog_preview_url,
            maicord::catalog_favorite_format,
            maicord::media_url_log_fields,
            maicord::write_media_proxy_target,
            maicord::write_custom_emoji_cdn_target,
            maicord::cdn_guild_icon_variant,
            maicord::write_guild_icon_cdn_target,
            maicord::write_user_avatar_cdn_target,
            maicord::write_channel_icon_cdn_target,
            maicord::write_sticker_cdn_target,
            maicord::cdn_host_name,
        },
        maicord::module_guild_moderation,
    };
    return true;
}

extern "C" BOOL WINAPI maicord_account_module_entry(HINSTANCE instance, DWORD reason, void*) noexcept {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
