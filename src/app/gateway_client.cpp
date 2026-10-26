#include "maicord/app/gateway_guild_folders.hpp"
#include "maicord/app/gateway_user_settings.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/client_session_metadata.hpp"
#include "maicord/app/user_client_configuration.hpp"
#include "maicord/protocol/user_settings_proto.hpp"
#include "maicord/app/voice_receive.hpp"
#include "maicord/app/gateway_stream_session.hpp"
#include "maicord/app/voice_send.hpp"

#include "maicord/core/byte_queue.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/reserved_bytes.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/random.hpp"
#include "maicord/platform/tls_client.hpp"
#include "maicord/protocol/etf.hpp"
#include "maicord/protocol/gateway_endpoint.hpp"
#include "maicord/protocol/gateway_qos_token.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/gateway_session.hpp"
#include "maicord/protocol/gif_favorites.hpp"
#include "maicord/protocol/websocket_frame.hpp"
#include "maicord/protocol/websocket_handshake.hpp"
#include "gateway_user_payload.hpp"
#include "gateway_dispatch_common.hpp"
#include "gateway_relationship_dispatch.hpp"
#include "gateway_ready_projection.hpp"
#include "../protocol/json_field_lists.hpp"
#include "gateway_subscription_thread_owner.hpp"
#include "gateway_self_status_owner.hpp"
#include "maicord/app/gateway_member_request_owner.hpp"
#include "maicord/app/gateway_channel_info_requests.hpp"
#include "maicord/app/rest_client.hpp"
#if defined(MAICORD_GATEWAY_SELF_STATUS_TEST)
#include "gateway_self_status_test.hpp"
#endif

#include "maicord/sys/winsock.hpp"

