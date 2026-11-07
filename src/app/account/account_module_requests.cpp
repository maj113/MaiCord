#include "account_module_internal.hpp"
#include "maicord/app/guild/account_guild_community.hpp"
#include "maicord/core/time.hpp"
#include "maicord/app/voice/log.hpp"
#include "maicord/app/chat/threads/account_thread_actions.hpp"
#include "maicord/app/chat/polls/account_message_poll.hpp"
#include "maicord/app/guild/account_guild_invite.hpp"
#include "maicord/app/settings/account_settings_rest.hpp"
#include "maicord/app/gateway/stream_control.hpp"
#include "maicord/protocol/media/attachment_refresh.hpp"
#include "maicord/app/guild/account_guild_assets.hpp"
#include "maicord/app/account/account_logout.hpp"
#include "maicord/app/account/account_profile_edit.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/protocol/messages/typing.hpp"

namespace maicord {
namespace account_module_detail {

RestClientSubmitResult WINAPI module_reaction_users(void* instance, u64 channel_id, u64 message_id,
                                                    u64 emoji_id, Utf8View emoji_name,
                                                    MessageReactionType type,
                                                    u64 after_user_id, u32 limit,
                                                    u64 request_id) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    RestClient* rest = module_rest(instance);
    if (!rest || channel_id == 0 || message_id == 0 || request_id == 0) {
        return invalid;
    }
    u8 target[kReactionUsersMaximumTargetBytes];
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
    RestClient* rest = module_rest(instance);
    if (!rest || channel_id == 0 || request_id == 0) {
        return invalid;
    }
    u8 target[kMessageTypingMaximumTargetBytes];
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

bool WINAPI module_guild_members_request(void* instance,
                                         const GatewayGuildMemberRequest* request) noexcept {
    auto* gateway = module_gateway(instance);
    if (!gateway)
        return false;
    if (!request) {
        gateway_client_cancel_guild_member_request(gateway);
        return true;
    }
    return gateway_client_request_guild_members(gateway, *request);
}
void WINAPI module_guild_members_query_cancel(void* instance) noexcept {
    gateway_client_cancel_guild_member_query(module_gateway(instance));
}
bool WINAPI module_channel_info(void* instance, u64 guild_id) noexcept {
    return gateway_client_request_channel_info(module_gateway(instance), guild_id);
}
void WINAPI module_client_focus(void* instance, bool focused) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (runtime && client_session_metadata_set_focus(&runtime->client_metadata, focused))
        gateway_client_activity_changed(module_gateway(instance));
}

RestClientSubmitResult WINAPI module_account_settings(void* instance, const AccountSettingsRequest* request,
                                                            u64 request_id) noexcept {
    if (!instance || !request_id)
        return {RestClientSubmitError::invalid_input};
    // Settings share the PRIMARY lane with the self-status settings writer;
    // a cancel without a request tries both lanes.
    if (!request) {
        const auto primary = rest_client_cancel_result(module_rest(instance), request_id);
        return primary.error == RestClientSubmitError::none
                   ? primary
                   : rest_client_cancel_result(module_rest(instance, true), request_id);
    }
    return account_settings_submit(module_rest(instance, !account_settings_primary(request->operation)), *request,
                                   request_id);
}
// Thread, forum and poll requests act as the signed-in user of this profile; forum posts and polls also
// need a published identity.
bool admit_thread_action(AccountModuleRuntime& runtime, const ThreadActionRequest& request) noexcept {
    const auto identity = gateway_client_self_status_snapshot(&runtime.coordinator.gateway);
    return identity.self_user_id && request.self_user_id == identity.self_user_id &&
           identity.profile_id == runtime.coordinator.metadata.profile_id;
}
bool admit_thread_creation(AccountModuleRuntime& runtime, const ThreadCreationRequest&) noexcept {
    const auto identity = gateway_client_self_status_snapshot(&runtime.coordinator.gateway);
    return identity.self_user_id && identity.profile_id == runtime.coordinator.metadata.profile_id;
}

RestClientSubmitResult WINAPI module_poll_create(void* instance, u64 channel_id, u64 nonce,
                                                 const PollCreate* poll, u64 request_id) noexcept {
    auto* rest = request_lane(instance, request_id);
    if (!rest)
        return kInvalidSubmit;
    return poll ? account_poll_create_submit(rest, channel_id, nonce, poll, request_id)
                : rest_client_cancel_result(rest, request_id);
}
bool WINAPI module_local_presence(void* instance, const GatewayLocalPresence* input,
                                  GatewayActivitySharing* output) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime) {
        if (output)
            reset_value(output);
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
RestClientSubmitResult WINAPI module_guild_assets(void* instance, const GuildAssetRequest* request,
                                                  u64 request_id) noexcept {
    // The image probe needs no lane or request id.
    if (instance && request && request->operation == GuildAssetOperation::image_probe)
        return {account_guild_assets_probe(request) ? RestClientSubmitError::none
                                                    : RestClientSubmitError::invalid_input};
    return module_lane<account_guild_assets_submit>(instance, request, request_id);
}
bool WINAPI module_profile_image(void* instance, ProfileImageRequest* request) noexcept {
    return instance && account_profile_image(request);
}

} // namespace account_module_detail
namespace {

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

} // namespace
namespace account_module_detail {

bool WINAPI module_voice_request(void* instance, const GatewayVoiceStateUpdate* state) noexcept {
    auto* gateway = module_gateway(instance);
    return gateway && state && gateway_client_request_voice_state(gateway, *state);
}
bool WINAPI module_voice_snapshot(void* instance, GatewayClientVoiceSnapshot* output) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    ensure_video_frames(runtime, gateway);
    return gateway && gateway_client_voice_snapshot(gateway, output);
}
bool WINAPI module_voice_video_snapshot(void* instance, GatewayClientVoiceVideoSnapshot* output) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    auto* gateway = runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
    ensure_video_frames(runtime, gateway);
    return gateway && gateway_client_voice_video_snapshot(gateway, output);
}
bool WINAPI module_voice_video_device(void* instance, GpuDevice* device) noexcept {
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
HANDLE WINAPI module_voice_video_notification(void* instance) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? runtime->video.frame_event : nullptr;
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
// The Gateway session id rides a lurking join and an invite accept, as in the official client.
bool joins_guild(const GuildCommunityRequest& request) noexcept {
    return request.operation == GuildCommunityOperation::guild_join;
}
bool accepts_invite(const GuildInviteRequest& request) noexcept {
    return request.operation == GuildInviteOperation::accept;
}
bool admit_invite(AccountModuleRuntime&, const GuildInviteRequest& request) noexcept {
    return guild_invite_request_valid(request);
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
    // The sound probe needs no lane or request id.
    if (instance && request && request->operation == SoundboardOperation::probe)
        return {account_soundboard_probe(request) ? RestClientSubmitError::none
                                                  : RestClientSubmitError::invalid_input};
    return module_lane<account_soundboard_submit>(instance, request, request_id);
}
bool WINAPI module_soundboard_audio(void* instance, const SoundboardAudioCommand* command) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && account_soundboard_command(&runtime->soundboard,
                                                 user_account_coordinator_gateway(&runtime->coordinator), command,
                                                 clock_monotonic_milliseconds());
}
bool WINAPI module_voice_message(void* instance, const VoiceMessageCommand* command,
                                 VoiceMessageStatus* status) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && account_voice_message_command(&runtime->voice_message, command, status);
}

