#pragma once
#include "maicord/app/video_stream_decoders.hpp"
#include <stddef.h>
#include "maicord/app/account_message_search.hpp"
#include "maicord/app/account_media_catalog.hpp"
#include "maicord/app/voice_message.hpp"
#include "maicord/app/account_gif_favorites.hpp"
#include "maicord/app/account_action_captcha.hpp"
#include "maicord/app/account_channel_overview.hpp"
#include "maicord/app/account_guild_admin.hpp"
#include "maicord/app/account_guild_assets.hpp"
#include "maicord/app/account_pinned_messages.hpp"
#include "maicord/app/account_thread_actions.hpp"
#include "maicord/app/account_thread_browser.hpp"
#include "maicord/app/account_thread_creation.hpp"
#include "maicord/app/account_forum_post.hpp"
#include "maicord/app/account_logout.hpp"
#include "maicord/app/account_bulk_message_ack.hpp"
#include "maicord/app/account_stage.hpp"
#include "maicord/app/account_soundboard.hpp"
#include "maicord/app/account_profile_edit.hpp"
#include "maicord/protocol/account_settings.hpp"
#include "maicord/protocol/cdn_url.hpp"

#include "maicord/app/account_media.hpp"
#include "maicord/app/account_video_playback.hpp"
#include "maicord/app/account_upload.hpp"
#include "maicord/app/account_download.hpp"
#include "maicord/app/account_module_version.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/remote_auth_client.hpp"
#include "maicord/app/user_auth_coordinator.hpp"
#include "maicord/app/captcha_bridge.hpp"
#include "maicord/protocol/message_history.hpp"
#include "maicord/protocol/message_create.hpp"
#include "maicord/protocol/message_delete.hpp"
#include "maicord/protocol/message_edit.hpp"
#include "maicord/protocol/message_reaction.hpp"
#include "maicord/protocol/message_poll.hpp"
#include "maicord/protocol/reaction_users.hpp"
#include "maicord/protocol/message_typing.hpp"
#include "maicord/protocol/private_conversation.hpp"
#include "maicord/protocol/relationships.hpp"
#include "maicord/protocol/guild_invite.hpp"
#include "maicord/protocol/message_interaction.hpp"
#include "maicord/protocol/user_social.hpp"
#include "maicord/protocol/guild_community.hpp"
#include "maicord/protocol/guild_moderation.hpp"
#include "maicord/protocol/user_guild_settings.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {

inline constexpr u32 kAccountHistoryResponseBytes = 256 * 1024;
inline constexpr wchar_t kAccountModuleFileNameWide[] = L"MaiCordAccount.dll";

enum class AccountModuleError : u8 {
    none,
    invalid_input,
    memory,
    profile,
    coordinator_start,
    coordinator,
    session_store,
    media_start,
    media,
    upload,
    download,
};

struct AccountModuleStatus {
    u64 profile_id;
    u64 frecency_settings_revision;
    u64 retry_at_ms;
    u64 next_wake_at_ms;
    u32 native_error;
    u32 gateway_sources;
    u32 gateway_ready;
    u32 session_saves;
    u32 session_save_skips;
    u32 session_save_failures;
    u32 session_native_error;
    u16 http_status;
    u8 coordinator_phase;
    u8 coordinator_error;
    u8 rest_error;
    u8 gateway_error;
    u8 session_error;
    AccountModuleError error;
    bool running;
    bool failed;
    AccountRequirement required_action;
};

struct AccountModuleStartResult {
    void* instance;
    AccountModuleStatus status;
    u32 native_error;
    u8 profile_error;
    u8 session_error;
    AccountModuleError error;
    bool session_loaded;

    constexpr bool ok() const noexcept {
        return error == AccountModuleError::none && instance != nullptr;
    }
};

enum class AccountLoginPhase : u8 {
    empty,
    idle,
    password_pending,
    mfa_required,
    mfa_pending,
    captcha_required,
    remote_starting,
    remote_qr_ready,
    remote_mobile_pending,
    remote_ticket_exchange,
    authenticated,
    rejected,
    failed,
    stopped,
    verification_pending,
    recovery_pending,
    recovery_sent,
    recovery_phone_required,
    recovery_rejected,
    recovery_rate_limited,
    reset_pending,
    reset_mfa_pending,
    reset_rejected,
    reset_rate_limited,
    phone_verify_pending,
    phone_resend_pending,
    phone_rejected,
    phone_rate_limited,
    phone_token_ready,
    phone_token_taken,
    browser_starting,
    browser_waiting,
    browser_exchange_pending,
    browser_expired,
    browser_failed,
};

enum class AccountLoginError : u8 {
    none,
    invalid_input,
    memory,
    coordinator,
    profile,
    remote_auth,
    stop,
    browser_auth,
};

