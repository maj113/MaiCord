#pragma once
// Shared by client.cpp and its parts (client_*.cpp); private to them.
#include "maicord/app/gateway/client.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "maicord/app/gateway/client_voice.hpp"
#include "maicord/app/account/client_session_metadata.hpp"
#include "maicord/app/voice/receive.hpp"
#include "maicord/app/gateway/stream_session.hpp"
#include "maicord/app/voice/send.hpp"

#include "maicord/core/byte_queue.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/reserved_bytes.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/net/tls_client.hpp"
#include "maicord/protocol/gateway/etf.hpp"
#include "maicord/protocol/gateway/endpoint.hpp"
#include "maicord/protocol/gateway/qos_token.hpp"
#include "maicord/protocol/gateway/message_events.hpp"
#include "maicord/protocol/gateway/session.hpp"
#include "maicord/protocol/http/websocket_frame.hpp"
#include "maicord/protocol/http/websocket_handshake.hpp"
#include "dispatch_common.hpp"
#include "relationship_dispatch.hpp"
#include "ready_projection.hpp"
#include "subscription_thread_owner.hpp"
#include "self_status_owner.hpp"
#include "maicord/app/gateway/member_request_owner.hpp"
#include "maicord/app/chat/messages/event_scratch.hpp"
#include "maicord/app/gateway/channel_info_requests.hpp"
#if defined(MAICORD_GATEWAY_SELF_STATUS_TEST)
#include "self_status_test.hpp"
#endif

#include "maicord/sys/winsock.hpp"

