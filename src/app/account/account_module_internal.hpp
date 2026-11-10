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

#include "maicord/app/account/login/account_action_captcha.hpp"
#include "maicord/app/media/account_download.hpp"
#include "maicord/app/media/account_upload.hpp"
#include "maicord/app/voice/video_stream_decoders.hpp"
#include "maicord/app/gateway/go_live_devices.hpp"
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
    wchar_t profile_path[kAccountProfileMaximumPathCharacters];
    // A token READY refreshed that the saved profile does not hold yet; its save is
    // retried with the session autosave, and once more at stop.
    SecureBuffer refreshed_credential;
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
    // Go Live producers (capture, encoder, stream audio) on the renderer's
    // device; installed into the current gateway runtime.
    GoLiveDevices go_live;
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
RestClientSubmitResult module_lane(void* instance, const decltype(lane_request(Submit))* request,
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

AccountModuleStatus module_failure(AccountModuleError error) noexcept;
__declspec(noinline) AccountMediaRequestResult rejected_media_request() noexcept;
bool save_snapshot(AccountModuleRuntime* runtime, const GatewayUserSessionSnapshot& snapshot,
                   bool include_sequence) noexcept;
// Takes a token READY refreshed into REST and saves it with the profile (account_profile_save's
// atomic write). A failed save keeps it; with `retry` (autosave, stop) a kept one is saved
// again. Rejected credentials are never saved.
void persist_refreshed_credential(AccountModuleRuntime* runtime, bool retry) noexcept;
AccountModuleStatus build_status(AccountModuleRuntime* runtime) noexcept;
AccountModuleStatus stop_failure(AccountModuleRuntime* runtime, AccountModuleError error) noexcept;
bool admit_thread_action(AccountModuleRuntime& runtime, const ThreadActionRequest& request) noexcept;
bool admit_thread_creation(AccountModuleRuntime& runtime, const ThreadCreationRequest&) noexcept;

template <typename Request>
bool admit_published_identity(AccountModuleRuntime& runtime, const Request&) noexcept {
    const auto identity = gateway_client_self_status_snapshot(&runtime.coordinator.gateway);
    return identity.available && identity.self_user_id && identity.profile_id &&
           identity.profile_id == runtime.coordinator.metadata.profile_id;
}

bool joins_guild(const GuildCommunityRequest& request) noexcept;
bool accepts_invite(const GuildInviteRequest& request) noexcept;
bool admit_invite(AccountModuleRuntime&, const GuildInviteRequest& request) noexcept;

} // namespace account_module_detail
} // namespace maicord
