#pragma once
// Shared by account_module.cpp and its parts (account_module_*.cpp); private to them.
#include "maicord/app/voice/account_soundboard.hpp"
#include "maicord/app/voice/account_voice_message.hpp"
#include "maicord/app/account/account_module_api.hpp"
#include "maicord/app/chat/polls/account_poll_create.hpp"

#include "maicord/app/account/account_session_store.hpp"
#include "maicord/app/account/client_session_metadata.hpp"
#include "maicord/app/media/account_media_worker.hpp"
#include "maicord/app/account/user_account_coordinator.hpp"
#include "maicord/app/account/user_client_configuration.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"

#include "maicord/app/account/login/account_action_captcha.hpp"
#include "maicord/app/media/account_download.hpp"
#include "maicord/app/media/account_upload.hpp"
#include "maicord/app/voice/video_stream_decoders.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace account_module_detail {}
using namespace account_module_detail;
namespace account_module_detail {

inline constexpr u64 kSessionAutosaveIntervalMs = 30'000;
inline constexpr u64 kSessionAutosaveRetryMs = 5'000;
inline constexpr u64 kMessageHistoryRouteId = 0x4D53474849535452ull;

extern u8 g_request_body[kMessageCreateMaximumBodyBytes];

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
    AccountSessionSaveState save_state;
    u32 session_saves;
    u32 session_save_skips;
    u32 session_save_failures;
    u32 session_native_error;
    AccountSessionStoreError session_error;
    u64 logout_request_id;
    bool logout_submitted;
    AccountActionCaptcha action_captcha;
    // Call video decoders; installed into the current gateway runtime.
    VideoStreamDecoders video;
    // Soundboard fetch/decode/cache and the effect mixer calls render.
    AccountSoundboard soundboard;
    // Voice message recording and preview.
    AccountVoiceMessage voice_message;
    // Views point into this stable owner. REST and replacement Gateway workers
    // share the same launch identity until the authenticated account stops.
    UserClientConfigurationStorage client_configuration;
    ClientSessionMetadata client_metadata;
    // The account's cookies, shared by its REST and Gateway workers; the
    // persistent ones are saved with the session.
    CookieJar cookies;
};

RestClient* module_rest(void* instance, bool details = false) noexcept;
GatewayClient* module_gateway(void* instance) noexcept;

constexpr RestClientSubmitResult kInvalidSubmit{RestClientSubmitError::invalid_input};

// Module entries that only forward are instantiated from these templates, so each feature names its
// function and nothing else. Parameters are those of the forwarded function.
//
// RestEntry: a REST function taking the runtime's PRIMARY (or, with `Details`, DETAILS) lane first.
template <auto Fn, bool Details = false>
struct RestEntry;
template <typename R, typename... A, R (*Fn)(RestClient*, A...) noexcept, bool Details>
struct RestEntry<Fn, Details> {
    static R WINAPI call(void* instance, A... arguments) noexcept {
        return Fn(module_rest(instance, Details), static_cast<A&&>(arguments)...);
    }
};
// GatewayEntry: a Gateway query answering false without a running Gateway.
template <auto Fn>
struct GatewayEntry;
template <typename Client, typename... A, bool (*Fn)(Client*, A...) noexcept>
struct GatewayEntry<Fn> {
    static bool WINAPI call(void* instance, A... arguments) noexcept {
        auto* gateway = module_gateway(instance);
        return gateway && Fn(gateway, static_cast<A&&>(arguments)...);
    }
};

RestClient* request_lane(void* instance, u64 request_id) noexcept;

template <auto Value>
inline constexpr bool kUnset = false;
template <>
inline constexpr bool kUnset<nullptr> = true;
// The request type of a lane's submit function (its optional last parameter is the Gateway session id).
template <typename Request>
Request lane_request(RestClientSubmitResult (*)(RestClient*, const Request*, u64) noexcept);
template <typename Request>
Request lane_request(RestClientSubmitResult (*)(RestClient*, const Request*, u64, Utf8View) noexcept);