namespace maicord {
namespace client_detail {}
using namespace client_detail;
namespace client_detail {

inline constexpr char kGatewayUrl[] = "wss://gateway.discord.gg";
inline constexpr u32 kHandshakeBytes = 128 * 1024 + kGatewayClientDispatchPayloadCapacity;
inline constexpr u32 kEncryptedInputBytes = 128 * 1024;
inline constexpr u32 kSessionInputBytes = 128 * 1024;
inline constexpr u32 kMessageBytes = 128 * 1024;
// Large READY payloads use separately reserved message and structural banks.
// Commit their prefixes only as decoding/scanning needs more capacity; wire,
// per-record and mailbox payload limits remain independent.
inline constexpr u32 kStructuralCount = kGatewaySessionMaximumStructuralCount;
inline constexpr u32 kInitialStructuralCount = 1024;
inline constexpr u32 kInitialDecodedMessageBytes = 4096;
inline constexpr u32 kPlainOutputBytes = 64 * 1024;
inline constexpr u32 kRecordOutputBytes = 20 * 1024;
inline constexpr u64 kPhaseTimeoutMs = 15'000;
// Typed READY records are far smaller than the decoded READY; a larger
// projection routes twice instead of staging.
inline constexpr u32 kReadyStagingBytes = 16u * 1024u * 1024u;
// zstd-stream storage: the window plus one message's ETF, growing for a
// large READY and trimmed back afterwards.
inline constexpr u32 kZstdInitialStorageBytes = 384u * 1024u;
inline constexpr u32 kZstdStorageBytes =
    kGatewaySessionMaximumDecodedMessageBytes + kZstdMaximumWindowBytes + kZstdMessageSlackBytes;
// An ETF command: binaries and integers outgrow their JSON text by at most 2.5x.
inline constexpr u32 kEtfCommandBytes = 12u * 1024u;
// The desktop client's connect reasons once its app skeleton has mounted,
// which it has by any identify after the launch's fast-connect one.
inline constexpr char kGatewayConnectReasons[] = "AppSkeleton";

static_assert(kGatewayClientDispatchPayloadCapacity <= kHandshakeBytes - kMessageBytes,
              "Gateway dispatch output must fit in the disjoint scratch half");

// Handshake sends come first; from pong on the send carries a Gateway frame (gateway_action on:
// one that also holds the plaintext record to erase).
enum class SendPurpose : u8 {
    none,
    tls_handshake,
    websocket_opening,
    pong,
    gateway_action,
    guild_subscription,
    voice_state,
    self_status,
    guild_members,
    stream,
    channel_info,
};

struct GatewayClientRuntime {
    NetworkResolver resolver;
    NetworkConnection connection;
    TlsClient tls;
    ByteQueue encrypted_input;
    GatewaySession session;
    GatewayEndpoint endpoint;
    GatewayEndpoint initial_endpoint;
    GatewayClientMailbox events;
    // An oversized guild record (beyond the ring) is routed into this
    // reservation, committed only while one is in flight. The worker owns it
    // while large_record_lease is 0; from the external push until the UI
    // copied the payload out, the consumer owns it and decommits it.
    ReservedBytes large_record;
    volatile LONG large_record_lease;
    // READY preflight records, replayed by publication (address space only
    // outside gateway_ready_project).
    ReservedBytes ready_staging;
    // Compressed payloads beyond handshake_output's first half (a large READY);
    // committed only while such a payload is received and inflated.
    ReservedBytes compressed_spill;
    HANDLE mailbox_space, mailbox_cancel;
    // Records published since the consumer was last signaled. The worker
    // signals once per handled network event (gateway_worker_handler).
    bool notify_pending;
    u64 mailbox_deadline_ms;
    SecureBuffer credential;
    GatewayResumeState resume;
    // Live Gateway sequence of `resume`; written by the worker, read by it and by the snapshot reader.
    volatile LONG64 resume_sequence;
    GatewayReactionBatchCursor reaction_batch;
    u64 resume_user_id; // Protected by resume_lock together with its matching session.
    SRWLOCK resume_lock;
    SRWLOCK subscription_lock;
    NetworkSignal subscription_signal;
    GatewayGuildSubscription desired_subscription;
    GatewaySubscriptionThreadOwner thread_subscription_owner;
    GatewaySelfStatusOwner self_status;
    GatewayMemberRequestOwner member_requests;
    GatewayChannelInfoRequests channel_info; // Under subscription_lock.
    u8* self_status_group;
    u32 self_status_group_bytes;
    GatewaySelfStatusError self_status_error;
    u64 self_status_deadline_ms;
    GatewayLocalPresence local_presence; // Under subscription_lock.
    GatewayVoiceControl voice;
    VoiceConnection voice_connection;
    VoiceReceive voice_receive;
    VoiceSend voice_send;
    NetworkSignal voice_send_signal; // Capture thread: queued frames or new settings.
    SRWLOCK voice_lock;
    NetworkSignal voice_signal;
    GatewayVoiceStateUpdate desired_voice;
    GatewayClientVoiceSnapshot voice_snapshot;
    GatewayClientVoiceVideoSnapshot voice_video_snapshot; // Under voice_lock.
    // UI-requested stopped video users (under voice_lock), applied by the worker.
    u64 video_unwanted[kVoiceVideoUnwantedUsers];
    u32 video_unwanted_count;
    bool video_unwanted_dirty;
    // Watched stream video want (tagged voice_video_wanted IDs), under voice_lock.
    u64 stream_unwanted_owner;
    bool stream_wants_dirty;
    GatewayVideoFrames video_frames; // Under voice_lock; installed by the worker.
    bool video_frames_dirty;
    bool voice_request_pending;
    // Go Live watch. UI requests are under voice_lock; the session is worker-owned.
    GatewayStreamSession stream;
    GatewayStreamKey desired_stream;
    GatewayClientStreamSnapshot stream_snapshot; // Under voice_lock.
    u32 desired_stream_volume;
    bool stream_request_pending, stream_volume_pending;
    struct GatewayUserRuntime* user;
    GatewayClient* owner;
    GatewayOutboundQueue outbound;
    // Heartbeat QoS, kept across reconnects; qos_reported is the last activity state.
    GatewayQosState qos;
    GatewayQosPayload qos_reported;
    bool qos_known;
    // The heartbeat session opcode 41 announces.
    ClientHeartbeatSession time_spent;
    // clips.allow_voice_recording is false (opcode 4 flags).
    bool voice_recording_disallowed;
    GatewayReconnectBackoff reconnect_backoff;
    // Consecutive READY payloads that could not be projected.
    u32 ready_failures;
    u32 allocation_bytes;
    ADDRINFOEXW* next_address;
    const u8* send_data;
    u64 deadline_ms;
    Clock clock;
    i64 start_counter;
    u32 send_bytes;
    u32 send_offset;
    u32 outstanding_socket_operations;
    u32 heartbeat_jitter;
    TlsHandshakeState after_tls_send;
    SendPurpose send_purpose;
    GatewayClientFailureStage failure_stage;
    GatewaySessionError session_error;
    WebSocketHandshakeError handshake_error;
    WebSocketError websocket_error;
    GatewayDecodeError gateway_decode_error;
    GatewayEventDecodeError event_decode_error;
    ZstdError compression_error;
    EtfError etf_error;
    JsonScanError json_scan_error;
    JsonReadError json_read_error;
    u32 route_error_offset;
    u32 route_detail;
    u32 subscription_revision;
    u32 sent_subscription_revision;
    u32 active_subscription_revision;
    // The members-only op 37 frame, under subscription_lock.
    GatewayGuildSubscription desired_members;
    u32 members_revision;
    u32 sent_members_revision;
    GatewayClientIdentity identity;
    u64 rate_deadline_ms;
    u64 member_deadline_ms;
    u64 channel_info_deadline_ms;
    u64 reconnect_deadline_ms;
    // The last heartbeat acknowledgement or connection start (zero before the first); a session silent for
    // three minutes is identified afresh rather than resumed.
    u64 last_ack_ms;
    u64 self_user_id;
    GatewayStateRecovery state_recovery;
    u8 pending_pong_bytes;
    bool tls_initialized;
    bool resolver_released;
    bool stopping;
    bool failed;
    bool authenticated;
    bool reconnecting;
    bool has_pending_pong;
    u8 pending_pong[125];
    u8 encrypted_storage[kEncryptedInputBytes];
    u8 handshake_output[kHandshakeBytes];
    u8 session_input[kSessionInputBytes];
    StableStorage structural_scratch;
    StableStorage decoded_message;
    StableStorage zstd_storage;
    StableStorage ready_record_index;
    // READY's top-level arrays are indexed here, so the message's own index (structural_scratch) stays intact.
    StableStorage ready_arrays;
    union {
        u8 plain_output[kPlainOutputBytes];
        u32 ready_object_scratch[kPlainOutputBytes / sizeof(u32)];
    };
    u8 gateway_payload[4096];
    u8 gateway_etf[kEtfCommandBytes];
    u8 record_output[kRecordOutputBytes];
    // The arrays one message dispatch decodes into (~36 KB, committed when the first message arrives).
    GatewayMessageDecodeScratch message_scratch;
};

template <u32 Capacity> struct GatewayUserTextStorage {
    char data[Capacity];
    u32 bytes;
};

struct GatewayUserRuntime {
    ZstdStreamDecoder zstd;
    // READY's QoS data, re-encoded for later identifies' qos_token.
    u8 qos_derived[kGatewayQosDerivedMaximumBytes];
    u32 qos_derived_bytes;
    bool has_qos_derived;
    AccountProfileMetadata metadata;
    u64 since_ms;
    GatewayUserTextStorage<kGatewayUserMaximumOperatingSystemBytes> operating_system;
    GatewayUserTextStorage<kGatewayUserMaximumBrowserBytes> browser;
    GatewayUserTextStorage<kGatewayUserMaximumReleaseChannelBytes> release_channel;
    GatewayUserTextStorage<kGatewayUserMaximumVersionBytes> client_version;
    GatewayUserTextStorage<kGatewayUserMaximumVersionBytes> os_version;
    GatewayUserTextStorage<kGatewayUserMaximumArchitectureBytes> os_arch;
    GatewayUserTextStorage<kGatewayUserMaximumArchitectureBytes> app_arch;
    GatewayUserTextStorage<kGatewayUserMaximumLocaleBytes> system_locale;
    GatewayUserTextStorage<kGatewayUserLaunchIdBytes> client_launch_id;
    GatewayUserTextStorage<kGatewayUserMaximumUserAgentBytes> browser_user_agent;
    GatewayUserTextStorage<kGatewayUserMaximumVersionBytes> browser_version;
    GatewayUserTextStorage<kGatewayUserMaximumVersionBytes> os_sdk_version;
    GatewayUserTextStorage<kGatewayUserLaunchIdBytes> launch_signature;
    u32 client_build_number;
    u32 native_build_number;
    ClientSessionMetadata* session_metadata;
    GatewayUserStatus status;
    bool has_since;
    bool afk;
};

inline constexpr u32 kGatewayUserRuntimeOffset =
    (static_cast<u32>(sizeof(GatewayClientRuntime)) + static_cast<u32>(alignof(GatewayUserRuntime)) - 1) &
    ~(static_cast<u32>(alignof(GatewayUserRuntime)) - 1);
inline constexpr u32 kGatewayUserClientRuntimeBytes =
    kGatewayUserRuntimeOffset + static_cast<u32>(sizeof(GatewayUserRuntime));

Slice<u32> main_index_view(GatewayClientRuntime* runtime) noexcept;
Slice<u8> decoded_message_view(GatewayClientRuntime* runtime) noexcept;
bool grow_decoded_message(void* context, u32 required, Slice<u8>* backed) noexcept;
bool grow_main_index(void* context, u32 required, Slice<u32>* backed) noexcept;
Slice<u8> zstd_storage_view(GatewayClientRuntime* runtime) noexcept;
bool grow_zstd_storage(void* context, u32 required, Slice<u8>* backed) noexcept;
bool grow_compressed_message(void* context, u32 required, Slice<u8>* backed) noexcept;
Slice<u32> ready_record_index_view(GatewayClientRuntime* runtime) noexcept;
Slice<u32> ready_arrays_view(GatewayClientRuntime* runtime) noexcept;
bool grow_ready_record_index(void* context, u32 required, Slice<u32>* backed) noexcept;
void trim_message_banks(GatewayClientRuntime* runtime) noexcept;
void release_runtime_allocation(GatewayClient* client) noexcept;
GatewayClientRuntime* user_runtime(const GatewayClient* client) noexcept;
GatewayClientRuntime* live_user_runtime(GatewayClient* client) noexcept;
ClientSessionMetadata* session_metadata(const GatewayClientRuntime* runtime) noexcept;
void drive_client_activity(GatewayClientRuntime* runtime, u64 now_ms) noexcept;
void client_session_ready(GatewayClientRuntime* runtime) noexcept;
bool wake_worker(GatewayClient* client, NetworkSignal* signal) noexcept;
void post_subscription_change(GatewayClient* client, GatewayClientRuntime* runtime) noexcept;
GatewayClientResult invalid_input_result() noexcept;

template <u32 Capacity> Utf8View user_text(const GatewayUserTextStorage<Capacity>& text) noexcept {
    return Utf8View{text.data, text.bytes};
}

bool take_fast_connect(char (&launch_id)[kGatewayUserLaunchIdBytes + 1]) noexcept;
bool qos_active(const GatewayQosState& qos) noexcept;
GatewayUserIdentify user_identify(const GatewayUserRuntime& user, Utf8View token,
                                  char (&heartbeat_id)[kGatewayUserLaunchIdBytes]) noexcept;
void store_user_configuration(GatewayUserRuntime* user,
                              const GatewayUserClientConfiguration& configuration) noexcept;
GatewayClientPhase load_phase(const GatewayClient* client) noexcept;
void publish_phase(GatewayClient* client, GatewayClientPhase phase) noexcept;
void publish_event(GatewayClientRuntime* runtime, const GatewayClientEvent& event) noexcept;
void notify_consumer(GatewayClient* client, GatewayClientRuntime* runtime) noexcept;
bool publish_event_payload(GatewayClient* client, GatewayClientRuntime* runtime,
                           const GatewayClientEvent& event, const ConstSlice<u8>& payload) noexcept;
bool acquire_large_record(void* opaque, u32 bytes, Slice<char>* storage) noexcept;
void settle_large_record(GatewayClientRuntime* runtime) noexcept;
void release_large_record(GatewayClientRuntime* runtime) noexcept;

struct RelationshipStreamBinding {
    GatewayClient* client;
    GatewayClientRuntime* runtime;
};

bool publish_relationship_stream(void* opaque, const GatewayClientEvent& event,
                                 ConstSlice<u8> payload) noexcept;
Slice<char> dispatch_scratch(GatewayClientRuntime* runtime) noexcept;
Slice<u32> object_scratch(GatewayClientRuntime* runtime) noexcept;
void publish_resume(GatewayClientRuntime* runtime, const GatewayResumeState& state) noexcept;
void publish_resume_sequence(GatewayClientRuntime* runtime, u64 sequence) noexcept;
GatewayClientResult result(const GatewayClient* client) noexcept;
void publish_failure(GatewayClient* client, GatewayClientError error, u32 native_error) noexcept;
void publish_voice_snapshot(GatewayClient* client, GatewayClientRuntime* runtime) noexcept;
void reset_voice(GatewayClient* client, GatewayClientRuntime* runtime) noexcept;
void apply_video_wants(GatewayClient* client, GatewayClientRuntime* runtime) noexcept;

struct ReadyProjectionBinding {
    GatewayClient* client;
    GatewayClientRuntime* runtime;
    AccountRequirement required_action;
    // The cancel event is polled once per tick (~16 ms), not per record.
    u64 cancel_checked_ms;
    bool cancelled;
};

bool ready_projection_work(void* opaque, GatewayReadyProjectionPhase) noexcept;
bool publish_ready_projection(void* opaque, const GatewayClientEvent& event,
                              ConstSlice<u8> payload) noexcept;
u32 next_worker_wait(const GatewayClient* client, const GatewayClientRuntime* runtime,
                     u64 now_ms) noexcept;
void release_resolver(GatewayClientRuntime* runtime) noexcept;
void clear_resume(GatewayClientRuntime* runtime) noexcept;
void clear_user_transport(GatewayClientRuntime* runtime) noexcept;

struct SelfSettingsJson {
    JsonIndexedDocument document;
    JsonValue encoded;
    u64 type;
    u8 seen;
    bool partial, out_of_date, typed_settings;
};

bool self_settings_json(Utf8View source, SelfSettingsJson* parsed) noexcept;
bool self_settings_observe_locked(GatewayClientRuntime* runtime, JsonValue value, bool partial) noexcept;
bool self_settings_dispatch(GatewayClient* client, GatewayClientRuntime* runtime,
                            Utf8View source) noexcept;
void settings_stores_dispatch(GatewayClient* client, GatewayClientRuntime* runtime,
                              const GatewayEnvelope& envelope, u64 now_ms) noexcept;
bool begin_resolution(GatewayClient* client, GatewayClientRuntime* runtime, NetworkLoop* loop,
                      u64 now_ms) noexcept;
bool schedule_reconnect(GatewayClient* client, GatewayClientRuntime* runtime, GatewayLifecycleAction action,
                        u64 now_ms, u32 delay_ms) noexcept;
void begin_shutdown(GatewayClient* client, GatewayClientRuntime* runtime) noexcept;
bool shutdown_drained(GatewayClientRuntime* runtime) noexcept;
NetworkWorkerStep fail_and_drain(GatewayClient* client, GatewayClientRuntime* runtime,
                                 GatewayClientError error, u32 native_error) noexcept;
NetworkWorkerStep reconnect_or_fail(GatewayClient* client, GatewayClientRuntime* runtime,
                                    GatewayClientError error, u32 native_error, u64 now_ms) noexcept;
bool start_receive(GatewayClientRuntime* runtime) noexcept;
bool ensure_receive(GatewayClientRuntime* runtime) noexcept;
bool continue_send(GatewayClientRuntime* runtime) noexcept;
bool start_connect_candidate(NetworkLoop* loop, GatewayClientRuntime* runtime) noexcept;
bool begin_websocket_opening(GatewayClientRuntime* runtime) noexcept;
bool drive_tls_handshake(GatewayClientRuntime* runtime) noexcept;
bool queue_pong(GatewayClientRuntime* runtime, ConstSlice<u8> payload) noexcept;
bool send_pending_pong(GatewayClientRuntime* runtime) noexcept;

enum class OutboundDrive : u8 {
    idle,
    sending,
    rate_limited,
    failed,
};

OutboundDrive send_control_command(GatewayClientRuntime* runtime, GatewayCommandWriteResult command,
                                   SendPurpose purpose) noexcept;
OutboundDrive drive_gateway_outbound(GatewayClient* client, GatewayClientRuntime* runtime,
                                     u64 now_ms) noexcept;
OutboundDrive drive_application_output(GatewayClient* client, GatewayClientRuntime* runtime,
                                       u64 now_ms) noexcept;

enum class PlaintextDrive : u8 {
    receive,
    sending,
    hello,
    reconnecting,
    failed,
};

PlaintextDrive fail_plaintext(GatewayClientRuntime* runtime, GatewayClientFailureStage stage) noexcept;
PlaintextDrive fail_session_event(GatewayClientRuntime* runtime,
                                  const GatewaySessionEvent& event) noexcept;

enum class ReadyRecordKind : u8 {
    guild,
    private_channel,
    user
};
struct ReadyStatePublishContext {
    GatewayClient* client;
    GatewayClientRuntime* runtime;
    Slice<u32> object_scratch;
    Slice<char> payload_scratch;
    u64 sequence;
    u64 now_ms;
    GatewayClientDispatchRouteResult route;
    bool publish_failed;
    ReadyRecordKind kind;
    u32 user_bytes;
    u32 user_count;
};

ReadyStatePublishContext publish_ready_state(GatewayClient* client, GatewayClientRuntime* runtime,
                                             JsonValue records, ReadyRecordKind kind, u64 sequence,
                                             u64 now_ms) noexcept;
PlaintextDrive drive_queued_actions(GatewayClient* client, GatewayClientRuntime* runtime,
                                    u64 now_ms) noexcept;
PlaintextDrive resume_reconnect(GatewayClient* client, GatewayClientRuntime* runtime, u64 now_ms) noexcept;
void checkpoint_resume_sequence(GatewayClientRuntime* runtime) noexcept;
void set_decode_failure(GatewayClientDispatchRouteResult* routed, const GatewayEventDecodeResult& decoded) noexcept;

// A route failure's categorical codes, for the terminal failure report or the diagnostic event.
template <class Target>
void note_route_error(Target* target, const GatewayClientDispatchRouteResult& route) noexcept {
    target->event_decode_error = route.event_error;
    target->json_scan_error = route.scan_error;
    target->json_read_error = route.read_error;
    target->route_error_offset = route.error_offset;
    target->route_detail = route.detail;
}

// Registers the READY document as the scan source until the projection returns.
struct ReadyScanSource {
    void set(const JsonIndexedDocument& document) noexcept {
        json_scan_source_set(document.input, document.structurals);
    }
    ~ReadyScanSource() { json_scan_source_clear(); }
};

GatewayClientResult start_client(GatewayClient* client, SecureBuffer* credential,
                                 const AccountProfileMetadata* user_metadata,
                                 const GatewayUserClientConfiguration* user_configuration,
                                 const GatewayEndpoint* gateway_endpoint,
                                 const GatewayResumeState* resume_state, u64 resume_user_id) noexcept;

} // namespace client_detail
} // namespace maicord