struct AccountLoginStatus {
    u64 user_id;
    u64 error_code;
    u64 generation;
    u64 request_id;
    u64 remote_ticket_error_code;
    u32 native_error;
    u32 remote_ticket_response_bytes;
    u32 coordinator_response_bytes;
    u32 remote_ticket_decode_error_offset;
    u32 coordinator_decode_error_offset;
    u16 http_status;
    u8 mfa_capabilities;
    UserAuthCoordinatorError coordinator_error;
    UserAuthCoordinatorEventKind event;
    AccountLoginPhase phase;
    AccountLoginError error;
    RemoteAuthClientEventKind remote_event;
    RemoteAuthClientError remote_error;
    RemoteAuthHandshakeError remote_handshake_error;
    RemoteAuthHandshakeStage remote_handshake_stage;
    RemoteAuthError remote_protocol_error;
    RemoteAuthCryptoError remote_crypto_error;
    Base64Error remote_base64_error;
    UserAuthResponseKind remote_ticket_response_kind;
    UserAuthDecodeError remote_ticket_decode_error;
    JsonScanError remote_ticket_scan_error;
    JsonReadError remote_ticket_read_error;
    RestContentType remote_ticket_content_type;
    RestContentEncoding remote_ticket_content_encoding;
    RestBodyKind remote_ticket_body_kind;
    CaptchaProvider captcha_provider;
    bool captcha_invisible;
    bool profile_saved;
    UserAuthDecodeError coordinator_decode_error;
    JsonScanError coordinator_scan_error;
    JsonReadError coordinator_read_error;
    RestClientError coordinator_transport_error;
    UserAuthTransitionError coordinator_transition_error;
    UserAuthResponseKind coordinator_response_kind;
    RestContentType coordinator_content_type;
    RestContentEncoding coordinator_content_encoding;
    RestBodyKind coordinator_body_kind;
    u64 retry_at_ms;

    u64 browser_generation;
    u64 browser_candidate_id;
    u64 browser_deadline_ms;
    u32 browser_native_error;
    u8 browser_phase;
    u8 browser_error;

    constexpr bool ok() const noexcept {
        return error == AccountLoginError::none;
    }
};

using AccountModuleStartFunction = AccountModuleStartResult(WINAPI*)(const wchar_t* profile_path,
                                                                     const wchar_t* session_path,
                                                                     u64 monotonic_now_ms,
                                                                     u64 unix_now_ms) noexcept;
using AccountModulePumpFunction = AccountModuleStatus(WINAPI*)(void* instance, u64 monotonic_now_ms,
                                                               u64 unix_now_ms) noexcept;
using AccountModuleNotificationHandlesFunction = u32(WINAPI*)(void* instance, HANDLE* output,
                                                              u32 capacity) noexcept;
using AccountModuleGatewaySourceFunction = void*(WINAPI*)(void* instance, u32 source_index) noexcept;
using AccountModuleGatewayPopFunction = GatewayClientPopResult(WINAPI*)(void* source,
                                                                        GatewayClientEvent* event,
                                                                        Slice<u8> payload_output) noexcept;
using AccountModuleGuildSubscribeFunction =
    bool(WINAPI*)(void* instance, const GatewayGuildSubscription* subscription) noexcept;
using AccountModuleGatewayRefreshFunction = GatewayStateRecoveryResult(WINAPI*)(void* instance) noexcept;
using AccountModulePrivateConversationFunction = RestClientSubmitResult(WINAPI*)(
    void* instance, PrivateConversationOperation operation, u64 id, u64 request_id) noexcept;
using AccountModuleAttachmentUploadFunction = RestClientSubmitResult(WINAPI*)(
    void* instance, u64 channel_id, u32 id, Utf8View filename, u64 file_bytes, u64 request_id) noexcept;
using AccountModuleUploadFunction = AccountUploadResult(WINAPI*)(
    void* instance, AccountUploadOperation operation, u64 id, const AccountUploadRequest* request) noexcept;
using AccountModuleHistorySubmitFunction = RestClientSubmitResult(WINAPI*)(
    void* instance, u64 channel_id, const MessageHistoryQuery* query, u64 request_id) noexcept;
using AccountModuleMessageSubmitFunction = RestClientSubmitResult(WINAPI*)(
    void* instance, u64 channel_id, Utf8View content, u64 nonce, u64 reply_message_id,
    ConstSlice<UploadedAttachment> attachments, ConstSlice<u64> sticker_ids, u64 request_id,
    bool mention_reply_author, u32 flags) noexcept;
using AccountModuleMessageForwardFunction = RestClientSubmitResult(WINAPI*)(
    void* instance, u64 destination_channel_id, Utf8View caption, u64 nonce,
    MessageForwardReference source, u64 request_id) noexcept;
using AccountModuleMessageDeleteFunction = RestClientSubmitResult(WINAPI*)(void* instance,
                                                                           u64 channel_id,
                                                                           u64 message_id,
                                                                           u64 request_id) noexcept;
using AccountModuleMessageEditFunction = RestClientSubmitResult(WINAPI*)(
    void* instance, u64 channel_id, u64 message_id, Utf8View content, u64 request_id) noexcept;
using AccountModuleMessageReactionFunction = RestClientSubmitResult(WINAPI*)(
    void* instance, u64 channel_id, u64 message_id, u64 emoji_id, Utf8View emoji_name,
    MessageReactionType type, bool add, u64 request_id) noexcept;
using AccountModuleReactionUsersFunction = RestClientSubmitResult(WINAPI*)(
    void* instance, u64 channel_id, u64 message_id, u64 emoji_id, Utf8View emoji_name,
    MessageReactionType type, u64 after_user_id, u32 limit, u64 request_id) noexcept;
using AccountModuleMessageTypingFunction = RestClientSubmitResult(WINAPI*)(void* instance,
                                                                           u64 channel_id,
                                                                           u64 request_id) noexcept;
using AccountModuleMediaUrlRefreshFunction =
    RestClientSubmitResult(WINAPI*)(void* instance, Utf8View source, u64 request_id) noexcept;
using AccountModuleRestTakeFunction = RestClientTakeResult(WINAPI*)(
    void* instance, RestClientResponse* response, Slice<u8> body_output) noexcept;