namespace maicord {

bool gateway_user_client_configuration_validate(
    const GatewayUserClientConfiguration& configuration) noexcept {
    u8 output[kGatewayUserMaximumCommandBytes]{};
    const GatewayCommandWriteResult written = gateway_write_user_identify(
        GatewayUserIdentify{utf8_literal("x"), configuration.properties, configuration.since_ms,
                            configuration.status, configuration.has_since, configuration.afk},
        Slice<u8>{output, static_cast<u32>(sizeof(output))});
    SecureZeroMemory(output, sizeof(output));
    return written.ok();
}

namespace {
#if defined(MAICORD_GATEWAY_SELF_STATUS_TEST)
GatewaySelfStatusTestSubmit self_status_test_submit;
#endif

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
inline constexpr u32 kRecordOutputBytes = 68 * 1024;
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
    SpscMailbox<GatewayClientEvent, kGatewayClientEventCapacity, kGatewayClientDispatchPayloadCapacity>
        events;
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
    GatewayClientIdentity identity;
    u64 rate_deadline_ms;
    u64 member_deadline_ms;
    u64 channel_info_deadline_ms;
    u64 reconnect_deadline_ms;
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
    union {
        u8 plain_output[kPlainOutputBytes];
        u32 ready_object_scratch[kPlainOutputBytes / sizeof(u32)];
    };
    u8 gateway_payload[4096];
    u8 gateway_etf[kEtfCommandBytes];
    u8 record_output[kRecordOutputBytes];
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

Slice<u32> main_index_view(GatewayClientRuntime* runtime) noexcept {
    return {stable_storage_data<u32>(runtime->structural_scratch), runtime->structural_scratch.capacity};
}
Slice<u8> decoded_message_view(GatewayClientRuntime* runtime) noexcept {
    return {stable_storage_data<u8>(runtime->decoded_message), runtime->decoded_message.capacity};
}
// Index and message storage grows only before shutdown, the READY deadline and owner cancellation.
bool growth_allowed(const GatewayClientRuntime* runtime) noexcept {
    return !runtime->stopping &&
           !(runtime->mailbox_deadline_ms && GetTickCount64() >= runtime->mailbox_deadline_ms) &&
           !(runtime->mailbox_cancel && WaitForSingleObject(runtime->mailbox_cancel, 0) != WAIT_TIMEOUT);
}

bool grow_decoded_message(void* context, u32 required, Slice<u8>* backed) noexcept {
    auto* runtime = static_cast<GatewayClientRuntime*>(context);
    if (!growth_allowed(runtime) || !stable_storage_grow(&runtime->decoded_message, required))
        return false;
    *backed = decoded_message_view(runtime);
    return true;
}

bool grow_main_index(void* context, u32 required, Slice<u32>* backed) noexcept {
    auto* runtime = static_cast<GatewayClientRuntime*>(context);
    if (!growth_allowed(runtime) || !stable_storage_grow(&runtime->structural_scratch, required))
        return false;
    *backed = main_index_view(runtime);
    return true;
}

Slice<u8> zstd_storage_view(GatewayClientRuntime* runtime) noexcept {
    return {stable_storage_data<u8>(runtime->zstd_storage), runtime->zstd_storage.capacity};
}
bool grow_zstd_storage(void* context, u32 required, Slice<u8>* backed) noexcept {
    auto* runtime = static_cast<GatewayClientRuntime*>(context);
    if (!growth_allowed(runtime) || !stable_storage_grow(&runtime->zstd_storage, required))
        return false;
    *backed = zstd_storage_view(runtime);
    return true;
}

bool grow_compressed_message(void* context, u32 required, Slice<u8>* backed) noexcept {
    auto* runtime = static_cast<GatewayClientRuntime*>(context);
    if (!required) {
        reserved_bytes_decommit(&runtime->compressed_spill);
        return true;
    }
    if (runtime->stopping || !reserved_bytes_commit(&runtime->compressed_spill, required)) return false;
    *backed = {runtime->compressed_spill.base, runtime->compressed_spill.committed};
    return true;
}

Slice<u32> ready_record_index_view(GatewayClientRuntime* runtime) noexcept {
    return {stable_storage_data<u32>(runtime->ready_record_index), runtime->ready_record_index.capacity};
}
bool grow_ready_record_index(void* context, u32 required, Slice<u32>* backed) noexcept {
    auto* runtime = static_cast<GatewayClientRuntime*>(context);
    if (!growth_allowed(runtime) || !stable_storage_grow(&runtime->ready_record_index, required))
        return false;
    *backed = ready_record_index_view(runtime);
    return true;
}

// Decoded bytes and indexes never outlive their event. After a payload such
// as READY grows them past 1 MiB, the pages are returned before the next one.
inline constexpr u32 kRetainedDecodedMessageBytes = 256 * 1024;
inline constexpr u32 kRetainedStructuralCount = kRetainedDecodedMessageBytes / sizeof(u32);
void trim_message_banks(GatewayClientRuntime* runtime) noexcept {
    if (runtime->decoded_message.capacity > 4 * kRetainedDecodedMessageBytes &&
        runtime->session.message_storage.data == stable_storage_data<u8>(runtime->decoded_message) &&
        stable_storage_shrink(&runtime->decoded_message, kRetainedDecodedMessageBytes))
        runtime->session.message_storage = decoded_message_view(runtime);
    if (runtime->structural_scratch.capacity > 4 * kRetainedStructuralCount &&
        runtime->session.structural_scratch.data == stable_storage_data<u32>(runtime->structural_scratch) &&
        stable_storage_shrink(&runtime->structural_scratch, kRetainedStructuralCount))
        runtime->session.structural_scratch = main_index_view(runtime);
    if (runtime->ready_record_index.capacity > kInitialStructuralCount)
        stable_storage_shrink(&runtime->ready_record_index, kInitialStructuralCount);
    // The window moves to the front; the pages past it and the next message's room are returned.
    if (runtime->user && runtime->session.zstd_stream == &runtime->user->zstd &&
        runtime->zstd_storage.capacity > 4 * kZstdInitialStorageBytes) {
        const u32 keep = zstd_stream_compact(&runtime->user->zstd);
        const u32 target = maximum(kZstdInitialStorageBytes, keep + kZstdMessageSlackBytes);
        if (stable_storage_shrink(&runtime->zstd_storage, target))
            zstd_stream_set_storage(&runtime->user->zstd, zstd_storage_view(runtime));
    }
}

void wipe_storage(const StableStorage& storage) noexcept {
    if (storage.arena.base)
        SecureZeroMemory(storage.arena.base, storage.arena.committed);
}

void release_wiped_storage(StableStorage* storage) noexcept {
    wipe_storage(*storage);
    stable_storage_release(storage);
}

void release_runtime_allocation(GatewayClient* client) noexcept {
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    const u32 bytes = runtime->allocation_bytes;
    release_wiped_storage(&runtime->structural_scratch);
    reserved_bytes_release(&runtime->large_record);
    reserved_bytes_release(&runtime->ready_staging);
    reserved_bytes_release(&runtime->compressed_spill);
    release_wiped_storage(&runtime->decoded_message);
    release_wiped_storage(&runtime->zstd_storage);
    release_wiped_storage(&runtime->ready_record_index);
    if (runtime->self_status_group) {
        SecureZeroMemory(runtime->self_status_group, kSelfStatusSettingsMaximumGroupBytes);
        VirtualFree(runtime->self_status_group, 0, MEM_RELEASE);
        runtime->self_status_group = nullptr;
    }
    SecureZeroMemory(runtime, bytes);
    VirtualFree(runtime, 0, MEM_RELEASE);
}

bool is_user(const GatewayClientRuntime* runtime) noexcept {
    return runtime && runtime->identity == GatewayClientIdentity::normal_user && runtime->user;
}

GatewayClientRuntime* user_runtime(const GatewayClient* client) noexcept {
    auto* runtime =
        client && client->runtime ? static_cast<GatewayClientRuntime*>(client->runtime) : nullptr;
    return is_user(runtime) ? runtime : nullptr;
}

// The runtime of a started user client whose worker is neither stopping nor failed.
GatewayClientRuntime* live_user_runtime(GatewayClient* client) noexcept {
    if (!client || !client->runtime || !client->worker.thread)
        return nullptr;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    return runtime->identity == GatewayClientIdentity::normal_user && !runtime->stopping && !runtime->failed
               ? runtime
               : nullptr;
}

ClientSessionMetadata* session_metadata(const GatewayClientRuntime* runtime) noexcept {
    return is_user(runtime) ? runtime->user->session_metadata : nullptr;
}

// The desktop client is active while focused (foregrounded) or connected to
// a call (rtc_connected).
u8 activity_reasons(const GatewayClientRuntime* runtime) noexcept {
    const VoiceConnection& voice = runtime->voice_connection;
    const bool rtc =
        voice.socket.phase == VoiceSocketPhase::open && voice.udp.phase == VoiceUdpPhase::ready;
    return static_cast<u8>(
        (client_session_metadata_focused(session_metadata(runtime)) ? gateway_qos_foregrounded : 0) |
        (rtc ? gateway_qos_rtc_connected : 0));
}

bool client_active(const GatewayClientRuntime* runtime) noexcept {
    return activity_reasons(runtime) != 0;
}

// Identified or resuming.
bool session_established(const GatewayClientRuntime* runtime) noexcept {
    const GatewayLifecycle& lifecycle = runtime->session.lifecycle;
    return lifecycle.phase == GatewayLifecyclePhase::ready ||
           (lifecycle.phase == GatewayLifecyclePhase::awaiting_session && lifecycle.attempting_resume);
}

// A heartbeat outside the interval, as the desktop client sends one.
void queue_heartbeat_now(GatewayClientRuntime* runtime) noexcept {
    if (gateway_outbound_enqueue(&runtime->outbound, GatewayLifecycleAction::send_heartbeat))
        gateway_qos_send_now(&runtime->qos);
}

// Opcode 41 and a heartbeat, in an identified session.
void queue_time_spent_session(GatewayClientRuntime* runtime,
                              const ClientHeartbeatSession& session) noexcept {
    if (!session.valid || runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready)
        return;
    runtime->time_spent = session;
    if (gateway_outbound_enqueue(&runtime->outbound, GatewayLifecycleAction::send_time_spent_session))
        queue_heartbeat_now(runtime);
}

// While active, the heartbeat session is used every kClientHeartbeatIntervalMs,
// starting when the client first becomes active.
u64 client_activity_deadline(const GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    ClientSessionMetadata* metadata = session_metadata(runtime);
    if (!metadata || runtime->stopping || !client_active(runtime))
        return 0;
    const u64 due = client_session_metadata_refresh_due(metadata);
    return due > now_ms ? due : now_ms;
}

// Reports activity changes to the heartbeat QoS (becoming active sends a
// heartbeat at once), and uses the heartbeat session while active; a new
// session is announced with opcode 41.
void drive_client_activity(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    ClientSessionMetadata* metadata = session_metadata(runtime);
    if (!metadata || runtime->stopping)
        return;
    const GatewayQosPayload reported = gateway_qos_payload(activity_reasons(runtime));
    if (!runtime->qos_known || !(reported == runtime->qos_reported)) {
        runtime->qos_known = true;
        runtime->qos_reported = reported;
        if (gateway_qos_update(&runtime->qos, reported, session_established(runtime)) &&
            !gateway_outbound_enqueue(&runtime->outbound, GatewayLifecycleAction::send_heartbeat))
            runtime->qos.has_immediate = false;
    }
    const u64 due = client_activity_deadline(runtime, now_ms);
    if (!due || now_ms < due)
        return;
    client_session_metadata_refreshed(metadata, now_ms);
    ClientHeartbeatSession session{};
    if (client_session_metadata_use_session(metadata, true, clock_unix_milliseconds(), &session))
        queue_time_spent_session(runtime, session);
}

// READY uses the heartbeat session too, starting one only when active, and
// announces it with opcode 41.
void client_session_ready(GatewayClientRuntime* runtime) noexcept {
    if (ClientSessionMetadata* metadata = session_metadata(runtime)) {
        ClientHeartbeatSession session{};
        client_session_metadata_use_session(metadata, client_active(runtime), clock_unix_milliseconds(),
                                            &session);
        queue_time_spent_session(runtime, session);
    }
}

// True once the worker is certain to wake for `signal`: it was already pending or the post took.
bool wake_worker(GatewayClient* client, NetworkSignal* signal) noexcept {
    return InterlockedCompareExchange(&signal->pending, 0, 0) != 0 ||
           network_signal_post(&client->worker.loop, signal, 1).ok() ||
           InterlockedCompareExchange(&signal->pending, 0, 0) != 0;
}

GatewayClientResult invalid_input_result() noexcept {
    GatewayClientResult value{};
    value.error = GatewayClientError::invalid_input;
    return value;
}

template <u32 Capacity> Utf8View user_text(const GatewayUserTextStorage<Capacity>& text) noexcept {
    return Utf8View{text.data, text.bytes};
}

// The launch's first identify is the desktop page's fast-connect one; the
// page's own module instance gives it a launch ID of its own.
volatile LONG fast_connect_taken;
bool take_fast_connect(char (&launch_id)[kGatewayUserLaunchIdBytes + 1]) noexcept {
    return InterlockedCompareExchange(&fast_connect_taken, 1, 0) == 0 && user_client_random_uuid(launch_id);
}

// The heartbeat QoS activity identify reports (the upcoming state, else the current one).
bool qos_active(const GatewayQosState& qos) noexcept {
    return qos.has_upcoming ? qos.upcoming.active : qos.has_current && qos.current.active;
}

// The user text fields in GatewayUserProperties order (its first twelve views, then launch_signature) and where
// each lives in GatewayUserRuntime.
struct UserTextSlot {
    u16 data, bytes;
};
#define MAICORD_USER_TEXT_SLOT(field)                                                       \
    {static_cast<u16>(__builtin_offsetof(GatewayUserRuntime, field.data)),                   \
     static_cast<u16>(__builtin_offsetof(GatewayUserRuntime, field.bytes))}
constexpr UserTextSlot kUserTextSlots[]{
    MAICORD_USER_TEXT_SLOT(operating_system), MAICORD_USER_TEXT_SLOT(browser),
    MAICORD_USER_TEXT_SLOT(release_channel),  MAICORD_USER_TEXT_SLOT(client_version),
    MAICORD_USER_TEXT_SLOT(os_version),       MAICORD_USER_TEXT_SLOT(os_arch),
    MAICORD_USER_TEXT_SLOT(app_arch),         MAICORD_USER_TEXT_SLOT(system_locale),
    MAICORD_USER_TEXT_SLOT(client_launch_id), MAICORD_USER_TEXT_SLOT(browser_user_agent),
    MAICORD_USER_TEXT_SLOT(browser_version),  MAICORD_USER_TEXT_SLOT(os_sdk_version),
    MAICORD_USER_TEXT_SLOT(launch_signature)};
#undef MAICORD_USER_TEXT_SLOT
inline constexpr u32 kUserTextViews = 12;
static_assert(__builtin_offsetof(GatewayUserProperties, operating_system) == 0 &&
              __builtin_offsetof(GatewayUserProperties, os_sdk_version) == (kUserTextViews - 1) * sizeof(Utf8View));

Utf8View load_user_text(const GatewayUserRuntime& user, const UserTextSlot& slot) noexcept {
    const auto* storage = reinterpret_cast<const u8*>(&user);
    return {reinterpret_cast<const char*>(storage + slot.data), *reinterpret_cast<const u32*>(storage + slot.bytes)};
}

void store_user_text(GatewayUserRuntime* user, const UserTextSlot& slot, const Utf8View& text) noexcept {
    auto* storage = reinterpret_cast<u8*>(user);
    copy_bytes(storage + slot.data, text.data, text.bytes);
    *reinterpret_cast<u32*>(storage + slot.bytes) = text.bytes;
}

// heartbeat_id backs the request-time heartbeat session the live metadata reports.
GatewayUserIdentify user_identify(const GatewayUserRuntime& user, Utf8View token,
                                  char (&heartbeat_id)[kGatewayUserLaunchIdBytes]) noexcept {
    GatewayUserIdentify identify{token, {}, user.since_ms, user.status, user.has_since, user.afk};
    auto& properties = identify.properties;
    auto* views = reinterpret_cast<Utf8View*>(&properties);
    for (u32 i = 0; i != kUserTextViews; ++i)
        views[i] = load_user_text(user, kUserTextSlots[i]);
    properties.launch_signature = load_user_text(user, kUserTextSlots[kUserTextViews]);
    properties.client_build_number = user.client_build_number;
    properties.native_build_number = user.native_build_number;
    if (user.session_metadata) {
        const GatewayUserProperties live =
            client_session_metadata_properties(user.session_metadata, heartbeat_id);
        identify.properties.client_heartbeat_session_id = live.client_heartbeat_session_id;
        identify.properties.client_app_state = live.client_app_state;
        identify.properties.app_state_first = live.app_state_first;
        identify.client_app_state = client_session_metadata_focused(user.session_metadata)
                                        ? utf8_literal("focused")
                                        : utf8_literal("unfocused");
    }
    return identify;
}

void store_user_configuration(GatewayUserRuntime* user,
                              const GatewayUserClientConfiguration& configuration) noexcept {
    const GatewayUserProperties& properties = configuration.properties;
    const auto* views = reinterpret_cast<const Utf8View*>(&properties);
    for (u32 i = 0; i != kUserTextViews; ++i)
        store_user_text(user, kUserTextSlots[i], views[i]);
    store_user_text(user, kUserTextSlots[kUserTextViews], properties.launch_signature);
    user->client_build_number = properties.client_build_number;
    user->native_build_number = properties.native_build_number;
    user->session_metadata = configuration.metadata;
    user->since_ms = configuration.since_ms;
    user->status = configuration.status;
    user->has_since = configuration.has_since;
    user->afk = configuration.afk;
}

GatewayClientPhase load_phase(const GatewayClient* client) noexcept {
    return static_cast<GatewayClientPhase>(ReadAcquire(&client->phase));
}

GatewayClientError load_error(const GatewayClient* client) noexcept {
    return static_cast<GatewayClientError>(ReadAcquire(&client->error));
}

void publish_phase(GatewayClient* client, GatewayClientPhase phase) noexcept {
    InterlockedExchange(&client->phase, static_cast<LONG>(phase));
}

void publish_event(GatewayClientRuntime* runtime, const GatewayClientEvent& event) noexcept {
    if (spsc_mailbox_push(&runtime->events, event, ConstSlice<u8>{nullptr, 0}).ok())
        runtime->notify_pending = true;
}

void notify_consumer(GatewayClient* client, GatewayClientRuntime* runtime) noexcept {
    if (runtime->notify_pending) {
        runtime->notify_pending = false;
        SetEvent(client->notification_event);
    }
}

bool publish_event_payload(GatewayClient* client, GatewayClientRuntime* runtime,
                           const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    const u64 deadline =
        runtime->mailbox_deadline_ms ? runtime->mailbox_deadline_ms : GetTickCount64() + kPhaseTimeoutMs;
    // A record routed into the large storage is handed over in place.
    if (payload.count && runtime->large_record.base && payload.data == runtime->large_record.base)
        return payload.count <= runtime->large_record.committed &&
               gateway_mailbox_publish_external(&runtime->events, runtime->mailbox_space, runtime->mailbox_cancel,
                                                client->notification_event, deadline, event, payload.count,
                                                &runtime->large_record_lease, &runtime->notify_pending);
    return gateway_mailbox_publish(&runtime->events, runtime->mailbox_space, runtime->mailbox_cancel,
                                   client->notification_event, deadline, event, payload, &runtime->notify_pending);
}

// Worker side: commits `bytes` of the large record storage, first waiting
// for the UI to take the previous oversized record.
bool acquire_large_record(void* opaque, u32 bytes, Slice<char>* storage) noexcept {
    auto* runtime = static_cast<GatewayClientRuntime*>(opaque);
    if (!runtime || !storage || !runtime->large_record.base || runtime->stopping)
        return false;
    GatewayClient* client = runtime->owner;
    const u64 deadline =
        runtime->mailbox_deadline_ms ? runtime->mailbox_deadline_ms : GetTickCount64() + kPhaseTimeoutMs;
    if (!client || !gateway_mailbox_wait_lease(runtime->mailbox_space, runtime->mailbox_cancel,
                                               client->notification_event, deadline, &runtime->large_record_lease) ||
        !reserved_bytes_commit(&runtime->large_record, bytes))
        return false;
    *storage = {reinterpret_cast<char*>(runtime->large_record.base), runtime->large_record.committed};
    return true;
}

// Worker side: returns pages of a large record that was routed but not handed over.
void settle_large_record(GatewayClientRuntime* runtime) noexcept {
    if (!ReadAcquire(&runtime->large_record_lease) && runtime->large_record.committed)
        reserved_bytes_decommit(&runtime->large_record);
}

// Consumer side, after an external record was copied out or discarded.
void release_large_record(GatewayClientRuntime* runtime) noexcept {
    reserved_bytes_decommit(&runtime->large_record);
    WriteRelease(&runtime->large_record_lease, 0);
}

struct RelationshipStreamBinding {
    GatewayClient* client;
    GatewayClientRuntime* runtime;
};
bool publish_relationship_stream(void* opaque, const GatewayClientEvent& event,
                                 ConstSlice<u8> payload) noexcept {
    const auto* binding = static_cast<const RelationshipStreamBinding*>(opaque);
    return publish_event_payload(binding->client, binding->runtime, event, payload);
}

// The second half of the handshake buffer: a dispatch is routed from the first half.
Slice<char> dispatch_scratch(GatewayClientRuntime* runtime) noexcept {
    return {reinterpret_cast<char*>(runtime->handshake_output + kMessageBytes),
            kGatewayClientDispatchPayloadCapacity};
}
Slice<u32> object_scratch(GatewayClientRuntime* runtime) noexcept {
    return {runtime->ready_object_scratch, static_cast<u32>(kPlainOutputBytes / sizeof(u32))};
}

void publish_resume(GatewayClientRuntime* runtime, const GatewayResumeState& state) noexcept {
    AcquireSRWLockExclusive(&runtime->resume_lock);
    runtime->resume = state;
    runtime->resume_user_id = runtime->self_user_id;
    ReleaseSRWLockExclusive(&runtime->resume_lock);
}

void publish_resume_sequence(GatewayClientRuntime* runtime, u64 sequence) noexcept {
    AcquireSRWLockExclusive(&runtime->resume_lock);
    runtime->resume.sequence = sequence;
    ReleaseSRWLockExclusive(&runtime->resume_lock);
}

GatewayClientResult result(const GatewayClient* client) noexcept {
    if (!client)
        return invalid_input_result();
    GatewayClientResult value{};
    value.phase = load_phase(client);
    value.error = load_error(client);
    value.native_error = static_cast<u32>(ReadAcquire(&client->native_error));
    value.hello_elapsed_us = static_cast<u64>(ReadAcquire64(&client->hello_elapsed_us));
    value.close_code = static_cast<u16>(ReadAcquire(&client->close_code));
    return value;
}

void publish_failure(GatewayClient* client, GatewayClientError error, u32 native_error) noexcept {
    if (InterlockedCompareExchange(&client->error, static_cast<LONG>(error),
                                   static_cast<LONG>(GatewayClientError::none)) ==
        static_cast<LONG>(GatewayClientError::none)) {
        InterlockedExchange(&client->native_error, static_cast<LONG>(native_error));
    }
    publish_phase(client, GatewayClientPhase::failed);
    SetEvent(client->state_event);
}

void publish_voice_snapshot(GatewayClient* client, GatewayClientRuntime* runtime) noexcept {
    const auto& setup = runtime->voice.setup;
    GatewayClientVoiceSnapshot snapshot{runtime->self_user_id,
                                        setup.guild_id,
                                        setup.channel_id,
                                        setup.generation,
                                        setup.flags,
                                        setup.present_flags,
                                        setup.phase,
                                        runtime->voice.error,
                                        runtime->voice.awaiting_acknowledgement,
                                        runtime->voice_connection.socket.phase,
                                        runtime->voice_connection.error,
                                        runtime->voice_connection.identify_submitted,
                                        runtime->voice_connection.socket.close_code,
                                        runtime->voice_connection.udp.phase,
                                        runtime->voice_connection.udp.error};
    snapshot.speaking_count = voice_receive_speaking(
        &runtime->voice_receive, {snapshot.speaking_users, kGatewayVoiceSpeakingUsers}, GetTickCount64());
    // The local user is never received; transmitting counts as speaking.
    if (const u64 self = runtime->self_user_id; self && voice_send_speaking(runtime->voice_send)) {
        u32 at = 0;
        while (at < snapshot.speaking_count && snapshot.speaking_users[at] < self)
            ++at;
        if ((at == snapshot.speaking_count || snapshot.speaking_users[at] != self) &&
            snapshot.speaking_count < kGatewayVoiceSpeakingUsers) {
            for (u32 k = snapshot.speaking_count; k > at; --k)
                snapshot.speaking_users[k] = snapshot.speaking_users[k - 1];
            snapshot.speaking_users[at] = self;
            ++snapshot.speaking_count;
        }
    }
    GatewayClientVoiceVideoSnapshot video{};
    video.generation = setup.generation;
    video.codec = runtime->voice_connection.media.video_codec;
    VoiceVideoUser users[kGatewayVoiceVideoUsers];
    video.count = voice_video_users(runtime->voice_connection, &runtime->voice_receive.video, runtime->self_user_id,
                                    {users, kGatewayVoiceVideoUsers});
    for (u32 i = 0; i != video.count; ++i)
        video.users[i] = {users[i].user_id, users[i].ssrc, users[i].frames, users[i].kind, users[i].quality, {}};
    video.frames_delivered = runtime->voice_receive.video.stats.delivered;
    video.frames_rejected = runtime->voice_receive.video.stats.dave_rejected;
    const auto stream = gateway_stream_session_snapshot(runtime->stream, runtime->self_user_id);
    AcquireSRWLockExclusive(&runtime->voice_lock);
    const bool changed = !(snapshot == runtime->voice_snapshot) || !(video == runtime->voice_video_snapshot) ||
                         !(stream == runtime->stream_snapshot);
    copy_value(&runtime->voice_snapshot, snapshot);
    copy_value(&runtime->voice_video_snapshot, video);
    copy_value(&runtime->stream_snapshot, stream);
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    if (changed && client->notification_event)
        SetEvent(client->notification_event);
}

void reset_voice(GatewayClient* client, GatewayClientRuntime* runtime) noexcept {
    voice_connection_stop(&runtime->voice_connection);
    voice_receive_stop(&runtime->voice_receive);
    voice_send_stop(&runtime->voice_send);
    gateway_voice_release(&runtime->voice);
    // The stream connection is bound to this Gateway session; nothing is sent.
    gateway_stream_session_reset(&runtime->stream, GatewayStreamEnd::session);
    voice_setup_release(&runtime->stream.control.setup);
    AcquireSRWLockExclusive(&runtime->voice_lock);
    runtime->voice_request_pending = false;
    runtime->stream_request_pending = false;
    reset_value(&runtime->desired_voice);
    runtime->video_unwanted_count = 0;
    runtime->video_unwanted_dirty = false;
    runtime->stream_unwanted_owner = 0;
    runtime->stream_wants_dirty = false;
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    gateway_stream_session_want_video(&runtime->stream, 0, true);
    publish_voice_snapshot(client, runtime);
}
// Decoder threads request keyframes; the voice drive after this wake sends them.
void wake_voice_worker(void* context) noexcept {
    auto* client = static_cast<GatewayClient*>(context);
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    if (runtime && !InterlockedCompareExchange(&runtime->voice_send_signal.pending, 0, 0))
        network_signal_post(&client->worker.loop, &runtime->voice_send_signal, 1);
}
// Decoder keyframe requests: tagged SSRCs belong to the watched stream.
void route_keyframe(void* context, u32 ssrc) noexcept {
    auto* runtime = static_cast<GatewayClientRuntime*>(context);
    gateway_stream_session_keyframe(&runtime->voice_receive.video, &runtime->stream, ssrc);
}
void apply_video_wants(GatewayClient* client, GatewayClientRuntime* runtime) noexcept {
    auto& video = runtime->voice_receive.video;
    video.wake = wake_voice_worker;
    video.wake_context = client;
    runtime->stream.receive.video.wake = wake_voice_worker;
    runtime->stream.receive.video.wake_context = client;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    if (runtime->video_unwanted_dirty) {
        copy_bytes(video.unwanted, runtime->video_unwanted, runtime->video_unwanted_count * static_cast<u32>(sizeof(u64)));
        video.unwanted_count = runtime->video_unwanted_count;
        runtime->video_unwanted_dirty = false;
    }
    if (runtime->stream_wants_dirty) {
        gateway_stream_session_want_video(&runtime->stream, runtime->stream_unwanted_owner,
                                          !runtime->stream_unwanted_owner);
        runtime->stream_wants_dirty = false;
    }
    const bool frames_changed = runtime->video_frames_dirty;
    const GatewayVideoFrames frames = runtime->video_frames;
    runtime->video_frames_dirty = false;
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    if (frames_changed) {
        // Streams delivered to a previous sink end there first.
        if (video.frames.push_frame && video.frames.context != frames.sink.context)
            voice_video_sink_stop(&video);
        video.frames = frames.sink;
        gateway_stream_session_set_target(&runtime->stream, frames.sink);
        if (frames.bind_keyframes)
            frames.bind_keyframes(frames.context, {runtime, route_keyframe});
    }
}

struct ReadyProjectionBinding {
    GatewayClient* client;
    GatewayClientRuntime* runtime;
    AccountRequirement required_action;
    // The cancel event is polled once per tick (~16 ms), not per record.
    u64 cancel_checked_ms;
    bool cancelled;
};
bool ready_projection_work(void* opaque, GatewayReadyProjectionPhase) noexcept {
    auto& binding = *static_cast<ReadyProjectionBinding*>(opaque);
    const auto& runtime = *binding.runtime;
    const u64 now = GetTickCount64();
    if (runtime.stopping || !runtime.mailbox_deadline_ms || now >= runtime.mailbox_deadline_ms || binding.cancelled)
        return false;
    if (binding.cancel_checked_ms == now)
        return true;
    binding.cancel_checked_ms = now;
    binding.cancelled = WaitForSingleObject(runtime.mailbox_cancel, 0) != WAIT_TIMEOUT;
    return !binding.cancelled;
}
bool publish_ready_projection(void* opaque, const GatewayClientEvent& event,
                              ConstSlice<u8> payload) noexcept {
    auto& binding = *static_cast<ReadyProjectionBinding*>(opaque);
    if (event.kind == GatewayClientEventKind::ready) {
        // Invoked only by the commit pass after every supported READY
        // child has passed its typed route. Preflight cannot change state.
        InterlockedExchange(&binding.client->required_action, static_cast<LONG>(binding.required_action));
        binding.runtime->self_user_id = event.state.id;
        reset_value(&binding.runtime->reaction_batch);
        reset_voice(binding.client, binding.runtime);
    }
    return publish_event_payload(binding.client, binding.runtime, event, payload);
}

u32 next_worker_wait(const GatewayClient* client, const GatewayClientRuntime* runtime,
                     u64 now_ms) noexcept {
    if (runtime->stopping || (!runtime->authenticated && load_phase(client) == GatewayClientPhase::hello)) {
        return INFINITE;
    }
    if (runtime->reconnecting) {
        const u64 main_deadline = runtime->resolver.pending || runtime->outstanding_socket_operations
                                      ? 0
                                      : runtime->reconnect_deadline_ms;
        const u64 deadline = earliest_deadline(
            earliest_deadline(earliest_deadline(main_deadline, voice_connection_deadline(runtime->voice_connection)),
                              voice_receive_deadline(runtime->voice_receive)),
            gateway_stream_session_deadline(runtime->stream));
        return deadline ? wait_milliseconds_until(now_ms, deadline) : INFINITE;
    }
    u64 deadline = runtime->deadline_ms;
    deadline = earliest_deadline(deadline, runtime->voice.acknowledgement_deadline_ms);
    deadline = earliest_deadline(deadline, runtime->self_status_deadline_ms);
    deadline = earliest_deadline(deadline, voice_connection_deadline(runtime->voice_connection));
    deadline = earliest_deadline(deadline, voice_receive_deadline(runtime->voice_receive));
    deadline = earliest_deadline(deadline, gateway_stream_session_deadline(runtime->stream));
    if (runtime->session.lifecycle.heartbeat_interval_ms != 0)
        deadline = earliest_deadline(deadline, runtime->session.lifecycle.next_heartbeat_ms);
    deadline = earliest_deadline(deadline, client_activity_deadline(runtime, now_ms));
    deadline = earliest_deadline(deadline, runtime->rate_deadline_ms);
    deadline =
        earliest_deadline(deadline, runtime->member_deadline_ms > now_ms ? runtime->member_deadline_ms : 0);
    deadline = earliest_deadline(
        deadline, runtime->channel_info_deadline_ms > now_ms ? runtime->channel_info_deadline_ms : 0);
    deadline = earliest_deadline(
        deadline, gateway_state_recovery_deadline(runtime->state_recovery,
                                                  load_phase(client) == GatewayClientPhase::ready));
    if (runtime->outbound.count != 0 && runtime->send_purpose == SendPurpose::none &&
        runtime->rate_deadline_ms == 0) {
        return 0;
    }
    GatewayVoiceStateUpdate voice_command{};
    GatewayStreamCommand stream_command{};
    if (runtime->send_purpose == SendPurpose::none && !runtime->rate_deadline_ms &&
        runtime->session.lifecycle.phase == GatewayLifecyclePhase::ready &&
        (gateway_voice_next(runtime->voice, &voice_command) ||
         gateway_stream_next(runtime->stream.control, &stream_command)))
        return 0;
    return deadline == 0 ? INFINITE : wait_milliseconds_until(now_ms, deadline);
}

void release_resolver(GatewayClientRuntime* runtime) noexcept {
    if (runtime->resolver_released || !network_resolver_release(&runtime->resolver))
        return;
    runtime->resolver_released = true;
    runtime->next_address = nullptr;
}

void clear_resume(GatewayClientRuntime* runtime) noexcept {
    reset_value(&runtime->reaction_batch);
    AcquireSRWLockExclusive(&runtime->resume_lock);
    gateway_resume_clear(&runtime->resume);
    runtime->resume_user_id = 0;
    ReleaseSRWLockExclusive(&runtime->resume_lock);
}

void clear_user_transport(GatewayClientRuntime* runtime) noexcept {
    if (!is_user(runtime)) {
        return;
    }
    zstd_stream_clear(&runtime->user->zstd);
    wipe_storage(runtime->zstd_storage);
    SecureZeroMemory(runtime->handshake_output, kHandshakeBytes);
    wipe_storage(runtime->decoded_message);
    wipe_storage(runtime->ready_record_index);
}

struct SelfSettingsJson {
    JsonIndexedDocument document;
    JsonValue encoded;
    u64 type;
    u8 seen;
    bool partial, out_of_date, typed_settings;
};
bool self_settings_json_member(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& parsed = *static_cast<SelfSettingsJson*>(opaque);
    const u8 field = static_cast<u8>(
        protocol_detail::member_bit<"settings", "proto", "type", "partial", "out_of_date">(key));
    if (!field)
        return true;
    if (parsed.seen & field)
        return false;
    parsed.seen |= field;
    if (field == 1 && value.kind == JsonValueKind::object) {
        parsed.typed_settings = true;
        return json_visit_indexed_object(parsed.document, value, 16, self_settings_json_member, &parsed)
            .ok();
    }
    if (field == 1 || field == 2) {
        parsed.encoded = value;
        return value.kind == JsonValueKind::string;
    }
    if (field == 4)
        return json_value_to_u64(value, &parsed.type);
    return json_value_to_bool(value, field == 8 ? &parsed.partial : &parsed.out_of_date);
}
bool self_settings_json(Utf8View source, SelfSettingsJson* parsed) noexcept {
    u32 structural[64]{};
    const auto scan = json_scan_structurals_avx1(source, {structural, 64});
    if (!scan.ok())
        return false;
    parsed->document = {source, {structural, scan.count}};
    return json_visit_root_object(source, parsed->document.structurals, 16, self_settings_json_member,
                                  parsed)
        .ok();
}
// The user's interface locale (localization.locale) becomes X-Discord-Locale,
// and clips.allow_voice_recording (true unless set) the opcode 4 flag;
// settings without them leave the current values.
void observe_settings_metadata(GatewayClientRuntime* runtime, JsonValue value, bool partial) noexcept {
    JsonString encoded{};
    if (!is_user(runtime) || !json_value_as_string(value, &encoded) || encoded.has_escapes)
        return;
    u8 extracted[1024];
    const UserSettingsExtractResult settings = user_settings_proto_extract_base64(
        encoded.encoded,
        user_settings_group_bit(user_settings_localization) | user_settings_group_bit(user_settings_clips),
        {}, {extracted, sizeof(extracted)});
    if (!settings.ok())
        return;
    const ConstSlice<u8> groups{extracted, settings.bytes};
    UserSettingsValue locale{};
    if (user_settings_proto_read(user_settings_proto_group(groups, user_settings_localization),
                                 {user_settings_localization, 1, 0, UserSettingsKind::string}, &locale) &&
        locale.present)
        client_session_metadata_set_locale(session_metadata(runtime), {locale.text, locale.text_bytes});
    const ConstSlice<u8> clips = user_settings_proto_group(groups, user_settings_clips);
    if (partial && !clips.count)
        return;
    UserSettingsValue recording{};
    const bool allowed = !user_settings_proto_read(
                             clips, {user_settings_clips, 1, 0, UserSettingsKind::boolean}, &recording) ||
                         !recording.present || recording.number != 0;
    runtime->voice_recording_disallowed = !allowed;
}

bool self_settings_observe_locked(GatewayClientRuntime* runtime, JsonValue value, bool partial) noexcept {
    observe_settings_metadata(runtime, value, partial);
    auto& owner = runtime->self_status;
    JsonString encoded{};
    u8 group[kSelfStatusSettingsMaximumGroupBytes];
    SelfStatusSettingsProjection projection{};
    const bool decoded = json_value_as_string(value, &encoded) && !encoded.has_escapes &&
                         self_status_settings_decode_base64(encoded.encoded, {group, sizeof(group)},
                                                            &projection) == SelfStatusSettingsError::none;
    if (decoded && !projection.present && partial)
        return true;
    ++owner.settings_revision;
    ++owner.revision_serial;
    if (!decoded || !projection.present || !projection.known) {
        owner.authoritative = false;
        runtime->self_status_error = GatewaySelfStatusError::settings;
        runtime->self_status_group_bytes = 0;
        return false;
    }
    if (!runtime->self_status_group)
        runtime->self_status_group = static_cast<u8*>(VirtualAlloc(
            nullptr, kSelfStatusSettingsMaximumGroupBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime->self_status_group) {
        owner.authoritative = false;
        runtime->self_status_error = GatewaySelfStatusError::unavailable;
        return false;
    }
    SelfCustomStatus before{}, after{};
    bool shared_before = true, shared_after = true;
    self_status_settings_read_custom({runtime->self_status_group, runtime->self_status_group_bytes}, &before);
    self_status_settings_read_show_game({runtime->self_status_group, runtime->self_status_group_bytes},
                                        &shared_before);
    copy_bytes(runtime->self_status_group, group, projection.group_bytes);
    runtime->self_status_group_bytes = projection.group_bytes;
    self_status_settings_read_custom({runtime->self_status_group, runtime->self_status_group_bytes}, &after);
    self_status_settings_read_show_game({runtime->self_status_group, runtime->self_status_group_bytes},
                                        &shared_after);
    // The presence carries the custom status and the shared game; republish
    // when either changes.
    if ((!self_custom_status_equal(before, after) ||
         (shared_before != shared_after && runtime->local_presence.game)) &&
        ++owner.revision == 0)
        owner.revision = 1;
    gateway_self_status_observe(&owner, projection.status);
    runtime->self_status_error = GatewaySelfStatusError::none;
    return true;
}

// One classifier serves the live dispatch and the controlled offline fixture.
// Typed settings2 invalidates the lazy Favorites snapshot only after its
// complete canonical protobuf validates; malformed envelopes cannot fabricate
// a remote settings revision. No settings bank is allocated on this path.
bool self_settings_dispatch(GatewayClient* client, GatewayClientRuntime* runtime,
                            Utf8View source) noexcept {
    SelfSettingsJson parsed{};
    if (!self_settings_json(source, &parsed))
        return false;
    if (parsed.type == 2) {
        if (!parsed.typed_settings || (parsed.seen & 7) != 7)
            return false;
        JsonString encoded{};
        if (!json_value_as_string(parsed.encoded, &encoded) || encoded.has_escapes)
            return false;
        const auto settings = gif_favorites_measure_base64(encoded.encoded);
        if (!settings.ok())
            return false;
        if (parsed.partial && !settings.group_present && !settings.versions_present)
            return true;
        InterlockedIncrement64(&client->frecency_settings_revision);
        if (client->notification_event)
            SetEvent(client->notification_event);
        return true;
    }
    if (parsed.type != 1)
        return false;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    const bool observed = self_settings_observe_locked(runtime, parsed.encoded, parsed.partial);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    if (client->notification_event)
        SetEvent(client->notification_event);
    return observed;
}

// Type 1 settings carry the guild folder layout beside the status group, and feed the app's user
// settings store.
void settings_stores_dispatch(GatewayClient* client, GatewayClientRuntime* runtime,
                              const GatewayEnvelope& envelope, u64 now_ms) noexcept {
    SelfSettingsJson parsed{};
    if (!self_settings_json(envelope.data.source, &parsed) || parsed.type != 1 ||
        parsed.encoded.kind != JsonValueKind::string)
        return;
    RelationshipStreamBinding binding{client, runtime};
    gateway_client_publish_guild_folders(parsed.encoded, parsed.partial, dispatch_scratch(runtime),
                                         envelope.sequence, envelope.has_sequence, now_ms,
                                         publish_relationship_stream, &binding);
    gateway_client_publish_user_settings(parsed.encoded, {}, parsed.partial, dispatch_scratch(runtime),
                                         envelope.sequence, envelope.has_sequence, now_ms,
                                         publish_relationship_stream, &binding);
}

bool begin_resolution(GatewayClient* client, GatewayClientRuntime* runtime, NetworkLoop* loop,
                      u64 now_ms) noexcept {
    runtime->failure_stage = GatewayClientFailureStage::none;
    runtime->session_error = GatewaySessionError::none;
    runtime->handshake_error = WebSocketHandshakeError::none;
    runtime->websocket_error = WebSocketError::none;
    runtime->gateway_decode_error = GatewayDecodeError::none;
    runtime->event_decode_error = GatewayEventDecodeError::none;
    runtime->compression_error = ZstdError::none;
    runtime->etf_error = EtfError::none;
    runtime->json_scan_error = JsonScanError::none;
    runtime->json_read_error = JsonReadError::none;
    runtime->route_error_offset = 0;
    runtime->route_detail = 0;
    byte_queue_clear(&runtime->encrypted_input, true);
    if (!byte_queue_initialize(&runtime->encrypted_input,
                               Slice<u8>{runtime->encrypted_storage, kEncryptedInputBytes}) ||
        !secure_random_u32(&runtime->heartbeat_jitter).ok()) {
        return false;
    }
    if (runtime->session.input.data) {
        byte_queue_clear(&runtime->session.input, true);
    }
    if (is_user(runtime)) {
        clear_user_transport(runtime);
    } else if (runtime->session.message_bytes != 0) {
        SecureZeroMemory(runtime->handshake_output, runtime->session.message_bytes);
    }
    gateway_outbound_reset(&runtime->outbound);
    runtime->qos.has_immediate = false;
    runtime->send_data = nullptr;
    runtime->send_bytes = 0;
    runtime->send_offset = 0;
    runtime->send_purpose = SendPurpose::none;
    runtime->sent_subscription_revision = 0;
    runtime->active_subscription_revision = 0;
    gateway_subscription_thread_owner_reset_connection(&runtime->thread_subscription_owner);
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    gateway_self_status_reset_connection(&runtime->self_status);
    gateway_member_request_reset_connection(&runtime->member_requests, !runtime->resume.valid);
    runtime->member_deadline_ms = 0;
    runtime->channel_info_deadline_ms = 0;
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    runtime->self_status_deadline_ms = 0;
    runtime->rate_deadline_ms = 0;
    runtime->pending_pong_bytes = 0;
    runtime->has_pending_pong = false;
    SecureZeroMemory(runtime->pending_pong, sizeof(runtime->pending_pong));

    if (runtime->resume.valid) {
        if (gateway_endpoint_parse_with_policy(
                Utf8View{runtime->resume.resume_url, runtime->resume.resume_url_bytes},
                GatewayEndpointPolicy::user_v9_etf_zstd_stream, &runtime->endpoint) != GatewayEndpointError::none) {
            return false;
        }
    } else {
        runtime->endpoint = runtime->initial_endpoint;
    }
    runtime->resolver_released = false;
    const NetworkResult resolving = network_resolver_start(
        loop, &runtime->resolver, runtime->endpoint.host_wide, runtime->endpoint.service_wide);
    if (!resolving.ok()) {
        return false;
    }
    runtime->reconnecting = false;
    runtime->reconnect_deadline_ms = 0;
    publish_phase(client, GatewayClientPhase::resolving);
    runtime->deadline_ms = now_ms + kPhaseTimeoutMs;
    return true;
}

bool schedule_reconnect(GatewayClient* client, GatewayClientRuntime* runtime, GatewayLifecycleAction action,
                        u64 now_ms, u32 delay_ms) noexcept {
    runtime->mailbox_deadline_ms = 0;
    if (action != GatewayLifecycleAction::reconnect_identify &&
        action != GatewayLifecycleAction::reconnect_resume) {
        return false;
    }
    if (action == GatewayLifecycleAction::reconnect_identify) {
        reset_voice(client, runtime);
        clear_resume(runtime);
    }
    runtime->reconnecting = true;
    runtime->reconnect_deadline_ms = deadline_after(now_ms, delay_ms);
    runtime->deadline_ms = 0;
    runtime->rate_deadline_ms = 0;
    gateway_outbound_reset(&runtime->outbound);
    runtime->qos.has_immediate = false;
    if (runtime->resolver.pending) {
        network_resolver_cancel(&runtime->resolver);
    }
    network_connection_close(&runtime->connection);
    if (runtime->tls_initialized) {
        tls_client_release(&runtime->tls);
        runtime->tls_initialized = false;
    }
    clear_user_transport(runtime);
    publish_phase(client, GatewayClientPhase::reconnect_wait);
    GatewayClientEvent reconnecting{};
    reconnecting.timestamp_ms = now_ms;
    reconnecting.kind = GatewayClientEventKind::reconnecting;
    reconnecting.has_sequence = runtime->resume.valid;
    reconnecting.sequence = runtime->resume.sequence;
    publish_event(runtime, reconnecting);
    return true;
}

bool schedule_backoff_reconnect(GatewayClient* client, GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    u32 random = 0;
    if (!secure_random_u32(&random).ok()) {
        return false;
    }
    const GatewayReconnectDelay delay = gateway_reconnect_next(&runtime->reconnect_backoff, random);
    const GatewayLifecycleAction action = runtime->resume.valid
                                              ? GatewayLifecycleAction::reconnect_resume
                                              : GatewayLifecycleAction::reconnect_identify;
    return schedule_reconnect(client, runtime, action, now_ms, delay.delay_ms);
}

void begin_shutdown(GatewayClient* client, GatewayClientRuntime* runtime) noexcept {
    if (runtime->mailbox_cancel)
        SetEvent(runtime->mailbox_cancel);
    if (!runtime->stopping) {
        runtime->stopping = true;
        reset_voice(client, runtime);
        if (!runtime->failed) {
            publish_phase(client, GatewayClientPhase::stopping);
        }
        if (runtime->resolver.pending) {
            network_resolver_cancel(&runtime->resolver);
        }
        network_connection_close(&runtime->connection);
        if (runtime->tls_initialized) {
            tls_client_release(&runtime->tls);
            runtime->tls_initialized = false;
        }
        clear_user_transport(runtime);
    }
}

bool shutdown_drained(GatewayClientRuntime* runtime) noexcept {
    if (runtime->resolver.pending || runtime->outstanding_socket_operations != 0 ||
        !voice_connection_drained(runtime->voice_connection) || !gateway_stream_session_drained(runtime->stream)) {
        return false;
    }
    release_resolver(runtime);
    return true;
}

NetworkWorkerStep fail_and_drain(GatewayClient* client, GatewayClientRuntime* runtime,
                                 GatewayClientError error, u32 native_error) noexcept {
    if (error == GatewayClientError::session && runtime->failure_stage == GatewayClientFailureStage::none) {
        runtime->failure_stage = GatewayClientFailureStage::application_output;
    }
    runtime->failed = true;
    GatewayClientEvent event{};
    event.timestamp_ms = GetTickCount64();
    event.native_error = native_error;
    event.kind = GatewayClientEventKind::failed;
    event.error = error;
    event.failure_stage = runtime->failure_stage;
    event.session_error = runtime->session_error;
    event.handshake_error = runtime->handshake_error;
    event.websocket_error = runtime->websocket_error;
    event.gateway_decode_error = runtime->gateway_decode_error;
    event.event_decode_error = runtime->event_decode_error;
    event.compression_error = runtime->compression_error;
    event.etf_error = runtime->etf_error;
    event.json_scan_error = runtime->json_scan_error;
    event.json_read_error = runtime->json_read_error;
    event.route_error_offset = runtime->route_error_offset;
    event.route_detail = runtime->route_detail;
    publish_event(runtime, event);
    // A consumer woken by the event above may run before the terminal status
    // is visible (or the mailbox was full): record the failure, then wake it again.
    publish_failure(client, error, native_error);
    if (client->notification_event)
        SetEvent(client->notification_event);
    begin_shutdown(client, runtime);
    return NetworkWorkerStep{INFINITE, shutdown_drained(runtime)};
}

NetworkWorkerStep reconnect_or_fail(GatewayClient* client, GatewayClientRuntime* runtime,
                                    GatewayClientError error, u32 native_error, u64 now_ms) noexcept {
    if (runtime->authenticated && schedule_backoff_reconnect(client, runtime, now_ms)) {
        return NetworkWorkerStep{next_worker_wait(client, runtime, now_ms), false};
    }
    return fail_and_drain(client, runtime, error, native_error);
}

bool start_receive(GatewayClientRuntime* runtime) noexcept {
    const Slice<u8> writable = byte_queue_writable(&runtime->encrypted_input);
    if (!writable.data || writable.count == 0 ||
        !network_connection_receive(&runtime->connection, writable).ok()) {
        return false;
    }
    ++runtime->outstanding_socket_operations;
    return true;
}

bool ensure_receive(GatewayClientRuntime* runtime) noexcept {
    return runtime->connection.receive_pending || start_receive(runtime);
}

bool start_send(GatewayClientRuntime* runtime, ConstSlice<u8> bytes, SendPurpose purpose) noexcept {
    if (!bytes.data || bytes.count == 0) {
        return false;
    }
    runtime->send_data = bytes.data;
    runtime->send_bytes = bytes.count;
    runtime->send_offset = 0;
    runtime->send_purpose = purpose;
    if (!network_connection_send(&runtime->connection, bytes).ok()) {
        return false;
    }
    ++runtime->outstanding_socket_operations;
    return true;
}

bool continue_send(GatewayClientRuntime* runtime) noexcept {
    const ConstSlice<u8> remaining{runtime->send_data + runtime->send_offset,
                                   runtime->send_bytes - runtime->send_offset};
    if (!network_connection_send(&runtime->connection, remaining).ok()) {
        return false;
    }
    ++runtime->outstanding_socket_operations;
    return true;
}

bool start_connect_candidate(NetworkLoop* loop, GatewayClientRuntime* runtime) noexcept {
    while (runtime->next_address) {
        ADDRINFOEXW* candidate = runtime->next_address;
        runtime->next_address = runtime->next_address->ai_next;
        if (!candidate->ai_addr || candidate->ai_addrlen == 0 ||
            candidate->ai_addrlen > sizeof(sockaddr_storage) ||
            (candidate->ai_family != AF_INET && candidate->ai_family != AF_INET6)) {
            continue;
        }
        const NetworkResult started = network_connection_connect(
            loop, &runtime->connection, candidate->ai_addr, static_cast<i32>(candidate->ai_addrlen));
        if (started.ok()) {
            ++runtime->outstanding_socket_operations;
            return true;
        }
    }
    return false;
}

// The account's cookies for this endpoint: the page reaches the Gateway
// cross-site, so only its SameSite=None cookies apply.
CookieJar* endpoint_cookies(const GatewayClientRuntime* runtime, CookieRequest* request) noexcept {
    const ClientSessionMetadata* metadata = session_metadata(runtime);
    const GatewayEndpoint& endpoint = runtime->endpoint;
    if (!metadata || !metadata->cookies || !endpoint.host_bytes || endpoint.authority[0] == '[')
        return nullptr;
    *request = {{endpoint.authority, endpoint.host_bytes}, {endpoint.target, endpoint.target_bytes}, false};
    return metadata->cookies;
}

bool initialize_session(GatewayClientRuntime* runtime,
                        const u8 client_key[kWebSocketClientKeyBytes]) noexcept {
    // A connection lost while a large payload was spilled leaves its pages behind.
    reserved_bytes_decommit(&runtime->compressed_spill);
    if (!gateway_session_initialize(
            &runtime->session, Slice<u8>{runtime->session_input, kSessionInputBytes},
            is_user(runtime) ? Slice<u8>{runtime->handshake_output + kMessageBytes, kMessageBytes}
                             : Slice<u8>{runtime->handshake_output, kMessageBytes},
            main_index_view(runtime), ConstSlice<u8>{client_key, kWebSocketClientKeyBytes},
            kSessionInputBytes - 14, kMessageBytes, runtime->resume.valid, runtime->resume.valid,
            runtime->resume.sequence)) {
        return false;
    }
    runtime->session.grow_index = grow_main_index;
    runtime->session.grow_index_context = runtime;
    runtime->session.cookies = endpoint_cookies(runtime, &runtime->session.cookie_request);
    return !is_user(runtime) ||
           (zstd_stream_initialize(&runtime->user->zstd, zstd_storage_view(runtime), kZstdStorageBytes,
                                   grow_zstd_storage, runtime) &&
            gateway_session_enable_zstd_etf(&runtime->session, &runtime->user->zstd,
                                            {runtime->handshake_output, kMessageBytes}) &&
            gateway_session_enable_output_growth(&runtime->session, decoded_message_view(runtime),
                                                 kGatewaySessionMaximumDecodedMessageBytes,
                                                 grow_decoded_message, runtime) &&
            (!runtime->compressed_spill.base ||
             gateway_session_enable_compressed_growth(&runtime->session, kGatewaySessionMaximumDecodedMessageBytes,
                                                      grow_compressed_message, runtime)));
}

bool begin_websocket_opening(GatewayClientRuntime* runtime) noexcept {
    u8 nonce[kWebSocketNonceBytes]{};
    u8 client_key[kWebSocketClientKeyBytes]{};
    if (!secure_random_fill(Slice<u8>{nonce, kWebSocketNonceBytes}).ok()) {
        return false;
    }
    const WebSocketHandshakeWriteResult encoded = websocket_encode_client_key(
        ConstSlice<u8>{nonce, kWebSocketNonceBytes}, Slice<u8>{client_key, kWebSocketClientKeyBytes});
    SecureZeroMemory(runtime->handshake_output, kMessageBytes);
    if (!encoded.ok() || !initialize_session(runtime, client_key)) {
        return false;
    }
    const ClientSessionMetadata* metadata = session_metadata(runtime);
    CookieRequest cookie_request{};
    CookieJar* cookies = endpoint_cookies(runtime, &cookie_request);
    u8 cookie[kCookieHeaderMaximumBytes];
    const u32 cookie_bytes =
        cookies ? cookie_jar_header(cookies, cookie_request, clock_unix_milliseconds(), {cookie, sizeof(cookie)})
                : 0;
    const WebSocketPageFields page{
        metadata ? ConstSlice<u8>{reinterpret_cast<const u8*>(metadata->accept_language.data),
                                  metadata->accept_language.bytes}
                 : ConstSlice<u8>{},
        {cookie, cookie_bytes}};
    const WebSocketHandshakeWriteResult request = websocket_write_opening_request(
        ConstSlice<u8>{reinterpret_cast<const u8*>(runtime->endpoint.target),
                       runtime->endpoint.target_bytes},
        ConstSlice<u8>{reinterpret_cast<const u8*>(runtime->endpoint.authority),
                       runtime->endpoint.authority_bytes},
        ConstSlice<u8>{client_key, kWebSocketClientKeyBytes},
        Slice<u8>{runtime->plain_output, kPlainOutputBytes}, &page);
    secure_clear_bytes(cookie, cookie_bytes);
    if (!request.ok()) {
        return false;
    }
    u32 encrypted_bytes = 0;
    const bool encrypted =
        tls_client_encrypt(&runtime->tls, ConstSlice<u8>{runtime->plain_output, request.bytes},
                           Slice<u8>{runtime->record_output, kRecordOutputBytes}, &encrypted_bytes)
            .ok();
    // The request carries cookies.
    secure_clear_bytes(runtime->plain_output, request.bytes);
    if (!encrypted) {
        return false;
    }
    return start_send(runtime, ConstSlice<u8>{runtime->record_output, encrypted_bytes},
                      SendPurpose::websocket_opening);
}

bool drive_tls_handshake(GatewayClientRuntime* runtime) noexcept {
    const ConstSlice<u8> incoming = byte_queue_readable(runtime->encrypted_input);
    const TlsHandshakeStep step = tls_client_handshake(
        &runtime->tls, incoming, Slice<u8>{runtime->handshake_output, kHandshakeBytes});
    if (!step.ok() || !byte_queue_consume(&runtime->encrypted_input, step.consumed)) {
        return false;
    }
    if (step.produced != 0) {
        runtime->after_tls_send = step.state;
        return start_send(runtime, ConstSlice<u8>{runtime->handshake_output, step.produced},
                          SendPurpose::tls_handshake);
    }
    if (step.state == TlsHandshakeState::complete) {
        return begin_websocket_opening(runtime);
    }
    return step.state == TlsHandshakeState::need_receive && start_receive(runtime);
}

bool queue_pong(GatewayClientRuntime* runtime, ConstSlice<u8> payload) noexcept {
    if (runtime->has_pending_pong || payload.count > sizeof(runtime->pending_pong)) {
        return false;
    }
    for (u32 index = 0; index < payload.count; ++index) {
        runtime->pending_pong[index] = payload[index];
    }
    runtime->pending_pong_bytes = static_cast<u8>(payload.count);
    runtime->has_pending_pong = true;
    return true;
}

bool send_pending_pong(GatewayClientRuntime* runtime) noexcept {
    if (!runtime->has_pending_pong || runtime->send_purpose != SendPurpose::none) {
        return false;
    }
    u32 masking_key = 0;
    if (!secure_random_u32(&masking_key).ok()) {
        return false;
    }
    const WebSocketWriteResult framed = websocket_write_client_frame(
        WebSocketOpcode::pong, true, ConstSlice<u8>{runtime->pending_pong, runtime->pending_pong_bytes},
        masking_key, Slice<u8>{runtime->plain_output, kPlainOutputBytes});
    if (!framed.ok()) {
        return false;
    }
    u32 encrypted_bytes = 0;
    if (!tls_client_encrypt(&runtime->tls, ConstSlice<u8>{runtime->plain_output, framed.bytes},
                            Slice<u8>{runtime->record_output, kRecordOutputBytes}, &encrypted_bytes)
             .ok()) {
        return false;
    }
    if (!start_send(runtime, ConstSlice<u8>{runtime->record_output, encrypted_bytes}, SendPurpose::pong)) {
        return false;
    }
    SecureZeroMemory(runtime->pending_pong, runtime->pending_pong_bytes);
    runtime->pending_pong_bytes = 0;
    runtime->has_pending_pong = false;
    return true;
}

enum class OutboundDrive : u8 {
    idle,
    sending,
    rate_limited,
    failed,
};

// Frames and sends the command written to gateway_payload (as ETF on the user
// Gateway); the plaintext copies are erased.
OutboundDrive send_control_command(GatewayClientRuntime* runtime, GatewayCommandWriteResult command,
                                   SendPurpose purpose) noexcept {
    if (!command.ok() || command.bytes == 0 || command.bytes > sizeof(runtime->gateway_payload)) {
        return OutboundDrive::failed;
    }
    ConstSlice<u8> payload{runtime->gateway_payload, command.bytes};
    WebSocketOpcode opcode = WebSocketOpcode::text;
    if (is_user(runtime)) {
        const EtfResult packed =
            etf_from_json({reinterpret_cast<const char*>(runtime->gateway_payload), command.bytes},
                          {runtime->gateway_etf, kEtfCommandBytes});
        SecureZeroMemory(runtime->gateway_payload, command.bytes);
        if (!packed.ok()) {
            SecureZeroMemory(runtime->gateway_etf, kEtfCommandBytes);
            return OutboundDrive::failed;
        }
        payload = {runtime->gateway_etf, packed.bytes};
        opcode = WebSocketOpcode::binary;
    }
    u32 masking_key = 0;
    if (!secure_random_u32(&masking_key).ok()) {
        SecureZeroMemory(const_cast<u8*>(payload.data), payload.count);
        return OutboundDrive::failed;
    }
    const WebSocketWriteResult framed = websocket_write_client_frame(
        opcode, true, payload, masking_key, Slice<u8>{runtime->plain_output, kPlainOutputBytes});
    SecureZeroMemory(runtime->gateway_etf, kEtfCommandBytes);
    if (!framed.ok()) {
        SecureZeroMemory(runtime->gateway_payload, command.bytes);
        return OutboundDrive::failed;
    }
    u32 encrypted_bytes = 0;
    if (!tls_client_encrypt(&runtime->tls, ConstSlice<u8>{runtime->plain_output, framed.bytes},
                            Slice<u8>{runtime->record_output, kRecordOutputBytes}, &encrypted_bytes)
             .ok()) {
        SecureZeroMemory(runtime->gateway_payload, command.bytes);
        SecureZeroMemory(runtime->plain_output, framed.bytes);
        return OutboundDrive::failed;
    }
    SecureZeroMemory(runtime->gateway_payload, command.bytes);
    SecureZeroMemory(runtime->plain_output, framed.bytes);
    if (!start_send(runtime, ConstSlice<u8>{runtime->record_output, encrypted_bytes}, purpose)) {
        SecureZeroMemory(runtime->record_output, encrypted_bytes);
        return OutboundDrive::failed;
    }
    return OutboundDrive::sending;
}

// The user identify into gateway_payload: the launch's first is the
// fast-connect one, later ones carry the connect reasons and QoS token.
GatewayCommandWriteResult write_user_identify_command(GatewayClientRuntime* runtime, Utf8View token) noexcept {
    char heartbeat_id[kGatewayUserLaunchIdBytes];
    char fast_connect_id[kGatewayUserLaunchIdBytes + 1];
    char qos_token[kGatewayQosTokenMaximumBytes];
    auto identify = user_identify(*runtime->user, token, heartbeat_id);
    if (take_fast_connect(fast_connect_id)) {
        identify.fast_connect = true;
        identify.properties.client_launch_id = {fast_connect_id, kGatewayUserLaunchIdBytes};
    } else {
        identify.connect_reasons = utf8_literal(kGatewayConnectReasons);
        const GatewayUserRuntime& user = *runtime->user;
        identify.qos_token = {qos_token, gateway_qos_token_write(qos_active(runtime->qos), user.has_qos_derived,
                                                                 {user.qos_derived, user.qos_derived_bytes},
                                                                 {qos_token, sizeof(qos_token)})};
    }
    AcquireSRWLockShared(&runtime->subscription_lock);
    if (runtime->self_status.authoritative) {
        identify.status = runtime->self_status.desired.status;
        identify.since_ms = runtime->self_status.desired.since_ms;
        identify.has_since = runtime->self_status.desired.has_since;
        identify.afk = runtime->self_status.desired.afk;
    }
    ReleaseSRWLockShared(&runtime->subscription_lock);
    const GatewayCommandWriteResult command =
        gateway_write_user_identify(identify, Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    SecureZeroMemory(qos_token, sizeof(qos_token));
    return command;
}

OutboundDrive drive_gateway_outbound(GatewayClient* client, GatewayClientRuntime* runtime,
                                     u64 now_ms) noexcept {
    if (runtime->send_purpose != SendPurpose::none) {
        return OutboundDrive::sending;
    }
    const GatewayOutboundStep next = gateway_outbound_take(&runtime->outbound, now_ms);
    if (next.state == GatewayOutboundState::idle) {
        runtime->rate_deadline_ms = 0;
        return OutboundDrive::idle;
    }
    if (next.state == GatewayOutboundState::rate_limited) {
        runtime->rate_deadline_ms = next.next_allowed_ms;
        return OutboundDrive::rate_limited;
    }
    if (next.state != GatewayOutboundState::action || !runtime->credential.data ||
        !runtime->credential.locked) {
        return OutboundDrive::failed;
    }

    GatewayCommandWriteResult command{};
    const ConstSlice<u8> credential = secure_buffer_view(runtime->credential);
    const Utf8View token{reinterpret_cast<const char*>(credential.data), credential.count};
    if (next.action == GatewayLifecycleAction::send_identify && is_user(runtime)) {
        command = write_user_identify_command(runtime, token);
    } else if (next.action == GatewayLifecycleAction::send_resume && runtime->resume.valid) {
        const GatewayResume resume{token,
                                   Utf8View{runtime->resume.session_id, runtime->resume.session_id_bytes},
                                   runtime->resume.sequence};
        command = gateway_write_resume(
            resume, Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    } else if (next.action == GatewayLifecycleAction::send_heartbeat) {
        GatewayQosPayload qos{};
        const bool has_qos = gateway_qos_take(&runtime->qos, &qos);
        const GatewayWriteResult heartbeat =
            gateway_write_heartbeat(runtime->session.lifecycle, has_qos ? &qos : nullptr,
                                    Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
        command.bytes = heartbeat.bytes;
        command.error = heartbeat.ok() ? GatewayCommandError::none : GatewayCommandError::output_too_small;
    } else if (next.action == GatewayLifecycleAction::send_time_spent_session && is_user(runtime)) {
        command = gateway_write_time_spent_session(
            runtime->time_spent.created_ms, {runtime->time_spent.uuid, kGatewayUserLaunchIdBytes},
            user_text(runtime->user->client_launch_id),
            Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    } else {
        return OutboundDrive::failed;
    }
    if (send_control_command(runtime, command, SendPurpose::gateway_action) != OutboundDrive::sending)
        return OutboundDrive::failed;
    runtime->rate_deadline_ms = 0;

    GatewayClientEvent event{};
    event.timestamp_ms = now_ms;
    if (next.action == GatewayLifecycleAction::send_identify) {
        event.kind = GatewayClientEventKind::identifying;
        publish_phase(client, GatewayClientPhase::identifying);
    } else if (next.action == GatewayLifecycleAction::send_resume) {
        event.kind = GatewayClientEventKind::resuming;
        publish_phase(client, GatewayClientPhase::resuming);
    } else if (next.action == GatewayLifecycleAction::send_heartbeat) {
        event.kind = GatewayClientEventKind::heartbeat_sent;
        event.has_sequence = runtime->session.lifecycle.has_sequence;
        event.sequence = runtime->session.lifecycle.last_sequence;
    } else {
        return OutboundDrive::sending;
    }
    publish_event(runtime, event);
    return OutboundDrive::sending;
}

// Takes one send from the outbound rate limit; otherwise records when the next one is allowed.
bool acquire_send(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    const auto rate = gateway_rate_limit_acquire(&runtime->outbound.limiter, now_ms);
    if (!rate.acquired)
        runtime->rate_deadline_ms = rate.next_allowed_ms;
    return rate.acquired;
}

OutboundDrive drive_guild_subscription(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->send_purpose != SendPurpose::none ||
        runtime->identity != GatewayClientIdentity::normal_user ||
        runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready) {
        return OutboundDrive::idle;
    }
    GatewayGuildSubscription subscription{};
    u32 revision = 0;
    AcquireSRWLockShared(&runtime->subscription_lock);
    copy_value(&subscription, runtime->desired_subscription);
    revision = runtime->subscription_revision;
    ReleaseSRWLockShared(&runtime->subscription_lock);
    if (revision == 0 || revision == runtime->sent_subscription_revision) {
        return OutboundDrive::idle;
    }
    GatewayGuildSubscription prepared{};
    if (!gateway_subscription_thread_owner_prepare(runtime->thread_subscription_owner, subscription,
                                                   &prepared))
        return OutboundDrive::failed;
    if (!acquire_send(runtime, now_ms))
        return OutboundDrive::rate_limited;
    const GatewayCommandWriteResult command = gateway_write_guild_subscriptions_bulk(
        prepared, Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    const auto result = send_control_command(runtime, command, SendPurpose::guild_subscription);
    if (result == OutboundDrive::sending) {
        runtime->active_subscription_revision = revision;
        gateway_subscription_thread_owner_admit(&runtime->thread_subscription_owner, prepared);
    }
    return result;
}

OutboundDrive drive_voice_output(GatewayClient* client, GatewayClientRuntime* runtime,
                                 u64 now_ms) noexcept {
    if (!is_user(runtime) || runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready)
        return OutboundDrive::idle;
    gateway_voice_expire(&runtime->voice, now_ms);
    GatewayVoiceStateUpdate desired{};
    AcquireSRWLockExclusive(&runtime->voice_lock);
    const bool requested = runtime->voice_request_pending;
    if (requested) {
        desired = runtime->desired_voice;
        runtime->voice_request_pending = false;
    }
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    if (requested) {
        gateway_voice_request(&runtime->voice, desired);
        if (!runtime->voice.setup.user_id) {
            const auto bound =
                voice_setup_bind_utf8(&runtime->voice.setup, runtime->self_user_id,
                                      {runtime->resume.session_id, runtime->resume.session_id_bytes});
            if (bound != VoiceSetupResult::updated)
                gateway_voice_cancel(&runtime->voice, GatewayVoiceError::storage);
        }
    }
    publish_voice_snapshot(client, runtime);
    GatewayVoiceStateUpdate state{};
    if (!gateway_voice_next(runtime->voice, &state))
        return OutboundDrive::idle;
    if (!acquire_send(runtime, now_ms))
        return OutboundDrive::rate_limited;
    const auto command = gateway_write_voice_state(
        state, runtime->voice_recording_disallowed ? 0 : kGatewayVoiceStateAllowVoiceRecording,
        {runtime->gateway_payload, static_cast<u32>(sizeof(runtime->gateway_payload))});
    const auto result = send_control_command(runtime, command, SendPurpose::voice_state);
    if (result == OutboundDrive::sending && !gateway_voice_submitted(&runtime->voice, state, now_ms))
        return OutboundDrive::failed;
    publish_voice_snapshot(client, runtime);
    return result;
}

// Go Live commands (ops 19-21). UI watch requests apply here, after the voice
// request, so a join and a watch posted together see the joined channel.
OutboundDrive drive_stream_output(GatewayClient* client, GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    if (!is_user(runtime) || runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready)
        return OutboundDrive::idle;
    auto& control = runtime->stream.control;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    const bool requested = runtime->stream_request_pending;
    const GatewayStreamKey desired = runtime->desired_stream;
    runtime->stream_request_pending = false;
    const bool volume = runtime->stream_volume_pending;
    if (volume)
        gateway_stream_session_set_volume(&runtime->stream, runtime->desired_stream_volume);
    runtime->stream_volume_pending = false;
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    if (volume && !requested)
        publish_voice_snapshot(client, runtime);
    if (requested) {
        if (desired.type == GatewayStreamType::none)
            gateway_stream_stop(&control, GatewayStreamEnd::stopped);
        else {
            if (!control.setup.user_id)
                gateway_stream_bind(&control, runtime->self_user_id,
                                    {runtime->resume.session_id, runtime->resume.session_id_bytes});
            gateway_stream_watch(&control, desired, now_ms);
        }
        publish_voice_snapshot(client, runtime);
    }
    GatewayStreamCommand command{};
    if (!gateway_stream_next(control, &command))
        return OutboundDrive::idle;
    if (!acquire_send(runtime, now_ms))
        return OutboundDrive::rate_limited;
    const auto written =
        gateway_stream_write(command, {runtime->gateway_payload, static_cast<u32>(sizeof(runtime->gateway_payload))});
    if (!written.ok()) {
        gateway_stream_submitted(&control, command); // Unencodable: drop it.
        return OutboundDrive::idle;
    }
    const auto result = send_control_command(runtime, written, SendPurpose::stream);
    if (result == OutboundDrive::sending)
        gateway_stream_submitted(&control, command);
    return result;
}

OutboundDrive drive_self_status(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    if (!is_user(runtime) || runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready)
        return OutboundDrive::idle;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    auto& owner = runtime->self_status;
    if (!owner.authoritative || !gateway_self_status_pending(owner)) {
        runtime->self_status_deadline_ms = 0;
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::idle;
    }
    runtime->self_status_deadline_ms = gateway_self_status_deadline(&owner, now_ms);
    if (runtime->self_status_deadline_ms) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::idle; // Presence cap must not delay other traffic.
    }
    if (!acquire_send(runtime, now_ms)) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::rate_limited;
    }
    SelfCustomStatus custom{};
    GatewayCustomStatusActivity activity{};
    const ConstSlice<u8> status_group{runtime->self_status_group, runtime->self_status_group_bytes};
    const bool visible = owner.desired.status != GatewayUserStatus::invisible;
    const bool publish = visible && self_status_settings_read_custom(status_group, &custom) &&
                         self_custom_status_active(custom, clock_unix_milliseconds());
    if (publish)
        activity = {{custom.text, custom.text_bytes}, {custom.emoji_name, custom.emoji_name_bytes}, custom.emoji_id,
                    custom.expires_at_ms};
    // The game is shared only while "Share my activity" is on.
    const auto& local = runtime->local_presence;
    bool share = true;
    const bool game = visible && local.game && self_status_settings_read_show_game(status_group, &share) && share;
    const GatewayGameActivity played{{local.game_name, local.game_name_bytes}, local.game_application_id,
                                     local.game_started_ms, local.game_ends_ms,
                                     {local.game_details, local.game_details_bytes},
                                     {local.game_state, local.game_state_bytes}, local.game_type};
    const auto written = gateway_write_presence_update(
        gateway_presence_effective(owner.desired, &local), publish ? &activity : nullptr, game ? &played : nullptr,
        {runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    const auto sent = send_control_command(runtime, written, SendPurpose::self_status);
    if (sent == OutboundDrive::sending)
        gateway_self_status_admit(&owner, owner.revision, now_ms);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return sent;
}
OutboundDrive drive_member_requests(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready ||
        runtime->identity != GatewayClientIdentity::normal_user)
        return OutboundDrive::idle;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    const GatewayGuildMemberRequest* next = gateway_member_request_next(runtime->member_requests, now_ms);
    runtime->member_deadline_ms = gateway_member_request_deadline(runtime->member_requests, now_ms);
    if (!next) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::idle;
    }
    if (!acquire_send(runtime, now_ms)) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::rate_limited;
    }
    runtime->rate_deadline_ms = 0;
    const auto command =
        gateway_write_guild_member_request(*next, {runtime->gateway_payload, kGatewayUserMaximumCommandBytes});
    const auto result = send_control_command(runtime, command, SendPurpose::guild_members);
    if (result == OutboundDrive::sending)
        gateway_member_request_sent(&runtime->member_requests, next);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return result;
}

// Request Channel Info (opcode 43), once per guild per session.
OutboundDrive drive_channel_info(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready ||
        runtime->identity != GatewayClientIdentity::normal_user)
        return OutboundDrive::idle;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    const u64 guild = gateway_channel_info_next(runtime->channel_info, now_ms);
    runtime->channel_info_deadline_ms = gateway_channel_info_deadline(runtime->channel_info, now_ms);
    if (!guild) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::idle;
    }
    if (!acquire_send(runtime, now_ms)) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::rate_limited;
    }
    const auto command =
        gateway_write_channel_info_request(guild, {runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    const auto result = send_control_command(runtime, command, SendPurpose::channel_info);
    if (result == OutboundDrive::sending)
        gateway_channel_info_sent(&runtime->channel_info, guild);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return result;
}

OutboundDrive drive_application_output(GatewayClient* client, GatewayClientRuntime* runtime,
                                       u64 now_ms) noexcept {
    if (runtime->send_purpose != SendPurpose::none) {
        return OutboundDrive::sending;
    }
    if (runtime->has_pending_pong) {
        return send_pending_pong(runtime) ? OutboundDrive::sending : OutboundDrive::failed;
    }
    const OutboundDrive lifecycle = drive_gateway_outbound(client, runtime, now_ms);
    if (lifecycle != OutboundDrive::idle)
        return lifecycle;
    const auto voice = drive_voice_output(client, runtime, now_ms);
    if (voice != OutboundDrive::idle)
        return voice;
    const auto stream = drive_stream_output(client, runtime, now_ms);
    if (stream != OutboundDrive::idle)
        return stream;
    const auto presence = drive_self_status(runtime, now_ms);
    if (presence != OutboundDrive::idle)
        return presence;
    // Finite targeted batches precede last-write-wins viewport subscriptions;
    // continuous sidebar scrolling cannot starve visible-author metadata.
    const auto members = drive_member_requests(runtime, now_ms);
    if (members != OutboundDrive::idle)
        return members;
    const auto subscription = drive_guild_subscription(runtime, now_ms);
    return subscription == OutboundDrive::idle ? drive_channel_info(runtime, now_ms) : subscription;
}

enum class PlaintextDrive : u8 {
    receive,
    sending,
    hello,
    reconnecting,
    failed,
};

PlaintextDrive fail_plaintext(GatewayClientRuntime* runtime, GatewayClientFailureStage stage) noexcept {
    runtime->failure_stage = stage;
    return PlaintextDrive::failed;
}

PlaintextDrive fail_session_event(GatewayClientRuntime* runtime,
                                  const GatewaySessionEvent& event) noexcept {
    runtime->failure_stage = GatewayClientFailureStage::session_event;
    runtime->session_error = event.error;
    runtime->handshake_error = event.handshake_error;
    runtime->websocket_error = event.websocket_error;
    runtime->gateway_decode_error = event.gateway_error;
    runtime->json_scan_error = event.scan_error;
    runtime->event_decode_error = event.event_error;
    runtime->compression_error = event.zstd_error;
    runtime->etf_error = event.etf_error;
    return PlaintextDrive::failed;
}

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

bool flush_ready_users(ReadyStatePublishContext* context) noexcept {
    if (!context->user_count)
        return true;
    GatewayClientEvent event = gateway_dispatch_event(GatewayClientDispatchKind::user_snapshot,
                                                      context->sequence, context->now_ms);
    event.state.record_count = context->user_count;
    event.dispatch_payload_bytes = context->user_bytes;
    if (!publish_event_payload(
            context->client, context->runtime, event,
            {reinterpret_cast<const u8*>(context->payload_scratch.data), context->user_bytes})) {
        context->publish_failed = true;
        return false;
    }
    context->user_count = context->user_bytes = 0;
    return true;
}

bool publish_ready_state_record(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<ReadyStatePublishContext*>(opaque);
    if (context->kind == ReadyRecordKind::user) {
        // Every maximum-size record, including serialized appearance, fits in
        // the existing batch buffer. Append directly; do not stage a second
        // copy in a smaller stack buffer.
        static_assert(64 * (sizeof(GatewayClientUserMutation) + 256 + 34 + 1024) <=
                      kHandshakeBytes - kMessageBytes);
        if (context->user_count == 64 && !flush_ready_users(context))
            return false;
        GatewayPayloadWriter batch{context->payload_scratch, context->user_bytes};
        context->route = gateway_append_user(value, context->object_scratch, 64, &batch);
        if (!context->route.ok())
            return false;
        context->user_bytes = batch.used;
        ++context->user_count;
        return true;
    }
    GatewayEnvelope envelope{};
    envelope.data = value;
    envelope.event_name =
        JsonString{context->kind == ReadyRecordKind::private_channel ? utf8_literal("CHANNEL_CREATE")
                                                                     : utf8_literal("READY_GUILD"),
                   0, false};
    envelope.sequence = context->sequence;
    envelope.opcode = 0;
    envelope.has_sequence = true;
    envelope.has_event_name = true;

    GatewayClientEvent record{};
    ConstSlice<u8> payload{};
    context->route = gateway_client_route_dispatch(envelope, context->object_scratch, 64,
                                                   context->payload_scratch, Slice<u64>{nullptr, 0},
                                                   context->runtime->self_user_id, &record, &payload);
    const bool expected = context->kind == ReadyRecordKind::private_channel
                              ? record.dispatch_kind == GatewayClientDispatchKind::channel_create &&
                                    record.state.guild_id == 0 &&
                                    (record.state.type == 1 || record.state.type == 3)
                              : record.dispatch_kind == GatewayClientDispatchKind::guild_create;
    if (!context->route.ok() || !expected) {
        if (context->route.ok()) {
            context->route.error = GatewayClientDispatchRouteError::decode;
            context->route.event_error = GatewayEventDecodeError::invalid_data;
        }
        return false;
    }
    record.timestamp_ms = context->now_ms;
    if (!publish_event_payload(context->client, context->runtime, record, payload)) {
        context->publish_failed = true;
        return false;
    }
    return true;
}

ReadyStatePublishContext publish_ready_state(GatewayClient* client, GatewayClientRuntime* runtime,
                                             JsonValue records, ReadyRecordKind kind, u64 sequence,
                                             u64 now_ms) noexcept {
    ReadyStatePublishContext context{};
    context.client = client;
    context.runtime = runtime;
    context.sequence = sequence;
    context.now_ms = now_ms;
    context.kind = kind;
    context.payload_scratch =
        Slice<char>{reinterpret_cast<char*>(runtime->handshake_output + kMessageBytes),
                    kHandshakeBytes - kMessageBytes};

    const JsonScanResult scan = json_scan_structurals_avx1(records.source, main_index_view(runtime));
    if (!scan.ok()) {
        context.route.error = GatewayClientDispatchRouteError::decode;
        context.route.event_error = GatewayEventDecodeError::scan_failed;
        context.route.scan_error = scan.error;
        context.route.error_offset = scan.error_offset;
        return context;
    }

    // The outer READY array still owns runtime->structural_scratch while its
    // visitor runs. The plaintext staging buffer is idle during inbound
    // dispatch, so reuse it as distinct per-record structural scratch instead
    // of reserving 128 KiB on the network-worker stack.
    context.object_scratch = object_scratch(runtime);
    if (kind == ReadyRecordKind::user && records.kind == JsonValueKind::object) {
        if (publish_ready_state_record(&context, 0, records))
            flush_ready_users(&context);
        return context;
    }
    const JsonReadResult read =
        json_visit_value_array(records, ConstSlice<u32>{main_index_view(runtime).data, scan.count},
                              64, publish_ready_state_record, &context);
    if (!read.ok() && context.route.ok() && !context.publish_failed) {
        context.route.error = GatewayClientDispatchRouteError::decode;
        context.route.event_error = GatewayEventDecodeError::invalid_json;
        context.route.read_error = read.error;
        context.route.error_offset = read.error_offset;
    }
    if (context.kind == ReadyRecordKind::user && context.route.ok() && !context.publish_failed)
        flush_ready_users(&context);
    return context;
}

PlaintextDrive drive_queued_actions(GatewayClient* client, GatewayClientRuntime* runtime,
                                    u64 now_ms) noexcept {
    const OutboundDrive outbound = drive_application_output(client, runtime, now_ms);
    return outbound == OutboundDrive::failed
               ? PlaintextDrive::failed
               : (outbound == OutboundDrive::sending ? PlaintextDrive::sending : PlaintextDrive::receive);
}

// A dispatch that cannot be handed to the consumer ends the connection; the session resumes.
PlaintextDrive resume_reconnect(GatewayClient* client, GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    return schedule_reconnect(client, runtime, GatewayLifecycleAction::reconnect_resume, now_ms, 0)
               ? PlaintextDrive::reconnecting
               : PlaintextDrive::failed;
}

// After a handled dispatch, so a RESUME continues behind it.
void checkpoint_resume_sequence(GatewayClientRuntime* runtime) noexcept {
    if (runtime->resume.valid && runtime->session.lifecycle.has_sequence)
        publish_resume_sequence(runtime, runtime->session.lifecycle.last_sequence);
}

// A decode failure as a route failure.
void set_decode_failure(GatewayClientDispatchRouteResult* routed, const GatewayEventDecodeResult& decoded) noexcept {
    routed->error = GatewayClientDispatchRouteError::decode;
    routed->event_error = decoded.error;
    routed->scan_error = decoded.scan_error;
    routed->read_error = decoded.read_error;
    routed->error_offset = decoded.error_offset;
}

// A route failure's categorical codes, kept for the terminal failure report.
void note_route_error(GatewayClientRuntime* runtime, const GatewayClientDispatchRouteResult& route) noexcept {
    runtime->event_decode_error = route.event_error;
    runtime->json_scan_error = route.scan_error;
    runtime->json_read_error = route.read_error;
    runtime->route_error_offset = route.error_offset;
    runtime->route_detail = route.detail;
}

void set_route_error(GatewayClientEvent* event, const GatewayClientDispatchRouteResult& route) noexcept {
    event->event_decode_error = route.event_error;
    event->json_scan_error = route.scan_error;
    event->json_read_error = route.read_error;
    event->route_error_offset = route.error_offset;
    event->route_detail = route.detail;
}

PlaintextDrive process_session(GatewayClient* client, GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    for (;;) {
        trim_message_banks(runtime);
        // Reuse the owner's deadline field during bounded envelope rescans;
        // no index growth can extend this absolute budget.
        runtime->mailbox_deadline_ms = GetTickCount64() + kPhaseTimeoutMs;
        GatewaySessionEvent event;
        gateway_session_next(&event, &runtime->session, runtime->heartbeat_jitter, now_ms);
        runtime->mailbox_deadline_ms = 0;
        if (event.kind == GatewaySessionEventKind::need_input) {
            return PlaintextDrive::receive;
        }
        if (event.kind == GatewaySessionEventKind::opened || event.kind == GatewaySessionEventKind::pong) {
            if (event.kind == GatewaySessionEventKind::opened) {
                GatewayClientEvent opened{};
                opened.timestamp_ms = now_ms;
                opened.kind = GatewayClientEventKind::websocket_opened;
                publish_event(runtime, opened);
            }
            continue;
        }
        if (event.kind == GatewaySessionEventKind::ping) {
            if (!queue_pong(runtime, event.control_payload)) {
                return fail_plaintext(runtime, GatewayClientFailureStage::pong_queue);
            }
            if (runtime->send_purpose == SendPurpose::none) {
                return send_pending_pong(runtime)
                           ? PlaintextDrive::sending
                           : fail_plaintext(runtime, GatewayClientFailureStage::pong_send);
            }
            continue;
        }
        if (event.kind == GatewaySessionEventKind::hello && event.hello.heartbeat_interval_ms != 0) {
            GatewayClientEvent hello{};
            hello.timestamp_ms = now_ms;
            hello.heartbeat_interval_ms = event.hello.heartbeat_interval_ms;
            hello.kind = GatewayClientEventKind::hello;
            publish_event(runtime, hello);
            InterlockedExchange64(&client->hello_elapsed_us,
                                  static_cast<LONG64>(clock_microseconds(
                                      runtime->clock, runtime->start_counter, clock_now())));
            if (!runtime->authenticated) {
                publish_phase(client, GatewayClientPhase::hello);
                SetEvent(client->state_event);
                return PlaintextDrive::hello;
            }

            const GatewayLifecycleAction session_action = event.lifecycle_step.action;
            if (runtime->session.lifecycle.next_heartbeat_ms <= now_ms) {
                const GatewayLifecycleStep heartbeat =
                    gateway_lifecycle_tick(&runtime->session.lifecycle, now_ms);
                if (!heartbeat.ok() || heartbeat.action != GatewayLifecycleAction::send_heartbeat ||
                    !gateway_outbound_enqueue(&runtime->outbound, heartbeat.action)) {
                    return fail_plaintext(runtime, GatewayClientFailureStage::hello_heartbeat);
                }
            }
            if ((session_action != GatewayLifecycleAction::send_identify &&
                 session_action != GatewayLifecycleAction::send_resume) ||
                !gateway_outbound_enqueue(&runtime->outbound, session_action)) {
                return fail_plaintext(runtime, GatewayClientFailureStage::hello_action);
            }
            runtime->deadline_ms = now_ms + kPhaseTimeoutMs;
            return drive_queued_actions(client, runtime, now_ms);
        }
        if (event.kind == GatewaySessionEventKind::heartbeat_requested) {
            if (event.lifecycle_step.action != GatewayLifecycleAction::send_heartbeat ||
                !gateway_outbound_enqueue(&runtime->outbound, event.lifecycle_step.action)) {
                return fail_plaintext(runtime, GatewayClientFailureStage::heartbeat_request);
            }
            return drive_queued_actions(client, runtime, now_ms);
        }
        if (event.kind == GatewaySessionEventKind::heartbeat_acknowledged) {
            GatewayClientEvent acknowledged{};
            acknowledged.timestamp_ms = now_ms;
            acknowledged.kind = GatewayClientEventKind::heartbeat_acknowledged;
            publish_event(runtime, acknowledged);
            continue;
        }
        if (event.kind == GatewaySessionEventKind::ready) {
            // One deadline spans the whole READY projection. No lock is
            // held while producer backpressure waits for the UI consumer.
            runtime->mailbox_deadline_ms = GetTickCount64() + kPhaseTimeoutMs;
            if (!gateway_client_ready_matches_identity(runtime->identity, event.ready)) {
                return fail_plaintext(runtime, GatewayClientFailureStage::ready_identity);
            }
            if (is_user(runtime)) {
                // A READY without one drops the stored data, as the client does.
                JsonString qos{};
                GatewayUserRuntime& user = *runtime->user;
                user.has_qos_derived =
                    json_value_as_string(event.ready.qos_token, &qos) && !qos.has_escapes &&
                    gateway_qos_derived_from_ready(qos.encoded, {user.qos_derived, kGatewayQosDerivedMaximumBytes},
                                                   &user.qos_derived_bytes);
            }
            GatewayResumeState captured_state{};
            const GatewayResumeCaptureError captured = gateway_resume_capture(
                &captured_state, event.ready, runtime->session.lifecycle.last_sequence);
            if (captured != GatewayResumeCaptureError::none) {
                return fail_plaintext(runtime, GatewayClientFailureStage::ready_resume_capture);
            }
            ReadyProjectionBinding binding{client, runtime, event.ready.required_action};
            // The zero-copy READY packet stays in the current session message
            // backing while both passes run. No push/next/compaction occurs
            // until all typed routes validate and the commit has finished.
            const auto projection = gateway_ready_project(
                event.ready,
                {main_index_view(runtime),
                 ready_record_index_view(runtime),
                 dispatch_scratch(runtime),
                 {reinterpret_cast<char*>(runtime->gateway_payload), sizeof(runtime->gateway_payload)},
                 grow_ready_record_index,
                 runtime,
                 {acquire_large_record, runtime},
                 runtime->ready_staging.base ? &runtime->ready_staging : nullptr},
                captured_state.sequence, now_ms, publish_ready_projection, ready_projection_work, &binding);
            settle_large_record(runtime);
            if (!projection.ok()) {
                note_route_error(runtime, projection.route);
                const GatewayClientFailureStage stage =
                    projection.failure_stage == GatewayClientFailureStage::none ? GatewayClientFailureStage::ready_route
                                                                                : projection.failure_stage;
                // An accepted READY prefix may already have replaced model
                // state. No prior resume checkpoint can certify that prefix,
                // so the next attempt is always a fresh IDENTIFY.
                if (projection.publication_started)
                    clear_resume(runtime);
                // Owner shutdown (stop or mailbox cancel) ends the worker as before.
                if (runtime->stopping || WaitForSingleObject(runtime->mailbox_cancel, 0) == WAIT_OBJECT_0)
                    return fail_plaintext(runtime, stage);
                // A READY that cannot be projected is not an authentication
                // failure: report the categorical reason, keep the session and
                // identify again later. The growing floor keeps a READY that
                // fails the same way every time far below the server's daily
                // IDENTIFY budget.
                GatewayClientEvent diagnostic{};
                diagnostic.timestamp_ms = now_ms;
                diagnostic.kind = GatewayClientEventKind::reconnecting;
                diagnostic.error = GatewayClientError::session;
                diagnostic.failure_stage = stage;
                set_route_error(&diagnostic, projection.route);
                publish_event(runtime, diagnostic);
                u32 random = 0;
                if (!secure_random_u32(&random).ok())
                    return fail_plaintext(runtime, stage);
                const u32 failures = runtime->ready_failures < 16 ? ++runtime->ready_failures : 16;
                const u32 floor_ms = gateway_ready_failure_delay(failures);
                const u32 jitter_ms = gateway_reconnect_next(&runtime->reconnect_backoff, random).delay_ms;
                return schedule_reconnect(client, runtime, GatewayLifecycleAction::reconnect_identify, now_ms,
                                          floor_ms + jitter_ms)
                           ? PlaintextDrive::reconnecting
                           : fail_plaintext(runtime, stage);
            }
            runtime->ready_failures = 0;
            if (projection.degraded_guilds) {
                // Guilds left unavailable are reported with their first
                // categorical failure; the rest of READY was applied.
                GatewayClientEvent degraded{};
                degraded.timestamp_ms = now_ms;
                degraded.kind = GatewayClientEventKind::dispatch;
                degraded.dispatch_kind = GatewayClientDispatchKind::unknown;
                degraded.failure_stage = GatewayClientFailureStage::ready_guild_route;
                set_route_error(&degraded, projection.degraded_route);
                if (degraded.event_decode_error == GatewayEventDecodeError::none)
                    degraded.event_decode_error = GatewayEventDecodeError::invalid_data;
                degraded.native_error = projection.degraded_guilds;
                degraded.sequence = captured_state.sequence;
                degraded.has_sequence = true;
                publish_event_payload(client, runtime, degraded, {});
            }
            runtime->mailbox_deadline_ms = 0;
            // Save the resume token only after the READY projection has
            // entered the consumer queue. Saving it before routing can persist
            // a session whose model never existed, so the next process would
            // resume into an empty shell.
            publish_resume(runtime, captured_state);
            AcquireSRWLockExclusive(&runtime->subscription_lock);
            gateway_state_recovery_ready(&runtime->state_recovery);
            gateway_subscription_thread_owner_ready(&runtime->thread_subscription_owner, true);
            gateway_channel_info_reset_session(&runtime->channel_info);
            if (gateway_self_status_bind(&runtime->self_status, event.ready.user_id))
                self_settings_observe_locked(runtime, event.ready.user_settings_proto, false);
            else {
                runtime->self_status.authoritative = false;
                runtime->self_status_error = GatewaySelfStatusError::settings;
                ++runtime->self_status.revision_serial;
            }
            runtime->sent_subscription_revision = 0;
            runtime->active_subscription_revision = 0;
            ReleaseSRWLockExclusive(&runtime->subscription_lock);
            gateway_reconnect_reset(&runtime->reconnect_backoff);
            runtime->deadline_ms = 0;
            InterlockedIncrement64(&client->frecency_settings_revision);
            publish_phase(client, GatewayClientPhase::ready);
            SetEvent(client->state_event);
            client_session_ready(runtime);
            continue;
        }
        if (event.kind == GatewaySessionEventKind::resumed) {
            InterlockedIncrement64(&client->frecency_settings_revision);
            AcquireSRWLockExclusive(&runtime->subscription_lock);
            gateway_channel_info_reset_session(&runtime->channel_info);
            ReleaseSRWLockExclusive(&runtime->subscription_lock);
            gateway_subscription_thread_owner_ready(&runtime->thread_subscription_owner, false);
            publish_voice_snapshot(client, runtime);
            runtime->deadline_ms = 0;
            gateway_reconnect_reset(&runtime->reconnect_backoff);
            GatewayClientEvent resumed{};
            resumed.timestamp_ms = now_ms;
            resumed.kind = GatewayClientEventKind::resumed;
            resumed.sequence = runtime->session.lifecycle.last_sequence;
            resumed.has_sequence = runtime->session.lifecycle.has_sequence;
            publish_event(runtime, resumed);
            publish_phase(client, GatewayClientPhase::ready);
            SetEvent(client->state_event);
            continue;
        }
        if (event.kind == GatewaySessionEventKind::dispatch) {
            // The events this handler routes itself, in list order.
            enum class Named : u32 {
                rate_limited,
                user_settings,
                voice_batch,
                voice_update,
                passive_update,
                supplemental,
                poll_votes,
                reactions,
                guild_create
            };
            const auto named = static_cast<Named>(protocol_detail::member_index<
                "RATE_LIMITED", "USER_SETTINGS_PROTO_UPDATE", "VOICE_STATE_UPDATE_BATCH", "VOICE_STATE_UPDATE",
                "PASSIVE_UPDATE_V2", "READY_SUPPLEMENTAL", "MESSAGE_POLL_VOTE_ADD_MANY",
                "MESSAGE_REACTION_ADD_MANY", "GUILD_CREATE">(event.envelope.event_name));
            // Neither grows while one dispatch is handled.
            const Slice<u32> index = main_index_view(runtime);
            const Slice<char> scratch = dispatch_scratch(runtime);
            RelationshipStreamBinding binding{client, runtime};
            if (named == Named::rate_limited) {
                GatewayRateLimited limited{};
                u32 rate_structurals[64]{};
                const auto decoded =
                    gateway_decode_rate_limited(event.envelope.data, {rate_structurals, 64}, 16, &limited);
                if (decoded.ok()) {
                    AcquireSRWLockExclusive(&runtime->subscription_lock);
                    gateway_member_request_rate_limited(&runtime->member_requests, limited, now_ms);
                    runtime->member_deadline_ms =
                        gateway_member_request_deadline(runtime->member_requests, now_ms);
                    gateway_channel_info_rate_limited(&runtime->channel_info, limited, now_ms);
                    runtime->channel_info_deadline_ms = gateway_channel_info_deadline(runtime->channel_info, now_ms);
                    ReleaseSRWLockExclusive(&runtime->subscription_lock);
                }
                checkpoint_resume_sequence(runtime);
                continue;
            }
            if (named == Named::user_settings) {
                self_settings_dispatch(client, runtime, event.envelope.data.source);
                settings_stores_dispatch(client, runtime, event.envelope, now_ms);
            }
            // STREAM_* carry the stream token: consumed here, never mailboxed.
            if (gateway_stream_dispatch(&runtime->stream.control, event.envelope, index, 64,
                                        now_ms)) {
                publish_voice_snapshot(client, runtime);
                checkpoint_resume_sequence(runtime);
                continue;
            }
            if (gateway_voice_dispatch(&runtime->voice, event.envelope, index, 64,
                                       now_ms)) {
                publish_voice_snapshot(client, runtime);
                const bool batch = named == Named::voice_batch;
                if ((batch || named == Named::voice_update) &&
                    !gateway_client_publish_voice_states(
                        event.envelope.data,
                        batch ? GatewayVoiceStateSource::batch : GatewayVoiceStateSource::update, 0,
                        index, scratch, event.envelope.sequence,
                        now_ms, publish_relationship_stream, &binding))
                    return resume_reconnect(client, runtime, now_ms);
                checkpoint_resume_sequence(runtime);
                continue;
            }
            if (named == Named::passive_update) {
                // Other members' voice joins/moves/leaves in passively synced
                // guilds; the rest of the document keeps its existing route.
                if (!gateway_client_publish_passive_voice_states(
                        event.envelope.data, index, scratch,
                        event.envelope.sequence, now_ms, publish_relationship_stream, &binding))
                    return resume_reconnect(client, runtime, now_ms);
            }
            GatewayClientEvent dispatch{};
            ConstSlice<u8> dispatch_payload{};
            u64 bulk_ids[kGatewayBulkDeleteMaximumIds]; // Only the ids the decoder counts are ever read.
            GatewayClientDispatchRouteResult routed{};
            const bool supplemental = named == Named::supplemental;
            const bool vote_batch = named == Named::poll_votes;
            const bool reaction_batch = vote_batch || named == Named::reactions;
            if (reaction_batch) {
                runtime->mailbox_deadline_ms = GetTickCount64() + kPhaseTimeoutMs;
                const auto batch =
                    vote_batch
                        ? gateway_client_publish_poll_vote_batch(event.envelope, index, 64,
                                                                 now_ms, publish_relationship_stream, &binding,
                                                                 &runtime->reaction_batch)
                        : gateway_client_publish_reaction_batch(
                              event.envelope, index, 64,
                              {reinterpret_cast<char*>(runtime->handshake_output + kMessageBytes),
                               kMessageReactionMaximumEmojiNameBytes},
                              now_ms, publish_relationship_stream, &binding, &runtime->reaction_batch);
                runtime->mailbox_deadline_ms = 0;
                if (batch.cancelled)
                    return resume_reconnect(client, runtime, now_ms);
                if (batch.ok()) {
                    checkpoint_resume_sequence(runtime);
                    continue;
                }
                set_decode_failure(&routed, batch.decoded);
            } else if (supplemental) {
                runtime->mailbox_deadline_ms = GetTickCount64() + kPhaseTimeoutMs;
                const Slice<u32> objects = object_scratch(runtime);
                JsonValue channels{};
                const auto decoded = gateway_decode_supplemental_private_channels(
                    event.envelope.data, index, 64, &channels);
                if (!decoded.ok()) {
                    set_decode_failure(&routed, decoded);
                } else if (channels.kind == JsonValueKind::array) {
                    const auto published =
                        publish_ready_state(client, runtime, channels, ReadyRecordKind::private_channel,
                                            event.envelope.sequence, now_ms);
                    routed = published.route;
                    if (published.publish_failed)
                        return resume_reconnect(client, runtime, now_ms);
                }
                if (routed.ok()) {
                    JsonValue merged{};
                    routed = gateway_relationship_merged_presences(event.envelope.data,
                                                                   index, &merged);
                    if (routed.ok()) {
                        routed = gateway_client_publish_friend_presences(
                            merged, index, objects,
                            event.envelope.sequence, now_ms, publish_relationship_stream, &binding);
                    }
                }
                // Server presences' rich activities; the member list carries their status.
                if (routed.ok()) {
                    routed = gateway_client_publish_guild_presences(
                        event.envelope.data, index, objects,
                        event.envelope.sequence, now_ms, publish_relationship_stream, &binding);
                }
                // Initial voice states of the other members arrive only here;
                // READY guilds carry none. Malformed guild rows are skipped.
                if (routed.ok()) {
                    if (!gateway_client_publish_supplemental_voice_states(
                            event.envelope.data, index, objects,
                            scratch, event.envelope.sequence, now_ms,
                            publish_relationship_stream, &binding))
                        return resume_reconnect(client, runtime, now_ms);
                }
                if (routed.ok()) {
                    if (!gateway_ready_publish_supplemental_members(
                             event.envelope.data, runtime->self_user_id, index,
                             objects, scratch, event.envelope.sequence,
                             now_ms, publish_relationship_stream, &binding)
                             .ok())
                        return resume_reconnect(client, runtime, now_ms);
                }
                if (routed.ok()) {
                    GatewayClientEvent complete{};
                    complete.kind = GatewayClientEventKind::dispatch;
                    complete.dispatch_kind = GatewayClientDispatchKind::supplemental_state_complete;
                    complete.timestamp_ms = now_ms;
                    complete.sequence = event.envelope.sequence;
                    complete.has_sequence = event.envelope.has_sequence;
                    if (!publish_event_payload(client, runtime, complete, {}))
                        return resume_reconnect(client, runtime, now_ms);
                    checkpoint_resume_sequence(runtime);
                    runtime->mailbox_deadline_ms = 0;
                    continue;
                }
                runtime->mailbox_deadline_ms = 0;
            } else
                routed = gateway_client_route_dispatch_large(
                    event.envelope, index, 64,
                    // The first half owns the decompressed envelope until routing
                    // completes. Message decoding revisits author/mention/rich
                    // JsonValue slices after copying content, so its output must
                    // use the disjoint second half. Aliasing these buffers
                    // corrupts the source in place and surfaces later as an
                    // unterminated string.
                    scratch, Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds},
                    runtime->self_user_id, {acquire_large_record, runtime}, &dispatch, &dispatch_payload);
            if (!routed.ok()) {
                settle_large_record(runtime);
                note_route_error(runtime, routed);
                // A schema/size failure in one evolving server dispatch must
                // not tear down an otherwise healthy Gateway. Queue a
                // pointer-free recovery marker; the UI catches up message
                // state through REST while the session continues at the next
                // sequence.
                gateway_client_route_failure_event(event.envelope, routed, now_ms, &dispatch);
                if (!publish_event_payload(client, runtime, dispatch, ConstSlice<u8>{}))
                    return resume_reconnect(client, runtime, now_ms);
                if (reaction_batch)
                    reset_value(&runtime->reaction_batch);
                // A joined guild that cannot be projected is still listed,
                // as unavailable, instead of silently missing.
                GatewayClientEvent placeholder{};
                if (named == Named::guild_create &&
                    gateway_client_route_guild_placeholder(event.envelope.data, index, 64,
                                                           &placeholder)) {
                    placeholder.timestamp_ms = now_ms;
                    placeholder.sequence = event.envelope.sequence;
                    placeholder.has_sequence = event.envelope.has_sequence;
                    if (!publish_event_payload(client, runtime, placeholder, ConstSlice<u8>{}))
                        return resume_reconnect(client, runtime, now_ms);
                }
                checkpoint_resume_sequence(runtime);
                continue;
            }
            if (dispatch.dispatch_kind == GatewayClientDispatchKind::account_required_action)
                InterlockedExchange(&client->required_action, static_cast<LONG>(dispatch.required_action));
            if (dispatch.dispatch_kind == GatewayClientDispatchKind::guild_members_chunk) {
                GatewayMemberChunkReceipt receipt{};
                if (gateway_decode_member_chunk_receipt(event.envelope.data, index, 64,
                                                        &receipt)
                        .ok()) {
                    AcquireSRWLockExclusive(&runtime->subscription_lock);
                    gateway_member_request_complete(&runtime->member_requests, receipt);
                    runtime->member_deadline_ms =
                        gateway_member_request_deadline(runtime->member_requests, now_ms);
                    ReleaseSRWLockExclusive(&runtime->subscription_lock);
                }
            }
            dispatch.timestamp_ms = now_ms;
            const bool dispatch_published =
                publish_event_payload(client, runtime, dispatch, dispatch_payload);
            settle_large_record(runtime);
            if (!dispatch_published)
                return resume_reconnect(client, runtime, now_ms);
            if (dispatch.dispatch_kind == GatewayClientDispatchKind::guild_create) {
                AcquireSRWLockExclusive(&runtime->subscription_lock);
                gateway_channel_info_forget(&runtime->channel_info, dispatch.state.id);
                ReleaseSRWLockExclusive(&runtime->subscription_lock);
                if (!gateway_client_publish_stage_instances(
                        event.envelope.data, dispatch.state.id, index,
                        scratch, event.envelope.sequence, now_ms,
                        publish_relationship_stream, &binding) ||
                    !gateway_client_publish_scheduled_events(
                        event.envelope.data, dispatch.state.id, index,
                        scratch, event.envelope.sequence, now_ms,
                        publish_relationship_stream, &binding) ||
                    !gateway_client_publish_voice_states(
                        event.envelope.data, GatewayVoiceStateSource::guild, dispatch.state.id,
                        index, scratch, event.envelope.sequence,
                        now_ms, publish_relationship_stream, &binding) ||
                    !gateway_client_publish_guild_stickers(
                        event.envelope.data, dispatch.state.id, index,
                        scratch, event.envelope.sequence, now_ms,
                        publish_relationship_stream, &binding))
                    return resume_reconnect(client, runtime, now_ms);
            }
            checkpoint_resume_sequence(runtime);
            continue;
        }
        if (event.kind == GatewaySessionEventKind::closed) {
            GatewayClientEvent closed{};
            closed.timestamp_ms = now_ms;
            closed.kind = GatewayClientEventKind::closed;
            closed.close_code = event.close_code;
            InterlockedExchange(&client->close_code, event.close_code);
            publish_event(runtime, closed);
            if (event.lifecycle_step.action == GatewayLifecycleAction::stop) {
                return fail_plaintext(runtime, GatewayClientFailureStage::close_stop);
            }
            u32 random = 0;
            if (!secure_random_u32(&random).ok()) {
                return fail_plaintext(runtime, GatewayClientFailureStage::close_random);
            }
            const GatewayReconnectDelay delay = gateway_reconnect_next(&runtime->reconnect_backoff, random);
            return schedule_reconnect(client, runtime, event.lifecycle_step.action, now_ms, delay.delay_ms)
                       ? PlaintextDrive::reconnecting
                       : fail_plaintext(runtime, GatewayClientFailureStage::close_reconnect);
        }
        if (event.kind == GatewaySessionEventKind::reconnect_requested) {
            return schedule_reconnect(client, runtime, event.lifecycle_step.action, now_ms, 0)
                       ? PlaintextDrive::reconnecting
                       : fail_plaintext(runtime, GatewayClientFailureStage::reconnect_request);
        }
        if (event.kind == GatewaySessionEventKind::invalid_session) {
            u32 random = 0;
            if (!secure_random_u32(&random).ok()) {
                return fail_plaintext(runtime, GatewayClientFailureStage::invalid_session_random);
            }
            return schedule_reconnect(client, runtime, event.lifecycle_step.action, now_ms,
                                      gateway_invalid_session_delay(random))
                       ? PlaintextDrive::reconnecting
                       : fail_plaintext(runtime, GatewayClientFailureStage::invalid_session_reconnect);
        }
        if (event.kind == GatewaySessionEventKind::error) {
            return fail_session_event(runtime, event);
        }
    }
}

PlaintextDrive process_encrypted_input(GatewayClient* client, GatewayClientRuntime* runtime,
                                       u64 now_ms) noexcept {
    while (byte_queue_size(runtime->encrypted_input) != 0) {
        const ConstSlice<u8> readable = byte_queue_readable(runtime->encrypted_input);
        const TlsDecryptResult decrypted =
            tls_client_decrypt(&runtime->tls, Slice<u8>{const_cast<u8*>(readable.data), readable.count});
        if (!decrypted.ok() || decrypted.state == TlsDecryptState::peer_closed ||
            decrypted.state == TlsDecryptState::renegotiate) {
            return fail_plaintext(runtime, GatewayClientFailureStage::tls_decrypt);
        }
        if (decrypted.state == TlsDecryptState::need_receive) {
            return PlaintextDrive::receive;
        }
        if (!gateway_session_push(&runtime->session,
                                  ConstSlice<u8>{decrypted.plaintext.data, decrypted.plaintext.count}) ||
            !byte_queue_consume(&runtime->encrypted_input, decrypted.consumed)) {
            return fail_plaintext(runtime, GatewayClientFailureStage::session_push);
        }
        const PlaintextDrive session = process_session(client, runtime, now_ms);
        if (session != PlaintextDrive::receive) {
            return session;
        }
    }
    return PlaintextDrive::receive;
}

PlaintextDrive resume_gateway_input(GatewayClient* client, GatewayClientRuntime* runtime,
                                    u64 now_ms) noexcept {
    const PlaintextDrive buffered = process_session(client, runtime, now_ms);
    return buffered == PlaintextDrive::receive ? process_encrypted_input(client, runtime, now_ms)
                                               : buffered;
}

NetworkWorkerStep handle_completion(GatewayClient* client, GatewayClientRuntime* runtime, NetworkLoop* loop,
                                    const NetworkWorkerEvent& event) noexcept {
    const NetworkCompletion& completion = event.poll.completion;
    if (voice_connection_owns(runtime->voice_connection, completion)) {
        voice_connection_completion(&runtime->voice_connection, runtime->voice.setup, loop, completion,
                                    event.now_ms);
        return {next_worker_wait(client, runtime, event.now_ms),
                runtime->stopping && shutdown_drained(runtime)};
    }
    if (gateway_stream_session_owns(runtime->stream, completion)) {
        gateway_stream_session_completion(&runtime->stream, loop, completion, event.now_ms);
        return {next_worker_wait(client, runtime, event.now_ms),
                runtime->stopping && shutdown_drained(runtime)};
    }
    // Capture wake-ups only need the voice drive that follows every event.
    if (completion.kind == NetworkOperationKind::signal && completion.signal == &runtime->voice_send_signal)
        return {next_worker_wait(client, runtime, event.now_ms), runtime->stopping && shutdown_drained(runtime)};
    if (((completion.kind == NetworkOperationKind::connect ||
          completion.kind == NetworkOperationKind::receive ||
          completion.kind == NetworkOperationKind::send) &&
         completion.connection != &runtime->connection) ||
        (completion.kind == NetworkOperationKind::resolve && completion.resolver != &runtime->resolver)) {
        return {next_worker_wait(client, runtime, event.now_ms), false};
    }
    if (completion.kind == NetworkOperationKind::signal &&
        (completion.signal == &runtime->subscription_signal ||
         completion.signal == &runtime->voice_signal)) {
        if (runtime->stopping) {
            return NetworkWorkerStep{INFINITE, shutdown_drained(runtime)};
        }
        AcquireSRWLockExclusive(&runtime->subscription_lock);
        gateway_state_recovery_schedule(&runtime->state_recovery, event.now_ms);
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        const OutboundDrive outbound = drive_application_output(client, runtime, event.now_ms);
        if (outbound == OutboundDrive::failed) {
            return fail_and_drain(client, runtime, GatewayClientError::session, 0);
        }
        if (runtime->session.opened && !ensure_receive(runtime)) {
            return fail_and_drain(client, runtime, GatewayClientError::receive, WSAGetLastError());
        }
        return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
    }
    if (completion.kind == NetworkOperationKind::resolve && completion.resolver == &runtime->resolver) {
        if (runtime->stopping) {
            release_resolver(runtime);
            return NetworkWorkerStep{INFINITE, shutdown_drained(runtime)};
        }
        if (runtime->reconnecting) {
            release_resolver(runtime);
            return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
        }
        if (!completion.succeeded || !runtime->resolver.results) {
            return reconnect_or_fail(client, runtime, GatewayClientError::resolve,
                                     static_cast<u32>(completion.native_error), event.now_ms);
        }
        runtime->next_address = runtime->resolver.results;
        if (!start_connect_candidate(loop, runtime)) {
            return reconnect_or_fail(client, runtime, GatewayClientError::connect, WSAEHOSTUNREACH,
                                     event.now_ms);
        }
        publish_phase(client, GatewayClientPhase::connecting);
        runtime->deadline_ms = event.now_ms + kPhaseTimeoutMs;
        return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
    }

    if (completion.kind == NetworkOperationKind::connect ||
        completion.kind == NetworkOperationKind::receive || completion.kind == NetworkOperationKind::send) {
        if (runtime->outstanding_socket_operations != 0) {
            --runtime->outstanding_socket_operations;
        }
        if (runtime->stopping) {
            return NetworkWorkerStep{INFINITE, shutdown_drained(runtime)};
        }
        if (runtime->reconnecting) {
            return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
        }
    }

    if (completion.kind == NetworkOperationKind::connect) {
        if (!completion.succeeded || completion.stale) {
            network_connection_close(&runtime->connection);
            if (start_connect_candidate(loop, runtime)) {
                return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
            }
            return reconnect_or_fail(client, runtime, GatewayClientError::connect,
                                     static_cast<u32>(completion.native_error), event.now_ms);
        }
        release_resolver(runtime);
        const TlsResult initialized = tls_client_initialize(&runtime->tls, runtime->endpoint.host_wide);
        if (!initialized.ok() || runtime->tls.maximum_token_bytes > kHandshakeBytes) {
            return reconnect_or_fail(client, runtime, GatewayClientError::tls,
                                     static_cast<u32>(initialized.native_status), event.now_ms);
        }
        runtime->tls_initialized = true;
        publish_phase(client, GatewayClientPhase::tls_handshake);
        runtime->deadline_ms = event.now_ms + kPhaseTimeoutMs;
        if (!drive_tls_handshake(runtime)) {
            return reconnect_or_fail(client, runtime, GatewayClientError::tls, 0, event.now_ms);
        }
        return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
    }

    if (completion.kind == NetworkOperationKind::send) {
        if (!completion.succeeded || completion.stale || completion.transferred == 0) {
            if (runtime->send_purpose == SendPurpose::guild_subscription)
                gateway_subscription_thread_owner_complete(&runtime->thread_subscription_owner, false);
            return reconnect_or_fail(client, runtime, GatewayClientError::send,
                                     static_cast<u32>(completion.native_error), event.now_ms);
        }
        runtime->send_offset += completion.transferred;
        if (runtime->send_offset < runtime->send_bytes) {
            if (!continue_send(runtime)) {
                if (runtime->send_purpose == SendPurpose::guild_subscription)
                    gateway_subscription_thread_owner_complete(&runtime->thread_subscription_owner, false);
                return reconnect_or_fail(client, runtime, GatewayClientError::send, WSAGetLastError(),
                                         event.now_ms);
            }
            return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
        }
        const SendPurpose purpose = runtime->send_purpose;
        runtime->send_purpose = SendPurpose::none;
        if (purpose >= SendPurpose::gateway_action)
            SecureZeroMemory(runtime->record_output, runtime->send_bytes);
        if (purpose == SendPurpose::guild_subscription) {
            gateway_subscription_thread_owner_complete(&runtime->thread_subscription_owner, true);
            runtime->sent_subscription_revision = runtime->active_subscription_revision;
            runtime->active_subscription_revision = 0;
        }
        if (purpose == SendPurpose::self_status) {
            AcquireSRWLockExclusive(&runtime->subscription_lock);
            gateway_self_status_complete(&runtime->self_status, true);
            ++runtime->self_status.revision_serial;
            ReleaseSRWLockExclusive(&runtime->subscription_lock);
            if (client->notification_event)
                SetEvent(client->notification_event);
        }
        if (purpose == SendPurpose::tls_handshake) {
            if (runtime->after_tls_send == TlsHandshakeState::complete_and_send) {
                publish_phase(client, GatewayClientPhase::websocket_opening);
                runtime->deadline_ms = event.now_ms + kPhaseTimeoutMs;
                if (!begin_websocket_opening(runtime)) {
                    return reconnect_or_fail(client, runtime, GatewayClientError::websocket_opening, 0,
                                             event.now_ms);
                }
            } else if (!drive_tls_handshake(runtime)) {
                return reconnect_or_fail(client, runtime, GatewayClientError::tls, 0, event.now_ms);
            }
        } else if (purpose == SendPurpose::websocket_opening) {
            publish_phase(client, GatewayClientPhase::awaiting_hello);
            runtime->deadline_ms = event.now_ms + kPhaseTimeoutMs;
            if (!start_receive(runtime)) {
                return reconnect_or_fail(client, runtime, GatewayClientError::receive, WSAGetLastError(),
                                         event.now_ms);
            }
        } else if (purpose >= SendPurpose::pong) {
            const OutboundDrive outbound = drive_application_output(client, runtime, event.now_ms);
            if (outbound == OutboundDrive::failed) {
                return fail_and_drain(client, runtime, GatewayClientError::session, 0);
            }
            if (outbound != OutboundDrive::sending) {
                const PlaintextDrive driven = resume_gateway_input(client, runtime, event.now_ms);
                if (driven == PlaintextDrive::failed) {
                    return fail_and_drain(client, runtime, GatewayClientError::session, 0);
                }
                if (driven == PlaintextDrive::receive && !ensure_receive(runtime)) {
                    return fail_and_drain(client, runtime, GatewayClientError::receive, WSAGetLastError());
                }
            }
        }
        return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
    }

    if (completion.kind == NetworkOperationKind::receive) {
        if (!completion.succeeded || completion.stale || completion.transferred == 0 ||
            !byte_queue_produce(&runtime->encrypted_input, completion.transferred)) {
            return reconnect_or_fail(client, runtime, GatewayClientError::receive,
                                     static_cast<u32>(completion.native_error), event.now_ms);
        }
        if (!runtime->tls.handshake_complete) {
            if (!drive_tls_handshake(runtime)) {
                return reconnect_or_fail(client, runtime, GatewayClientError::tls, 0, event.now_ms);
            }
            return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
        }
        const PlaintextDrive driven = process_encrypted_input(client, runtime, event.now_ms);
        if (driven == PlaintextDrive::failed) {
            return fail_and_drain(client, runtime, GatewayClientError::session, 0);
        }
        if (driven == PlaintextDrive::receive && !ensure_receive(runtime)) {
            return fail_and_drain(client, runtime, GatewayClientError::receive, WSAGetLastError());
        }
        return NetworkWorkerStep{
            driven == PlaintextDrive::hello ? INFINITE : next_worker_wait(client, runtime, event.now_ms),
            false};
    }
    return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
}

NetworkWorkerStep gateway_worker_dispatch(void* opaque, NetworkLoop* loop,
                                          const NetworkWorkerEvent& event) noexcept {
    auto* client = static_cast<GatewayClient*>(opaque);
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    if (event.kind == NetworkWorkerEventKind::started) {
        runtime->clock = clock_initialize();
        runtime->start_counter = clock_now();
        runtime->connection.socket = INVALID_SOCKET;
        if (!begin_resolution(client, runtime, loop, event.now_ms)) {
            return fail_and_drain(client, runtime, GatewayClientError::resolve, WSAEINVAL);
        }
        return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
    }
    if (event.kind == NetworkWorkerEventKind::stop_requested) {
        begin_shutdown(client, runtime);
        return NetworkWorkerStep{INFINITE, shutdown_drained(runtime)};
    }
    if (event.kind == NetworkWorkerEventKind::deadline) {
        if (runtime->reconnecting) {
            if (runtime->resolver.pending || runtime->outstanding_socket_operations != 0) {
                return NetworkWorkerStep{INFINITE, false};
            }
            release_resolver(runtime);
            if (event.now_ms < runtime->reconnect_deadline_ms) {
                return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
            }
            if (!begin_resolution(client, runtime, loop, event.now_ms)) {
                return fail_and_drain(client, runtime, GatewayClientError::resolve, WSAEINVAL);
            }
            return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
        }
        if (gateway_voice_expire(&runtime->voice, event.now_ms))
            publish_voice_snapshot(client, runtime);
        AcquireSRWLockExclusive(&runtime->subscription_lock);
        const bool recover = gateway_state_recovery_due(
            &runtime->state_recovery, load_phase(client) == GatewayClientPhase::ready && !runtime->stopping,
            event.now_ms);
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        if (recover) {
            if (!schedule_reconnect(client, runtime, GatewayLifecycleAction::reconnect_identify,
                                    event.now_ms, 0))
                return fail_and_drain(client, runtime, GatewayClientError::session, 0);
            return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
        }
        if (!runtime->stopping && runtime->deadline_ms != 0 && event.now_ms >= runtime->deadline_ms &&
            load_phase(client) != GatewayClientPhase::hello) {
            if (runtime->authenticated && schedule_backoff_reconnect(client, runtime, event.now_ms)) {
                return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
            }
            return fail_and_drain(client, runtime, GatewayClientError::timeout, WAIT_TIMEOUT);
        }
        if (runtime->authenticated && runtime->session.lifecycle.heartbeat_interval_ms != 0 &&
            event.now_ms >= runtime->session.lifecycle.next_heartbeat_ms) {
            const GatewayLifecycleStep heartbeat =
                gateway_lifecycle_tick(&runtime->session.lifecycle, event.now_ms);
            if (!heartbeat.ok()) {
                return fail_and_drain(client, runtime, GatewayClientError::session, 0);
            }
            if (heartbeat.action == GatewayLifecycleAction::send_heartbeat) {
                if (!gateway_outbound_enqueue(&runtime->outbound, heartbeat.action)) {
                    return fail_and_drain(client, runtime, GatewayClientError::session, 0);
                }
            } else if (heartbeat.action == GatewayLifecycleAction::reconnect_identify ||
                       heartbeat.action == GatewayLifecycleAction::reconnect_resume) {
                u32 random = 0;
                if (!secure_random_u32(&random).ok()) {
                    return fail_and_drain(client, runtime, GatewayClientError::random, 0);
                }
                const GatewayReconnectDelay delay =
                    gateway_reconnect_next(&runtime->reconnect_backoff, random);
                if (!schedule_reconnect(client, runtime, heartbeat.action, event.now_ms, delay.delay_ms)) {
                    return fail_and_drain(client, runtime, GatewayClientError::session, 0);
                }
                return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
            }
        }
        if (runtime->rate_deadline_ms != 0 && event.now_ms >= runtime->rate_deadline_ms) {
            runtime->rate_deadline_ms = 0;
        }
        const OutboundDrive outbound = drive_application_output(client, runtime, event.now_ms);
        if (outbound == OutboundDrive::failed) {
            return fail_and_drain(client, runtime, GatewayClientError::session, 0);
        }
        if (runtime->session.opened && !ensure_receive(runtime)) {
            return fail_and_drain(client, runtime, GatewayClientError::receive, WSAGetLastError());
        }
        return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
    }
    if (event.kind == NetworkWorkerEventKind::poll_error) {
        return fail_and_drain(client, runtime, GatewayClientError::worker,
                              static_cast<u32>(event.poll.completion.native_error));
    }
    return handle_completion(client, runtime, loop, event);
}

NetworkWorkerStep gateway_worker_handler(void* opaque, NetworkLoop* loop,
                                         const NetworkWorkerEvent& event) noexcept {
    auto* client = static_cast<GatewayClient*>(opaque);
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    const auto step = gateway_worker_dispatch(opaque, loop, event);
    // One consumer wake for everything this network event published.
    notify_consumer(client, runtime);
    if (step.exit)
        return step;
    // Main dispatch first: an endpoint/token replacement or leave in this
    // completion must invalidate the old voice attempt before further sends.
    const bool sending = !runtime->stopping && voice_send_active(runtime->voice_send);
    if (runtime->voice.requested || !voice_connection_drained(runtime->voice_connection) ||
        runtime->voice_connection.command.data || runtime->voice_receive.media_revision || sending) {
        runtime->voice_connection.video_decodable = voice_video_sink_decodable(runtime->voice_receive.video);
        voice_connection_drive(&runtime->voice_connection, runtime->voice.setup, loop, event.now_ms);
        apply_video_wants(client, runtime);
        voice_receive_drive(&runtime->voice_receive, &runtime->voice_connection, runtime->voice, event.now_ms);
        if (!runtime->stopping) {
            const auto& receive = runtime->voice_receive;
            voice_send_drive(&runtime->voice_send, &runtime->voice_connection, runtime->voice,
                             receive.media_revision && receive.media_revision == runtime->voice_connection.ready_revision,
                             loop, &runtime->voice_send_signal);
            voice_receive_configure(&runtime->voice_receive, runtime->voice_send.applied);
        }
        publish_voice_snapshot(client, runtime);
    }
    if (gateway_stream_session_busy(runtime->stream)) {
        gateway_stream_session_drive(&runtime->stream, runtime->voice, loop, runtime->voice_send.applied,
                                     event.now_ms);
        publish_voice_snapshot(client, runtime);
    }
    drive_client_activity(runtime, event.now_ms);
    notify_consumer(client, runtime);
    return {next_worker_wait(client, runtime, event.now_ms),
            runtime->stopping && shutdown_drained(runtime)};
}

// Frees the runtime of a start that failed after allocating it and records the error.
GatewayClientResult fail_start(GatewayClient* client, GatewayClientError error, u32 native_error) noexcept {
    release_runtime_allocation(client);
    client->runtime = nullptr;
    InterlockedExchange(&client->error, static_cast<LONG>(error));
    InterlockedExchange(&client->native_error, static_cast<LONG>(native_error));
    return result(client);
}

// Closes the events a failed start created so far; the ones not created yet are null.
void close_start_events(GatewayClient* client, GatewayClientRuntime* runtime) noexcept {
    if (runtime->mailbox_space)
        CloseHandle(runtime->mailbox_space);
    if (runtime->mailbox_cancel)
        CloseHandle(runtime->mailbox_cancel);
    if (client->notification_event)
        CloseHandle(client->notification_event);
    if (client->state_event)
        CloseHandle(client->state_event);
    client->state_event = client->notification_event = nullptr;
}

GatewayClientResult start_client(GatewayClient* client, SecureBuffer* credential,
                                 const AccountProfileMetadata* user_metadata,
                                 const GatewayUserClientConfiguration* user_configuration,
                                 const GatewayEndpoint* gateway_endpoint,
                                 const GatewayResumeState* resume_state, u64 resume_user_id) noexcept {
    const bool valid_credential =
        credential && credential->data && credential->locked && credential->bytes != 0 &&
        credential->bytes <= kGatewayUserMaximumTokenBytes &&
        utf8_validate(Utf8View{reinterpret_cast<const char*>(credential->data), credential->bytes}).valid;
    if (!client || client->worker.thread || client->runtime || client->state_event ||
        client->notification_event ||
        (resume_state && resume_state->valid && !gateway_resume_validate(*resume_state)) ||
        (gateway_endpoint && (!gateway_endpoint_validate(*gateway_endpoint) ||
                              gateway_endpoint->policy != GatewayEndpointPolicy::user_v9_etf_zstd_stream)) ||
        !valid_credential || !user_metadata || user_metadata->profile_id == 0 || !user_configuration ||
        !gateway_user_client_configuration_validate(*user_configuration)) {
        return invalid_input_result();
    }
    reset_value<GatewayClient>(client);
    const u32 allocation_bytes = kGatewayUserClientRuntimeBytes;
    client->runtime = VirtualAlloc(nullptr, allocation_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!client->runtime) {
        InterlockedExchange(&client->error, static_cast<LONG>(GatewayClientError::memory));
        InterlockedExchange(&client->native_error, static_cast<LONG>(thread_last_error()));
        return result(client);
    }
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    runtime->allocation_bytes = allocation_bytes;
    runtime->identity = GatewayClientIdentity::normal_user;
    runtime->owner = client;
    if (!stable_storage_initialize(&runtime->structural_scratch, kStructuralCount, kInitialStructuralCount,
                                   sizeof(u32), alignof(u32)) ||
        // Address space only: nothing is committed until an oversized guild.
        !reserved_bytes_initialize(&runtime->large_record, kGatewayClientGuildRecordCapacity) ||
        !stable_storage_initialize(&runtime->decoded_message, kGatewaySessionMaximumDecodedMessageBytes,
                                   kInitialDecodedMessageBytes, sizeof(u8), alignof(u8)) ||
        !stable_storage_initialize(&runtime->ready_record_index, kStructuralCount, kInitialStructuralCount,
                                   sizeof(u32), alignof(u32)) ||
        !reserved_bytes_initialize(&runtime->ready_staging, kReadyStagingBytes) ||
        !reserved_bytes_initialize(&runtime->compressed_spill, kGatewaySessionMaximumDecodedMessageBytes) ||
        !stable_storage_initialize(&runtime->zstd_storage, kZstdStorageBytes, kZstdInitialStorageBytes,
                                   sizeof(u8), alignof(u8))) {
        return fail_start(client, GatewayClientError::memory, thread_last_error());
    }
    runtime->user = reinterpret_cast<GatewayUserRuntime*>(static_cast<u8*>(client->runtime) +
                                                          kGatewayUserRuntimeOffset);
    runtime->user->metadata = *user_metadata;
    store_user_configuration(runtime->user, *user_configuration);
    gateway_self_status_initialize(&runtime->self_status, user_metadata->profile_id,
                                   {user_configuration->since_ms, user_configuration->status,
                                    user_configuration->has_since, user_configuration->afk});
    if (gateway_endpoint) {
        runtime->initial_endpoint = *gateway_endpoint;
    } else if (gateway_endpoint_parse_with_policy(
                   Utf8View{kGatewayUrl, static_cast<u32>(sizeof(kGatewayUrl) - 1)},
                   GatewayEndpointPolicy::user_v9_etf_zstd_stream, &runtime->initial_endpoint) !=
               GatewayEndpointError::none) {
        release_runtime_allocation(client);
        client->runtime = nullptr;
        return invalid_input_result();
    }
    if (resume_state && resume_state->valid) {
        runtime->resume = *resume_state;
        runtime->self_user_id = runtime->resume_user_id = resume_user_id;
        gateway_self_status_bind(&runtime->self_status, resume_user_id);
    }
    const ConstSlice<u8> token_bytes = secure_buffer_view(*credential);
    char heartbeat_id[kGatewayUserLaunchIdBytes];
    const GatewayCommandWriteResult preflight = gateway_write_user_identify(
        user_identify(*runtime->user, Utf8View{reinterpret_cast<const char*>(token_bytes.data), token_bytes.count},
                      heartbeat_id),
        Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    SecureZeroMemory(runtime->gateway_payload, sizeof(runtime->gateway_payload));
    if (!preflight.ok()) {
        release_runtime_allocation(client);
        client->runtime = nullptr;
        return invalid_input_result();
    }
    client->state_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!client->state_event)
        return fail_start(client, GatewayClientError::event, thread_last_error());
    client->notification_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!client->notification_event) {
        const u32 native_error = thread_last_error();
        close_start_events(client, runtime);
        return fail_start(client, GatewayClientError::event, native_error);
    }
    runtime->mailbox_space = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    runtime->mailbox_cancel =
        runtime->mailbox_space ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
    if (!runtime->mailbox_space || !runtime->mailbox_cancel) {
        const u32 native_error = thread_last_error();
        close_start_events(client, runtime);
        return fail_start(client, GatewayClientError::event, native_error);
    }
    if (credential) {
        if (!secure_buffer_transfer(&runtime->credential, credential)) {
            close_start_events(client, runtime);
            release_runtime_allocation(client);
            reset_value<GatewayClient>(client);
            return invalid_input_result();
        }
        runtime->authenticated = true;
    }
    const NetworkWorkerResult started =
        network_worker_start(&client->worker, gateway_worker_handler, client);
    if (!started.ok()) {
        if (credential && runtime->credential.data) {
            secure_buffer_transfer(credential, &runtime->credential);
        }
        close_start_events(client, runtime);
        return fail_start(client, GatewayClientError::worker, started.native_error);
    }
    return result(client);
}

} // namespace

