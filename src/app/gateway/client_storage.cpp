#include "client_internal.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/app/gateway/guild_folders.hpp"
#include "maicord/app/gateway/user_settings.hpp"
#include "maicord/app/account/user_client_configuration.hpp"
#include "maicord/protocol/user/settings_proto.hpp"
#include "maicord/protocol/user/gif_favorites.hpp"
#include "../../protocol/encoding/json_field_lists.hpp"

namespace maicord {
namespace client_detail {

Slice<u32> main_index_view(GatewayClientRuntime* runtime) noexcept {
    return {stable_storage_data<u32>(runtime->structural_scratch), runtime->structural_scratch.capacity};
}
Slice<u8> decoded_message_view(GatewayClientRuntime* runtime) noexcept {
    return {stable_storage_data<u8>(runtime->decoded_message), runtime->decoded_message.capacity};
}

} // namespace client_detail
namespace {

// Index and message storage grows only before shutdown, the READY deadline and owner cancellation.
bool growth_allowed(const GatewayClientRuntime* runtime) noexcept {
    return !runtime->stopping &&
           !(runtime->mailbox_deadline_ms && clock_monotonic_milliseconds() >= runtime->mailbox_deadline_ms) &&
           !(runtime->mailbox_cancel && wait_one(runtime->mailbox_cancel, 0) != WAIT_TIMEOUT);
}

} // namespace
namespace client_detail {

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
Slice<u32> ready_arrays_view(GatewayClientRuntime* runtime) noexcept {
    return {stable_storage_data<u32>(runtime->ready_arrays), runtime->ready_arrays.capacity};
}
bool grow_ready_record_index(void* context, u32 required, Slice<u32>* backed) noexcept {
    auto* runtime = static_cast<GatewayClientRuntime*>(context);
    if (!growth_allowed(runtime) || !stable_storage_grow(&runtime->ready_record_index, required))
        return false;
    *backed = ready_record_index_view(runtime);
    return true;
}

} // namespace client_detail
namespace {

// Decoded bytes and indexes never outlive their event. After a payload such
// as READY grows them past 1 MiB, the pages are returned before the next one.
inline constexpr u32 kRetainedDecodedMessageBytes = 256 * 1024;
inline constexpr u32 kRetainedStructuralCount = kRetainedDecodedMessageBytes / sizeof(u32);

} // namespace
namespace client_detail {

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
    if (runtime->ready_arrays.capacity > kInitialStructuralCount)
        stable_storage_shrink(&runtime->ready_arrays, kInitialStructuralCount);
    // The window moves to the front; the pages past it and the next message's room are returned.
    if (runtime->user && runtime->session.zstd_stream == &runtime->user->zstd &&
        runtime->zstd_storage.capacity > 4 * kZstdInitialStorageBytes) {
        const u32 keep = zstd_stream_compact(&runtime->user->zstd);
        const u32 target = maximum(kZstdInitialStorageBytes, keep + kZstdMessageSlackBytes);
        if (stable_storage_shrink(&runtime->zstd_storage, target))
            zstd_stream_set_storage(&runtime->user->zstd, zstd_storage_view(runtime));
    }
}

} // namespace client_detail
namespace {

void wipe_storage(const StableStorage& storage) noexcept {
    if (storage.arena.base)
        SecureZeroMemory(storage.arena.base, storage.arena.committed);
}

void release_wiped_storage(StableStorage* storage) noexcept {
    wipe_storage(*storage);
    stable_storage_release(storage);
}

} // namespace
namespace client_detail {

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
    release_wiped_storage(&runtime->ready_arrays);
    if (runtime->self_status_group) {
        SecureZeroMemory(runtime->self_status_group, kSelfStatusSettingsMaximumGroupBytes);
        pages_release(runtime->self_status_group);
        runtime->self_status_group = nullptr;
    }
    secure_buffer_release(&runtime->refreshed_credential);
    SecureZeroMemory(runtime, bytes);
    pages_release(runtime);
}

} // namespace client_detail
namespace {

bool is_user(const GatewayClientRuntime* runtime) noexcept {
    return runtime && runtime->user;
}

} // namespace
namespace client_detail {

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
    return !runtime->stopping && !runtime->failed ? runtime : nullptr;
}

