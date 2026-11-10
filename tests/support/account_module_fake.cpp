#include "account_module_fake.hpp"

#include "maicord/app/account/account_logout.hpp"
#include "maicord/app/account/account_profile_edit.hpp"
#include "maicord/app/account/account_relationships.hpp"
#include "maicord/app/account/account_user_social.hpp"
#include "maicord/app/account/login/account_action_captcha.hpp"
#include "maicord/app/account/user_account_coordinator.hpp"
#include "maicord/app/chat/account_private_channel.hpp"
#include "maicord/app/chat/messages/account_bulk_message_ack.hpp"
#include "maicord/app/chat/messages/account_message_ack.hpp"
#include "maicord/app/chat/messages/account_pinned_messages.hpp"
#include "maicord/app/chat/polls/account_message_poll.hpp"
#include "maicord/app/chat/polls/account_poll_create.hpp"
#include "maicord/app/chat/search/account_message_search.hpp"
#include "maicord/app/chat/threads/account_forum_post.hpp"
#include "maicord/app/chat/threads/account_thread_actions.hpp"
#include "maicord/app/chat/threads/account_thread_browser.hpp"
#include "maicord/app/chat/threads/account_thread_creation.hpp"
#include "maicord/app/gateway/go_live_devices.hpp"
#include "maicord/app/gateway/stream_control.hpp"
#include "maicord/app/guild/account_channel_overview.hpp"
#include "maicord/app/guild/account_guild_admin.hpp"
#include "maicord/app/guild/account_guild_assets.hpp"
#include "maicord/app/guild/account_guild_automation.hpp"
#include "maicord/app/guild/account_guild_community.hpp"
#include "maicord/app/guild/account_guild_invite.hpp"
#include "maicord/app/guild/account_guild_moderation.hpp"
#include "maicord/app/media/account_download.hpp"
#include "maicord/app/media/account_gif_favorites.hpp"
#include "maicord/app/media/account_media_catalog.hpp"
#include "maicord/app/media/account_media_worker.hpp"
#include "maicord/app/media/account_upload.hpp"
#include "maicord/app/notifications/account_notification_settings.hpp"
#include "maicord/app/settings/account_settings_rest.hpp"
#include "maicord/app/voice/account_soundboard.hpp"
#include "maicord/app/voice/account_stage.hpp"
#include "maicord/app/voice/account_voice_message.hpp"
#include "maicord/app/voice/video_stream_decoders.hpp"
#include "maicord/protocol/messages/interaction.hpp"
#include "maicord/protocol/messages/interaction_data.hpp"