GatewayClientResult gateway_client_start_user(GatewayClient* client, AccountProfile* profile,
                                              const GatewayUserClientConfiguration* configuration,
                                              const GatewayEndpoint* gateway_endpoint,
                                              const GatewayUserSessionSnapshot* session) noexcept {
    if (!profile || (session && (session->profile_id != profile->metadata.profile_id ||
                                 (session->has_resume && !gateway_resume_validate(session->resume)) ||
                                 (!session->has_resume && session->resume.valid)))) {
        return invalid_input_result();
    }
    const AccountProfileMetadata metadata = profile->metadata;
    const GatewayClientResult started = start_client(
        client, &profile->credential, &metadata, configuration, gateway_endpoint,
        session && session->has_resume ? &session->resume : nullptr,
        session && session->has_resume ? session->self_user_id : 0);
    if (!profile->credential.data) {
        reset_value<AccountProfile>(profile);
    }
    return started;
}

bool gateway_client_ready_matches_identity(GatewayClientIdentity identity,
                                           const GatewayReady& ready) noexcept {
    return identity == GatewayClientIdentity::normal_user && ready.api_version == 9;
}

GatewayClientResult stop_client(GatewayClient* client, u32 timeout_milliseconds,
                                AccountProfile* returned_profile) noexcept {
    const auto* initial_runtime =
        client && client->runtime ? static_cast<const GatewayClientRuntime*>(client->runtime) : nullptr;
    if (!client || !client->worker.thread || !initial_runtime ||
        (returned_profile &&
         (returned_profile->credential.data || returned_profile->credential.locked ||
          returned_profile->credential.bytes != 0 || returned_profile->credential.capacity != 0 ||
          returned_profile->metadata.profile_id != 0 ||
          initial_runtime->identity != GatewayClientIdentity::normal_user || !initial_runtime->user))) {
        return invalid_input_result();
    }
    if (WaitForSingleObject(client->worker.thread, 0) == WAIT_TIMEOUT) {
        SetEvent(initial_runtime->mailbox_cancel);
        network_worker_request_stop(&client->worker);
    }
    const NetworkWorkerResult joined = network_worker_join(&client->worker, timeout_milliseconds);
    GatewayClientResult value = result(client);
    if (!joined.ok()) {
        value.error = GatewayClientError::worker;
        value.native_error = joined.native_error;
        return value;
    }
    if (client->state_event) {
        CloseHandle(client->state_event);
    }
    if (client->notification_event) {
        CloseHandle(client->notification_event);
    }
    if (client->runtime) {
        auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
        CloseHandle(runtime->mailbox_space);
        CloseHandle(runtime->mailbox_cancel);
        voice_receive_stop(&runtime->voice_receive);
        // Decode threads must stop asking a freed receive path for keyframes.
        if (runtime->video_frames.bind_keyframes)
            runtime->video_frames.bind_keyframes(runtime->video_frames.context, {});
        gateway_voice_release(&runtime->voice);
        gateway_stream_session_release(&runtime->stream);
        if (returned_profile) {
            if (runtime->credential.data &&
                secure_buffer_transfer(&returned_profile->credential, &runtime->credential)) {
                returned_profile->metadata = runtime->user->metadata;
            } else {
                secure_buffer_release(&runtime->credential);
                value.error = GatewayClientError::invalid_input;
            }
        } else {
            secure_buffer_release(&runtime->credential);
        }
        release_runtime_allocation(client);
    }
    reset_value<GatewayClient>(client);
    value.phase =
        value.error == GatewayClientError::none ? GatewayClientPhase::stopped : GatewayClientPhase::failed;
    return value;
}

