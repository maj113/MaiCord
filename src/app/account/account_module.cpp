#include "account_module_internal.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/app/net/signed_out_session.hpp"
#include "login/account_login_captcha.hpp"
#include "maicord/app/account/account_user_social.hpp"
#include "maicord/app/guild/account_guild_community.hpp"
#include "maicord/app/guild/account_guild_moderation.hpp"
#include "maicord/app/guild/account_guild_automation.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/app/account/account_relationships.hpp"
#include "maicord/app/chat/messages/account_message_ack.hpp"
#include "maicord/app/chat/messages/account_bulk_message_ack.hpp"
#include "maicord/app/chat/threads/account_thread_actions.hpp"
#include "maicord/app/chat/threads/account_thread_browser.hpp"
#include "maicord/app/chat/polls/account_message_poll.hpp"
#include "maicord/app/notifications/account_notification_settings.hpp"
#include "maicord/app/chat/account_private_channel.hpp"
#include "maicord/app/guild/account_guild_invite.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "../chat/composer/command_request.hpp"
#include "maicord/app/guild/account_channel_overview.hpp"
#include "maicord/app/chat/threads/account_forum_post.hpp"
#include "maicord/app/media/account_gif_favorites.hpp"
#include "maicord/app/guild/account_guild_admin.hpp"
#include "maicord/app/media/account_media_catalog.hpp"
#include "maicord/app/chat/search/account_message_search.hpp"
#include "maicord/app/chat/messages/account_pinned_messages.hpp"
#include "maicord/app/account/account_profile_edit.hpp"
#include "maicord/app/voice/account_stage.hpp"
#include "maicord/app/chat/threads/account_thread_creation.hpp"
#include "maicord/protocol/messages/delete.hpp"
#include "maicord/protocol/messages/edit.hpp"
#include "maicord/protocol/messages/interaction.hpp"
#include "maicord/protocol/messages/interaction_data.hpp"