// The common body of a request entry: a null request cancels `request_id`, any other goes to `submit`,
// which validates its own requests.
using LaneSubmit = RestClientSubmitResult (*)(RestClient*, const void*, u64) noexcept;
__declspec(noinline) inline RestClientSubmitResult lane_submit(void* instance, const void* request, u64 request_id,
                                                              LaneSubmit submit) noexcept {
    auto* rest = request_lane(instance, request_id);
    if (!rest)
        return kInvalidSubmit;
    return request ? submit(rest, request, request_id) : rest_client_cancel_result(rest, request_id);
}

// A request entry on the DETAILS lane for `Submit`. Optional hooks:
//   Admit(runtime, request)  false refuses the request before it is sent;
//   Session(request)         true sends the account's Gateway session id with the request (Submit then
//                            takes it as its last parameter), as the official client does.
template <auto Submit, auto Admit = nullptr, auto Session = nullptr>
RestClientSubmitResult WINAPI module_lane(void* instance, const decltype(lane_request(Submit))* request,
                                          u64 request_id) noexcept {
    if constexpr (kUnset<Admit> && kUnset<Session>) {
        using Request = decltype(lane_request(Submit));
        return lane_submit(instance, request, request_id,
                           [](RestClient* rest, const void* payload, u64 id) noexcept {
                               return Submit(rest, static_cast<const Request*>(payload), id);
                           });
    } else {
        auto* rest = request_lane(instance, request_id);
        if (!rest)
            return kInvalidSubmit;
        if (!request)
            return rest_client_cancel_result(rest, request_id);
        auto* runtime = static_cast<AccountModuleRuntime*>(instance);
        if constexpr (!kUnset<Admit>)
            if (!Admit(*runtime, *request))
                return kInvalidSubmit;
        if constexpr (kUnset<Session>) {
            return Submit(rest, request, request_id);
        } else {
            GatewayUserSessionSnapshot session{};
            const bool known = Session(*request) &&
                               user_account_coordinator_snapshot_session(&runtime->coordinator, &session) &&
                               session.has_resume;
            const auto result =
                Submit(rest, request, request_id,
                       known ? Utf8View{session.resume.session_id, session.resume.session_id_bytes} : Utf8View{});
            SecureZeroMemory(&session, sizeof(session));
            return result;
        }
    }
}

GatewayClientSelfStatusSnapshot WINAPI module_self_status_snapshot(void* instance) noexcept;
RestClientSubmitResult WINAPI module_self_status_submit(void* instance, u64 profile_id,
                                                        u64 self_user_id,
                                                        SelfStatusSettingsOperation operation,
                                                        GatewayUserStatus status, u64 request_id,
                                                        u64 unix_now_ms) noexcept;
bool WINAPI module_self_status_complete(void* instance, u64 profile_id, u64 self_user_id, u64 request_id,
                                        const RestClientResponse* response, Utf8View body) noexcept;
AccountModuleStatus module_failure(AccountModuleError error) noexcept;
__declspec(noinline) AccountMediaRequestResult rejected_media_request() noexcept;
AccountLoginStatus WINAPI module_login_start(void** instance, const wchar_t* profile_path) noexcept;
AccountLoginStatus WINAPI module_login_start_reset(void** instance, const wchar_t* profile_path) noexcept;
AccountLoginStatus WINAPI module_login_submit_password(void* instance, const SecureBuffer* login,
                                                       const SecureBuffer* password) noexcept;
AccountLoginStatus WINAPI module_login_submit_recovery(void* instance, const SecureBuffer* login) noexcept;
AccountLoginStatus WINAPI module_login_start_browser(void* instance, u64 generation,
                                                     u32 timeout_ms) noexcept;
SecureBufferResult WINAPI module_login_copy_browser_url(void* instance, u64 generation,
                                                        SecureBuffer* output) noexcept;
AccountLoginStatus WINAPI module_login_submit_verification(void* instance,
                                                           const SecureBuffer* link_token) noexcept;
AccountLoginStatus WINAPI module_login_submit_phone_verify(void* instance,
                                                           const SecureBuffer* code) noexcept;
AccountLoginStatus WINAPI module_login_submit_phone_resend(void* instance) noexcept;
SecureBufferResult WINAPI module_login_take_phone_reset_token(void* instance, u64 generation,
                                                              SecureBuffer* output) noexcept;