ClientSessionMetadata* session_metadata(const GatewayClientRuntime* runtime) noexcept {
    return is_user(runtime) ? runtime->user->session_metadata : nullptr;
}

} // namespace client_detail
namespace {

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

} // namespace
namespace client_detail {

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

// Wakes the worker for a changed subscription-lock state and tells the consumer.
void post_subscription_change(GatewayClient* client, GatewayClientRuntime* runtime) noexcept {
    network_signal_post(&client->worker.loop, &runtime->subscription_signal, 1);
    if (client->notification_event)
        event_set(client->notification_event);
}

GatewayClientResult invalid_input_result() noexcept {
    GatewayClientResult value{};
    value.error = GatewayClientError::invalid_input;
    return value;
}

} // namespace client_detail
namespace {

// A launch that starts with a stored token fast-connects once, with the page's own
// launch ID; an interactive sign-in spends it first, as the page only fast-connects
// when it loads with a token.
volatile LONG fast_connect_taken;
// The page's presence store: unknown until its first session opens, then the
// status it last committed, which survives a sign-out. Status + 1; zero is unknown.
volatile LONG launch_presence;
// The page's socket connects every later session, a sign-out and the next
// sign-in included, to the last READY's resume_gateway_url.
SRWLOCK remembered_url_lock;
char remembered_url[kGatewayResumeUrlCapacity];
u32 remembered_url_bytes;

} // namespace

void gateway_user_launch_signed_in() noexcept {
    InterlockedExchange(&fast_connect_taken, 1);
}

namespace client_detail {

bool take_fast_connect(char (&launch_id)[kGatewayUserLaunchIdBytes + 1]) noexcept {
    return InterlockedCompareExchange(&fast_connect_taken, 1, 0) == 0 && user_client_random_uuid(launch_id);
}

GatewayUserStatus launch_presence_status() noexcept {
    const LONG status = ReadAcquire(&launch_presence);
    return status ? static_cast<GatewayUserStatus>(status - 1) : GatewayUserStatus::unknown;
}

void note_launch_presence(GatewayUserStatus status) noexcept {
    InterlockedExchange(&launch_presence, static_cast<LONG>(status) + 1);
}

void remember_gateway_url(const GatewayResumeState& resume) noexcept {
    if (!resume.valid || !resume.resume_url_bytes || resume.resume_url_bytes > kGatewayResumeUrlCapacity)
        return;
    AcquireSRWLockExclusive(&remembered_url_lock);
    copy_bytes(remembered_url, resume.resume_url, resume.resume_url_bytes);
    remembered_url_bytes = resume.resume_url_bytes;
    ReleaseSRWLockExclusive(&remembered_url_lock);
}

bool remembered_gateway_url(GatewayEndpoint* endpoint) noexcept {
    AcquireSRWLockShared(&remembered_url_lock);
    const bool parsed = remembered_url_bytes &&
                        gateway_endpoint_parse_with_policy(Utf8View{remembered_url, remembered_url_bytes},
                                                           GatewayEndpointPolicy::user_v9_etf_zstd_stream,
                                                           endpoint) == GatewayEndpointError::none;
    ReleaseSRWLockShared(&remembered_url_lock);
    return parsed;
}

// The heartbeat QoS activity identify reports (the upcoming state, else the current one).
bool qos_active(const GatewayQosState& qos) noexcept {
    return qos.has_upcoming ? qos.upcoming.active : qos.has_current && qos.current.active;
}

} // namespace client_detail
namespace {

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

template <u32 Capacity> void store_host_text(GatewayUserTextStorage<Capacity>* storage, const Utf8View& text) noexcept {
    storage->bytes = text.bytes < Capacity ? text.bytes : Capacity;
    copy_bytes(storage->data, text.data, storage->bytes);
}

} // namespace
namespace client_detail {

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
#if defined(_WIN32)
    properties.os_build_revision = {user.os_build_revision.data, user.os_build_revision.bytes};
#else
    properties.linux_desktop = user.linux_desktop;
    properties.window_manager = {user.window_manager.data, user.window_manager.bytes};
    properties.distro = {user.distro.data, user.distro.bytes};
    properties.runtime_environment = {user.runtime_environment.data, user.runtime_environment.bytes};
    properties.display_server = {user.display_server.data, user.display_server.bytes};
#endif
    if (user.session_metadata) {
        const GatewayUserProperties live =
            client_session_metadata_properties(user.session_metadata, heartbeat_id);
        identify.properties.client_heartbeat_session_id = live.client_heartbeat_session_id;
        identify.properties.client_app_state = live.client_app_state;
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
#if defined(_WIN32)
    store_host_text(&user->os_build_revision, properties.os_build_revision);
#else
    user->linux_desktop = properties.linux_desktop;
    store_host_text(&user->window_manager, properties.window_manager);
    store_host_text(&user->distro, properties.distro);
    store_host_text(&user->runtime_environment, properties.runtime_environment);
    store_host_text(&user->display_server, properties.display_server);
#endif
    user->session_metadata = configuration.metadata;
    user->since_ms = configuration.since_ms;
    user->status = configuration.status;
    user->has_since = configuration.has_since;
    user->afk = configuration.afk;
}

GatewayClientPhase load_phase(const GatewayClient* client) noexcept {
    return static_cast<GatewayClientPhase>(ReadAcquire(&client->phase));
}

} // namespace client_detail
namespace {

GatewayClientError load_error(const GatewayClient* client) noexcept {
    return static_cast<GatewayClientError>(ReadAcquire(&client->error));
}

} // namespace
namespace client_detail {

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
        event_set(client->notification_event);
    }
}