using AccountModuleMediaRequestFunction = AccountMediaRequestResult(WINAPI*)(void* instance,
                                                                             MediaAssetKey key,
                                                                             u64 monotonic_now_ms,
                                                                             u64 unix_now_seconds) noexcept;
using AccountModuleMediaGuildIconRequestFunction =
    AccountMediaRequestResult(WINAPI*)(void* instance, const AccountMediaGuildIconRequest* request,
                                       u64 monotonic_now_ms, u64 unix_now_seconds) noexcept;
using AccountModuleMediaAttachmentRequestFunction =
    AccountMediaRequestResult(WINAPI*)(void* instance, const AccountMediaAttachmentRequest* request,
                                       u64 monotonic_now_ms, u64 unix_now_seconds) noexcept;
using AccountModuleMediaAdvanceFunction = AccountMediaRequestResult(WINAPI*)(void* instance,
                                                                             MediaAssetKey key,
                                                                             u16 cache_generation,
                                                                             u32 frame_index,
                                                                             u64 unix_now_seconds) noexcept;
using AccountModuleMediaTakeFunction = bool(WINAPI*)(void* instance, AccountMediaFrame* frame) noexcept;
using AccountModuleRuntimeBytesFunction = u32(WINAPI*)() noexcept;
using AccountModuleStopFunction = AccountModuleStatus(WINAPI*)(void** instance, u32 timeout_ms,
                                                               u64 monotonic_now_ms,
                                                               u64 unix_now_ms) noexcept;
using AccountModuleLoginStartFunction = AccountLoginStatus(WINAPI*)(void** instance,
                                                                    const wchar_t* profile_path) noexcept;
using AccountModuleLoginSubmitPasswordFunction = AccountLoginStatus(WINAPI*)(
    void* instance, const SecureBuffer* login, const SecureBuffer* password) noexcept;
using AccountModuleLoginSubmitVerificationFunction =
    AccountLoginStatus(WINAPI*)(void* instance, const SecureBuffer* link_token) noexcept;
using AccountModuleLoginSubmitRecoveryFunction =
    AccountLoginStatus(WINAPI*)(void* instance, const SecureBuffer* login) noexcept;
using AccountModuleLoginSubmitResetFunction = AccountLoginStatus(WINAPI*)(
    void* instance, const SecureBuffer* link_token, const SecureBuffer* password) noexcept;
using AccountModuleLoginSubmitPhoneVerifyFunction =
    AccountLoginStatus(WINAPI*)(void* instance, const SecureBuffer* code) noexcept;
using AccountModuleLoginSubmitPhoneResendFunction = AccountLoginStatus(WINAPI*)(void* instance) noexcept;
using AccountModuleLoginTakePhoneResetTokenFunction =
    SecureBufferResult(WINAPI*)(void* instance, u64 expected_generation, SecureBuffer* output) noexcept;
using AccountModuleLoginSubmitMfaFunction = AccountLoginStatus(WINAPI*)(void* instance,
                                                                        const SecureBuffer* code,
                                                                        UserAuthMfaType type) noexcept;
using AccountModuleLoginSendSmsFunction = AccountLoginStatus(WINAPI*)(void* instance) noexcept;
using AccountModuleLoginStartRemoteAuthFunction = AccountLoginStatus(WINAPI*)(void* instance) noexcept;
using AccountModuleLoginCopyRemoteAuthQrFunction = bool(WINAPI*)(void* instance, Slice<char> output,
                                                                 u32* bytes) noexcept;
using AccountModuleLoginCopyRemoteAuthUserFunction = bool(WINAPI*)(void* instance, Slice<char> output,
                                                                   u32* bytes) noexcept;
using AccountModuleLoginCancelRemoteAuthFunction = AccountLoginStatus(WINAPI*)(void* instance) noexcept;
using AccountModuleLoginPumpFunction = AccountLoginStatus(WINAPI*)(void* instance) noexcept;
using AccountModuleLoginCopyCaptchaSecretFunction = SecureBufferResult(WINAPI*)(
    void* instance, CaptchaChallengeSecretKind kind, SecureBuffer* output) noexcept;
using AccountModuleLoginSubmitCaptchaFunction =
    AccountLoginStatus(WINAPI*)(void* instance, const SecureBuffer* solution) noexcept;
using AccountModuleLoginCopyWebAuthnChallengeFunction =
    SecureBufferResult(WINAPI*)(void* instance, SecureBuffer* output) noexcept;
using AccountModuleLoginNotificationHandleFunction = HANDLE(WINAPI*)(void* instance) noexcept;
using AccountModuleLoginStopFunction = AccountLoginStatus(WINAPI*)(void** instance,
                                                                   u32 timeout_ms) noexcept;

// Operations: 0 GET a profile (guild: its server); 1-3 PATCH the bio, pronouns
// or global name (value); 4 GET collectibles product `user`; 5 GET public
// applications (value: comma-separated IDs); 6 GET activity metadata (value:
// the session, guild: its application or 0). Older modules reject 5 and 6.
using AccountModuleProfileFunction = RestClientSubmitResult(WINAPI*)(void* instance, u64 user,
                                                                     u64 guild, u64 request_id,
                                                                     u8 operation,
                                                                     Utf8View value) noexcept;
using AccountModuleRelationshipsFunction =
    RestClientSubmitResult(WINAPI*)(void* instance, RelationshipOperation operation,
                                    u64 user_id, Utf8View username, u64 request_id) noexcept;