AccountLoginStatus WINAPI module_login_submit_mfa(void* instance, const SecureBuffer* code,
                                                  UserAuthMfaType type) noexcept;
AccountLoginStatus WINAPI module_login_submit_reset(void* instance, const SecureBuffer* link_token,
                                                    const SecureBuffer* password) noexcept;
AccountLoginStatus WINAPI module_login_send_sms(void* instance) noexcept;
AccountLoginStatus WINAPI module_login_start_remote_auth(void* instance) noexcept;
bool WINAPI module_login_copy_remote_auth_qr(void* instance, Slice<char> output, u32* bytes) noexcept;
bool WINAPI module_login_copy_remote_auth_user(void* instance, Slice<char> output, u32* bytes) noexcept;
AccountLoginStatus WINAPI module_login_cancel_remote_auth(void* instance) noexcept;
SecureBufferResult WINAPI module_login_copy_webauthn_challenge(void* instance,
                                                               SecureBuffer* output) noexcept;
bool copy_path(const wchar_t* source, wchar_t* destination) noexcept;
bool save_snapshot(AccountModuleRuntime* runtime, const GatewayUserSessionSnapshot& snapshot,
                   bool include_sequence) noexcept;
AccountModuleStatus build_status(AccountModuleRuntime* runtime) noexcept;
AccountModuleStatus stop_failure(AccountModuleRuntime* runtime, AccountModuleError error) noexcept;
RestClientSubmitResult WINAPI module_reaction_users(void* instance, u64 channel_id, u64 message_id,
                                                    u64 emoji_id, Utf8View emoji_name,
                                                    MessageReactionType type,
                                                    u64 after_user_id, u32 limit,
                                                    u64 request_id) noexcept;
RestClientSubmitResult WINAPI module_message_typing(void* instance, u64 channel_id,
                                                    u64 request_id) noexcept;
bool WINAPI module_guild_members_request(void* instance,
                                         const GatewayGuildMemberRequest* request) noexcept;
void WINAPI module_guild_members_query_cancel(void* instance) noexcept;
bool WINAPI module_channel_info(void* instance, u64 guild_id) noexcept;
void WINAPI module_client_focus(void* instance, bool focused) noexcept;
RestClientSubmitResult WINAPI module_account_settings(void* instance, const AccountSettingsRequest* request,
                                                            u64 request_id) noexcept;
bool admit_thread_action(AccountModuleRuntime& runtime, const ThreadActionRequest& request) noexcept;
bool admit_thread_creation(AccountModuleRuntime& runtime, const ThreadCreationRequest&) noexcept;

template <typename Request>
bool admit_published_identity(AccountModuleRuntime& runtime, const Request&) noexcept {
    const auto identity = gateway_client_self_status_snapshot(&runtime.coordinator.gateway);
    return identity.available && identity.self_user_id && identity.profile_id &&
           identity.profile_id == runtime.coordinator.metadata.profile_id;
}

RestClientSubmitResult WINAPI module_poll_create(void* instance, u64 channel_id, u64 nonce,
                                                 const PollCreate* poll, u64 request_id) noexcept;
bool WINAPI module_local_presence(void* instance, const GatewayLocalPresence* input,
                                  GatewayActivitySharing* output) noexcept;
RestClientSubmitResult WINAPI module_activity_sharing(void* instance, u64 profile_id, u64 self_user_id,
                                                      bool share, u64 request_id) noexcept;
bool WINAPI module_custom_status_snapshot(void* instance, SelfCustomStatus* output) noexcept;
RestClientSubmitResult WINAPI module_custom_status_submit(void* instance, u64 profile_id, u64 self_user_id,
                                                          const SelfCustomStatus* custom,
                                                          u64 request_id) noexcept;
RestClientSubmitResult WINAPI module_guild_assets(void* instance, const GuildAssetRequest* request,
                                                  u64 request_id) noexcept;