GatewayClientResult gateway_client_stop(GatewayClient* client, u32 timeout_milliseconds) noexcept {
    return stop_client(client, timeout_milliseconds, nullptr);
}

GatewayClientResult gateway_client_stop_return_user_profile(GatewayClient* client, u32 timeout_milliseconds,
                                                            AccountProfile* profile) noexcept {
    return profile ? stop_client(client, timeout_milliseconds, profile) : invalid_input_result();
}

AccountRequirement gateway_client_required_action(const GatewayClient* client) noexcept {
    return client ? static_cast<AccountRequirement>(InterlockedCompareExchange(
                        const_cast<volatile LONG*>(&client->required_action), 0, 0))
                  : AccountRequirement::none;
}

GatewayClientResult gateway_client_status(const GatewayClient* client) noexcept {
    return result(client);
}

bool gateway_client_pop_event(GatewayClient* client, GatewayClientEvent* event) noexcept {
    if (!client || !client->runtime || !event) {
        return false;
    }
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    const SpscMailboxResult discarded = spsc_mailbox_discard(&runtime->events, event);
    if (!discarded.ok()) {
        return false;
    }
    if (discarded.external)
        release_large_record(runtime);
    SetEvent(runtime->mailbox_space);
    if (spsc_mailbox_size(runtime->events) != 0) {
        SetEvent(client->notification_event);
    }
    return true;
}