bool publish_event_payload(GatewayClient* client, GatewayClientRuntime* runtime,
                           const GatewayClientEvent& event, const ConstSlice<u8>& payload) noexcept {
    const u64 deadline =
        runtime->mailbox_deadline_ms ? runtime->mailbox_deadline_ms : clock_monotonic_milliseconds() + kPhaseTimeoutMs;
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
        runtime->mailbox_deadline_ms ? runtime->mailbox_deadline_ms : clock_monotonic_milliseconds() + kPhaseTimeoutMs;
    if (!client || !gateway_mailbox_wait_lease(&runtime->events, runtime->mailbox_space, runtime->mailbox_cancel,
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
    runtime->resume_sequence = static_cast<LONG64>(state.sequence);
    runtime->resume_user_id = runtime->self_user_id;
    ReleaseSRWLockExclusive(&runtime->resume_lock);
}

} // namespace client_detail

GatewayClientResult gateway_client_status(const GatewayClient* client) noexcept {
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

namespace client_detail {

void publish_failure(GatewayClient* client, GatewayClientError error, u32 native_error) noexcept {
    if (InterlockedCompareExchange(&client->error, static_cast<LONG>(error),
                                   static_cast<LONG>(GatewayClientError::none)) ==
        static_cast<LONG>(GatewayClientError::none)) {
        InterlockedExchange(&client->native_error, static_cast<LONG>(native_error));
    }
    publish_phase(client, GatewayClientPhase::failed);
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
        &runtime->voice_receive, {snapshot.speaking_users, kGatewayVoiceSpeakingUsers}, clock_monotonic_milliseconds());
    // The local user is never received; transmitting counts as speaking.
    if (const u64 self = runtime->self_user_id; self && voice_send_speaking(runtime->voice_send)) {
        u32 at = 0;
        while (at < snapshot.speaking_count && snapshot.speaking_users[at] < self)
            ++at;
        if ((at == snapshot.speaking_count || snapshot.speaking_users[at] != self) &&
            snapshot.speaking_count < kGatewayVoiceSpeakingUsers) {
#pragma clang loop vectorize(disable) interleave(disable)
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
    const auto stream = gateway_stream_session_snapshot(runtime->stream, runtime->self_user_id);
    AcquireSRWLockExclusive(&runtime->voice_lock);
    const bool changed = !(snapshot == runtime->voice_snapshot) || !(video == runtime->voice_video_snapshot) ||
                         !(stream == runtime->stream_snapshot);
    copy_value(&runtime->voice_snapshot, snapshot);
    copy_value(&runtime->voice_video_snapshot, video);
    copy_value(&runtime->stream_snapshot, stream);
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    if (changed && client->notification_event)
        event_set(client->notification_event);
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

} // namespace client_detail
namespace {

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

} // namespace
namespace client_detail {

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

bool ready_projection_work(void* opaque, GatewayReadyProjectionPhase) noexcept {
    auto& binding = *static_cast<ReadyProjectionBinding*>(opaque);
    const auto& runtime = *binding.runtime;
    const u64 now = clock_monotonic_milliseconds();
    if (runtime.stopping || !runtime.mailbox_deadline_ms || now >= runtime.mailbox_deadline_ms || binding.cancelled)
        return false;
    if (binding.cancel_checked_ms == now)
        return true;
    binding.cancel_checked_ms = now;
    binding.cancelled = wait_one(runtime.mailbox_cancel, 0) != WAIT_TIMEOUT;
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
    if (runtime->closing)
        return wait_milliseconds_until(now_ms, runtime->closing_deadline_ms);
    if (runtime->stopping)
        return INFINITE;
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
    deadline = earliest_deadline(deadline, runtime->members_release_ms);
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
    GatewayVoiceStateUpdate voice_command;
    GatewayStreamCommand stream_command;
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
    runtime->next_address = 0;
}

void clear_resume(GatewayClientRuntime* runtime) noexcept {
    reset_value(&runtime->reaction_batch);
    AcquireSRWLockExclusive(&runtime->resume_lock);
    gateway_resume_clear(&runtime->resume);
    runtime->resume_sequence = 0;
    runtime->resume_user_id = 0;
    ReleaseSRWLockExclusive(&runtime->resume_lock);
}

void clear_user_transport(GatewayClientRuntime* runtime) noexcept {
    zstd_stream_clear(&runtime->user->zstd);
    wipe_storage(runtime->zstd_storage);
    SecureZeroMemory(runtime->handshake_output, kHandshakeBytes);
    wipe_storage(runtime->decoded_message);
    wipe_storage(runtime->ready_record_index);
    wipe_storage(runtime->ready_arrays);
}

} // namespace client_detail
namespace {

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

} // namespace
namespace client_detail {

bool self_settings_json(Utf8View source, SelfSettingsJson* parsed) noexcept {
    u32 structural[64];
    const auto scan = json_scan_structurals_avx1(source, {structural, 64});
    if (!scan.ok())
        return false;
    parsed->document = {source, {structural, scan.count}};
    return json_visit_root_object(source, parsed->document.structurals, 16, self_settings_json_member,
                                  parsed)
        .ok();
}

} // namespace client_detail
namespace {

// The user's interface locale (localization.locale) becomes X-Discord-Locale,
// and clips.allow_voice_recording (true unless set) the opcode 4 flag;
// settings without them leave the current values.
void observe_settings_metadata(GatewayClientRuntime* runtime, JsonValue value, bool partial) noexcept {
    JsonString encoded{};
    if (!json_value_as_string(value, &encoded) || encoded.has_escapes)
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

} // namespace
namespace client_detail {

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
    if (!decoded) {
        owner.authoritative = owner.settings_known = false;
        runtime->self_status_error = GatewaySelfStatusError::settings;
        runtime->self_status_group_bytes = 0;
        return false;
    }
    if (!runtime->self_status_group)
        runtime->self_status_group = static_cast<u8*>(commit_pages(kSelfStatusSettingsMaximumGroupBytes));
    if (!runtime->self_status_group) {
        owner.authoritative = owner.settings_known = false;
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
    owner.settings_known = true;
    runtime->self_status_error = GatewaySelfStatusError::none;
    // No saved status (Userdoccers: "unknown" by default) stays unconfirmed, never a fabricated Online.
    if (!projection.present || !projection.known)
        owner.authoritative = false;
    else
        gateway_self_status_observe(&owner, projection.status);
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
            event_set(client->notification_event);
        return true;
    }
    if (parsed.type != 1)
        return false;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    const bool observed = self_settings_observe_locked(runtime, parsed.encoded, parsed.partial);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    if (client->notification_event)
        event_set(client->notification_event);
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

} // namespace client_detail
} // namespace maicord