// v121: stateless decoders the client uses on REST bodies it already took.
// Each entry is exactly the protocol function of the same name; the client
// links forwarders (account_module_decoders.cpp) instead of a second copy of
// the Gateway message JSON decoder and channel router. Inputs, scratch and
// outputs are caller owned; nothing is retained across the call.
struct AccountModuleDecoders {
    GatewayEventDecodeResult(WINAPI* message_create)(JsonValue, Slice<u32>, u32, Slice<char>,
                                                     GatewayMessageEvent*) noexcept;
    GatewayEventDecodeResult(WINAPI* message_create_mentions)(JsonValue, Slice<u32>, u32, Slice<char>, Slice<u64>,
                                                              u64, GatewayMessageEvent*) noexcept;
    GatewayEventDecodeResult(WINAPI* message_create_users)(JsonValue, Slice<u32>, u32, Slice<char>, Slice<u64>,
                                                           Slice<GatewayMessageNamedUser>, u64,
                                                           GatewayMessageEvent*) noexcept;
    GatewayEventDecodeResult(WINAPI* message_create_rich)(JsonValue, Slice<u32>, u32, Slice<char>, Slice<u64>,
                                                          Slice<u64>, Slice<GatewayMessageNamedUser>,
                                                          const GatewayMessageRichScratch&, u64,
                                                          GatewayMessageEvent*) noexcept;
    GatewayEventDecodeResult(WINAPI* message_snapshot_core)(JsonValue, Slice<u32>, u32, Slice<char>,
                                                            Slice<GatewayMessageNamedUser>,
                                                            GatewayMessageEvent*) noexcept;
    GatewayEventDecodeResult(WINAPI* message_snapshot_extract)(JsonValue, Slice<u32>, u32, JsonValue*,
                                                               bool*) noexcept;
    GatewayEventDecodeResult(WINAPI* message_poll)(JsonValue, const JsonIndexedDocument&, u32, Slice<char>, u32*,
                                                   const GatewayMessagePollScratch&, GatewayMessagePoll*,
                                                   bool) noexcept;
    GatewayClientDispatchRouteResult(WINAPI* channel_create)(JsonValue, Slice<u32>, u32, Slice<char>, u64,
                                                             GatewayClientEvent*, ConstSlice<u8>*) noexcept;
    GatewayClientDispatchRouteResult(WINAPI* private_channel)(JsonValue, Slice<u32>, u32, Slice<char>,
                                                              GatewayClientEvent*, ConstSlice<u8>*) noexcept;
};
static_assert(sizeof(AccountModuleDecoders) == 72);

// v122: the CDN URL functions of protocol/cdn_url.hpp, entry for entry. The
// client links forwarders (account_module_cdn_urls.cpp) instead of a second
// copy of the URL parsers; guild_icon_hash_valid stays in both images because
// the client's Gateway user records check it per record. Stateless and caller
// owned like AccountModuleDecoders.
struct AccountModuleCdnUrls {
    AttachmentUrlResult (*parse_signed_attachment_url)(Utf8View, u64, Slice<u8>) noexcept;
    MediaUrlResult (*parse_media_url)(Utf8View, Slice<u8>) noexcept;
    bool (*catalog_provider_target_valid)(CdnHost, ConstSlice<u8>) noexcept;
    bool (*sticker_json_target_valid)(ConstSlice<u8>) noexcept;
    bool (*soundboard_target_valid)(ConstSlice<u8>) noexcept;
    MediaUrlResult (*parse_catalog_preview_url)(Utf8View, Slice<u8>, MediaAttachmentFormat, u32,
                                                u32) noexcept;
    MediaAttachmentFormat (*catalog_favorite_format)(Utf8View) noexcept;
    u32 (*media_url_log_fields)(Utf8View, Slice<char>) noexcept;
    CdnTargetResult (*write_media_proxy_target)(ConstSlice<u8>, CdnHost, u32, u32, CdnMediaFormat,
                                                Slice<u8>, bool) noexcept;
    CdnTargetResult (*write_custom_emoji_cdn_target)(u64, bool, u32, Slice<u8>) noexcept;
    u32 (*cdn_guild_icon_variant)(Utf8View) noexcept;
    CdnTargetResult (*write_guild_icon_cdn_target)(u64, Utf8View, u32, Slice<u8>, bool) noexcept;
    CdnTargetResult (*write_user_avatar_cdn_target)(u64, Utf8View, u32, Slice<u8>) noexcept;
    CdnTargetResult (*write_channel_icon_cdn_target)(u64, Utf8View, u32, Slice<u8>) noexcept;
    CdnTargetResult (*write_sticker_cdn_target)(u64, u32, Slice<u8>) noexcept;
    Utf8View (*cdn_host_name)(CdnHost) noexcept;
};
static_assert(sizeof(AccountModuleCdnUrls) == 128);