GatewayClientPopResult gateway_client_pop_event_payload(GatewayClient* client, GatewayClientEvent* event,
                                                        Slice<u8> payload_output) noexcept {
    if (!client || !client->runtime || !event || (!payload_output.data && payload_output.count != 0)) {
        return GatewayClientPopResult{0, GatewayClientPopError::invalid_input};
    }
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    const u32 before = spsc_mailbox_load_u32(&runtime->events.read_sequence);
    // Only after the head descriptor is visible: the worker sets the lease
    // (and its committed bytes) before it publishes an external record.
    const ConstSlice<u8> large =
        spsc_mailbox_head_external(runtime->events) && ReadAcquire(&runtime->large_record_lease)
            ? ConstSlice<u8>{runtime->large_record.base, runtime->large_record.committed}
            : ConstSlice<u8>{};
    const SpscMailboxResult popped = spsc_mailbox_pop(&runtime->events, event, payload_output, large);
    if (popped.ok() && popped.external)
        release_large_record(runtime);
    if (spsc_mailbox_load_u32(&runtime->events.read_sequence) != before)
        SetEvent(runtime->mailbox_space);
    GatewayClientPopError error = GatewayClientPopError::none;
    switch (popped.error) {
    case SpscMailboxError::none:
        break;
    case SpscMailboxError::empty:
        error = GatewayClientPopError::empty;
        break;
    case SpscMailboxError::output_too_small:
        error = GatewayClientPopError::output_too_small;
        break;
    case SpscMailboxError::corrupt:
        error = GatewayClientPopError::corrupt;
        break;
    default:
        error = GatewayClientPopError::invalid_input;
        break;
    }
    if (spsc_mailbox_size(runtime->events) != 0) {
        SetEvent(client->notification_event);
    }
    return GatewayClientPopResult{popped.payload_bytes, error};
}