CaptchaBridgeStatus WINAPI module_action_captcha(void* instance, const AccountActionCaptchaRequest* request) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request)
        return {0, 0, 0, 0, CaptchaBridgePhase::empty, CaptchaBridgeError::invalid_input, false};
    return account_action_captcha(&runtime->action_captcha, user_account_coordinator_rest(&runtime->coordinator),
                                  user_account_coordinator_rest(&runtime->coordinator, true), *request);
}

AccountMediaRequestResult WINAPI module_collection_title(void* instance, const AccountMediaCollectionTitleRequest* request, MediaTrackTitle* title) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request || !title) {
        return rejected_media_request();
    }
    return account_media_worker_collection_title(&runtime->media, *request, title);
}
AccountMediaRequestResult WINAPI
module_catalog_preview_request(void* instance, const AccountMediaCatalogPreviewRequest* request, u64 now_ms,
                               u64 now_seconds) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request) {
        return rejected_media_request();
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
        return rejected_media_request();
    }
    if (operation == AccountLocalPreviewOperation::presented)
        return account_media_worker_local_previews_presented(&runtime->media);
    if (operation == AccountLocalPreviewOperation::uploaded)
        return account_media_worker_local_preview_uploaded(&runtime->media, *request);
    return operation == AccountLocalPreviewOperation::request
               ? account_media_worker_request_local_preview(&runtime->media, *request, now_ms, now_seconds)
               : account_media_worker_retire_local_preview(&runtime->media, *request);
}

RestClientSubmitResult WINAPI module_media_url_refresh(void* instance, Utf8View source,
                                                       u64 request_id) noexcept {
    RestClient* rest = module_rest(instance);
    u8* const body = g_request_body;
    const u32 bytes = attachment_refresh_body(source, {body, kCdnMaximumTargetBytes * 2});
    if (!rest || request_id == 0 || bytes == 0)
        return {RestClientSubmitError::invalid_input};
    return rest_client_submit(
        rest, {reinterpret_cast<const u8*>(kAttachmentRefreshTarget.data), kAttachmentRefreshTarget.bytes},
        {body, bytes},
        kAttachmentRefreshRoute,
        request_id,
        true);
}