struct AccountModuleApi {
    u32 version;
    u32 bytes;
    AccountModuleStartFunction start;
    AccountModulePumpFunction pump;
    AccountModuleNotificationHandlesFunction notification_handles;
    AccountModuleGatewaySourceFunction gateway_source;
    AccountModuleGatewayPopFunction gateway_pop;
    AccountModuleGuildSubscribeFunction guild_subscribe;
    AccountModuleGatewayRefreshFunction gateway_refresh;
    AccountModulePrivateConversationFunction private_conversation;
    AccountModuleAttachmentUploadFunction attachment_upload;
    AccountModuleUploadFunction upload;
    AccountModuleHistorySubmitFunction history_submit;
    AccountModuleMessageSubmitFunction message_submit;
    AccountModuleMessageDeleteFunction message_delete;
    AccountModuleMessageEditFunction message_edit;
    AccountModuleMessageReactionFunction message_reaction;
    AccountModuleReactionUsersFunction reaction_users;
    AccountModuleMessageTypingFunction message_typing;
    AccountModuleMediaUrlRefreshFunction media_url_refresh;
    AccountModuleRestTakeFunction rest_take;
    AccountModuleMediaRequestFunction media_request;
    AccountModuleMediaGuildIconRequestFunction media_guild_icon_request;
    AccountModuleMediaAttachmentRequestFunction media_attachment_request;
    AccountModuleMediaAdvanceFunction media_advance;
    AccountModuleMediaTakeFunction media_take;
    AccountModuleRuntimeBytesFunction runtime_bytes;
    AccountModuleStopFunction stop;
    AccountModuleLoginStartFunction login_start;
    AccountModuleLoginSubmitPasswordFunction login_submit_password;
    AccountModuleLoginSubmitMfaFunction login_submit_mfa;
    AccountModuleLoginSendSmsFunction login_send_sms;
    AccountModuleLoginStartRemoteAuthFunction login_start_remote_auth;
    AccountModuleLoginCopyRemoteAuthQrFunction login_copy_remote_auth_qr;
    AccountModuleLoginCopyRemoteAuthUserFunction login_copy_remote_auth_user;
    AccountModuleLoginCancelRemoteAuthFunction login_cancel_remote_auth;
    AccountModuleLoginPumpFunction login_pump;
    AccountModuleLoginCopyCaptchaSecretFunction login_copy_captcha_secret;
    AccountModuleLoginSubmitCaptchaFunction login_submit_captcha;
    AccountModuleLoginCopyWebAuthnChallengeFunction login_copy_webauthn_challenge;
    AccountModuleLoginNotificationHandleFunction login_notification_handle;
    AccountModuleLoginStopFunction login_stop;
    AccountModuleProfileFunction profile_get;
    RestClientSubmitResult(WINAPI* application_command)(void* instance, u64 channel_id, u64 guild_id,
                                                        u64 application_id, u64 nonce, Utf8View data,
                                                        u64 request_id, u8 operation) noexcept;
    void (*set_rest_trace)(RestTraceFunction trace) noexcept;
    AccountModuleLoginSubmitVerificationFunction login_submit_verification;
    AccountModuleRelationshipsFunction relationships;
    RestClientSubmitResult(WINAPI* message_ack)(void* instance, u64 channel_id, u64 message_id,
                                                ConstSlice<u8> body, u64 request_id) noexcept;
    AccountModuleLoginSubmitRecoveryFunction login_submit_recovery;
    AccountModuleLoginStartFunction login_start_reset;
    AccountModuleLoginSubmitResetFunction login_submit_reset;
    AccountModuleLoginSubmitPhoneVerifyFunction login_submit_phone_verify;
    AccountModuleLoginSubmitPhoneResendFunction login_submit_phone_resend;
    AccountModuleLoginTakePhoneResetTokenFunction login_take_phone_reset_token;
    AccountVideoResult(WINAPI* video_select)(void*, const AccountVideoSelection*) noexcept;
    AccountVideoResult(WINAPI* video_control)(void*, const AccountVideoControl*) noexcept;
    AccountVideoPlaybackStatus(WINAPI* video_status)(void*) noexcept;
    bool(WINAPI* video_take)(void*, AccountVideoFrame*) noexcept;
    bool(WINAPI* video_acknowledge)(void*, const AccountVideoFrame*) noexcept;
    RestClientSubmitResult(WINAPI* message_search)(void*, AccountMessageSearchOperation,
                                                   const MessageSearchQuery*,
                                                   u64 request_id) noexcept;
    AccountLoginStatus(WINAPI* login_start_browser)(void*, u64 generation, u32 timeout_ms) noexcept;
    SecureBufferResult(WINAPI* login_copy_browser_url)(void*, u64 generation, SecureBuffer*) noexcept;
    GatewayClientSelfStatusSnapshot(WINAPI* self_status_snapshot)(void*) noexcept;
    RestClientSubmitResult(WINAPI* self_status_submit)(void*, u64 profile_id, u64 self_user_id,
                                                       SelfStatusSettingsOperation,
                                                       GatewayUserStatus, u64 request_id,
                                                       u64 unix_now_ms) noexcept;
    bool(WINAPI* self_status_complete)(void*, u64 profile_id, u64 self_user_id, u64 request_id,
                                       const RestClientResponse*, Utf8View) noexcept;
    RestClientSubmitResult(WINAPI* media_catalog)(void*, AccountMediaCatalogOperation,
                                                  const MediaCatalogQuery*, u64) noexcept;
    AccountMediaRequestResult(WINAPI* catalog_preview_request)(void*,
                                                               const AccountMediaCatalogPreviewRequest*,
                                                               u64, u64) noexcept;
    RestClientSubmitResult(WINAPI* gif_favorites)(void*, AccountGifFavoritesOperation,
                                                  ConstSlice<u8> patch_json,
                                                  u64 request_id) noexcept;
    RestClientSubmitResult(WINAPI* channel_overview)(void*, ChannelOverviewOperation, u64 channel_id,
                                                     const ChannelOverviewPatch*,
                                                     u64 request_id) noexcept;
    // Null request cancels only an unsent batch; no wire message is emitted.
    bool(WINAPI* guild_members_request)(void*, const GatewayGuildMemberRequest*) noexcept;
    // A null request cancels this request ID on the shared DETAILS worker.
    RestClientSubmitResult(WINAPI* pinned_messages)(void*, const MessagePinsRequest*,
                                                    u64 request_id) noexcept;
    AccountModuleMessageForwardFunction message_forward;
    AccountDownloadResult(WINAPI* download)(void*, AccountDownloadOperation, u64,
                                            const AccountDownloadRequest*) noexcept;
    // A null request cancels this request ID on the shared DETAILS worker.
    RestClientSubmitResult(WINAPI* thread_action)(void*, const ThreadActionRequest*,
                                                  u64 request_id) noexcept;
    // A null request cancels this request ID on the shared DETAILS worker.
    RestClientSubmitResult(WINAPI* thread_browser)(void*, const ThreadBrowserRequest*,
                                                   u64 request_id) noexcept;
    // A null request cancels this request ID on the shared DETAILS worker.
    RestClientSubmitResult(WINAPI* thread_creation)(void*, const ThreadCreationRequest*,
                                                    u64 request_id) noexcept;
    // A null request cancels this request ID on the shared DETAILS worker.
    RestClientSubmitResult(WINAPI* forum_post)(void*, const ForumPostRequest*,
                                               u64 request_id) noexcept;
    AccountMediaRequestResult(WINAPI* media_local_preview)(void*, AccountLocalPreviewOperation,
                                                           const AccountMediaLocalPreviewRequest*,
                                                           u64 monotonic_now_ms,
                                                           u64 unix_now_seconds) noexcept;
    AccountLogoutResult(WINAPI* logout)(void*, AccountLogoutOperation, u64 request_id,
                                        AccountLogoutResponse*) noexcept;
    // Null entries or a zero count cancels only this PRIMARY request ID.
    RestClientSubmitResult(WINAPI* bulk_message_ack)(void*,
                                                     const BulkMessageAckEntry* entries,
                                                     u32 count, u64 request_id) noexcept;
    // Null request cancels only this DETAILS request ID.
    RestClientSubmitResult(WINAPI* poll)(void*, const PollRequest*, u64 request_id) noexcept;
    // Null request cancels only this DETAILS request ID. v112 adds guild_leave.
    RestClientSubmitResult(WINAPI* guild_admin)(void*, const GuildAdminRequest*, u64 request_id) noexcept;
    // Desired guild voice channel; a zero channel leaves. The worker serializes Opcode 4.
    bool(WINAPI* voice_request)(void*, const GatewayVoiceStateUpdate*) noexcept;
    bool(WINAPI* voice_snapshot)(void*, GatewayClientVoiceSnapshot*) noexcept;
    // Loopback browser CAPTCHA bridge for the current login challenge. Start
    // only on explicit user action; the URL holds the single-use state.
    AccountLoginStatus(WINAPI* login_start_captcha_bridge)(void*, u64 generation, u32 timeout_ms) noexcept;
    CaptchaBridgeStatus(WINAPI* login_captcha_bridge_status)(void*) noexcept;
    SecureBufferResult(WINAPI* login_copy_captcha_bridge_url)(void*, u64 generation, SecureBuffer*) noexcept;
    AccountLoginStatus(WINAPI* login_cancel_captcha_bridge)(void*) noexcept;
    // Local playback volume (0-200 %, default 100) and mute for one remote
    // voice user; never sent to the server. Self-deafen goes through voice_request.
    bool(WINAPI* voice_user_audio)(void*, u64 user_id, u32 gain_percent, bool muted) noexcept;
    // Microphone/output devices, volumes and input mode; the latest call wins.
    bool(WINAPI* voice_audio_configure)(void*, const VoiceAudioSettings*) noexcept;
    bool(WINAPI* voice_audio_status)(void*, VoiceAudioStatus*) noexcept;
    // Invite resolve/accept on the DETAILS worker; a null request cancels this ID.
    RestClientSubmitResult(WINAPI* guild_invite)(void*, const GuildInviteRequest*, u64 request_id) noexcept;
    // settings-proto/2 emoji favorites/frecency on the PRIMARY lane; updates
    // carry only groups 5, 6 and 13. Cancel takes an empty patch.
    RestClientSubmitResult(WINAPI* emoji_preferences)(void*, AccountGifFavoritesOperation,
                                                      ConstSlice<u8> patch_json, u64 request_id) noexcept;
    // settings-proto/1 on PRIMARY, account pages (devices, apps, profile) on
    // DETAILS. Bodies are built in the module; a null request cancels this ID.
    // Since v112 a settings_patch may carry the guild folders group alone.
    RestClientSubmitResult(WINAPI* account_settings)(void*, const AccountSettingsRequest*,
                                                     u64 request_id) noexcept;
    // CAPTCHA for a signed-in user action: the module decodes the rejected body,
    // runs the loopback bridge and arms the single X-Captcha-* retry itself.
    CaptchaBridgeStatus(WINAPI* action_captcha)(void*, const AccountActionCaptchaRequest*) noexcept;
    // Poll message create on DETAILS; a null poll cancels only this request ID.
    RestClientSubmitResult(WINAPI* poll_create)(void*, u64 channel_id, u64 nonce, const PollCreate*,
                                                u64 request_id) noexcept;
    // Group DM create, membership, rename, leave and call ringing on DETAILS;
    // a null request cancels only this request ID.
    RestClientSubmitResult(WINAPI* private_channel)(void*, const PrivateChannelActionRequest*,
                                                    u64 request_id) noexcept;
    // StatusSettings custom status (settings-proto field 11.2); false until the
    // status group is authoritative.
    bool(WINAPI* custom_status_snapshot)(void*, SelfCustomStatus*) noexcept;
    // Replaces (null clears) the custom status on PRIMARY, keeping the rest of
    // the status group; completes through self_status_complete.
    RestClientSubmitResult(WINAPI* custom_status_submit)(void*, u64 profile_id, u64 self_user_id,
                                                         const SelfCustomStatus*, u64 request_id) noexcept;
    // Notification settings PATCH on DETAILS; a null patch cancels only this ID.
    RestClientSubmitResult(WINAPI* guild_settings)(void*, const UserGuildSettingsPatch*,
                                                   u64 request_id) noexcept;
    // Recent mentions GET on DETAILS; a null query cancels only this ID.
    RestClientSubmitResult(WINAPI* mentions)(void*, const MentionsQuery*, u64 request_id) noexcept;
    // Server icon/banner, role icons, emoji, stickers, 2FA level and deletion on
    // DETAILS (and, since v112, server creation). Picked images are prepared in
    // the module; image_probe prepares only (no transport, any request ID). A
    // null request cancels only this ID.
    RestClientSubmitResult(WINAPI* guild_assets)(void*, const GuildAssetRequest*, u64 request_id) noexcept;
    // Remote participants with an active camera/screen stream in the current
    // call ("video available") and receive counters.
    bool(WINAPI* voice_video_snapshot)(void*, GatewayClientVoiceVideoSnapshot*) noexcept;
    // Stop (false) or resume (true) receiving one user's video (op 15 sink wants).
    // gateway_stream_video_user(owner) addresses the watched Go Live stream.
    bool(WINAPI* voice_video_wanted)(void*, u64 user_id, bool wanted) noexcept;
    // The renderer's multithread-protected D3D11 device for call video decode
    // (null detaches it for streams that start later). The module referenced it.
    bool(WINAPI* voice_video_device)(void*, ID3D11Device*) noexcept;
    // Latest decoded picture of a remote video SSRC (voice_video_snapshot users[].ssrc).
    // On success the texture and each non-null shader view hold one reference.
    // The caller releases these references after it submits the GPU read commands.
    bool(WINAPI* voice_video_frame)(void*, u32 ssrc, VideoStreamFrame* output) noexcept;
    // Go Live watch: one stream at a time, only while the call is in the
    // key's channel (gateway_stream_key_for(guild, channel, owner)). A null
    // key stops watching. Video renders through voice_video_frame with the
    // snapshot's tagged video_ssrc; audio plays on its own bus.
    bool(WINAPI* stream_watch)(void*, const GatewayStreamKey* key) noexcept;
    bool(WINAPI* stream_snapshot)(void*, GatewayClientStreamSnapshot*) noexcept;
    // Stream audio volume 0-200 % (default 100) on top of the output volume.
    bool(WINAPI* stream_volume)(void*, u32 percent) noexcept;
    // GET /streams/{key}/preview on DETAILS; the body ({"url"}) arrives through
    // rest_take (gateway_stream_preview_url decodes it). Not end-to-end
    // encrypted; never log the URL. A null key cancels only this request ID.
    RestClientSubmitResult(WINAPI* stream_preview)(void*, const GatewayStreamKey* key,
                                                   u64 request_id) noexcept;
    // Stage channels on DETAILS: request to speak, speaker/audience moves and
    // stage instances (start, topic, end). A null request cancels only this ID.
    RestClientSubmitResult(WINAPI* stage)(void*, const StageRequest*, u64 request_id) noexcept;
    // Message component and modal submit interactions (POST /interactions)
    // on the command lane; the module adds the Gateway session_id. The
    // response is 204; lifecycle events arrive as interaction_* dispatches.
    RestClientSubmitResult(WINAPI* message_interaction)(void*, const MessageInteractionRequest*,
                                                        u64 request_id) noexcept;
    // v113: user notes, report menus and submissions, and message flag edits
    // (Remove embeds) on DETAILS. A null request cancels only this ID.
    RestClientSubmitResult(WINAPI* user_social)(void*, const UserSocialRequest*, u64 request_id) noexcept;
    // v113: membership screening, onboarding and scheduled events on DETAILS
    // (guild_community.hpp builds bodies). A null request cancels only this ID.
    RestClientSubmitResult(WINAPI* guild_community)(void*, const GuildCommunityRequest*,
                                                    u64 request_id) noexcept;
    // v113: local presence (automatic idle, the shared game) merged into
    // opcode 3 by the worker; a null input only reads "Share my activity".
    bool(WINAPI* local_presence)(void*, const GatewayLocalPresence*, GatewayActivitySharing*) noexcept;
    // v113: StatusSettings.show_current_game PATCH on PRIMARY; completes
    // through self_status_complete. Also v113: private_channel carries
    // message request accept/ignore and DM safety warning acknowledgements.
    RestClientSubmitResult(WINAPI* activity_sharing)(void*, u64 profile_id, u64 self_user_id, bool share,
                                                     u64 request_id) noexcept;
    // v114: soundboard sends, guild/default sound lists and the Manage
    // Expressions page on DETAILS (creates take the large-body path). probe
    // checks a picked file locally (any request ID). A null request cancels
    // only this ID.
    RestClientSubmitResult(WINAPI* soundboard)(void*, const SoundboardRequest*, u64 request_id) noexcept;
    // v114: local soundboard playback: settings, received sounds (call only),
    // previews and stop. Sounds are fetched from the CDN, decoded and mixed into
    // the call output (or a preview output outside calls). Null command: status.
    bool(WINAPI* soundboard_audio)(void*, const SoundboardAudioCommand*, SoundboardAudioStatus*) noexcept;
    // v115: recorded voice messages: microphone capture (the voice input
    // settings), Opus/Ogg encoding and the preview player run in the module;
    // the client copies the finished stream out to upload it. Null command: status.
    bool(WINAPI* voice_message)(void*, const VoiceMessageCommand*, VoiceMessageStatus*) noexcept;
    // v120: the profile editor's saves (PATCH /users/@me, /users/@me/profile
    // and their per-server forms; image data URIs take the large-body path),
    // its profile load and owned collectibles on DETAILS. A null request
    // cancels only this ID.
    RestClientSubmitResult(WINAPI* profile_edit)(void*, const ProfileEditRequest*, u64 request_id) noexcept;
    // v120: probes a picked avatar/banner or crops it into a data URI exactly
    // as the official client's cropper does (local, no transport).
    bool(WINAPI* profile_image)(void*, ProfileImageRequest*) noexcept;
    // v121: stateless REST body decoders (no instance; see AccountModuleDecoders).
    AccountModuleDecoders decoders;
    // v122: stateless CDN URL functions (no instance; see AccountModuleCdnUrls).
    AccountModuleCdnUrls cdn_urls;
    // v125: members search, bulk ban and ban search on DETAILS. A null
    // request cancels only this ID (a superseded search).
    RestClientSubmitResult(WINAPI* guild_moderation)(void*, const GuildModerationRequest*,
                                                     u64 request_id) noexcept;
    // v126: cancels only the member name query, including a retained retry.
    // Visible-author batches keep their separate cancellation path.
    void(WINAPI* guild_members_query_cancel)(void*) noexcept;
};