namespace maicord {
namespace account_module_detail {

bool copy_path(const wchar_t* source, wchar_t* destination) noexcept {
    u32 length = 0;
    return source && source[0] && destination &&
           wide_append(destination, kAccountProfileMaximumPathCharacters, &length, source);
}

} // namespace account_module_detail
namespace {

// The heartbeat session the session file keeps, or null.
const ClientHeartbeatSession* persisted_heartbeat(const ClientHeartbeatSession& session) noexcept {
    return client_heartbeat_session_valid(session) ? &session : nullptr;
}

bool snapshot_is_persistable(const GatewayUserSessionSnapshot& snapshot) noexcept {
    return snapshot.profile_id != 0 && snapshot.has_resume && gateway_resume_validate(snapshot.resume);
}

} // namespace
namespace account_module_detail {

bool save_snapshot(AccountModuleRuntime* runtime, const GatewayUserSessionSnapshot& snapshot,
                   bool include_sequence) noexcept {
    const ClientHeartbeatSession session = client_session_metadata_session(&runtime->client_metadata);
    const ClientHeartbeatSession* heartbeat = persisted_heartbeat(session);
    u32 cookie_revision = 0;
    const bool cookies = cookie_jar_has_persistent(&runtime->cookies, &cookie_revision);
    if (runtime->coordinator.state.error == UserAccountCoordinatorError::authentication_rejected ||
        snapshot.profile_id == 0 || (!snapshot_is_persistable(snapshot) && !heartbeat && !cookies)) {
        return true;
    }
    if (account_session_save_state_current(runtime->save_state, snapshot, heartbeat, cookie_revision,
                                           include_sequence)) {
        ++runtime->session_save_skips;
        return true;
    }
    // A resume token that is not persistable is not written with the heartbeat session.
    GatewayUserSessionSnapshot stored = snapshot;
    if (!snapshot_is_persistable(stored)) {
        stored.has_resume = false;
        reset_value(&stored.resume);
    }
    const AccountSessionStoreResult saved =
        account_user_session_store_save(runtime->session_path, stored, heartbeat, &runtime->cookies);
    runtime->session_error = saved.error;
    runtime->session_native_error = saved.native_error;
    if (!saved.ok()) {
        ++runtime->session_save_failures;
        return false;
    }
    account_session_save_state_record(&runtime->save_state, snapshot, heartbeat, cookie_revision);
    ++runtime->session_saves;
    return true;
}

void persist_refreshed_credential(AccountModuleRuntime* runtime, bool retry) noexcept {
    SecureBuffer taken{};
    const bool fresh = user_account_coordinator_take_refreshed_credential(&runtime->coordinator, &taken);
    if (fresh) {
        // A newer token replaces one still waiting for its save.
        secure_buffer_release(&runtime->refreshed_credential);
        secure_buffer_transfer(&runtime->refreshed_credential, &taken);
    }
    // A failed save keeps the token; the session autosave and the stop try again.
    if ((fresh || retry) && runtime->refreshed_credential.data &&
        runtime->coordinator.state.error != UserAccountCoordinatorError::authentication_rejected &&
        account_profile_save(runtime->profile_path, runtime->coordinator.metadata, runtime->refreshed_credential)
            .ok())
        secure_buffer_release(&runtime->refreshed_credential);
}

} // namespace account_module_detail
namespace {

void autosave_if_due(AccountModuleRuntime* runtime, u64 monotonic_now_ms) noexcept {
    if (monotonic_now_ms < runtime->next_autosave_at_ms) {
        return;
    }
    GatewayUserSessionSnapshot snapshot{};
    const bool captured = user_account_coordinator_snapshot_session(&runtime->coordinator, &snapshot);
    const bool saved = !captured || save_snapshot(runtime, snapshot, false);
    persist_refreshed_credential(runtime, true);
    runtime->next_autosave_at_ms =
        deadline_after(monotonic_now_ms, saved ? kSessionAutosaveIntervalMs : kSessionAutosaveRetryMs);
}

} // namespace
namespace account_module_detail {

AccountModuleStatus build_status(AccountModuleRuntime* runtime) noexcept {
    const UserAccountCoordinatorResult coordinator = user_account_coordinator_status(&runtime->coordinator);
    AccountModuleStatus status{};
    status.profile_id = coordinator.profile_id;
    status.frecency_settings_revision =
        static_cast<u64>(ReadAcquire64(&runtime->coordinator.gateway.frecency_settings_revision));
    status.required_action = gateway_client_required_action(&runtime->coordinator.gateway);
    status.native_error = coordinator.native_error;
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

AccountModuleStatus stop_failure(AccountModuleRuntime* runtime, AccountModuleError error) noexcept {
    AccountModuleStatus status = build_status(runtime);
    status.error = error;
    status.failed = true;
    return status;
}

} // namespace account_module_detail
namespace {

// Drops a runtime whose start failed: stops the media worker and releases the profile when they
// were started, and wipes the runtime before it is freed.
__declspec(noinline) void discard_runtime(AccountModuleRuntime* runtime, AccountProfile* profile = nullptr,
                                          bool stop_media = false) noexcept {
    if (stop_media)
        account_media_worker_stop(&runtime->media, 5'000);
    if (profile)
        account_profile_release(profile);
    SecureZeroMemory(runtime, sizeof(*runtime));
    pages_release(runtime);
}

AccountModuleStartResult WINAPI module_start(const wchar_t* profile_path, const wchar_t* session_path,
                                             u64 monotonic_now_ms, u64 unix_now_ms) noexcept {
    AccountModuleStartResult result{};
    if (!profile_path || !session_path || unix_now_ms == 0) {
        result.error = AccountModuleError::invalid_input;
        return result;
    }
    auto* runtime = commit_pages<AccountModuleRuntime>();
    if (!runtime) {
        result.error = AccountModuleError::memory;
        result.native_error = thread_last_error();
        return result;
    }
    video_stream_decoders_initialize(&runtime->video);
    runtime->go_live.picture_event = runtime->video.frame_event;
    account_soundboard_initialize(&runtime->soundboard);
    account_voice_message_initialize(&runtime->voice_message);
    if (!copy_path(session_path, runtime->session_path) || !copy_path(profile_path, runtime->profile_path)) {
        discard_runtime(runtime);
        result.error = AccountModuleError::invalid_input;
        return result;
    }

    AccountProfile profile{};
    const AccountProfileResult loaded = account_profile_load(profile_path, &profile);
    if (!loaded.ok()) {
        discard_runtime(runtime);
        result.error = AccountModuleError::profile;
        result.profile_error = static_cast<u8>(loaded.error);
        result.native_error = loaded.native_error;
        return result;
    }

    GatewayUserSessionSnapshot restored{};
    ClientHeartbeatSession restored_heartbeat{};
    const AccountSessionStoreResult session = account_user_session_store_load(
        session_path, profile.metadata.profile_id, &restored, &restored_heartbeat, &runtime->cookies);
    runtime->session_error = session.error;
    runtime->session_native_error = session.native_error;
    result.session_error = static_cast<u8>(session.error);
    result.session_loaded = session.ok();
    const ClientHeartbeatSession* heartbeat =
        session.ok() ? persisted_heartbeat(restored_heartbeat) : nullptr;
    u32 cookie_revision = 0;
    const bool cookies = cookie_jar_has_persistent(&runtime->cookies, &cookie_revision);
    if (session.ok() && (snapshot_is_persistable(restored) || heartbeat || cookies)) {
        account_session_save_state_record(&runtime->save_state, restored, heartbeat, cookie_revision);
    }
    // An account that just signed in continues with the login page's cookies,
    // which replace any saved ones; they are saved with its session.
    signed_out_adopt_cookies(profile.metadata.profile_id, &runtime->cookies);

    const AccountMediaRequestResult media_started = account_media_worker_initialize(&runtime->media);
    if (!media_started.ok()) {
        discard_runtime(runtime, &profile);
        result.error = AccountModuleError::media_start;
        return result;
    }

    UserClientConfigurationStorage& configuration = runtime->client_configuration;
    const UserClientConfigurationResult configured = user_client_configuration_initialize(&configuration);
    if (!configured.ok()) {
        discard_runtime(runtime, &profile, true);
        result.error = AccountModuleError::coordinator_start;
        result.native_error = configured.native_error;
        return result;
    }
    // Saved desired status arrives with READY; avoid a transient Online publish
    // while an account's Invisible preference is still unknown to this process.
    configuration.configuration.status = GatewayUserStatus::invisible;
    // The client reports its focus once started (client_focus).
    if (!client_session_metadata_initialize(
            &runtime->client_metadata, configuration.configuration.properties,
            {configuration.discord_locale, configuration.discord_locale_bytes}, heartbeat, false)) {
        discard_runtime(runtime, &profile, true);
        result.error = AccountModuleError::coordinator_start;
        return result;
    }
    runtime->client_metadata.cookies = &runtime->cookies;
    runtime->client_metadata.accept_language = {configuration.accept_language, configuration.accept_language_bytes};
    configuration.configuration.metadata = &runtime->client_metadata;
    // The persisted Gateway resume token is tied to its matching guild/channel/message model. That model is not
    // persisted, so a cross-process RESUMED dispatch cannot reconstruct the UI: the session file is validated, but
    // the coordinator identifies as a fresh session.
    const UserAccountCoordinatorResult started = user_account_coordinator_start(
        &runtime->coordinator, &profile, &configuration.configuration,
        {configuration.timezone, configuration.timezone_bytes},
        {configuration.accept_language, configuration.accept_language_bytes});
    if (!started.ok()) {
        discard_runtime(runtime, &profile, true);
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
    persist_refreshed_credential(runtime, false);
    if (user_account_coordinator_gateway_restart_due(&runtime->coordinator, monotonic_now_ms)) {
        // Reuse the authenticated owner's initial identity; the saved status
        // follows READY. Soundboard and video sinks re-attach on their next
        // pump/snapshot, as for any fresh session.
        user_account_coordinator_restart_gateway(&runtime->coordinator,
                                                 &runtime->client_configuration.configuration,
                                                 monotonic_now_ms);
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

void* WINAPI module_gateway_source(void* instance) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
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
    auto* rest = module_rest(instance);
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

RestClientSubmitResult WINAPI module_history_submit(void* instance, u64 channel_id,
                                                    const MessageHistoryQuery* query,
                                                    u64 request_id) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    // The restored channel's first page may go out before READY.
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    RestClient* rest = runtime ? user_account_coordinator_startup_rest(&runtime->coordinator) : nullptr;
    if (!rest || !query || request_id == 0) {
        return invalid;
    }
    u8 target[128];
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
        auto* owner = commit_pages<AccountDownloadOwner>();
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
            pages_release(owner);
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
                pages_release(owner);
                return result;
            }
        } else if (operation != AccountDownloadOperation::status) {
            result.submit_error = HttpsFileDownloadError::invalid_input;
            return result;
        }
    }
    auto& transfer = runtime->download->transfer;
    if (transfer.notification)
        event_reset(transfer.notification);
    result.transfer = https_file_download_status(&transfer);
    return result;
}

RestClientSubmitResult WINAPI module_attachment_upload(void* instance, u64 channel_id, u32 id,
                                                       Utf8View filename, u64 file_bytes,
                                                       u64 request_id) noexcept {
    RestClient* rest = module_rest(instance);
    if (!rest || !request_id)
        return {RestClientSubmitError::invalid_input};
    u8 target[96]{};
    u8* const body = g_request_body;
    const auto written = attachment_upload_write(channel_id, id, filename, file_bytes,
                                                 {target, sizeof(target)}, {body, 8 * 1024});
    if (!written.ok)
        return {RestClientSubmitError::invalid_input};
    const auto result = rest_client_submit(rest, {target, written.target_bytes},
                                           {body, written.body_bytes},
                                           attachment_upload_route(channel_id),
                                           request_id,
                                           true);
    SecureZeroMemory(body, written.body_bytes);
    return result;
}

// Submits the JSON body the caller wrote into g_request_body to `target` on a channel route, then wipes
// the body. A failed write (`written` false) wipes `capacity` bytes instead and submits nothing.
__declspec(noinline) RestClientSubmitResult submit_channel_body(RestClient* rest, bool written, const u8* target,
                                                                u32 target_bytes, u32 body_bytes, u32 capacity,
                                                                u64 route_id, u64 channel_id, RestMethod method,
                                                                u64 request_id) noexcept {
    if (!written) {
        SecureZeroMemory(g_request_body, capacity);
        return kInvalidSubmit;
    }
    const RestClientSubmitResult submitted =
        rest_client_submit(rest, {target, target_bytes}, {g_request_body, body_bytes},
                           {route_id, channel_id, method, RestMajorParameter::channel}, request_id, true);
    SecureZeroMemory(g_request_body, body_bytes);
    return submitted;
}

RestClientSubmitResult WINAPI
module_message_submit(void* instance, u64 channel_id, Utf8View content, u64 nonce, u64 reply_message_id,
                      ConstSlice<UploadedAttachment> attachments, ConstSlice<u64> sticker_ids,
                      u64 request_id, bool mention_reply_author, u32 flags) noexcept {
    RestClient* rest = module_rest(instance);
    if (!rest || channel_id == 0 || nonce == 0 || request_id == 0) {
        return kInvalidSubmit;
    }
    u8 target[96];
    const MessageCreateWriteResult target_written =
        write_message_create_target(channel_id, Slice<u8>{target, sizeof(target)});
    const MessageCreateWriteResult body_written = write_message_create_body(
        content, nonce, reply_message_id ? channel_id : 0, reply_message_id, attachments, sticker_ids,
        {g_request_body, kMessageCreateMaximumBodyBytes}, mention_reply_author, flags);
    return submit_channel_body(rest, target_written.ok() && body_written.ok(), target, target_written.bytes,
                               body_written.bytes, kMessageCreateMaximumBodyBytes, kMessageCreateRouteId,
                               channel_id, RestMethod::post, request_id);
}

RestClientSubmitResult WINAPI module_message_forward(void* instance, u64 destination_channel_id,
                                                     Utf8View caption, u64 nonce,
                                                     MessageForwardReference source,
                                                     u64 request_id) noexcept {
    RestClient* rest = module_rest(instance);
    if (!rest || !destination_channel_id || !nonce || !request_id)
        return {RestClientSubmitError::invalid_input};
    u8 target[96];
    const auto target_written =
        write_message_create_target(destination_channel_id, {target, sizeof(target)});
    const auto body_written =
        write_message_forward_body(caption, nonce, source, {g_request_body, kMessageCreateMaximumBodyBytes});
    return submit_channel_body(rest, target_written.ok() && body_written.ok(), target, target_written.bytes,
                               body_written.bytes, kMessageCreateMaximumBodyBytes, kMessageCreateRouteId,
                               destination_channel_id, RestMethod::post, request_id);
}

RestClientSubmitResult WINAPI module_message_delete(void* instance, u64 channel_id, u64 message_id,
                                                    u64 request_id) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    RestClient* rest = module_rest(instance);
    if (!rest || channel_id == 0 || message_id == 0 || request_id == 0) {
        return invalid;
    }
    u8 target[128];
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
    RestClient* rest = module_rest(instance);
    if (!rest || channel_id == 0 || message_id == 0 || (!content.data && content.bytes != 0) ||
        request_id == 0) {
        return kInvalidSubmit;
    }
    u8 target[128];
    const MessageEditWriteResult target_written =
        write_message_edit_target(channel_id, message_id, Slice<u8>{target, sizeof(target)});
    const MessageEditWriteResult body_written =
        write_message_edit_body(content, Slice<u8>{g_request_body, kMessageEditMaximumBodyBytes});
    return submit_channel_body(rest, target_written.ok() && body_written.ok(), target, target_written.bytes,
                               body_written.bytes, kMessageEditMaximumBodyBytes, kMessageEditRouteId, channel_id,
                               RestMethod::patch, request_id);
}

RestClientSubmitResult WINAPI module_message_reaction(void* instance, u64 channel_id, u64 message_id,
                                                      u64 emoji_id, Utf8View emoji_name,
                                                      MessageReactionType type, bool add,
                                                      u64 request_id) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    RestClient* rest = module_rest(instance);
    if (!rest || channel_id == 0 || message_id == 0 || request_id == 0) {
        return invalid;
    }
    u8 target[kMessageReactionMaximumTargetBytes];
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

// The interaction POST of slash commands and message components: `write` fills a body of up to `capacity`
// bytes that carries the account's Gateway session id; none without a resumable session.
using InteractionBodyWriter = bool (*)(const void* context, Utf8View session_id, ProtocolWriter* body) noexcept;
__declspec(noinline) RestClientSubmitResult submit_interaction(AccountModuleRuntime* runtime, RestClient* rest,
                                                               u32 capacity, InteractionBodyWriter write,
                                                               const void* context, u64 request_id,
                                                               bool autocomplete) noexcept {
    GatewayUserSessionSnapshot snapshot{};
    if (!user_account_coordinator_snapshot_session(&runtime->coordinator, &snapshot) || !snapshot.has_resume)
        return kInvalidSubmit;
    Arena storage{};
    if (!capacity || !arena_initialize(&storage, capacity, 0, 4096))
        return kInvalidSubmit;
    u8* bytes = arena_allocate<u8>(&storage, capacity);
    ProtocolWriter body{{bytes, capacity}, 0};
    auto result = kInvalidSubmit;
    if (bytes && write(context, {snapshot.resume.session_id, snapshot.resume.session_id_bytes}, &body))
        result = rest_client_submit(
            rest, app_detail::application_command_interaction_request({bytes, body.used}, request_id, autocomplete));
    arena_release(&storage);
    return result;
}

RestClientSubmitResult WINAPI module_application_command(void* instance, u64 channel, u64 guild,
                                                         u64 application, u64 nonce, Utf8View data,
                                                         u64 request_id, u8 operation) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator, true) : nullptr;
    if (!rest || !request_id || operation > 8)
        return invalid;
    // 7/8: the built-in /nick (guild) and /thread (channel); data is the JSON body.
    if (operation >= app_detail::kBuiltinCommandNick) {
        if (!data.bytes || !utf8_validate(data).valid)
            return invalid;
        u8 target[96];
        const auto request = app_detail::builtin_command_request(
            operation, guild, channel, {reinterpret_cast<const u8*>(data.data), data.bytes}, request_id,
            {target, sizeof(target)});
        return request.target.count ? rest_client_submit(rest, request) : invalid;
    }
    // 6: GET the interaction data of message `nonce` in `channel` (the command chip's popout).
    if (operation == 6) {
        u8 target[kInteractionDataTargetBytes];
        const u32 bytes = interaction_data_write_target(channel, nonce, {target, sizeof(target)});
        return bytes ? rest_client_submit(rest, {target, bytes}, {}, interaction_data_route(channel), request_id, false)
                     : invalid;
    }
    // 4/5: GET / PATCH the command frecency in settings-proto/2.
    if (operation >= 4)
        return account_command_frecency(
            rest, operation == 4 ? AccountGifFavoritesOperation::fetch : AccountGifFavoritesOperation::update,
            {reinterpret_cast<const u8*>(data.data), data.bytes}, request_id);
    u8 target[160];
    ProtocolWriter path{{target, sizeof(target)}, 0};
    if (operation <= 1) {
        if (operation == 1) {
            if (!protocol_writer_append_literal(&path, "/api/v9/users/@me/application-command-index"))
                return invalid;
        } else if (!channel || !protocol_writer_append_literal(&path, "/api/v9/") ||
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
    struct Command {
        u64 application, guild, channel, nonce;
        Utf8View data;
        bool autocomplete;
    } const command{application, guild, channel, nonce, data, operation == 3};
    return submit_interaction(
        runtime, rest, data.bytes <= ~0u - 1024 ? data.bytes + 1024 : 0,
        [](const void* context, Utf8View session_id, ProtocolWriter* body) noexcept {
            const auto& c = *static_cast<const Command*>(context);
            return app_detail::application_command_interaction_body(c.autocomplete, c.application, c.guild,
                                                                    c.channel, session_id, c.nonce, c.data, body);
        },
        &command, request_id, command.autocomplete);
}

RestClientSubmitResult WINAPI module_message_interaction(void* instance,
                                                         const MessageInteractionRequest* request,
                                                         u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* rest = runtime ? user_account_coordinator_rest(&runtime->coordinator, true) : nullptr;
    if (!rest || !request || !request_id || !message_interaction_request_valid(*request))
        return kInvalidSubmit;
    return submit_interaction(
        runtime, rest, request->data_bytes + 1024u,
        [](const void* context, Utf8View session_id, ProtocolWriter* body) noexcept {
            return message_interaction_write_body(*static_cast<const MessageInteractionRequest*>(context),
                                                  session_id, body);
        },
        request, request_id, false);
}

RestClientSubmitResult WINAPI module_profile_get(void* instance, u64 user, u64 guild, u64 request_id,
                                                 u8 operation, Utf8View value) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* rest = module_rest(instance, true);
    if (!rest || !user || !request_id)
        return invalid;
    u8 target[256];
    u8* const body = g_request_body;
    ConstSlice<u8> path{target, 0}, payload{};
    u32 route = 0;
    RestMethod method = RestMethod::get;
    if (operation == 0 || operation == 4 || operation == 7) {
        // 0: the profile of `user` (with `guild`'s member data for a guild), 4: its collectibles products,
        // 7: Application Test Mode's access check, `user` being the application (Settings > Developer).
        static constexpr struct {
            const char* prefix;
            const char* suffix;
            u32 route;
        } kUserPaths[] = {
            {"/api/v9/users/",
             "/profile?with_mutual_guilds=true&with_mutual_friends=true&with_mutual_friends_count=true", 0x5052464c},
            {"/api/v9/collectibles-products/", "", 0x50524546},
            {"/api/v9/activities/", "/test-mode", 0x544d4f44},
        };
        const auto& user_path = kUserPaths[operation / 3];
        ProtocolWriter writer{{target, sizeof(target)}, 0};
        if (!protocol_write(&writer, user_path.prefix, user, user_path.suffix) ||
            (!operation && guild && !protocol_write(&writer, "&guild_id=", guild)))
            return invalid;
        path.count = writer.used;
        route = user_path.route;
    } else if (operation == 5 || operation == 6) {
        // 5: the public applications whose icons stand in for missing activity
        // art (value: comma-separated IDs). 6: an activity's metadata (value:
        // its session; guild: its application, 0 for none).
        u32 bytes = 0;
        if (operation == 5) {
            u64 ids[kUserActivityMaximum];
            u32 count = 0, begin = 0;
            for (u32 i = 0; i <= value.bytes; ++i) {
                if (i < value.bytes && value.data[i] != ',')
                    continue;
                if (count == kUserActivityMaximum || !snowflake_parse({value.data + begin, i - begin}, &ids[count]))
                    return invalid;
                ++count;
                begin = i + 1;
            }
            bytes = user_activity_applications_target({ids, count}, {target, sizeof(target)});
        } else
            bytes = user_activity_metadata_target(user, value, guild, {target, sizeof(target)});
        if (!bytes)
            return invalid;
        path.count = bytes;
        route = operation == 5 ? 0x41505053u : 0x414d4554u;
    } else {
        if (operation > 3 || !utf8_validate(value).valid || value.bytes > (operation == 1 ? 1024u : 128u))
            return invalid;
        ProtocolWriter writer{{body, 6200}, 0};
        const char* field = operation == 1   ? "{\"bio\":"
                            : operation == 2 ? "{\"pronouns\":"
                                             : "{\"global_name\":";
        if (!protocol_writer_append_c_string(&writer, field) ||
            !protocol_writer_append_json_string(&writer, value) ||
            !protocol_writer_append_byte(&writer, '}'))
            return invalid;
        const auto patch_target = operation == 3 ? utf8_literal("/api/v9/users/@me")
                                                 : utf8_literal("/api/v9/users/@me/profile");
        path = {reinterpret_cast<const u8*>(patch_target.data), patch_target.bytes};
        payload = {body, writer.used};
        route = 0x5052464d;
        method = RestMethod::patch;
    }
    return rest_client_submit(rest, path, payload, {route, 0, method, RestMajorParameter::none}, request_id,
                              false);
}

AccountLoginStatus module_login_start_mode(void** instance, const wchar_t* profile_path,
                                           bool reset) noexcept {
    if (!instance || *instance || !profile_path)
        return login_invalid_input();
    auto* runtime = commit_pages<AccountLoginRuntime>();
    if (!runtime) {
        AccountLoginStatus status{};
        status.error = AccountLoginError::memory;
        status.native_error = thread_last_error();
        return status;
    }
    if (!copy_path(profile_path, runtime->profile_path)) {
        SecureZeroMemory(runtime, sizeof(*runtime));
        pages_release(runtime);
        AccountLoginStatus status{};
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    runtime->reset_mode = reset;
    *instance = runtime;
    return login_status(runtime);
}

} // namespace
namespace account_module_detail {

AccountLoginStatus WINAPI module_login_start(void** instance, const wchar_t* profile_path) noexcept {
    return module_login_start_mode(instance, profile_path, false);
}

AccountLoginStatus WINAPI module_login_start_reset(void** instance, const wchar_t* profile_path) noexcept {
    return module_login_start_mode(instance, profile_path, true);
}

} // namespace account_module_detail
namespace {

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

} // namespace
namespace account_module_detail {

AccountLoginStatus WINAPI module_login_submit_password(void* instance, const SecureBuffer* login,
                                                       const SecureBuffer* password) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !login || !password || runtime->remote_mode || runtime->reset_mode ||
        runtime->browser_mode) {
        return login_invalid_input();
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
    else
        runtime->error = AccountLoginError::coordinator;
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_submit_recovery(void* instance, const SecureBuffer* login) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !login || runtime->remote_mode || runtime->reset_mode || runtime->browser_mode ||
        runtime->profile_saved || account_profile_promotion_has_credential(runtime->promotion)) {
        return login_invalid_input();
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
        return login_invalid_input();
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
    if (!runtime || !link_token || runtime->remote_mode || runtime->reset_mode || runtime->browser_mode)
        return login_invalid_input();
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

} // namespace account_module_detail
namespace {

bool phone_recovery_ready(const AccountLoginRuntime* runtime) noexcept {
    return runtime && runtime->coordinator_started && runtime->recovery_mode && !runtime->remote_mode &&
           !runtime->reset_mode && !runtime->profile_saved &&
           !account_profile_promotion_has_credential(runtime->promotion);
}

AccountLoginStatus phone_submitted(AccountLoginRuntime* runtime, bool ok) noexcept {
    runtime->error = ok ? AccountLoginError::none : AccountLoginError::coordinator;
    if (ok) {
        runtime->last_event = UserAuthCoordinatorEventKind::none;
        runtime->error_code = 0;
    }
    return login_status(runtime);
}

} // namespace
namespace account_module_detail {

AccountLoginStatus WINAPI module_login_submit_phone_verify(void* instance,
                                                           const SecureBuffer* code) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!phone_recovery_ready(runtime))
        return login_invalid_input();
    return phone_submitted(runtime, user_auth_coordinator_submit_phone_verify(&runtime->coordinator, code).ok());
}
AccountLoginStatus WINAPI module_login_submit_phone_resend(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!phone_recovery_ready(runtime))
        return login_invalid_input();
    return phone_submitted(runtime, user_auth_coordinator_submit_phone_resend(&runtime->coordinator).ok());
}
SecureBufferResult WINAPI module_login_take_phone_reset_token(void* instance, u64 generation,
                                                              SecureBuffer* output) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!phone_recovery_ready(runtime) || !output)
        return {0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    return user_auth_coordinator_take_phone_reset_token(&runtime->coordinator, generation, output);
}

