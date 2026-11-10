#pragma once
#include "maicord/app/gateway/client_event.hpp"
#include "maicord/app/gateway/client_voice.hpp"
#include "maicord/app/voice/video_frame_sink.hpp"
#include "maicord/app/gateway/state_recovery.hpp"
#include "maicord/app/voice/audio_settings.hpp"
#include "maicord/app/account/account_profile.hpp"
#include "maicord/platform/net/worker.hpp"
#include "maicord/app/gateway/user_configuration.hpp"
#include "maicord/protocol/gateway/endpoint.hpp"
#include "maicord/protocol/gateway/runtime.hpp"
#include "maicord/protocol/user/self_status_settings.hpp"
#include "maicord/protocol/gateway/presence.hpp"

namespace maicord {

struct GatewayStreamKey;
struct GatewayClientStreamSnapshot;
struct GoLiveCommand;
struct GoLiveSnapshot;
inline constexpr u32 kGatewayGoLiveCommands = 8;
struct GatewayClient;
struct RestClient;
struct RestClientResponse;
struct RestClientSubmitResult;
enum class GatewaySelfStatusError : u8 {
    none,
    unavailable,
    settings,
    transport,
    rejected,
    out_of_date
};
struct GatewayClientSelfStatusSnapshot {
    u64 profile_id, self_user_id, revision, settings_revision;
    GatewayUserStatus status, confirmed_status;
    GatewaySelfStatusError last_error;
    bool available, authoritative, pending;
    // Saved settings without a status: unconfirmed, but a status choice may be saved.
    bool settings_known;
};
GatewayClientSelfStatusSnapshot gateway_client_self_status_snapshot(const GatewayClient*) noexcept;
RestClientSubmitResult gateway_client_self_status_submit(GatewayClient*, RestClient*,
                                                         u64 profile_id, u64 self_user_id,
                                                         SelfStatusSettingsOperation operation,
                                                         GatewayUserStatus status, u64 request_id,
                                                         u64 unix_now_ms) noexcept;
bool gateway_client_self_status_complete(GatewayClient*, u64 profile_id, u64 self_user_id, u64 request_id,
                                         const RestClientResponse*, Utf8View response_body) noexcept;
// Custom status (StatusSettings field 2) of the observed settings group; false
// while the group is not authoritative.
bool gateway_client_custom_status_snapshot(const GatewayClient*, SelfCustomStatus* output) noexcept;
// Replaces (null clears) the custom status with a settings-proto PATCH that
// keeps the rest of the status group. Completes through self_status_complete.
RestClientSubmitResult gateway_client_custom_status_submit(GatewayClient*, RestClient*,
                                                           u64 profile_id, u64 self_user_id,
                                                           const SelfCustomStatus*, u64 request_id) noexcept;
// "Share my activity" (StatusSettings.show_current_game) as last observed.
struct GatewayActivitySharing {
    bool known, share;
    u8 reserved[6];
};
static_assert(sizeof(GatewayActivitySharing) == 8);
// Local presence owned by the UI (automatic idle, shared game). A non-null
// input replaces it and republishes opcode 3 when it changed; the game is
// sent only while sharing is on and the status is not invisible. A non-null
// output receives the sharing setting. Returns false for invalid input.
bool gateway_client_local_presence(GatewayClient*, const GatewayLocalPresence* input,
                                   GatewayActivitySharing* output) noexcept;
// Wakes the worker after the client's focus changed in its session metadata.
void gateway_client_activity_changed(GatewayClient*) noexcept;
// Sets show_current_game with a settings-proto PATCH that keeps the rest of
// the status group. Completes through self_status_complete.
RestClientSubmitResult gateway_client_activity_sharing_submit(GatewayClient*, RestClient*,
                                                              u64 profile_id, u64 self_user_id, bool share,
                                                              u64 request_id) noexcept;
AccountRequirement gateway_client_required_action(const GatewayClient* client) noexcept;

struct GatewayClient {
    NetworkWorker worker;
    void* runtime;
    HANDLE notification_event;
    volatile LONG phase;
    volatile LONG error;
    volatile LONG native_error;
    volatile LONG close_code;
    volatile LONG required_action;
    volatile LONG64 hello_elapsed_us;
    volatile LONG64 frecency_settings_revision;
};

// Resume state is profile-bound and contains only the one user session.
struct GatewayUserSessionSnapshot {
    GatewayResumeState resume;
    u64 profile_id;
    // Authenticated READY identity, separate from the local profile key.
    // Zero means unknown in a legacy snapshot; never infer it from a token.
    u64 self_user_id;
    bool has_resume;
};

// Normal-user path. A successful start consumes the complete normal_user
// profile and binds the WebSocket to the v9 ETF zstd-stream endpoint policy;
// commands go out as ETF.
// Invalid or failed synchronous startup leaves the profile unchanged.
GatewayClientResult gateway_client_start_user(GatewayClient* client, AccountProfile* profile,
                                              const GatewayUserClientConfiguration* configuration,
                                              const GatewayEndpoint* gateway_endpoint,
                                              const GatewayUserSessionSnapshot* session) noexcept;
// Process-wide: this launch signed in interactively, so no identify of it is the
// fast-connect one (the desktop page fast-connects only when it loads with a stored token).
void gateway_user_launch_signed_in() noexcept;
// A token a READY handed out (AUTH_TOKEN_REFRESH, the bit 8 capability) moves into the
// empty `output`; the worker already identifies and resumes with it. False when none
// is waiting. The owner hands it to REST and saves it with the profile.
bool gateway_client_take_refreshed_credential(GatewayClient* client, SecureBuffer* output) noexcept;
// A token a REST response handed out (a password change, MFA turned on or off: the old
// one stops working): the worker sends it from its next IDENTIFY or RESUME, and the owner
// takes a copy through gateway_client_take_refreshed_credential. False for a malformed token.
bool gateway_client_replace_credential(GatewayClient* client, ConstSlice<u8> token) noexcept;
// After the worker finishes, the profile metadata and locked credential are
// returned together. A client that is not a user one is rejected without
// stopping and the empty output remains unchanged.
GatewayClientResult gateway_client_stop_return_user_profile(GatewayClient* client, u32 timeout_milliseconds,
                                                            AccountProfile* profile) noexcept;
GatewayClientResult gateway_client_status(const GatewayClient* client) noexcept;
// Observer APIs are UI-thread operations. They must not race
// gateway_client_stop_return_user_profile(), which destroys the worker-owned runtime after joining.
// A pop never signals the notification handle: the worker signals once after each batch it publishes. A
// consumer must therefore drain until empty (or at least the mailbox capacity), as the account pump does.
GatewayClientPopResult gateway_client_pop_event_payload(GatewayClient* client, GatewayClientEvent* event,
                                                        Slice<u8> payload_output) noexcept;
HANDLE gateway_client_notification_handle(const GatewayClient* client) noexcept;
bool gateway_client_snapshot_user_session(const GatewayClient* client,
                                          GatewayUserSessionSnapshot* session) noexcept;
// Replaces the desired visible guild/channel subscription. The worker keeps
// the latest revision across reconnects and transmits it with opcode
// 37 once the authenticated session is READY.
bool gateway_client_subscribe_guild_members(GatewayClient* client,
                                            const GatewayGuildSubscription& subscription) noexcept;
bool gateway_client_request_guild_members(GatewayClient*, const GatewayGuildMemberRequest&) noexcept;
// Queues Request Channel Info (opcode 43) for a guild's voice channel statuses
// and start times; false when it was already asked this session or is queued.
bool gateway_client_request_channel_info(GatewayClient*, u64 guild_id) noexcept;
void gateway_client_cancel_guild_member_request(GatewayClient*) noexcept;
// Retires only name-query interest, including server-rejected replay authority.
void gateway_client_cancel_guild_member_query(GatewayClient*) noexcept;

// UI-thread APIs, with the same stop/join exclusion as the observer APIs above.
// No capture is enabled here. Credentials-ready is not a connected voice call.
bool gateway_client_request_voice_state(GatewayClient* client,
                                        const GatewayVoiceStateUpdate& state) noexcept;
bool gateway_client_voice_snapshot(const GatewayClient* client,
                                   GatewayClientVoiceSnapshot* output) noexcept;
// Local playback volume (0-200 %) and mute for one remote user; applies
// immediately to the mix. Not sent to the server and not persisted here.
bool gateway_client_voice_user_audio(GatewayClient* client, u64 user_id, u32 gain_percent, bool muted) noexcept;
// Microphone, output and input-mode settings; the worker applies the latest.
bool gateway_client_voice_audio_configure(GatewayClient* client, const VoiceAudioSettings& settings) noexcept;
bool gateway_client_voice_audio_status(GatewayClient* client, VoiceAudioStatus* output) noexcept;
bool gateway_client_voice_video_snapshot(const GatewayClient* client,
                                         GatewayClientVoiceVideoSnapshot* output) noexcept;
// Stop (false) or resume (true) receiving one remote user's video: media sink
// wants (op 15) quality 0 for that user's video SSRCs. Kept for the call.
bool gateway_client_voice_video_wanted(GatewayClient* client, u64 user_id, bool wanted) noexcept;
// Where assembled, DAVE-opened call video goes (the stream decoders). The
// worker installs the sink on its next voice drive and hands bind_keyframes
// the thread-safe keyframe requester of this client's video receive path;
// gateway stop ends every stream and binds an empty requester before the
// runtime is freed. The sink context must outlive the client's runtime.
struct GatewayVideoFrames {
    VideoFrameSink sink;
    void* context;
    void (*bind_keyframes)(void* context, VideoKeyframeRequester requester) noexcept;
};
bool gateway_client_set_video_frames(GatewayClient* client, const GatewayVideoFrames& frames) noexcept;
// True when the current runtime already holds frames whose context is `context`.
bool gateway_client_has_video_frames(const GatewayClient* client, const void* context) noexcept;
// Installs the soundboard mixer into this runtime's call playback (a new
// session starts without one; null detaches). The mixer must outlive the
// runtime. Returns true while call audio is live (media attached).
bool gateway_client_attach_soundboard(GatewayClient* client, struct SoundboardMixer* mixer) noexcept;
// Go Live watch (UI thread). Watching requires the local call in the key's
// voice channel; one stream at a time (watching another key deletes the
// previous one). A key with type none stops watching. The worker sends
// STREAM_WATCH, opens the stream's own voice connection and ends it on
// STREAM_DELETE, stop, or when the call leaves the channel.
bool gateway_client_stream_watch(GatewayClient* client, const GatewayStreamKey& key) noexcept;
bool gateway_client_stream_snapshot(const GatewayClient* client, GatewayClientStreamSnapshot* output) noexcept;
// Stream audio bus volume (0-200 %, default 100), on top of the output volume.
bool gateway_client_stream_volume(GatewayClient* client, u32 percent) noexcept;
// Go Live sending (UI thread; go_live_session.hpp). Commands queue for the
// worker (false when the queue is full or the client is not ready); start
// needs the local call in the command's channel.
bool gateway_client_go_live(GatewayClient* client, const GoLiveCommand& command) noexcept;
bool gateway_client_go_live_snapshot(const GatewayClient* client, GoLiveSnapshot* output) noexcept;
// The producers (capture, encoder, stream audio: GoLiveBackend's calls with this context) this runtime streams
// with; they must outlive it. A new session starts without them.
bool gateway_client_set_go_live_backend(GatewayClient* client, void* context) noexcept;
bool gateway_client_has_go_live_backend(const GatewayClient* client) noexcept;
// UI-thread request; transport teardown and fresh identify run on the worker.
GatewayStateRecoveryResult gateway_client_request_fresh_state(GatewayClient* client) noexcept;

} // namespace maicord