// This is the only executable/DLL boundary. The module loads the DPAPI profile
// itself and returns an opaque handle. Login copies caller-owned locked input
// into the DLL's locked arena; neither plaintext nor internal worker pointers
// are retained across the ABI. Gateway sources are borrowed only for immediate
// UI-thread mailbox drains. Completed REST bodies are copied into caller-owned
// output before stop/unload. Decoded media frames are borrowed until the next
// media request/pump and must be uploaded during the media_take call sequence
// before the DLL can be stopped or unloaded.
static_assert(sizeof(AccountModuleStatus) == 72);
static_assert(sizeof(AccountModuleStartResult) == 88);
static_assert(sizeof(AccountLoginStatus) == 136);
static_assert(sizeof(AccountModuleApi) == 1176);
static_assert(offsetof(AccountModuleApi, guild_members_query_cancel) == 1168);
static_assert(offsetof(AccountModuleApi, guild_moderation) == 1160);
static_assert(offsetof(AccountModuleApi, decoders) == 960);
static_assert(offsetof(AccountModuleApi, cdn_urls) == 1032);
static_assert(offsetof(AccountModuleApi, poll) == 632);
static_assert(offsetof(AccountModuleApi, guild_admin) == 640);
static_assert(offsetof(AccountModuleApi, voice_snapshot) == 656);
static_assert(offsetof(AccountModuleApi, voice_user_audio) == 696);
static_assert(offsetof(AccountModuleApi, voice_audio_status) == 712);
static_assert(offsetof(AccountModuleApi, guild_invite) == 720);
static_assert(offsetof(AccountModuleApi, emoji_preferences) == 728);
static_assert(offsetof(AccountModuleApi, account_settings) == 736);
static_assert(offsetof(AccountModuleApi, action_captcha) == 744);
static_assert(offsetof(AccountModuleApi, poll_create) == 752);
static_assert(offsetof(AccountModuleApi, private_channel) == 760);
static_assert(offsetof(AccountModuleApi, custom_status_snapshot) == 768);
static_assert(offsetof(AccountModuleApi, mentions) == 792);
static_assert(offsetof(AccountModuleApi, guild_assets) == 800);
static_assert(offsetof(AccountModuleApi, voice_video_snapshot) == 808);
static_assert(offsetof(AccountModuleApi, voice_video_wanted) == 816);
static_assert(offsetof(AccountModuleApi, voice_video_device) == 824);
static_assert(offsetof(AccountModuleApi, voice_video_frame) == 832);
static_assert(offsetof(AccountModuleApi, stream_watch) == 840);
static_assert(offsetof(AccountModuleApi, stream_snapshot) == 848);
static_assert(offsetof(AccountModuleApi, stream_volume) == 856);
static_assert(offsetof(AccountModuleApi, stream_preview) == 864);
static_assert(offsetof(AccountModuleApi, stage) == 872);
static_assert(offsetof(AccountModuleApi, message_interaction) == 880);
static_assert(offsetof(AccountModuleApi, user_social) == 888);
static_assert(offsetof(AccountModuleApi, guild_community) == 896);
static_assert(offsetof(AccountModuleApi, local_presence) == 904);
static_assert(offsetof(AccountModuleApi, activity_sharing) == 912);
static_assert(offsetof(AccountModuleApi, voice_message) == 936);
static_assert(offsetof(AccountModuleApi, soundboard) == 920);
static_assert(offsetof(AccountModuleApi, soundboard_audio) == 928);

using AccountModuleGetApiFunction = bool(WINAPI*)(u32 requested_version, AccountModuleApi* api) noexcept;

} // namespace maicord