HANDLE gateway_client_notification_handle(const GatewayClient* client) noexcept {
    return client ? client->notification_event : nullptr;
}

u32 gateway_client_dropped_events(const GatewayClient* client) noexcept {
    if (!client || !client->runtime) {
        return 0;
    }
    const auto* runtime = static_cast<const GatewayClientRuntime*>(client->runtime);
    return spsc_mailbox_dropped(runtime->events);
}

bool gateway_client_snapshot_user_session(const GatewayClient* client,
                                          GatewayUserSessionSnapshot* session) noexcept {
    if (!client || !client->runtime || !session) {
        return false;
    }
    const auto* runtime = static_cast<const GatewayClientRuntime*>(client->runtime);
    if (runtime->identity != GatewayClientIdentity::normal_user || !runtime->user ||
        runtime->user->metadata.profile_id == 0) {
        return false;
    }
    AcquireSRWLockShared(const_cast<SRWLOCK*>(&runtime->resume_lock));
    const GatewayResumeState resume = runtime->resume;
    const u64 self_user_id = runtime->resume_user_id;
    ReleaseSRWLockShared(const_cast<SRWLOCK*>(&runtime->resume_lock));
    GatewayUserSessionSnapshot snapshot{};
    snapshot.profile_id = runtime->user->metadata.profile_id;
    if (resume.valid) {
        if (!gateway_resume_validate(resume)) {
            return false;
        }
        snapshot.resume = resume;
        snapshot.self_user_id = self_user_id;
        snapshot.has_resume = true;
    }
    *session = snapshot;
    return true;
}