namespace maicord {
namespace {

// The answer without a hook: what the module returns without an instance.
template <typename Result>
Result unavailable() noexcept {
    return {};
}
template <>
RestClientSubmitResult unavailable<RestClientSubmitResult>() noexcept {
    return {RestClientSubmitError::invalid_input};
}
template <>
RestClientTakeResult unavailable<RestClientTakeResult>() noexcept {
    return {0, RestClientTakeError::invalid_input};
}
template <>
AccountLoginStatus unavailable<AccountLoginStatus>() noexcept {
    AccountLoginStatus status{};
    status.error = AccountLoginError::invalid_input;
    return status;
}
template <>
SecureBufferResult unavailable<SecureBufferResult>() noexcept {
    return {0, 0, SecureBufferError::invalid_input};
}
template <>
GatewayClientPopResult unavailable<GatewayClientPopResult>() noexcept {
    return {0, GatewayClientPopError::invalid_input};
}
template <>
GatewayStateRecoveryResult unavailable<GatewayStateRecoveryResult>() noexcept {
    return GatewayStateRecoveryResult::unavailable;
}
template <>
AccountMediaRequestResult unavailable<AccountMediaRequestResult>() noexcept {
    AccountMediaRequestResult result{};
    result.error = AccountMediaError::invalid_input;
    return result;
}
template <>
AccountUploadResult unavailable<AccountUploadResult>() noexcept {
    AccountUploadResult result{};
    result.submit_error = HttpsFileUploadError::invalid_input;
    return result;
}
template <>
AccountDownloadResult unavailable<AccountDownloadResult>() noexcept {
    AccountDownloadResult result{};
    result.submit_error = HttpsFileDownloadError::invalid_input;
    return result;
}
template <>
AccountLogoutResult unavailable<AccountLogoutResult>() noexcept {
    return {RestClientSubmitError::invalid_input, false, false};
}
template <>
AccountVideoResult unavailable<AccountVideoResult>() noexcept {
    AccountVideoResult result{};
    result.error = AccountVideoError::invalid_input;
    return result;
}
template <>
AccountModuleStatus unavailable<AccountModuleStatus>() noexcept {
    AccountModuleStatus status{};
    status.error = AccountModuleError::invalid_input;
    status.failed = true;
    return status;
}
template <>
AccountModuleStartResult unavailable<AccountModuleStartResult>() noexcept {
    AccountModuleStartResult result{};
    result.error = AccountModuleError::invalid_input;
    return result;
}

template <typename Result, typename Hook, typename... Arguments>
Result run(Hook hook, Arguments... arguments) noexcept {
    return hook ? hook(arguments...) : unavailable<Result>();
}

} // namespace

AccountModuleStartResult account_module_start(const wchar_t* profile_path, const wchar_t* session_path,
                                              u64 monotonic_now_ms, u64 unix_now_ms) noexcept {
    return run<AccountModuleStartResult>(g_account_fake.start, profile_path, session_path, monotonic_now_ms,
        unix_now_ms);
}
AccountModuleStatus account_module_pump(void* instance, u64 monotonic_now_ms, u64 unix_now_ms) noexcept {
    return run<AccountModuleStatus>(g_account_fake.pump, instance, monotonic_now_ms, unix_now_ms);
}
u32 account_module_notification_handles(void* instance, HANDLE* output, u32 capacity) noexcept {
    return run<u32>(g_account_fake.notification_handles, instance, output, capacity);
}
void* account_module_gateway_source(void* instance) noexcept {
    return run<void*>(g_account_fake.gateway_source, instance);
}
GatewayClientPopResult account_module_gateway_pop(void* source, GatewayClientEvent* event,
                                                  Slice<u8> payload_output) noexcept {
    return run<GatewayClientPopResult>(g_account_fake.gateway_pop, source, event, payload_output);
}
bool account_module_guild_subscribe(void* instance, const GatewayGuildSubscription* subscription) noexcept {
    return run<bool>(g_account_fake.guild_subscribe, instance, subscription);
}
GatewayStateRecoveryResult account_module_gateway_refresh(void* instance) noexcept {
    return run<GatewayStateRecoveryResult>(g_account_fake.gateway_refresh, instance);
}
RestClientSubmitResult account_module_private_conversation(void* instance, PrivateConversationOperation operation,
                                                           u64 id, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.private_conversation, instance, operation, id, request_id);
}
RestClientSubmitResult account_module_attachment_upload(void* instance, u64 channel_id, u32 id, Utf8View filename,
                                                        u64 file_bytes, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.attachment_upload, instance, channel_id, id, filename,
        file_bytes, request_id);
}
AccountUploadResult account_module_upload(void* instance, AccountUploadOperation operation, u64 id,
                                          const AccountUploadRequest* request) noexcept {
    return run<AccountUploadResult>(g_account_fake.upload, instance, operation, id, request);
}
RestClientSubmitResult account_module_history_submit(void* instance, u64 channel_id, const MessageHistoryQuery* query,
                                                     u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.history_submit, instance, channel_id, query, request_id);
}
RestClientSubmitResult account_module_message_submit(void* instance, u64 channel_id, Utf8View content, u64 nonce,
                                                     u64 reply_message_id, ConstSlice<UploadedAttachment> attachments,
                                                     ConstSlice<u64> sticker_ids, u64 request_id,
                                                     bool mention_reply_author, u32 flags) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.message_submit, instance, channel_id, content, nonce,
        reply_message_id, attachments, sticker_ids, request_id, mention_reply_author, flags);
}
RestClientSubmitResult account_module_message_delete(void* instance, u64 channel_id, u64 message_id,
                                                     u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.message_delete, instance, channel_id, message_id, request_id);
}
RestClientSubmitResult account_module_message_edit(void* instance, u64 channel_id, u64 message_id, Utf8View content,
                                                   u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.message_edit, instance, channel_id, message_id, content,
        request_id);
}
RestClientSubmitResult account_module_message_reaction(void* instance, u64 channel_id, u64 message_id, u64 emoji_id,
                                                       Utf8View emoji_name, MessageReactionType type, bool add,
                                                       u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.message_reaction, instance, channel_id, message_id, emoji_id,
        emoji_name, type, add, request_id);
}
RestClientSubmitResult account_module_reaction_users(void* instance, u64 channel_id, u64 message_id, u64 emoji_id,
                                                     Utf8View emoji_name, MessageReactionType type, u64 after_user_id,
                                                     u32 limit, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.reaction_users, instance, channel_id, message_id, emoji_id,
        emoji_name, type, after_user_id, limit, request_id);
}
RestClientSubmitResult account_module_message_typing(void* instance, u64 channel_id, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.message_typing, instance, channel_id, request_id);
}
RestClientSubmitResult account_module_media_url_refresh(void* instance, Utf8View source, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.media_url_refresh, instance, source, request_id);
}
RestClientTakeResult account_module_rest_take(void* instance, RestClientResponse* response,
                                              Slice<u8> body_output) noexcept {
    return run<RestClientTakeResult>(g_account_fake.rest_take, instance, response, body_output);
}
AccountMediaRequestResult account_module_media_request(void* instance, MediaAssetKey key, u64 monotonic_now_ms,
                                                       u64 unix_now_seconds) noexcept {
    return run<AccountMediaRequestResult>(g_account_fake.media_request, instance, key, monotonic_now_ms,
        unix_now_seconds);
}
AccountMediaRequestResult account_module_media_guild_icon_request(void* instance,
                                                                  const AccountMediaGuildIconRequest* request,
                                                                  u64 monotonic_now_ms,
                                                                  u64 unix_now_seconds) noexcept {
    return run<AccountMediaRequestResult>(g_account_fake.media_guild_icon_request, instance, request,
        monotonic_now_ms, unix_now_seconds);
}
AccountMediaRequestResult account_module_media_attachment_request(void* instance,
                                                                  const AccountMediaAttachmentRequest* request,
                                                                  u64 monotonic_now_ms,
                                                                  u64 unix_now_seconds) noexcept {
    return run<AccountMediaRequestResult>(g_account_fake.media_attachment_request, instance, request,
        monotonic_now_ms, unix_now_seconds);
}
AccountMediaRequestResult account_module_media_advance(void* instance, MediaAssetKey key, u16 cache_generation,
                                                       u32 frame_index, u64 unix_now_seconds) noexcept {
    return run<AccountMediaRequestResult>(g_account_fake.media_advance, instance, key, cache_generation, frame_index,
        unix_now_seconds);
}
bool account_module_media_take(void* instance, AccountMediaFrame* frame) noexcept {
    return run<bool>(g_account_fake.media_take, instance, frame);
}
AccountModuleStatus account_module_stop(void** instance, u32 timeout_ms, u64 monotonic_now_ms,
                                        u64 unix_now_ms) noexcept {
    return run<AccountModuleStatus>(g_account_fake.stop, instance, timeout_ms, monotonic_now_ms, unix_now_ms);
}
AccountLoginStatus account_module_login_start(void** instance, const wchar_t* profile_path) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_start, instance, profile_path);
}
AccountLoginStatus account_module_login_submit_password(void* instance, const SecureBuffer* login,
                                                        const SecureBuffer* password) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_submit_password, instance, login, password);
}
AccountLoginStatus account_module_login_submit_mfa(void* instance, const SecureBuffer* code,
                                                   UserAuthMfaType type) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_submit_mfa, instance, code, type);
}
AccountLoginStatus account_module_login_send_sms(void* instance) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_send_sms, instance);
}
AccountLoginStatus account_module_login_start_remote_auth(void* instance) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_start_remote_auth, instance);
}
bool account_module_login_copy_remote_auth_qr(void* instance, Slice<char> output, u32* bytes) noexcept {
    return run<bool>(g_account_fake.login_copy_remote_auth_qr, instance, output, bytes);
}
bool account_module_login_copy_remote_auth_user(void* instance, Slice<char> output, u32* bytes) noexcept {
    return run<bool>(g_account_fake.login_copy_remote_auth_user, instance, output, bytes);
}
AccountLoginStatus account_module_login_cancel_remote_auth(void* instance) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_cancel_remote_auth, instance);
}
AccountLoginStatus account_module_login_pump(void* instance) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_pump, instance);
}
SecureBufferResult account_module_login_copy_webauthn_challenge(void* instance, SecureBuffer* output) noexcept {
    return run<SecureBufferResult>(g_account_fake.login_copy_webauthn_challenge, instance, output);
}
HANDLE account_module_login_notification_handle(void* instance) noexcept {
    return run<HANDLE>(g_account_fake.login_notification_handle, instance);
}
AccountLoginStatus account_module_login_stop(void** instance, u32 timeout_ms) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_stop, instance, timeout_ms);
}
RestClientSubmitResult account_module_profile_get(void* instance, u64 user, u64 guild, u64 request_id, u8 operation,
                                                  Utf8View value) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.profile_get, instance, user, guild, request_id, operation,
        value);
}
RestClientSubmitResult account_module_application_command(void* instance, u64 channel_id, u64 guild_id,
                                                          u64 application_id, u64 nonce, Utf8View data,
                                                          u64 request_id, u8 operation) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.application_command, instance, channel_id, guild_id,
        application_id, nonce, data, request_id, operation);
}
AccountLoginStatus account_module_login_submit_verification(void* instance, const SecureBuffer* link_token) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_submit_verification, instance, link_token);
}
RestClientSubmitResult account_module_relationships(void* instance, RelationshipOperation operation, u64 user_id,
                                                    Utf8View username, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.relationships, instance, operation, user_id, username,
        request_id);
}
RestClientSubmitResult account_module_message_ack(void* instance, u64 channel_id, u64 message_id, ConstSlice<u8> body,
                                                  u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.message_ack, instance, channel_id, message_id, body,
        request_id);
}
AccountLoginStatus account_module_login_submit_recovery(void* instance, const SecureBuffer* login) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_submit_recovery, instance, login);
}
AccountLoginStatus account_module_login_start_reset(void** instance, const wchar_t* profile_path) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_start_reset, instance, profile_path);
}
AccountLoginStatus account_module_login_submit_reset(void* instance, const SecureBuffer* link_token,
                                                     const SecureBuffer* password) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_submit_reset, instance, link_token, password);
}
AccountLoginStatus account_module_login_submit_phone_verify(void* instance, const SecureBuffer* code) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_submit_phone_verify, instance, code);
}
AccountLoginStatus account_module_login_submit_phone_resend(void* instance) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_submit_phone_resend, instance);
}
SecureBufferResult account_module_login_take_phone_reset_token(void* instance, u64 expected_generation,
                                                               SecureBuffer* output) noexcept {
    return run<SecureBufferResult>(g_account_fake.login_take_phone_reset_token, instance, expected_generation,
        output);
}
AccountVideoResult account_module_video_select(void* instance, const AccountVideoSelection* selection) noexcept {
    return run<AccountVideoResult>(g_account_fake.video_select, instance, selection);
}
AccountVideoResult account_module_video_control(void* instance, const AccountVideoControl* control) noexcept {
    return run<AccountVideoResult>(g_account_fake.video_control, instance, control);
}
AccountVideoPlaybackStatus account_module_video_status(void* instance) noexcept {
    return run<AccountVideoPlaybackStatus>(g_account_fake.video_status, instance);
}
bool account_module_video_take(void* instance, AccountVideoFrame* frame) noexcept {
    return run<bool>(g_account_fake.video_take, instance, frame);
}
bool account_module_video_acknowledge(void* instance, const AccountVideoFrame* frame) noexcept {
    return run<bool>(g_account_fake.video_acknowledge, instance, frame);
}
RestClientSubmitResult account_module_message_search(void* instance, AccountMessageSearchOperation operation,
                                                     const MessageSearchQuery* query, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.message_search, instance, operation, query, request_id);
}
AccountLoginStatus account_module_login_start_browser(void* instance, u64 generation, u32 timeout_ms) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_start_browser, instance, generation, timeout_ms);
}
SecureBufferResult account_module_login_copy_browser_url(void* instance, u64 generation,
                                                         SecureBuffer* argument) noexcept {
    return run<SecureBufferResult>(g_account_fake.login_copy_browser_url, instance, generation, argument);
}
GatewayClientSelfStatusSnapshot account_module_self_status_snapshot(void* instance) noexcept {
    return run<GatewayClientSelfStatusSnapshot>(g_account_fake.self_status_snapshot, instance);
}
RestClientSubmitResult account_module_self_status_submit(void* instance, u64 profile_id, u64 self_user_id,
                                                         SelfStatusSettingsOperation operation,
                                                         GatewayUserStatus status, u64 request_id,
                                                         u64 unix_now_ms) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.self_status_submit, instance, profile_id, self_user_id,
        operation, status, request_id, unix_now_ms);
}
bool account_module_self_status_complete(void* instance, u64 profile_id, u64 self_user_id, u64 request_id,
                                         const RestClientResponse* response, Utf8View argument) noexcept {
    return run<bool>(g_account_fake.self_status_complete, instance, profile_id, self_user_id, request_id, response,
        argument);
}
RestClientSubmitResult account_module_media_catalog(void* instance, AccountMediaCatalogOperation operation,
                                                    const MediaCatalogQuery* query, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.media_catalog, instance, operation, query, request_id);
}
AccountMediaRequestResult account_module_catalog_preview_request(void* instance,
                                                                 const AccountMediaCatalogPreviewRequest* request,
                                                                 u64 value, u64 value2) noexcept {
    return run<AccountMediaRequestResult>(g_account_fake.catalog_preview_request, instance, request, value, value2);
}
RestClientSubmitResult account_module_gif_favorites(void* instance, AccountGifFavoritesOperation operation,
                                                    ConstSlice<u8> patch_json, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.gif_favorites, instance, operation, patch_json, request_id);
}
RestClientSubmitResult account_module_channel_overview(void* instance, ChannelOverviewOperation operation,
                                                       u64 channel_id, const ChannelOverviewPatch* patch,
                                                       u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.channel_overview, instance, operation, channel_id, patch,
        request_id);
}
bool account_module_guild_members_request(void* instance, const GatewayGuildMemberRequest* request) noexcept {
    return run<bool>(g_account_fake.guild_members_request, instance, request);
}
RestClientSubmitResult account_module_pinned_messages(void* instance, const MessagePinsRequest* request,
                                                      u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.pinned_messages, instance, request, request_id);
}
RestClientSubmitResult account_module_message_forward(void* instance, u64 destination_channel_id, Utf8View caption,
                                                      u64 nonce, MessageForwardReference source,
                                                      u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.message_forward, instance, destination_channel_id, caption,
        nonce, source, request_id);
}
AccountDownloadResult account_module_download(void* instance, AccountDownloadOperation operation, u64 value,
                                              const AccountDownloadRequest* request) noexcept {
    return run<AccountDownloadResult>(g_account_fake.download, instance, operation, value, request);
}
RestClientSubmitResult account_module_thread_action(void* instance, const ThreadActionRequest* request,
                                                    u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.thread_action, instance, request, request_id);
}
RestClientSubmitResult account_module_thread_browser(void* instance, const ThreadBrowserRequest* request,
                                                     u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.thread_browser, instance, request, request_id);
}
RestClientSubmitResult account_module_thread_creation(void* instance, const ThreadCreationRequest* request,
                                                      u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.thread_creation, instance, request, request_id);
}
RestClientSubmitResult account_module_forum_post(void* instance, const ForumPostRequest* request,
                                                 u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.forum_post, instance, request, request_id);
}
AccountMediaRequestResult account_module_media_local_preview(void* instance, AccountLocalPreviewOperation operation,
                                                             const AccountMediaLocalPreviewRequest* request,
                                                             u64 monotonic_now_ms, u64 unix_now_seconds) noexcept {
    return run<AccountMediaRequestResult>(g_account_fake.media_local_preview, instance, operation, request,
        monotonic_now_ms, unix_now_seconds);
}
AccountLogoutResult account_module_logout(void* instance, AccountLogoutOperation operation, u64 request_id,
                                          AccountLogoutResponse* response) noexcept {
    return run<AccountLogoutResult>(g_account_fake.logout, instance, operation, request_id, response);
}
RestClientSubmitResult account_module_bulk_message_ack(void* instance, const BulkMessageAckEntry* entries, u32 count,
                                                       u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.bulk_message_ack, instance, entries, count, request_id);
}
RestClientSubmitResult account_module_poll(void* instance, const PollRequest* request, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.poll, instance, request, request_id);
}
RestClientSubmitResult account_module_guild_admin(void* instance, const GuildAdminRequest* request,
                                                  u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.guild_admin, instance, request, request_id);
}
bool account_module_voice_request(void* instance, const GatewayVoiceStateUpdate* update) noexcept {
    return run<bool>(g_account_fake.voice_request, instance, update);
}
bool account_module_voice_snapshot(void* instance, GatewayClientVoiceSnapshot* snapshot) noexcept {
    return run<bool>(g_account_fake.voice_snapshot, instance, snapshot);
}
AccountLoginStatus account_module_login_start_captcha_bridge(void* instance, u64 generation,
                                                             u32 timeout_ms) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_start_captcha_bridge, instance, generation, timeout_ms);
}
CaptchaBridgeStatus account_module_login_captcha_bridge_status(void* instance) noexcept {
    return run<CaptchaBridgeStatus>(g_account_fake.login_captcha_bridge_status, instance);
}
SecureBufferResult account_module_login_copy_captcha_bridge_url(void* instance, u64 generation,
                                                                SecureBuffer* output) noexcept {
    return run<SecureBufferResult>(g_account_fake.login_copy_captcha_bridge_url, instance, generation, output);
}
AccountLoginStatus account_module_login_cancel_captcha_bridge(void* instance) noexcept {
    return run<AccountLoginStatus>(g_account_fake.login_cancel_captcha_bridge, instance);
}
bool account_module_voice_user_audio(void* instance, u64 user_id, u32 gain_percent, bool muted) noexcept {
    return run<bool>(g_account_fake.voice_user_audio, instance, user_id, gain_percent, muted);
}
bool account_module_voice_audio_configure(void* instance, const VoiceAudioSettings* settings) noexcept {
    return run<bool>(g_account_fake.voice_audio_configure, instance, settings);
}
bool account_module_voice_audio_status(void* instance, VoiceAudioStatus* status) noexcept {
    return run<bool>(g_account_fake.voice_audio_status, instance, status);
}
RestClientSubmitResult account_module_guild_invite(void* instance, const GuildInviteRequest* request,
                                                   u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.guild_invite, instance, request, request_id);
}
RestClientSubmitResult account_module_emoji_preferences(void* instance, AccountGifFavoritesOperation operation,
                                                        ConstSlice<u8> patch_json, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.emoji_preferences, instance, operation, patch_json, request_id);
}
RestClientSubmitResult account_module_account_settings(void* instance, const AccountSettingsRequest* request,
                                                       u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.account_settings, instance, request, request_id);
}
CaptchaBridgeStatus account_module_action_captcha(void* instance,
                                                  const AccountActionCaptchaRequest* request) noexcept {
    return run<CaptchaBridgeStatus>(g_account_fake.action_captcha, instance, request);
}
RestClientSubmitResult account_module_poll_create(void* instance, u64 channel_id, u64 nonce, const PollCreate* poll,
                                                  u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.poll_create, instance, channel_id, nonce, poll, request_id);
}
RestClientSubmitResult account_module_private_channel(void* instance, const PrivateChannelActionRequest* request,
                                                      u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.private_channel, instance, request, request_id);
}
bool account_module_custom_status_snapshot(void* instance, SelfCustomStatus* status) noexcept {
    return run<bool>(g_account_fake.custom_status_snapshot, instance, status);
}
RestClientSubmitResult account_module_custom_status_submit(void* instance, u64 profile_id, u64 self_user_id,
                                                           const SelfCustomStatus* status, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.custom_status_submit, instance, profile_id, self_user_id,
        status, request_id);
}
RestClientSubmitResult account_module_guild_settings(void* instance, const UserGuildSettingsPatch* request,
                                                     u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.guild_settings, instance, request, request_id);
}
RestClientSubmitResult account_module_mentions(void* instance, const MentionsQuery* request,
                                               u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.mentions, instance, request, request_id);
}
RestClientSubmitResult account_module_guild_assets(void* instance, const GuildAssetRequest* request,
                                                   u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.guild_assets, instance, request, request_id);
}
bool account_module_voice_video_snapshot(void* instance, GatewayClientVoiceVideoSnapshot* snapshot) noexcept {
    return run<bool>(g_account_fake.voice_video_snapshot, instance, snapshot);
}
bool account_module_voice_video_wanted(void* instance, u64 user_id, bool wanted) noexcept {
    return run<bool>(g_account_fake.voice_video_wanted, instance, user_id, wanted);
}
bool account_module_voice_video_device(void* instance, GpuDevice* device) noexcept {
    return run<bool>(g_account_fake.voice_video_device, instance, device);
}
bool account_module_voice_video_frame(void* instance, u32 ssrc, VideoStreamFrame* output) noexcept {
    return run<bool>(g_account_fake.voice_video_frame, instance, ssrc, output);
}
bool account_module_stream_watch(void* instance, const GatewayStreamKey* key) noexcept {
    return run<bool>(g_account_fake.stream_watch, instance, key);
}
bool account_module_stream_snapshot(void* instance, GatewayClientStreamSnapshot* snapshot) noexcept {
    return run<bool>(g_account_fake.stream_snapshot, instance, snapshot);
}
bool account_module_stream_volume(void* instance, u32 percent) noexcept {
    return run<bool>(g_account_fake.stream_volume, instance, percent);
}
RestClientSubmitResult account_module_stream_preview(void* instance, const GatewayStreamKey* key,
                                                     u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.stream_preview, instance, key, request_id);
}
RestClientSubmitResult account_module_stage(void* instance, const StageRequest* request, u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.stage, instance, request, request_id);
}
RestClientSubmitResult account_module_message_interaction(void* instance, const MessageInteractionRequest* request,
                                                          u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.message_interaction, instance, request, request_id);
}
RestClientSubmitResult account_module_user_social(void* instance, const UserSocialRequest* request,
                                                  u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.user_social, instance, request, request_id);
}
RestClientSubmitResult account_module_guild_community(void* instance, const GuildCommunityRequest* request,
                                                      u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.guild_community, instance, request, request_id);
}
bool account_module_local_presence(void* instance, const GatewayLocalPresence* presence,
                                   GatewayActivitySharing* sharing) noexcept {
    return run<bool>(g_account_fake.local_presence, instance, presence, sharing);
}
RestClientSubmitResult account_module_activity_sharing(void* instance, u64 profile_id, u64 self_user_id, bool share,
                                                       u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.activity_sharing, instance, profile_id, self_user_id, share,
        request_id);
}
RestClientSubmitResult account_module_soundboard(void* instance, const SoundboardRequest* request,
                                                 u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.soundboard, instance, request, request_id);
}
bool account_module_soundboard_audio(void* instance, const SoundboardAudioCommand* command) noexcept {
    return run<bool>(g_account_fake.soundboard_audio, instance, command);
}
bool account_module_voice_message(void* instance, const VoiceMessageCommand* command,
                                  VoiceMessageStatus* status) noexcept {
    return run<bool>(g_account_fake.voice_message, instance, command, status);
}
RestClientSubmitResult account_module_profile_edit(void* instance, const ProfileEditRequest* request,
                                                   u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.profile_edit, instance, request, request_id);
}
bool account_module_profile_image(void* instance, ProfileImageRequest* request) noexcept {
    return run<bool>(g_account_fake.profile_image, instance, request);
}
RestClientSubmitResult account_module_guild_moderation(void* instance, const GuildModerationRequest* request,
                                                       u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.guild_moderation, instance, request, request_id);
}
void account_module_guild_members_query_cancel(void* instance) noexcept {
    if (g_account_fake.guild_members_query_cancel)
        g_account_fake.guild_members_query_cancel(instance);
}
bool account_module_channel_info(void* instance, u64 guild_id) noexcept {
    return run<bool>(g_account_fake.channel_info, instance, guild_id);
}
AccountMediaRequestResult account_module_collection_title(void* instance,
                                                          const AccountMediaCollectionTitleRequest* request,
                                                          MediaTrackTitle* title) noexcept {
    return run<AccountMediaRequestResult>(g_account_fake.collection_title, instance, request, title);
}
void account_module_client_focus(void* instance, bool focused) noexcept {
    if (g_account_fake.client_focus)
        g_account_fake.client_focus(instance, focused);
}
HANDLE account_module_voice_video_notification(void* instance) noexcept {
    return run<HANDLE>(g_account_fake.voice_video_notification, instance);
}
void account_module_media_vp8_cpu(void* instance, bool on_cpu) noexcept {
    if (g_account_fake.media_vp8_cpu)
        g_account_fake.media_vp8_cpu(instance, on_cpu);
}
RestClientSubmitResult account_module_guild_automation(void* instance, const GuildAutomationRequest* request,
                                                       u64 request_id) noexcept {
    return run<RestClientSubmitResult>(g_account_fake.guild_automation, instance, request, request_id);
}
bool account_module_credential_replace(void* instance, const u8* token, u32 bytes) noexcept {
    return run<bool>(g_account_fake.credential_replace, instance, token, bytes);
}
bool account_module_go_live(void* instance, const GoLiveCommand* command) noexcept {
    return run<bool>(g_account_fake.go_live, instance, command);
}
bool account_module_go_live_snapshot(void* instance, GoLiveSnapshot* snapshot) noexcept {
    return run<bool>(g_account_fake.go_live_snapshot, instance, snapshot);
}
bool account_module_go_live_preview(void* instance, VideoStreamFrame* frame) noexcept {
    return run<bool>(g_account_fake.go_live_preview, instance, frame);
}

} // namespace maicord