RestClientTakeResult WINAPI module_rest_take(void* instance, RestClientResponse* response,
                                             Slice<u8> body_output) noexcept {
    RestClientTakeResult invalid{};
    invalid.error = RestClientTakeError::invalid_input;
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime)
        return invalid;
    if (runtime->logout_request_id)
        return module_rest(instance) ? rest_client_take_response(&runtime->coordinator.details, response, body_output)
                                     : invalid;
    // The startup history page may complete before READY.
    return user_account_coordinator_take(&runtime->coordinator, response, body_output);
}

AccountLogoutResult WINAPI module_logout(void* instance, AccountLogoutOperation operation, u64 request_id,
                                         AccountLogoutResponse* response) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request_id)
        return {RestClientSubmitError::invalid_input, false, false};
    if (operation == AccountLogoutOperation::begin) {
        if ((runtime->logout_request_id && runtime->logout_request_id != request_id) ||
            runtime->logout_submitted)
            return {RestClientSubmitError::busy, false, false};
        runtime->logout_request_id = request_id;
    } else if (runtime->logout_request_id != request_id) {
        return {RestClientSubmitError::invalid_input, false, false};
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
        return rejected_media_request();
    }
    return account_media_worker_request_static_asset(&runtime->media, key, monotonic_now_ms,
                                                     unix_now_seconds);
}

AccountMediaRequestResult WINAPI
module_media_guild_icon_request(void* instance, const AccountMediaGuildIconRequest* request,
                                u64 monotonic_now_ms, u64 unix_now_seconds) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request) {
        return rejected_media_request();
    }
    return account_media_worker_request_guild_icon(&runtime->media, *request, monotonic_now_ms,
                                                   unix_now_seconds);
}

AccountMediaRequestResult WINAPI
module_media_attachment_request(void* instance, const AccountMediaAttachmentRequest* request,
                                u64 monotonic_now_ms, u64 unix_now_seconds) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime || !request) {
        return rejected_media_request();
    }
    return account_media_worker_request_attachment(&runtime->media, *request, monotonic_now_ms,
                                                   unix_now_seconds);
}

AccountMediaRequestResult WINAPI module_media_advance(void* instance, MediaAssetKey key,
                                                      u16 cache_generation, u32 frame_index,
                                                      u64 unix_now_seconds) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    if (!runtime) {
        return rejected_media_request();
    }
    return account_media_worker_advance_animation(&runtime->media, key, cache_generation, frame_index,
                                                  unix_now_seconds);
}

bool WINAPI module_media_take(void* instance, AccountMediaFrame* frame) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && account_media_worker_take_frame(&runtime->media, frame);
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
AccountModuleStatus WINAPI module_stop(void** instance, u32 timeout_ms, u64, u64 unix_now_ms) noexcept {
    if (!instance || !*instance || timeout_ms == 0 || unix_now_ms == 0) {
        return module_failure(AccountModuleError::invalid_input);
    }
    auto* runtime = static_cast<AccountModuleRuntime*>(*instance);
    if (runtime->download) {
        if (!https_file_download_release(&runtime->download->transfer, timeout_ms)) {
            return stop_failure(runtime, AccountModuleError::download);
        }
        pages_release(runtime->download);
        runtime->download = nullptr;
    }
    if (!https_file_upload_release(&runtime->upload, timeout_ms)) {
        return stop_failure(runtime, AccountModuleError::upload);
    }
    runtime->upload_id = 0;
    account_voice_message_release(&runtime->voice_message);
    if (!account_soundboard_stop(&runtime->soundboard, user_account_coordinator_gateway(&runtime->coordinator),
                                 timeout_ms)) {
        return stop_failure(runtime, AccountModuleError::media);
    }
    if (!account_media_worker_stop(&runtime->media, timeout_ms)) {
        return stop_failure(runtime, AccountModuleError::media);
    }
    account_action_captcha_release(&runtime->action_captcha, user_account_coordinator_rest(&runtime->coordinator),
                                   user_account_coordinator_rest(&runtime->coordinator, true));
    const bool rejected =
        runtime->coordinator.state.error == UserAccountCoordinatorError::authentication_rejected;
    // A refreshed token not saved yet gets one more try before its Gateway goes.
    persist_refreshed_credential(runtime, true);
    secure_buffer_release(&runtime->refreshed_credential);
    GatewayUserSessionSnapshot snapshot{};
    const UserAccountCoordinatorResult stopped =
        user_account_coordinator_stop(&runtime->coordinator, timeout_ms, &snapshot);
    bool final_session_saved = true;
    if (!rejected)
        final_session_saved = save_snapshot(runtime, snapshot, true);
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
    pages_release(runtime);
    *instance = nullptr;
    return status;
}

// One host diagnostic sink for the module: REST traces and voice records
// ("voice:" prefixed, routed by the host into the voice log category).
void module_set_trace(RestTraceFunction trace) noexcept {
    rest_client_set_trace(trace);
    voice_log_set_sink(trace);
}

} // namespace account_module_detail
} // namespace maicord