bool gateway_client_subscribe_guild_members(GatewayClient* client,
                                            const GatewayGuildSubscription& subscription) noexcept {
    auto* runtime = live_user_runtime(client);
    if (!runtime || subscription.guild_id == 0 || subscription.clear_thread_guild_id != 0) {
        return false;
    }
    u8 validation[kGatewayUserMaximumCommandBytes]{};
    const GatewayCommandWriteResult valid = gateway_write_guild_subscriptions_bulk(
        subscription, Slice<u8>{validation, kGatewayUserMaximumCommandBytes});
    SecureZeroMemory(validation, sizeof(validation));
    if (!valid.ok()) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    copy_value(&runtime->desired_subscription, subscription);
    ++runtime->subscription_revision;
    if (runtime->subscription_revision == 0) {
        runtime->subscription_revision = 1;
        runtime->sent_subscription_revision = 0;
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    // A concurrent already-pending post also guarantees a worker wake and the
    // desired state is last-write-wins under subscription_lock.
    return wake_worker(client, &runtime->subscription_signal);
}

bool gateway_client_request_guild_members(GatewayClient* client,
                                          const GatewayGuildMemberRequest& request) noexcept {
    auto* runtime = live_user_runtime(client);
    if (!runtime || !gateway_guild_member_request_valid(request))
        return false;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    const auto prior = runtime->member_requests;
    bool admitted = gateway_member_request_admit(&runtime->member_requests, request, GetTickCount64());
    if (admitted && !wake_worker(client, &runtime->subscription_signal)) {
        runtime->member_requests = prior;
        admitted = false;
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return admitted;
}

bool gateway_client_request_channel_info(GatewayClient* client, u64 guild_id) noexcept {
    auto* runtime = live_user_runtime(client);
    if (!runtime || !guild_id)
        return false;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    bool admitted = gateway_channel_info_admit(&runtime->channel_info, guild_id);
    if (admitted && !wake_worker(client, &runtime->subscription_signal)) {
        gateway_channel_info_unqueue(&runtime->channel_info, guild_id);
        admitted = false;
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return admitted;
}

void gateway_client_cancel_guild_member_request(GatewayClient* client) noexcept {
    if (!client || !client->runtime)
        return;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    gateway_member_request_cancel_batch(&runtime->member_requests);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
}

void gateway_client_cancel_guild_member_query(GatewayClient* client) noexcept {
    if (!client || !client->runtime)
        return;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    gateway_member_request_cancel_query(&runtime->member_requests);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
}

GatewayClientSelfStatusSnapshot gateway_client_self_status_snapshot(const GatewayClient* client) noexcept {
    GatewayClientSelfStatusSnapshot result{};
    auto* runtime = user_runtime(client);
    if (!runtime)
        return result;
    AcquireSRWLockShared(&runtime->subscription_lock);
    const auto& owner = runtime->self_status;
    result.profile_id = owner.profile_id;
    result.self_user_id = owner.self_user_id;
    result.settings_revision = owner.settings_revision;
    result.status = owner.desired.status;
    result.confirmed_status = owner.confirmed_status;
    result.last_error = runtime->self_status_error;
    result.authoritative = owner.authoritative;
    result.available = owner.self_user_id && load_phase(client) == GatewayClientPhase::ready;
    result.pending = owner.request_id || (owner.authoritative && gateway_self_status_pending(owner));
    result.revision =
        owner.revision_serial * 4 + (result.authoritative ? 1 : 0) + (result.available ? 2 : 0);
    ReleaseSRWLockShared(&runtime->subscription_lock);
    return result;
}
namespace {
// Sends the self-status settings-proto/1 GET (fetch) or PATCH (update) with the
// subscription lock held, and records an accepted request on the owner.
RestClientSubmitResult submit_self_settings_locked(GatewayClientRuntime* runtime,
                                                   RestClient* rest,
                                                   SelfStatusSettingsOperation operation,
                                                   ConstSlice<u8> body, u64 request_id) noexcept {
    const bool update = operation == SelfStatusSettingsOperation::update;
    static constexpr char target[] = "/api/v10/users/@me/settings-proto/1";
    const RestClientRequest rest_request{{reinterpret_cast<const u8*>(target), sizeof(target) - 1},
                                         body,
                                         {0x5354415455535052ull, 0,
                                          update ? RestMethod::patch : RestMethod::get,
                                          RestMajorParameter::none},
                                         request_id,
                                         kRestClientDefaultTimeoutMs,
                                         update,
                                         false};
#if defined(MAICORD_GATEWAY_SELF_STATUS_TEST)
    const auto submitted = self_status_test_submit ? self_status_test_submit(rest_request)
                                                   : rest_client_submit(rest, rest_request);
#else
    const auto submitted = rest_client_submit(rest, rest_request);
#endif
    if (submitted.ok()) {
        auto& owner = runtime->self_status;
        owner.request_id = request_id;
        owner.request_settings_revision = owner.settings_revision;
        ++owner.revision_serial;
        runtime->self_status_error = GatewaySelfStatusError::none;
    }
    return submitted;
}

// A settings-proto PATCH that rewrites one part of the observed status group: `encode(argument, group,
// scratch, body)` writes it under the same lock as the checks and the enqueue.
using StatusGroupEncode = GatewayCommandWriteResult (*)(const void* argument, ConstSlice<u8> group,
                                                        Slice<u8> scratch, Slice<u8> body) noexcept;
RestClientSubmitResult submit_status_group_patch(GatewayClient* client, RestClient* rest, u64 profile,
                                                 u64 self, u64 request_id, StatusGroupEncode encode,
                                                 const void* argument) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = user_runtime(client);
    if (!runtime || !rest || !request_id || !profile || !self ||
        load_phase(client) != GatewayClientPhase::ready)
        return invalid;
    u8 scratch[kSelfStatusSettingsMaximumGroupBytes + 704];
    u8 body[kSelfStatusSettingsMaximumPatchBytes + 1024];
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    const auto& owner = runtime->self_status;
    if (load_phase(client) != GatewayClientPhase::ready || owner.profile_id != profile ||
        owner.self_user_id != self || owner.request_id || !owner.authoritative) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return invalid;
    }
    const auto encoded =
        encode(argument, ConstSlice<u8>{runtime->self_status_group, runtime->self_status_group_bytes},
               Slice<u8>{scratch, sizeof(scratch)}, Slice<u8>{body, sizeof(body)});
    if (!encoded.ok()) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return invalid;
    }
    const auto submitted = submit_self_settings_locked(runtime, rest, SelfStatusSettingsOperation::update,
                                                       {body, encoded.bytes}, request_id);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    SecureZeroMemory(scratch, sizeof(scratch));
    SecureZeroMemory(body, sizeof(body));
    return submitted;
}
} // namespace
RestClientSubmitResult gateway_client_self_status_submit(GatewayClient* client,
                                                         RestClient* rest, u64 profile,
                                                         u64 self,
                                                         SelfStatusSettingsOperation operation,
                                                         GatewayUserStatus status, u64 request_id,
                                                         u64 unix_now_ms) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = user_runtime(client);
    if (!runtime || !rest || !request_id || !profile || !self ||
        operation > SelfStatusSettingsOperation::update || status > GatewayUserStatus::invisible ||
        load_phase(client) != GatewayClientPhase::ready)
        return invalid;
    // Submit copies synchronously; hold one existing lock through validation,
    // body construction and enqueueing so a remote update cannot race the patch.
    u8 scratch[kSelfStatusSettingsMaximumGroupBytes + 64];
    u8 body[kSelfStatusSettingsMaximumPatchBytes];
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    auto& owner = runtime->self_status;
    if (load_phase(client) != GatewayClientPhase::ready || owner.profile_id != profile ||
        owner.self_user_id != self || owner.request_id ||
        (operation == SelfStatusSettingsOperation::update && !owner.authoritative)) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return invalid;
    }
    GatewayCommandWriteResult encoded{};
    if (operation == SelfStatusSettingsOperation::update) {
        encoded = self_status_settings_write_patch(
            {runtime->self_status_group, runtime->self_status_group_bytes}, status, unix_now_ms,
            {scratch, sizeof(scratch)}, {body, sizeof(body)});
        if (!encoded.ok()) {
            ReleaseSRWLockExclusive(&runtime->subscription_lock);
            return invalid;
        }
    }
    const auto submitted =
        submit_self_settings_locked(runtime, rest, operation, {body, encoded.bytes}, request_id);
    if (submitted.ok() && operation == SelfStatusSettingsOperation::update)
        gateway_self_status_request(&owner, profile, self, status);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    SecureZeroMemory(scratch, sizeof(scratch));
    SecureZeroMemory(body, sizeof(body));
    if (submitted.ok()) {
        network_signal_post(&client->worker.loop, &runtime->subscription_signal, 1);
        if (client->notification_event)
            SetEvent(client->notification_event);
    }
    return submitted;
}
bool gateway_client_self_status_complete(GatewayClient* client, u64 profile, u64 self, u64 request_id,
                                         const RestClientResponse* response,
                                         Utf8View body) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !response || !request_id || response->request_id != request_id)
        return false;
    SelfSettingsJson parsed{};
    const bool success = response->transport_ok() && response->status_code >= 200 &&
                         response->status_code < 300 && self_settings_json(body, &parsed) &&
                         (parsed.seen & 1) && !parsed.out_of_date;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    auto& owner = runtime->self_status;
    // A disconnected worker does not invalidate the primary REST request ID. The
    // UI observer contract excludes stop/join; account/request ID guards
    // allow its completion to be consumed without making controls available during reconnect.
    if (owner.profile_id != profile || owner.self_user_id != self || owner.request_id != request_id) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return false;
    }
    owner.request_id = 0;
    ++owner.revision_serial;
    if (owner.settings_revision == owner.request_settings_revision) {
        if (success)
            self_settings_observe_locked(runtime, parsed.encoded, false);
        else {
            runtime->self_status_error = parsed.out_of_date          ? GatewaySelfStatusError::out_of_date
                                         : !response->transport_ok() ? GatewaySelfStatusError::transport
                                                                     : GatewaySelfStatusError::rejected;
            if (owner.authoritative && owner.desired.status != owner.confirmed_status) {
                owner.desired = {0, owner.confirmed_status, false, false};
                if (++owner.revision == 0)
                    owner.revision = 1;
            }
        }
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    network_signal_post(&client->worker.loop, &runtime->subscription_signal, 1);
    if (client->notification_event)
        SetEvent(client->notification_event);
    return true;
}