AccountLoginStatus WINAPI module_login_submit_mfa(void* instance, const SecureBuffer* code,
                                                  UserAuthMfaType type) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !code || runtime->remote_mode || !runtime->coordinator_started)
        return login_invalid_input();
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
        return login_invalid_input();
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
    if (!runtime || runtime->remote_mode || !runtime->coordinator_started)
        return login_invalid_input();
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
        return login_invalid_input();
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
        reset_value<UserAuthCoordinator>(&runtime->coordinator);
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
    if (!runtime || !runtime->remote_mode || !runtime->remote_active)
        return login_invalid_input();
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

} // namespace account_module_detail

const AccountModuleApi kAccountModuleApi{
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
        maicord::module_login_submit_verification,
        maicord::RestEntry<maicord::account_relationship_submit>::call,
        maicord::RestEntry<maicord::account_message_ack_submit>::call,
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
        maicord::RestEntry<maicord::account_message_search>::call,
        maicord::module_login_start_browser,
        maicord::module_login_copy_browser_url,
        maicord::module_self_status_snapshot,
        maicord::module_self_status_submit,
        maicord::module_self_status_complete,
        maicord::RestEntry<maicord::account_media_catalog>::call,
        maicord::module_catalog_preview_request,
        maicord::RestEntry<maicord::account_gif_favorites>::call,
        maicord::RestEntry<maicord::account_channel_overview_submit, true>::call,
        maicord::module_guild_members_request,
        maicord::module_lane<maicord::account_pinned_messages_submit>,
        maicord::module_message_forward,
        maicord::module_download,
        maicord::module_lane<maicord::account_thread_action_submit, maicord::admit_thread_action>,
        maicord::module_lane<maicord::account_thread_browser_submit>,
        maicord::module_lane<maicord::account_thread_creation_submit, maicord::admit_thread_creation>,
        maicord::module_lane<maicord::account_forum_post_submit, maicord::admit_published_identity<maicord::ForumPostRequest>>,
        maicord::module_media_local_preview,
        maicord::module_logout,
        maicord::RestEntry<maicord::account_bulk_message_ack_submit>::call,
        maicord::module_lane<maicord::account_message_poll_submit, maicord::admit_published_identity<maicord::PollRequest>>,
        maicord::module_lane<maicord::account_guild_admin_submit>,
        maicord::module_voice_request,
        maicord::module_voice_snapshot,
        maicord::module_login_start_captcha_bridge,
        maicord::module_login_captcha_bridge_status,
        maicord::module_login_copy_captcha_bridge_url,
        maicord::module_login_cancel_captcha_bridge,
        maicord::GatewayEntry<maicord::gateway_client_voice_user_audio>::call,
        maicord::module_voice_audio_configure,
        maicord::GatewayEntry<maicord::gateway_client_voice_audio_status>::call,
        maicord::module_lane<maicord::account_guild_invite_submit, maicord::admit_invite, maicord::accepts_invite>,
        maicord::RestEntry<maicord::account_emoji_preferences>::call,
        maicord::module_account_settings,
        maicord::module_action_captcha,
        maicord::module_poll_create,
        maicord::module_lane<maicord::account_private_channel_submit>,
        maicord::module_custom_status_snapshot,
        maicord::module_custom_status_submit,
        maicord::module_lane<maicord::account_guild_settings_submit>,
        maicord::module_lane<maicord::account_mentions_submit>,
        maicord::module_guild_assets,
        maicord::module_voice_video_snapshot,
        maicord::GatewayEntry<maicord::gateway_client_voice_video_wanted>::call,
        maicord::module_voice_video_device,
        maicord::module_voice_video_frame,
        maicord::module_stream_watch,
        maicord::GatewayEntry<maicord::gateway_client_stream_snapshot>::call,
        maicord::GatewayEntry<maicord::gateway_client_stream_volume>::call,
        maicord::module_stream_preview,
        maicord::module_lane<maicord::account_stage_submit>,
        maicord::module_message_interaction,
        maicord::module_lane<maicord::account_user_social_submit>,
        maicord::module_lane<maicord::account_guild_community_submit, nullptr, maicord::joins_guild>,
        maicord::module_local_presence,
        maicord::module_activity_sharing,
        maicord::module_soundboard,
        maicord::module_soundboard_audio,
        maicord::module_voice_message,
        maicord::module_lane<maicord::account_profile_edit_submit>,
        maicord::module_profile_image,
        maicord::module_lane<maicord::account_guild_moderation_submit>,
        maicord::module_guild_members_query_cancel,
        maicord::module_channel_info,
        maicord::module_collection_title,
        maicord::module_client_focus,
        maicord::module_voice_video_notification,
        maicord::module_media_vp8_cpu,
        maicord::module_lane<maicord::account_guild_automation_submit>,
        maicord::module_credential_replace,
        maicord::module_go_live,
        maicord::module_go_live_snapshot,
        maicord::module_go_live_preview,
};

} // namespace maicord