bool WINAPI module_profile_image(void* instance, ProfileImageRequest* request) noexcept;
bool WINAPI module_voice_request(void* instance, const GatewayVoiceStateUpdate* state) noexcept;
bool WINAPI module_voice_snapshot(void* instance, GatewayClientVoiceSnapshot* output) noexcept;
bool WINAPI module_voice_video_snapshot(void* instance, GatewayClientVoiceVideoSnapshot* output) noexcept;
bool WINAPI module_voice_video_device(void* instance, ID3D11Device* device) noexcept;
bool WINAPI module_voice_video_frame(void* instance, u32 ssrc, VideoStreamFrame* output) noexcept;
HANDLE WINAPI module_voice_video_notification(void* instance) noexcept;
bool WINAPI module_stream_watch(void* instance, const GatewayStreamKey* key) noexcept;
RestClientSubmitResult WINAPI module_stream_preview(void* instance, const GatewayStreamKey* key,
                                                    u64 request_id) noexcept;
bool joins_guild(const GuildCommunityRequest& request) noexcept;
bool accepts_invite(const GuildInviteRequest& request) noexcept;
bool admit_invite(AccountModuleRuntime&, const GuildInviteRequest& request) noexcept;
bool WINAPI module_voice_audio_configure(void* instance, const VoiceAudioSettings* settings) noexcept;
RestClientSubmitResult WINAPI module_soundboard(void* instance, const SoundboardRequest* request,
                                                u64 request_id) noexcept;
bool WINAPI module_soundboard_audio(void* instance, const SoundboardAudioCommand* command) noexcept;
bool WINAPI module_voice_message(void* instance, const VoiceMessageCommand* command,
                                 VoiceMessageStatus* status) noexcept;
CaptchaBridgeStatus WINAPI module_action_captcha(void* instance, const AccountActionCaptchaRequest* request) noexcept;
AccountMediaRequestResult WINAPI module_collection_title(void* instance, const AccountMediaCollectionTitleRequest* request, MediaTrackTitle* title) noexcept;
AccountMediaRequestResult WINAPI
module_catalog_preview_request(void* instance, const AccountMediaCatalogPreviewRequest* request, u64 now_ms,
                               u64 now_seconds) noexcept;
AccountMediaRequestResult WINAPI module_media_local_preview(void* instance,
                                                            AccountLocalPreviewOperation operation,
                                                            const AccountMediaLocalPreviewRequest* request,
                                                            u64 now_ms, u64 now_seconds) noexcept;
RestClientSubmitResult WINAPI module_media_url_refresh(void* instance, Utf8View source,
                                                       u64 request_id) noexcept;
RestClientTakeResult WINAPI module_rest_take(void* instance, RestClientResponse* response,
                                             Slice<u8> body_output) noexcept;
AccountLogoutResult WINAPI module_logout(void* instance, AccountLogoutOperation operation, u64 request_id,
                                         AccountLogoutResponse* response) noexcept;
AccountMediaRequestResult WINAPI module_media_request(void* instance, MediaAssetKey key,
                                                      u64 monotonic_now_ms, u64 unix_now_seconds) noexcept;
AccountMediaRequestResult WINAPI
module_media_guild_icon_request(void* instance, const AccountMediaGuildIconRequest* request,
                                u64 monotonic_now_ms, u64 unix_now_seconds) noexcept;
AccountMediaRequestResult WINAPI
module_media_attachment_request(void* instance, const AccountMediaAttachmentRequest* request,
                                u64 monotonic_now_ms, u64 unix_now_seconds) noexcept;
AccountMediaRequestResult WINAPI module_media_advance(void* instance, MediaAssetKey key,
                                                      u16 cache_generation, u32 frame_index,
                                                      u64 unix_now_seconds) noexcept;
bool WINAPI module_media_take(void* instance, AccountMediaFrame* frame) noexcept;
AccountVideoResult WINAPI module_video_select(void* instance,
                                              const AccountVideoSelection* selection) noexcept;
AccountVideoResult WINAPI module_video_control(void* instance,
                                               const AccountVideoControl* command) noexcept;
AccountVideoPlaybackStatus WINAPI module_video_status(void* instance) noexcept;
bool WINAPI module_video_take(void* instance, AccountVideoFrame* frame) noexcept;
bool WINAPI module_video_acknowledge(void* instance, const AccountVideoFrame* frame) noexcept;
AccountModuleStatus WINAPI module_stop(void** instance, u32 timeout_ms, u64, u64 unix_now_ms) noexcept;
void module_set_trace(RestTraceFunction trace) noexcept;

} // namespace account_module_detail
} // namespace maicord