bool gateway_client_custom_status_snapshot(const GatewayClient* client, SelfCustomStatus* output) noexcept {
    if (!output)
        return false;
    reset_value(output);
    auto* runtime = user_runtime(client);
    if (!runtime)
        return false;
    AcquireSRWLockShared(&runtime->subscription_lock);
    const bool known = runtime->self_status.authoritative && runtime->self_status_group &&
                       self_status_settings_read_custom({runtime->self_status_group, runtime->self_status_group_bytes},
                                                        output);
    ReleaseSRWLockShared(&runtime->subscription_lock);
    return known;
}
RestClientSubmitResult gateway_client_custom_status_submit(GatewayClient* client, RestClient* rest,
                                                           u64 profile, u64 self,
                                                           const SelfCustomStatus* custom,
                                                           u64 request_id) noexcept {
    return submit_status_group_patch(
        client, rest, profile, self, request_id,
        [](const void* argument, ConstSlice<u8> group, Slice<u8> scratch, Slice<u8> body) noexcept {
            return self_status_settings_write_custom_patch(
                group, static_cast<const SelfCustomStatus*>(argument), scratch, body);
        },
        custom);
}

bool gateway_client_local_presence(GatewayClient* client, const GatewayLocalPresence* input,
                                   GatewayActivitySharing* output) noexcept {
    if (output)
        reset_value(output);
    auto* runtime = user_runtime(client);
    if (!runtime || (input && !gateway_local_presence_valid(*input)))
        return false;
    bool changed = false;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    auto& owner = runtime->self_status;
    if (input && !gateway_local_presence_equal(runtime->local_presence, *input)) {
        copy_value(&runtime->local_presence, *input);
        if (++owner.revision == 0)
            owner.revision = 1;
        changed = true;
    }
    if (output) {
        output->known = owner.authoritative && runtime->self_status_group &&
                        self_status_settings_read_show_game(
                            {runtime->self_status_group, runtime->self_status_group_bytes}, &output->share);
        if (!output->known)
            output->share = false;
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    if (changed) {
        network_signal_post(&client->worker.loop, &runtime->subscription_signal, 1);
        if (client->notification_event)
            SetEvent(client->notification_event);
    }
    return true;
}
void gateway_client_activity_changed(GatewayClient* client) noexcept {
    if (user_runtime(client))
        network_signal_post(&client->worker.loop,
                            &static_cast<GatewayClientRuntime*>(client->runtime)->subscription_signal, 1);
}
RestClientSubmitResult gateway_client_activity_sharing_submit(GatewayClient* client, RestClient* rest,
                                                              u64 profile, u64 self, bool share,
                                                              u64 request_id) noexcept {
    return submit_status_group_patch(
        client, rest, profile, self, request_id,
        [](const void* argument, ConstSlice<u8> group, Slice<u8> scratch, Slice<u8> body) noexcept {
            return self_status_settings_write_show_game_patch(group, *static_cast<const bool*>(argument),
                                                              scratch, body);
        },
        &share);
}

#if defined(MAICORD_GATEWAY_SELF_STATUS_TEST)
bool gateway_self_status_test_create(GatewayClient* client, u64 profile, u64 self,
                                     GatewaySelfStatusTestSubmit submit) noexcept {
    if (!client || client->runtime || !profile || !self || !submit)
        return false;
    auto* runtime = static_cast<GatewayClientRuntime*>(
        VirtualAlloc(nullptr, kGatewayUserClientRuntimeBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime)
        return false;
    client->runtime = runtime;
    runtime->identity = GatewayClientIdentity::normal_user;
    runtime->user =
        reinterpret_cast<GatewayUserRuntime*>(reinterpret_cast<u8*>(runtime) + kGatewayUserRuntimeOffset);
    runtime->user->metadata.profile_id = profile;
    runtime->session.lifecycle.phase = GatewayLifecyclePhase::ready;
    runtime->self_user_id = self;
    gateway_self_status_initialize(&runtime->self_status, profile,
                                   {0, GatewayUserStatus::online, false, false});
    gateway_self_status_bind(&runtime->self_status, self);
    client->phase = static_cast<LONG>(GatewayClientPhase::ready);
    self_status_test_submit = submit;
    return true;
}
bool gateway_self_status_test_observe(GatewayClient* client, Utf8View dispatch) noexcept {
    if (!client || !client->runtime)
        return false;
    return self_settings_dispatch(client, static_cast<GatewayClientRuntime*>(client->runtime), dispatch);
}

void gateway_self_status_test_ready(GatewayClient* client, bool ready) noexcept {
    if (!client || !client->runtime)
        return;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    gateway_self_status_reset_connection(&runtime->self_status);
    runtime->session.lifecycle.phase =
        ready ? GatewayLifecyclePhase::ready : GatewayLifecyclePhase::awaiting_hello;
    client->phase =
        static_cast<LONG>(ready ? GatewayClientPhase::ready : GatewayClientPhase::reconnect_wait);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
}
u32 gateway_self_status_test_group_bytes(const GatewayClient* client) noexcept {
    return client && client->runtime
               ? static_cast<const GatewayClientRuntime*>(client->runtime)->self_status_group_bytes
               : 0;
}
u32 gateway_self_status_test_destroy(GatewayClient* client) noexcept {
    if (!client || !client->runtime)
        return 0;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    u32 wiped = 0;
    if (runtime->self_status_group) {
        SecureZeroMemory(runtime->self_status_group, kSelfStatusSettingsMaximumGroupBytes);
        for (u32 i = 0; i < kSelfStatusSettingsMaximumGroupBytes; ++i)
            if (!runtime->self_status_group[i])
                ++wiped;
        VirtualFree(runtime->self_status_group, 0, MEM_RELEASE);
    }
    SecureZeroMemory(runtime, kGatewayUserClientRuntimeBytes);
    VirtualFree(runtime, 0, MEM_RELEASE);
    reset_value(client);
    self_status_test_submit = nullptr;
    return wiped;
}
#endif

bool gateway_client_request_voice_state(GatewayClient* client,
                                        const GatewayVoiceStateUpdate& state) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !client->worker.thread || load_phase(client) != GatewayClientPhase::ready)
        return false;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    if (!runtime->voice_snapshot.user_id) {
        ReleaseSRWLockExclusive(&runtime->voice_lock);
        return false;
    }
    const auto previous = runtime->desired_voice;
    const bool was_pending = runtime->voice_request_pending;
    runtime->desired_voice = state;
    runtime->voice_request_pending = true;
    const bool posted = wake_worker(client, &runtime->voice_signal);
    if (!posted) {
        runtime->desired_voice = previous;
        runtime->voice_request_pending = was_pending;
    }
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    return posted;
}

bool gateway_client_voice_snapshot(const GatewayClient* client,
                                   GatewayClientVoiceSnapshot* output) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !output)
        return false;
    AcquireSRWLockShared(&runtime->voice_lock);
    *output = runtime->voice_snapshot;
    ReleaseSRWLockShared(&runtime->voice_lock);
    return true;
}

bool gateway_client_voice_user_audio(GatewayClient* client, u64 user_id, u32 gain_percent, bool muted) noexcept {
    auto* runtime = user_runtime(client);
    // Settings live beside the playback lock, so this is safe from the UI thread.
    return runtime && voice_receive_set_user(&runtime->voice_receive, user_id, gain_percent, muted);
}

bool gateway_client_voice_audio_configure(GatewayClient* client, const VoiceAudioSettings& settings) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !client->worker.thread)
        return false;
    voice_send_configure(&runtime->voice_send, settings);
    wake_worker(client, &runtime->voice_send_signal);
    return true;
}

bool gateway_client_voice_audio_status(GatewayClient* client, VoiceAudioStatus* output) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !output)
        return false;
    *output = voice_send_status(&runtime->voice_send);
    return true;
}

bool gateway_client_voice_video_snapshot(const GatewayClient* client,
                                         GatewayClientVoiceVideoSnapshot* output) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !output)
        return false;
    AcquireSRWLockShared(&runtime->voice_lock);
    copy_value(output, runtime->voice_video_snapshot);
    ReleaseSRWLockShared(&runtime->voice_lock);
    return true;
}

bool gateway_client_stream_watch(GatewayClient* client, const GatewayStreamKey& key) noexcept {
    auto* runtime = user_runtime(client);
    const bool stop = key.type == GatewayStreamType::none;
    if (!runtime || !client->worker.thread || load_phase(client) != GatewayClientPhase::ready ||
        (!stop && !gateway_stream_key_valid(key)))
        return false;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    const auto& voice = runtime->voice_snapshot;
    // Watching needs the local call in the stream's channel; the worker
    // re-checks and ends the stream when the call leaves it.
    if (!voice.user_id || (!stop && (voice.phase == VoiceSetupPhase::idle || voice.channel_id != key.channel_id ||
                                     key.owner_id == voice.user_id))) {
        ReleaseSRWLockExclusive(&runtime->voice_lock);
        return false;
    }
    const auto previous = runtime->desired_stream;
    const bool was_pending = runtime->stream_request_pending;
    runtime->desired_stream = stop ? GatewayStreamKey{} : key;
    runtime->stream_request_pending = true;
    const bool posted = wake_worker(client, &runtime->voice_signal);
    if (!posted) {
        runtime->desired_stream = previous;
        runtime->stream_request_pending = was_pending;
    }
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    return posted;
}

bool gateway_client_stream_snapshot(const GatewayClient* client, GatewayClientStreamSnapshot* output) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !output)
        return false;
    AcquireSRWLockShared(&runtime->voice_lock);
    *output = runtime->stream_snapshot;
    ReleaseSRWLockShared(&runtime->voice_lock);
    return true;
}

bool gateway_client_stream_volume(GatewayClient* client, u32 percent) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || percent > kGatewayStreamVolumeMaximum)
        return false;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    runtime->desired_stream_volume = percent;
    runtime->stream_volume_pending = true;
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    wake_worker(client, &runtime->voice_signal);
    return true;
}

bool gateway_client_voice_video_wanted(GatewayClient* client, u64 user_id, bool wanted) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !client->worker.thread || !user_id)
        return false;
    bool ok = true;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    if (gateway_stream_video_user_tagged(user_id)) {
        // The watched stream's tile: its own connection's op 15.
        const u64 owner = gateway_stream_video_owner(user_id);
        if (wanted ? runtime->stream_unwanted_owner == owner : runtime->stream_unwanted_owner != owner) {
            runtime->stream_unwanted_owner = wanted ? 0 : owner;
            runtime->stream_wants_dirty = true;
        }
        ReleaseSRWLockExclusive(&runtime->voice_lock);
        wake_worker(client, &runtime->voice_send_signal);
        return owner != 0;
    }
    u32 at = 0;
    while (at != runtime->video_unwanted_count && runtime->video_unwanted[at] != user_id)
        ++at;
    if (wanted && at != runtime->video_unwanted_count)
        runtime->video_unwanted[at] = runtime->video_unwanted[--runtime->video_unwanted_count];
    else if (!wanted && at == runtime->video_unwanted_count) {
        ok = at != kVoiceVideoUnwantedUsers;
        if (ok)
            runtime->video_unwanted[runtime->video_unwanted_count++] = user_id;
    }
    runtime->video_unwanted_dirty = true;
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    if (ok)
        wake_worker(client, &runtime->voice_send_signal);
    return ok;
}

GatewayStateRecoveryResult gateway_client_request_fresh_state(GatewayClient* client) noexcept {
    if (!client || !client->runtime || !client->worker.thread)
        return GatewayStateRecoveryResult::unavailable;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    if (runtime->identity != GatewayClientIdentity::normal_user)
        return GatewayStateRecoveryResult::unavailable;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    auto result = gateway_state_recovery_request(&runtime->state_recovery,
                                                 load_phase(client) == GatewayClientPhase::ready);
    if (result == GatewayStateRecoveryResult::queued &&
        !wake_worker(client, &runtime->subscription_signal)) {
        gateway_state_recovery_cancel_post(&runtime->state_recovery);
        result = GatewayStateRecoveryResult::unavailable;
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return result;
}

bool gateway_client_set_video_frames(GatewayClient* client, const GatewayVideoFrames& frames) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !client->worker.thread)
        return false;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    runtime->video_frames = frames;
    runtime->video_frames_dirty = true;
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    wake_worker(client, &runtime->voice_send_signal);
    return true;
}

bool gateway_client_has_video_frames(const GatewayClient* client, const void* context) noexcept {
    if (!client || !client->runtime)
        return false;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    AcquireSRWLockShared(&runtime->voice_lock);
    const bool installed = runtime->video_frames.context == context;
    ReleaseSRWLockShared(&runtime->voice_lock);
    return installed;
}

bool gateway_client_attach_soundboard(GatewayClient* client, SoundboardMixer* mixer) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !client->worker.thread)
        return false;
    auto& playback = runtime->voice_receive.playback;
    if (voice_playback_effects(&playback) != mixer)
        voice_playback_set_effects(&playback, mixer);
    AcquireSRWLockShared(playback.lock.native());
    const bool live = playback.block != nullptr;
    ReleaseSRWLockShared(playback.lock.native());
    return live;
}

u32 gateway_client_runtime_bytes() noexcept {
    return static_cast<u32>(sizeof(GatewayClientRuntime));
}

u32 gateway_client_user_runtime_bytes() noexcept {
    return kGatewayUserClientRuntimeBytes;
}

} // namespace maicord
