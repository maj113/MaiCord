#include "maicord/core/win32_process.hpp"
#include "maicord/app/rest_client.hpp"
#include "network_request_state.hpp"
#include "maicord/app/account_logout.hpp"
#include "maicord/app/user_client_configuration.hpp"

#include "maicord/core/ascii.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/trace_record.hpp"
#include "maicord/platform/tls_stream.hpp"
#include "maicord/media/deflate.hpp"

#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_records.hpp"

namespace maicord {
namespace {

RestTraceFunction g_trace = nullptr;
volatile LONG g_trace_serial = 0;

inline constexpr wchar_t kApiHostWide[] = L"discord.com";
inline constexpr u32 kEncryptedInputBytes = 128 * 1024;
inline constexpr u32 kHandshakeOutputBytes = 256 * 1024;
inline constexpr u32 kRecordOutputBytes = 64 * 1024;
inline constexpr u32 kRequestHeadBytes = 4 * 1024;
inline constexpr u32 kNormalUserRequestHeadBytes = 20 * 1024;
inline constexpr u32 kUserAuthRequestHeadBytes = 12 * 1024;
static_assert(kRequestHeadBytes <= kRestMaximumHeaderBytes &&
              kNormalUserRequestHeadBytes <= kRestMaximumHeaderBytes &&
              kUserAuthRequestHeadBytes <= kRestMaximumHeaderBytes);
inline constexpr u32 kUserAuthEncryptedInputBytes = 32 * 1024;
inline constexpr u32 kUserAuthRecordOutputBytes = 20 * 1024;
inline constexpr u32 kUserClientMetadataBytes = kRestUserSuperPropertiesMaximumBytes +
                                               kRestUserLocaleMaximumBytes + kRestUserTimezoneMaximumBytes;
inline constexpr u32 kRateStructuralCount = 256;
inline constexpr u32 kRateBucketCount = 32;
inline constexpr u32 kRateBindingCount = 64;
inline constexpr u32 kRequestSignalValue = 0x52514553u;
inline constexpr u32 kCancelSignalValue = 0x43414E43u;
inline constexpr u32 kRateSignalValue = 0x52415445u;
inline constexpr u32 kMaximumRequestTimeoutMs = 10 * 60 * 1000;
inline constexpr u32 kMaximumCredentialBytes = 2 * 1024;

struct RestEndpointKey {
    u64 key_low, key_high;
    u32 path_bytes;
};
struct RestEndpointLease {
    u16 index, generation; // generation zero means no reservation
};
enum class EndpointGateKind : u8 {
    acquired,
    waiting,
    busy,
    error
};
struct EndpointGateResult {
    EndpointGateKind kind;
    u64 wait_until_ms;
};

RestEndpointKey endpoint_key(ConstSlice<u8> target) noexcept {
    if (!target.data || !target.count)
        return {};
    u32 bytes = 0;
    while (bytes < target.count && target[bytes] != '?')
        ++bytes;
    return {hash64_counted_bytes(kHash64Seed, target.data, bytes),
            hash64_counted_bytes(~kHash64Seed, target.data, bytes), bytes};
}

RestEndpointGateEntry* endpoint_entry(RestEndpointGate* gate, RestEndpointLease lease) noexcept {
    if (!gate || !lease.generation || lease.index >= kRestEndpointGateCapacity)
        return nullptr;
    auto& entry = gate->entries[lease.index];
    return entry.generation == lease.generation && entry.in_flight ? &entry : nullptr;
}

EndpointGateResult endpoint_gate_acquire(RestEndpointGate* gate, RestEndpointKey key, u64 now_ms,
                                         RestEndpointLease* lease) noexcept {
    if (!gate || !key.path_bytes || !lease || lease->generation)
        return {EndpointGateKind::error, 0};
    u32 available = kRestEndpointGateCapacity;
    u64 reclaim_at_ms = 0;
    for (u32 i = 0; i < kRestEndpointGateCapacity; ++i) {
        auto& entry = gate->entries[i];
        if (entry.generation && entry.path_bytes == key.path_bytes && entry.key_low == key.key_low &&
            entry.key_high == key.key_high) {
            if (entry.retry_at_ms > now_ms)
                return {EndpointGateKind::waiting, entry.retry_at_ms};
            // After an outage, release one probe at a time. Requests already
            // admitted before the floor was observed may still be completing.
            if (entry.retry_at_ms && entry.in_flight)
                return {EndpointGateKind::busy, 0};
            if (entry.in_flight == 0xffffu)
                return {EndpointGateKind::error, 0};
            if (!entry.in_flight) {
                const u16 generation = static_cast<u16>(entry.generation + 1);
                entry.generation = generation ? generation : static_cast<u16>(1);
            }
            ++entry.in_flight;
            *lease = {static_cast<u16>(i), entry.generation};
            return {EndpointGateKind::acquired, 0};
        }
        if (!entry.in_flight) {
            if ((!entry.generation || entry.retry_at_ms <= now_ms) &&
                available == kRestEndpointGateCapacity)
                available = i;
            else if (entry.retry_at_ms > now_ms)
                reclaim_at_ms = earliest_deadline(reclaim_at_ms, entry.retry_at_ms);
        }
    }
    if (available == kRestEndpointGateCapacity)
        return reclaim_at_ms ? EndpointGateResult{EndpointGateKind::waiting, reclaim_at_ms}
                             : EndpointGateResult{EndpointGateKind::busy, 0};
    auto& entry = gate->entries[available];
    const u16 generation = static_cast<u16>(entry.generation + 1);
    entry = {
        key.key_low, key.key_high, 0, key.path_bytes, generation ? generation : static_cast<u16>(1), 1};
    *lease = {static_cast<u16>(available), entry.generation};
    return {EndpointGateKind::acquired, 0};
}

EndpointGateResult endpoint_gate_recheck(RestEndpointGate* gate, RestEndpointLease lease,
                                         u64 now_ms) noexcept {
    const auto* entry = endpoint_entry(gate, lease);
    if (!entry)
        return {EndpointGateKind::error, 0};
    return entry->retry_at_ms > now_ms ? EndpointGateResult{EndpointGateKind::waiting, entry->retry_at_ms}
                                       : EndpointGateResult{EndpointGateKind::acquired, 0};
}

bool endpoint_gate_observe(RestEndpointGate* gate, RestEndpointLease lease, u64 deadline_ms) noexcept {
    auto* entry = endpoint_entry(gate, lease);
    if (!entry)
        return false;
    entry->retry_at_ms = maximum(entry->retry_at_ms, deadline_ms);
    return true;
}

bool endpoint_gate_release(RestEndpointGate* gate, RestEndpointLease* lease, u64 now_ms,
                           bool settled = true) noexcept {
    if (!lease)
        return false;
    auto* entry = endpoint_entry(gate, *lease);
    reset_value(lease);
    if (!entry)
        return false;
    --entry->in_flight;
    if (settled && !entry->in_flight && entry->retry_at_ms <= now_ms)
        entry->retry_at_ms = 0;
    return true;
}

struct RestClientCommand {
    RestRouteKey route;
    RestEndpointKey endpoint_key;
    u64 request_id;
    i64 submitted_counter;
    u32 target_bytes;
    u32 body_bytes;
    u32 head_bytes;
    u32 timeout_ms;
    bool json_body;
    bool identity_response;
    u32 audit_reason_bytes;
    u32 extra_headers_bytes;
    u32 multipart_boundary_bytes;
    bool send_empty_audit_reason;
};

struct RestLocalRates {
    RestRateScheduler scheduler;
    RestRateBucket buckets[kRateBucketCount];
    RestRateBinding bindings[kRateBindingCount];
    RestEndpointGate endpoint_gate;
};

struct RestClientRuntime {
    TlsStream transport;
    NetworkSignal request_signal;
    NetworkSignal cancel_signal;
    NetworkSignal rate_signal;
    RestClient* owner;
    RestResponse response;
    RestResponseStep response_step;
    RestRateScheduler* scheduler;
    RestEndpointGate* endpoint_gate;
    RestEndpointLease endpoint_lease;
    RestTransaction transaction;
    RestClientCommand command;
    RestClientResponse completed;
    // Request-kind ID set by the producer; survives worker command erasure
    // and is read atomically by logout cancellation and response intake.
    volatile LONG64 logout_request_id;
    volatile LONG64 logout_reservation_id;
    u64 logout_preempt_id;
    bool logout_cancelled;
    SecureBuffer credential;
    SecureBuffer secret_storage;
    Clock clock;
    i64 request_started_counter;
    i64 phase_counter, first_byte_counter, receive_counter, ready_counter;
    u64 trace_request_id;
    u32 trace_worker_id;
    u8 trace_lane;
    RestClientPhase trace_phase;
    // Producer-only rejection aggregation, separate from the active command.
    u64 busy_since_ms, busy_count;
    u8* request_target;
    u8* request_body;
    u8* encrypted_storage;
    u8* handshake_output;
    u8* record_output;
    u8* request_head;
    u8* response_input;
    u8* response_body;
    u8* decompression;
    u8* default_body;
    u8* extended_body;
    u32 extended_body_bytes;
    u32 default_response_capacity;
    u32 decompression_capacity;
    DeflateWorkspace inflate_workspace;
    u32 wire_body_bytes;
    u32 decode_us;
    RestContentEncoding wire_encoding;
    bool transport_ready;
    bool write_commit_failed;
    bool response_callback_failed;
    bool draining;
    bool stopping;
    bool user_auth_mode;
    bool waiting_for_peer;
    u8* user_super_properties;
    char* user_locale;
    char* user_timezone;
    u64 rate_wait_until_ms;
    u32 allocation_bytes;
    u32 response_capacity;
    u32 request_body_capacity;
    u32 request_head_capacity;
    u32 connection_request_count;
    u32 pending_native_error;
    u32 user_super_properties_bytes;
    u32 user_locale_bytes;
    u32 user_timezone_bytes;
    u32 rate_structural_scratch[kRateStructuralCount];
    RestAuthorization authorization;
    RestClientError pending_error;
    u8 audit_reason[kRestAuditReasonMaximumEncodedBytes];
    u8 extra_headers[kRestExtraHeadersMaximumBytes];
    u8 multipart_boundary[kRestMultipartBoundaryMaximumBytes];
    // Producer-owned one-shot headers for armed_request_id.
    u8 armed_headers[kRestExtraHeadersMaximumBytes];
    u64 armed_request_id;
    u32 armed_headers_bytes;
};

struct RateLock {
    RestSharedState* shared;
    explicit RateLock(RestClientRuntime* runtime) noexcept : shared(runtime->owner->shared) {
        if (shared)
            AcquireSRWLockExclusive(shared->lock.native());
    }
    ~RateLock() noexcept {
        if (shared)
            ReleaseSRWLockExclusive(shared->lock.native());
    }
};

RestRateScheduler* rate_scheduler(RestClientRuntime* runtime) noexcept {
    return runtime->scheduler;
}

// Called with the shared lock held. IOCP signals coalesce; idle workers stay
// asleep between real completions rather than polling each other's state.
void rate_changed(RestClientRuntime* runtime) noexcept {
    auto* shared = runtime->owner->shared;
    if (!shared)
        return;
    if (shared->mutation_owner == runtime->owner)
        shared->mutation_owner = nullptr;
    for (auto* peer : shared->clients) {
        if (peer && peer != runtime->owner && peer->runtime) {
            auto* other = static_cast<RestClientRuntime*>(peer->runtime);
            network_signal_post(&peer->worker.loop, &other->rate_signal, kRateSignalValue);
        }
    }
}

void cancel_rate_lease(RestClientRuntime* runtime) noexcept {
    RateLock guard(runtime);
    bool changed = endpoint_gate_release(runtime->endpoint_gate, &runtime->endpoint_lease,
                                         clock_monotonic_milliseconds());
    if (runtime->transaction.lease.active) {
        rest_rate_cancel(rate_scheduler(runtime), &runtime->transaction.lease);
        changed = true;
    }
    if (changed)
        rate_changed(runtime);
}

bool authentication_rejected(const RestClient* client) noexcept {
    return read_acquire(&client->authentication_rejected) !=
               0 ||
           (client->shared &&
            read_acquire(&client->shared->authentication_rejected) != 0);
}

AccountRequirement required_action(RestClient* client) noexcept {
    return static_cast<AccountRequirement>(InterlockedCompareExchange(
        client->shared ? &client->shared->required_action : &client->required_action, 0, 0));
}

constexpr u32 ordinary_fixed_storage_bytes(bool normal_user) noexcept {
    return kRestClientMaximumTargetBytes +
           kEncryptedInputBytes + kHandshakeOutputBytes + kRecordOutputBytes +
           kRestMaximumHeaderBytes +
           (normal_user ? kRestUserSuperPropertiesMaximumBytes + kRestUserLocaleMaximumBytes +
                              kRestUserTimezoneMaximumBytes
                        : 0);
}

constexpr u32 user_auth_secure_fixed_storage_bytes(u32 request_capacity) noexcept {
    return request_capacity + kUserAuthEncryptedInputBytes +
           kUserAuthRecordOutputBytes + kRestMaximumHeaderBytes;
}

inline u32 page_rounded_bytes(u64 raw) noexcept {
    const u64 rounded = (raw + 4095ull) & ~4095ull;
    return rounded <= ~0u ? static_cast<u32>(rounded) : 0;
}

u32 user_auth_secure_bytes(u32 response_capacity, u32 request_capacity) noexcept {
    if (response_capacity < kRestClientMinimumResponseBytes ||
        response_capacity > kUserAuthMaximumResponseBytes ||
        (request_capacity != kUserAuthMaximumRequestBodyBytes &&
         request_capacity != kUserAuthResetRequestBodyBytes)) {
        return 0;
    }
    return page_rounded_bytes(user_auth_secure_fixed_storage_bytes(request_capacity));
}

u32 allocation_bytes(u32 response_capacity, bool user_auth_mode, bool normal_user, bool shared = false) noexcept {
    if (response_capacity < kRestClientMinimumResponseBytes ||
        response_capacity > (user_auth_mode ? kUserAuthMaximumResponseBytes
                                            : kRestClientMaximumResponseBytes)) {
        return 0;
    }
    const u64 raw =
        static_cast<u64>(sizeof(RestClientRuntime)) + (shared ? 0 : sizeof(RestLocalRates)) +
        (user_auth_mode ? kHandshakeOutputBytes + kUserClientMetadataBytes
                        : static_cast<u64>(ordinary_fixed_storage_bytes(normal_user)) +
                              maximum(kRestClientMaximumRequestBodyBytes, response_capacity));
    return page_rounded_bytes(raw);
}

void assign_storage(RestClientRuntime* runtime, RestSharedState* shared = nullptr) noexcept {
    u8* cursor = reinterpret_cast<u8*>(runtime) + sizeof(RestClientRuntime);
    if (shared) {
        runtime->scheduler = &shared->scheduler;
        runtime->endpoint_gate = &shared->endpoint_gate;
    } else {
        auto* local = reinterpret_cast<RestLocalRates*>(cursor);
        cursor += sizeof(RestLocalRates);
        runtime->scheduler = &local->scheduler;
        runtime->endpoint_gate = &local->endpoint_gate;
        rest_rate_scheduler_initialize(runtime->scheduler, {local->buckets, kRateBucketCount},
                                       {local->bindings, kRateBindingCount});
    }
    if (runtime->user_auth_mode) {
        runtime->user_super_properties = cursor;
        cursor += kRestUserSuperPropertiesMaximumBytes;
        runtime->user_locale = reinterpret_cast<char*>(cursor);
        cursor += kRestUserLocaleMaximumBytes;
        runtime->user_timezone = reinterpret_cast<char*>(cursor);
        // Metadata uses the ordinary allocation, before its final aligned TLS
        // output block. Passwords, CAPTCHA values, and request heads stay locked.
        runtime->handshake_output = reinterpret_cast<u8*>(runtime) + runtime->allocation_bytes -
                                    kHandshakeOutputBytes;
        cursor = runtime->secret_storage.data;
        runtime->request_body = cursor;
        cursor += runtime->request_body_capacity;
        runtime->response_body = runtime->request_body;
        runtime->encrypted_storage = cursor;
        cursor += kUserAuthEncryptedInputBytes;
        runtime->record_output = cursor;
        cursor += kUserAuthRecordOutputBytes;
        runtime->request_head = cursor;
        runtime->response_input = cursor;
        runtime->request_head_capacity = kUserAuthRequestHeadBytes;
        return;
    }
    runtime->request_target = cursor;
    cursor += kRestClientMaximumTargetBytes;
    runtime->encrypted_storage = cursor;
    cursor += kEncryptedInputBytes;
    // Put the page-sized handshake/inflate block at the end of the already
    // page-rounded allocation. Its output then touches no extra boundary page;
    // the existing tail padding sits before it instead of after the small fields.
    runtime->handshake_output = reinterpret_cast<u8*>(runtime) + runtime->allocation_bytes - kHandshakeOutputBytes;
    runtime->record_output = cursor;
    cursor += kRecordOutputBytes;
    runtime->request_head_capacity = runtime->authorization == RestAuthorization::normal_user
                                         ? kNormalUserRequestHeadBytes
                                         : kRequestHeadBytes;
    // Response reads begin after the outgoing plaintext has been committed
    // and erased, so the two header phases share the larger response buffer.
    // Keep outgoing headers at its tail so typical incoming headers use other
    // cache lines immediately after the outgoing-header erase.
    runtime->request_head = cursor + kRestMaximumHeaderBytes - runtime->request_head_capacity;
    runtime->response_input = cursor;
    cursor += kRestMaximumHeaderBytes;
    // Keep response headers and body adjacent. Reads start only after all
    // outgoing plaintext is committed and erased, so both phases share a body.
    runtime->request_body = cursor;
    runtime->response_body = cursor;
    cursor += maximum(kRestClientMaximumRequestBodyBytes, runtime->response_capacity);
    if (runtime->authorization == RestAuthorization::normal_user) {
        runtime->user_super_properties = cursor;
        cursor += kRestUserSuperPropertiesMaximumBytes;
        runtime->user_locale = reinterpret_cast<char*>(cursor);
        cursor += kRestUserLocaleMaximumBytes;
        runtime->user_timezone = reinterpret_cast<char*>(cursor);
    }
}

RestClientPhase load_phase(const RestClient* client) noexcept {
    return static_cast<RestClientPhase>(
        read_acquire(&client->phase));
}

RestClientError load_error(const RestClient* client) noexcept {
    return static_cast<RestClientError>(
        read_acquire(&client->error));
}

void publish_phase(RestClient* client, RestClientPhase phase) noexcept {
    InterlockedExchange(&client->phase, static_cast<LONG>(phase));
}

const char* phase_name(RestClientPhase phase) noexcept {
    switch (phase) {
    case RestClientPhase::queued: return "queued";
    case RestClientPhase::rate_wait: return "rate_wait";
    case RestClientPhase::resolving: return "dns";
    case RestClientPhase::connecting: return "connect";
    case RestClientPhase::tls_handshake: return "tls";
    case RestClientPhase::sending: return "send";
    case RestClientPhase::receiving: return "receive";
    case RestClientPhase::response_ready: return "ready";
    case RestClientPhase::draining: return "drain";
    case RestClientPhase::idle: return "idle";
    default: return "other";
    }
}
void trace_identity(TraceRecord& line, const RestClientRuntime* runtime, u64 request) noexcept {
    line.field("worker", runtime->trace_worker_id);
    line.field("lane", runtime->trace_lane == 1 ? "primary" : runtime->trace_lane == 2 ? "details" : "standalone");
    line.field("request", request);
}
// Worker-only timestamps. Producer queued/idle updates must not mutate
// timing state while the previous completion is still being committed.
void publish_worker_phase(RestClient* client, RestClientRuntime* runtime,
                          RestClientPhase phase) noexcept {
    publish_phase(client, phase);
    if (!g_trace || !runtime->trace_request_id || runtime->trace_phase == phase) return;
    const i64 now = clock_now();
    TraceRecord line("phase");
    trace_identity(line, runtime, runtime->trace_request_id);
    line.field("phase", phase_name(phase));
    line.field("previous", phase_name(runtime->trace_phase));
    line.field("previous_us", clock_microseconds(runtime->clock, runtime->phase_counter, now));
    line.field("elapsed_us", clock_microseconds(runtime->clock, runtime->request_started_counter, now));
    runtime->phase_counter = now;
    runtime->trace_phase = phase;
    g_trace(line.text);
}
void trace_busy(RestClient* client, RestClientRuntime* runtime,
                const RestClientRequest& request) noexcept {
    if (!g_trace || ++runtime->busy_count != 1) return;
    runtime->busy_since_ms = clock_monotonic_milliseconds();
    TraceRecord line("submit_busy");
    trace_identity(line, runtime, request.request_id);
    line.field("route", request.route.route_id);
    line.field("blocking_request", static_cast<u64>(InterlockedCompareExchange64(&client->current_request_id, 0, 0)));
    line.field("blocking_phase", phase_name(load_phase(client)));
    if (client->shared) {
        for (auto* peer : client->shared->clients) if (peer && peer != client) {
            line.field("peer_request", static_cast<u64>(InterlockedCompareExchange64(&peer->current_request_id, 0, 0)));
            line.field("peer_phase", phase_name(load_phase(peer)));
        }
    }
    g_trace(line.text);
}

RestClientResult invalid_result() noexcept {
    RestClientResult value{};
    value.error = RestClientError::invalid_input;
    return value;
}

RestClientRuntime* runtime_of(RestClient* client) noexcept {
    return client && client->runtime ? static_cast<RestClientRuntime*>(client->runtime) : nullptr;
}

bool worker_ending(const RestClient* client) noexcept {
    const RestClientPhase phase = load_phase(client);
    return phase == RestClientPhase::stopping || phase == RestClientPhase::stopped ||
           phase == RestClientPhase::failed;
}

RestClientResult client_result(const RestClient* client) noexcept {
    if (!client)
        return invalid_result();
    RestClientResult value{};
    value.authentication_rejected = authentication_rejected(client);
    value.phase = load_phase(client);
    value.error = load_error(client);
    value.native_error = static_cast<u32>(
        read_acquire(&client->native_error));
    value.connections = static_cast<u64>(
        read_acquire(&client->connections));
    value.requests = static_cast<u64>(
        read_acquire(&client->requests));
    value.reused_requests = static_cast<u64>(
        read_acquire(&client->reused_requests));
    value.responses = static_cast<u64>(
        read_acquire(&client->responses));
    return value;
}

void publish_client_failure(RestClient* client, RestClientError error,
                            u32 native_error) noexcept {
    if (InterlockedCompareExchange(&client->error, static_cast<LONG>(error),
                                   static_cast<LONG>(RestClientError::none)) ==
        static_cast<LONG>(RestClientError::none)) {
        InterlockedExchange(&client->native_error, static_cast<LONG>(native_error));
    }
    publish_phase(client, RestClientPhase::failed);
    SetEvent(client->response_event);
}

#include "../protocol/rest_metadata.inc"

bool valid_user_configuration(const RestUserConfiguration& configuration) noexcept {
    return valid_base64_metadata(configuration.super_properties) &&
           ascii_graphic_string(configuration.locale, kRestUserLocaleMaximumBytes, true) &&
           ascii_graphic_string(configuration.timezone, kRestUserTimezoneMaximumBytes);
}

bool copy_user_configuration(RestClientRuntime* runtime,
                             const RestUserConfiguration& configuration) noexcept {
    if (!runtime->user_super_properties || !runtime->user_locale || !runtime->user_timezone ||
        !valid_user_configuration(configuration) ||
        !copy_bytes(runtime->user_super_properties, kRestUserSuperPropertiesMaximumBytes,
                    configuration.super_properties.data, configuration.super_properties.count) ||
        !copy_bytes(runtime->user_locale, kRestUserLocaleMaximumBytes, configuration.locale.data,
                    configuration.locale.bytes) ||
        !copy_bytes(runtime->user_timezone, kRestUserTimezoneMaximumBytes,
                    configuration.timezone.data, configuration.timezone.bytes)) {
        return false;
    }
    runtime->user_super_properties_bytes = configuration.super_properties.count;
    runtime->user_locale_bytes = configuration.locale.bytes;
    runtime->user_timezone_bytes = configuration.timezone.bytes;
    return true;
}

bool initialize_auth_user_configuration(RestClientRuntime* runtime) noexcept {
    UserClientConfigurationStorage storage{};
    if (!user_client_configuration_initialize(&storage).ok())
        return false;
    u8 scratch[kRestUserSuperPropertiesMaximumBytes];
    const auto properties = write_rest_user_super_properties(
        storage.configuration.properties, {scratch, sizeof(scratch)},
        {runtime->user_super_properties, kRestUserSuperPropertiesMaximumBytes});
    if (!properties.ok())
        return false;
    return copy_user_configuration(runtime,
        {{runtime->user_super_properties, properties.bytes}, storage.configuration.properties.system_locale,
         {storage.timezone, storage.timezone_bytes}});
}

bool valid_route(const RestRouteKey& route) noexcept {
    if (route.route_id == 0) {
        return false;
    }
    switch (route.method) {
    case RestMethod::get:
    case RestMethod::post:
    case RestMethod::put:
    case RestMethod::patch:
    case RestMethod::delete_method:
    case RestMethod::head:
        break;
    default:
        return false;
    }
    switch (route.major) {
    case RestMajorParameter::none:
        return route.major_key == 0;
    case RestMajorParameter::channel:
    case RestMajorParameter::guild:
    case RestMajorParameter::webhook:
        return route.major_key != 0;
    default:
        return false;
    }
}

bool valid_target(ConstSlice<u8> target) noexcept {
    if (!target.data || target.count == 0 || target[0] != '/' || (target.count > 1 && target[1] == '/')) {
        return false;
    }
    for (u32 index = 0; index < target.count; ++index) {
        if (target[index] < 0x21 || target[index] > 0x7E || target[index] == '#') {
            return false;
        }
    }
    return true;
}

bool valid_locked_value(const SecureBuffer* value, u32 maximum_bytes, bool required) noexcept {
    if (!value) {
        return !required;
    }
    return value->data && value->locked && value->capacity >= value->bytes && value->bytes != 0 &&
           value->bytes <= maximum_bytes;
}

Utf8View locked_utf8(const SecureBuffer* value) noexcept {
    return value ? Utf8View{reinterpret_cast<const char*>(value->data), value->bytes} : Utf8View{};
}

// Rate-limit identity per operation; the two reset operations share one.
RestRouteKey user_auth_route(UserAuthOperation operation) noexcept {
    u64 route_id = 0;
    switch (operation) {
    case UserAuthOperation::password: route_id = 0x415554480001ull; break;
    case UserAuthOperation::mfa_totp: route_id = 0x415554480002ull; break;
    case UserAuthOperation::mfa_sms: route_id = 0x415554480003ull; break;
    case UserAuthOperation::mfa_backup: route_id = 0x415554480004ull; break;
    case UserAuthOperation::mfa_sms_send: route_id = 0x415554480005ull; break;
    case UserAuthOperation::mfa_webauthn: route_id = 0x415554480006ull; break;
    case UserAuthOperation::remote_auth_ticket: route_id = 0x415554480007ull; break;
    case UserAuthOperation::verification: route_id = 0x415554480008ull; break;
    case UserAuthOperation::recovery: route_id = 0x415554480009ull; break;
    case UserAuthOperation::reset_password:
    case UserAuthOperation::reset_mfa: route_id = 0x41555448000Aull; break;
    case UserAuthOperation::phone_resend: route_id = 0x41555448000Bull; break;
    case UserAuthOperation::phone_verify: route_id = 0x41555448000Cull; break;
    case UserAuthOperation::browser_handoff_exchange: route_id = 0x41555448000Dull; break;
    default: break;
    }
    return RestRouteKey{route_id, 0, RestMethod::post, RestMajorParameter::none};
}

void erase_command(RestClientRuntime* runtime, bool erase_body = true) noexcept {
    if (runtime->command.target_bytes != 0) {
        secure_clear_bytes(runtime->request_target, runtime->command.target_bytes);
    }
    if (erase_body && runtime->command.body_bytes != 0) {
        secure_clear_bytes(runtime->request_body, runtime->command.body_bytes);
    }
    if (runtime->command.head_bytes != 0) {
        secure_clear_bytes(runtime->request_head, runtime->command.head_bytes);
    }
    if (runtime->command.audit_reason_bytes != 0)
        secure_clear_bytes(runtime->audit_reason, runtime->command.audit_reason_bytes);
    if (runtime->command.extra_headers_bytes != 0)
        secure_clear_bytes(runtime->extra_headers, runtime->command.extra_headers_bytes);
    if (runtime->command.multipart_boundary_bytes != 0)
        secure_clear_bytes(runtime->multipart_boundary, runtime->command.multipart_boundary_bytes);
    reset_value<RestClientCommand>(&runtime->command);
}

void erase_response_storage(RestClientRuntime* runtime) noexcept {
    secure_clear_bytes(runtime->response_input, kRestMaximumHeaderBytes);
    if (runtime->response.http.body_bytes != 0) {
        secure_clear_bytes(runtime->response_body, runtime->response.http.body_bytes);
    }
    reset_value<RestResponse>(&runtime->response);
}

void release_decompression(RestClientRuntime* runtime) noexcept {
    if (runtime->decompression) {
        secure_clear_bytes(runtime->decompression, runtime->decompression_capacity);
        if (runtime->decompression != runtime->handshake_output)
            VirtualFree(runtime->decompression, 0, MEM_RELEASE);
    }
    runtime->decompression = nullptr;
    runtime->decompression_capacity = 0;
}

// Caller owns writing/ready state or has joined the worker. Never release a
// bank while TLS still borrows an outgoing span or while a response is ready.
void release_extended_body(RestClientRuntime* runtime) noexcept {
    if (!runtime->extended_body) return;
    secure_clear_bytes(runtime->extended_body, runtime->extended_body_bytes);
    VirtualFree(runtime->extended_body, 0, MEM_RELEASE);
    runtime->extended_body = nullptr;
    runtime->extended_body_bytes = 0;
    runtime->request_body = runtime->response_body = runtime->default_body;
    runtime->request_body_capacity = kRestClientMaximumRequestBodyBytes;
    runtime->response_capacity = runtime->default_response_capacity;
    if (runtime->decompression_capacity > maximum(kHandshakeOutputBytes, runtime->response_capacity))
        release_decompression(runtime);
}

void release_completed_response(RestClient* client, RestClientRuntime* runtime,
                                u32 body_bytes) noexcept {
    if (g_trace) {
        TraceRecord line("consumed");
        trace_identity(line, runtime, runtime->completed.request_id);
        line.field("ready_hold_us", clock_microseconds(runtime->clock, runtime->ready_counter, clock_now()));
        g_trace(line.text);
    }
    secure_clear_bytes(runtime->response_input, kRestMaximumHeaderBytes);
    if (body_bytes != 0) {
        secure_clear_bytes(runtime->response_body, body_bytes);
    }
    reset_value<RestResponse>(&runtime->response);
    reset_value<RestClientResponse>(&runtime->completed);
    const auto logout = InterlockedCompareExchange64(&runtime->logout_request_id, 0, 0);
    if (logout && InterlockedCompareExchange64(&runtime->logout_reservation_id, 0, 0) == logout) {
        InterlockedExchange64(&runtime->logout_reservation_id, 0);
        runtime->logout_preempt_id = 0;
        runtime->logout_cancelled = false;
    }
    InterlockedExchange64(&runtime->logout_request_id, 0);
    reset_value<RestTransaction>(&runtime->transaction);
    release_extended_body(runtime);
    ResetEvent(client->response_event);
    InterlockedExchange64(&client->current_request_id, 0);
    publish_phase(client, RestClientPhase::idle);
    InterlockedExchange(&client->request_state, request_state_empty);
}

void close_transport(RestClientRuntime* runtime) noexcept {
    tls_stream_close(&runtime->transport);
    runtime->transport_ready = false;
    runtime->connection_request_count = 0;
}

bool transaction_in_progress(const RestTransaction& transaction) noexcept {
    return transaction.phase == RestTransactionPhase::acquired ||
           transaction.phase == RestTransactionPhase::sending ||
           transaction.phase == RestTransactionPhase::receiving;
}

bool end_transaction(RestTransaction* transaction, RestTransactionPhase phase) noexcept {
    if (!transaction || !transaction_in_progress(*transaction))
        return false;
    transaction->phase = phase;
    return true;
}

u32 next_wait(const RestClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->stopping || runtime->draining) {
        return INFINITE;
    }
    u64 deadline = runtime->rate_wait_until_ms;
    if (transaction_in_progress(runtime->transaction))
        deadline = earliest_deadline(deadline, runtime->transaction.deadline_ms);
    return deadline == 0 ? INFINITE : wait_milliseconds_until(now_ms, deadline);
}

void publish_completed(RestClient* client, RestClientRuntime* runtime,
                       RestClientResponse response) noexcept {
    bool cancelled = false, active_cancel = false;
    if (!network_client_begin_publish(client, &cancelled, &active_cancel)) {
        return;
    }
    if (runtime->transaction.lease.active || runtime->endpoint_lease.generation)
        cancel_rate_lease(runtime);
    if (cancelled) {
        rest_transaction_cancel(&runtime->transaction);
        erase_response_storage(runtime);
        reset_value<RestClientResponse>(&response);
        response.error = RestClientError::cancelled;
        response.native_error = ERROR_OPERATION_ABORTED;
        response.reused_connection = runtime->transaction.reused_connection;
        if (active_cancel) {
            close_transport(runtime);
        }
    }
    if (runtime->request_started_counter != 0) {
        response.elapsed_us =
            clock_microseconds(runtime->clock, runtime->request_started_counter, clock_now());
    }
    response.request_id = runtime->command.request_id;
    if (runtime->authorization == RestAuthorization::normal_user) {
        response.diagnostics = {runtime->command.route.route_id,
                                static_cast<u64>(InterlockedCompareExchange64(&client->requests, 0, 0)),
                                static_cast<u64>(InterlockedCompareExchange64(&client->responses, 0, 0)) +
                                    1,
                                runtime->command.route.method};
    }
    if (g_trace) {
        TraceRecord line("complete");
        trace_identity(line, runtime, response.request_id);
        line.field("status", response.status_code);
        line.field("error", static_cast<u64>(response.error));
        line.field("native_error", response.native_error);
        line.field("elapsed_us", response.elapsed_us);
        line.field("transfer_decode_us", runtime->first_byte_counter ? clock_microseconds(runtime->clock, runtime->first_byte_counter, clock_now()) : 0);
        line.field("wire_bytes", response.wire_body_bytes);
        line.field("body_bytes", response.body_bytes);
        line.field("encoding", static_cast<u64>(response.wire_content_encoding));
        line.field("decode_us", response.decode_us);
        line.field("reused", static_cast<u64>(response.reused_connection));
        g_trace(line.text);
    }
    runtime->completed = response;
    runtime->rate_wait_until_ms = 0;
    secure_clear_bytes(runtime->request_head, runtime->request_head_capacity);
    // The outgoing request storage is reused as the incoming response storage.
    // Every committed request span was already erased by
    // request_plaintext_committed(). Once a transport-clean
    // response exists, erasing the old command body here would erase the
    // response prefix before the UI thread can decode it. Cancellation and
    // transport failures still erase the body because an unsent suffix may
    // remain secret.
    const bool response_owns_shared_body = response.error == RestClientError::none &&
                                           response.body_bytes != 0;
    erase_command(runtime, !response_owns_shared_body);
    InterlockedIncrement64(&client->responses);
    publish_worker_phase(client, runtime, RestClientPhase::response_ready);
    runtime->trace_request_id = 0;
    runtime->ready_counter = clock_now();
    InterlockedExchange(&client->request_state, request_state_ready);
    SetEvent(client->response_event);
}

void publish_request_error(RestClient* client, RestClientRuntime* runtime,
                           RestClientError error, u32 native_error) noexcept {
    erase_response_storage(runtime);
    RestClientResponse response{};
    response.error = error;
    response.native_error = native_error;
    response.reused_connection = runtime->transaction.reused_connection;
    publish_completed(client, runtime, response);
}

bool drained(const RestClientRuntime* runtime) noexcept {
    return tls_stream_drained(const_cast<TlsStream*>(&runtime->transport));
}

void begin_drain(RestClient* client, RestClientRuntime* runtime, RestClientError error,
                 u32 native_error, bool cancelled) noexcept {
    if (runtime->draining || runtime->stopping) {
        return;
    }
    cancel_rate_lease(runtime);
    if (cancelled) {
        rest_transaction_cancel(&runtime->transaction);
    } else {
        rest_transaction_fail(&runtime->transaction);
    }
    runtime->pending_error = error;
    runtime->pending_native_error = native_error;
    runtime->draining = true;
    publish_worker_phase(client, runtime, RestClientPhase::draining);
    close_transport(runtime);
}

NetworkWorkerStep finish_drain(RestClient* client, RestClientRuntime* runtime) noexcept {
    if (!drained(runtime)) {
        return NetworkWorkerStep{INFINITE, false};
    }
    if (runtime->stopping) {
        return NetworkWorkerStep{INFINITE, true};
    }
    const RestClientError error = runtime->pending_error;
    const u32 native_error = runtime->pending_native_error;
    runtime->pending_error = RestClientError::none;
    runtime->pending_native_error = 0;
    runtime->draining = false;
    publish_request_error(client, runtime, error, native_error);
    return NetworkWorkerStep{INFINITE, false};
}

void request_plaintext_committed(void* opaque, ConstSlice<u8> committed) noexcept {
    auto* runtime = static_cast<RestClientRuntime*>(opaque);
    if (!rest_transaction_commit_plaintext(&runtime->transaction, committed.count)) {
        runtime->write_commit_failed = true;
    }
    if (committed.data && committed.count != 0) {
        secure_clear_bytes(const_cast<u8*>(committed.data), committed.count);
    }
}

TlsStreamReadAction response_plaintext_received(void* opaque, ConstSlice<u8> plaintext,
                                                bool peer_closed) noexcept {
    auto* runtime = static_cast<RestClientRuntime*>(opaque);
    if (g_trace && plaintext.count && !runtime->first_byte_counter) {
        runtime->first_byte_counter = clock_now();
        TraceRecord line("first_byte");
        trace_identity(line, runtime, runtime->trace_request_id);
        line.field("ttfb_us", clock_microseconds(runtime->clock, runtime->receive_counter,
                                                  runtime->first_byte_counter));
        g_trace(line.text);
    }
    runtime->response_step = rest_response_feed(&runtime->response, plaintext, peer_closed);
    if (!runtime->response_step.ok() ||
        (runtime->response_step.state == RestResponseState::complete &&
         runtime->response_step.extra_bytes != 0)) {
        runtime->response_callback_failed = true;
        return TlsStreamReadAction::reject;
    }
    if (runtime->response_step.state == RestResponseState::complete) {
        return TlsStreamReadAction::complete;
    }
    if (peer_closed) {
        runtime->response_callback_failed = true;
        return TlsStreamReadAction::reject;
    }
    return TlsStreamReadAction::continue_read;
}

TlsStreamEvent begin_resolution(RestClient* client, RestClientRuntime* runtime,
                                NetworkLoop* loop) noexcept {
    publish_worker_phase(client, runtime, RestClientPhase::resolving);
    return tls_stream_begin_connect(&runtime->transport, loop);
}

bool reject_restricted_request(RestClient* client, RestClientRuntime* runtime) noexcept {
    const auto required = required_action(client);
    const RestClientRequest request{{runtime->request_target, runtime->command.target_bytes},
                                    {runtime->request_body, runtime->command.body_bytes},
                                    runtime->command.route,
                                    runtime->command.request_id,
                                    runtime->command.timeout_ms,
                                    runtime->command.json_body,
                                    runtime->command.identity_response,
                                    {runtime->audit_reason, runtime->command.audit_reason_bytes},
                                    {runtime->extra_headers, runtime->command.extra_headers_bytes}};
    const bool logout = runtime->authorization == RestAuthorization::normal_user &&
                        account_logout_request_valid(request);
    if (runtime->authorization != RestAuthorization::normal_user || logout ||
        (!authentication_rejected(client) &&
         account_request_allowed(required, runtime->command.route.method)))
        return false;
    cancel_rate_lease(runtime);
    rest_transaction_fail(&runtime->transaction);
    publish_request_error(client, runtime, RestClientError::account_restricted, 0);
    return true;
}

TlsStreamEvent begin_request_send(RestClient* client, RestClientRuntime* runtime) noexcept {
    // Recheck after DNS/TLS or a rate wait: a request can be accepted before a Gateway restriction applies.
    if (reject_restricted_request(client, runtime))
        return {0, TlsStreamEventKind::drained};
    bool invalid_endpoint = false;
    bool deferred = false;
    {
        RateLock guard(runtime);
        const u64 now_ms = clock_monotonic_milliseconds();
        const auto endpoint =
            endpoint_gate_recheck(runtime->endpoint_gate, runtime->endpoint_lease, now_ms);
        const u64 reset = maximum(rate_scheduler(runtime)->global_reset_at_ms, endpoint.wait_until_ms);
        invalid_endpoint = endpoint.kind == EndpointGateKind::error;
        if (invalid_endpoint || reset > now_ms) {
            rest_rate_cancel(rate_scheduler(runtime), &runtime->transaction.lease);
            endpoint_gate_release(runtime->endpoint_gate, &runtime->endpoint_lease, now_ms,
                                  invalid_endpoint);
            rate_changed(runtime);
            reset_value(&runtime->transaction);
            runtime->rate_wait_until_ms = reset;
            runtime->waiting_for_peer = false;
            deferred = true;
        }
    }
    if (deferred) {
        if (invalid_endpoint)
            publish_request_error(client, runtime, RestClientError::request, 0);
        else
            publish_worker_phase(client, runtime, RestClientPhase::rate_wait);
        return {0, TlsStreamEventKind::drained};
    }
    u32 head_bytes = runtime->command.head_bytes;
    if (!runtime->user_auth_mode) {
        const ConstSlice<u8> token = runtime->authorization == RestAuthorization::none
                                         ? ConstSlice<u8>{nullptr, 0}
                                         : secure_buffer_view(runtime->credential);
        const RestUserRequestHeaders user_headers{
            ConstSlice<u8>{runtime->user_super_properties, runtime->user_super_properties_bytes},
            Utf8View{runtime->user_locale, runtime->user_locale_bytes},
            Utf8View{runtime->user_timezone, runtime->user_timezone_bytes}};
        const bool normal_user = runtime->authorization == RestAuthorization::normal_user;
        const RestRequestWriteResult written = write_rest_request_head(
            RestRequestHead{ConstSlice<u8>{runtime->request_target, runtime->command.target_bytes},
                            token, runtime->command.body_bytes, runtime->command.route.method,
                            runtime->authorization, runtime->command.json_body, false,
                            normal_user ? &user_headers : nullptr,
                            !runtime->command.identity_response,
                            {runtime->audit_reason, runtime->command.audit_reason_bytes},
                            {runtime->extra_headers, runtime->command.extra_headers_bytes},
                            {runtime->multipart_boundary, runtime->command.multipart_boundary_bytes},
                            runtime->command.send_empty_audit_reason},
            Slice<u8>{runtime->request_head, runtime->request_head_capacity});
        if (!written.ok()) {
            secure_clear_bytes(runtime->request_head, runtime->request_head_capacity);
            return TlsStreamEvent{0, TlsStreamEventKind::failed, TlsStreamError::invalid_input};
        }
        head_bytes = written.bytes;
    }
    if (head_bytes == 0 || head_bytes > runtime->request_head_capacity ||
        !rest_transaction_begin_send(&runtime->transaction,
                                     head_bytes + runtime->command.body_bytes)) {
        secure_clear_bytes(runtime->request_head, runtime->request_head_capacity);
        return TlsStreamEvent{0, TlsStreamEventKind::failed, TlsStreamError::invalid_input};
    }
    runtime->write_commit_failed = false;
    runtime->response_callback_failed = false;
    reset_value<RestResponseStep>(&runtime->response_step);
    InterlockedIncrement64(&client->requests);
    if (runtime->transaction.reused_connection) {
        InterlockedIncrement64(&client->reused_requests);
    }
    publish_worker_phase(client, runtime, RestClientPhase::sending);
    ConstSlice<u8> spans[2]{ConstSlice<u8>{runtime->request_head, head_bytes}, {}};
    u32 span_count = 1;
    if (runtime->command.body_bytes != 0) {
        spans[1] = ConstSlice<u8>{runtime->request_body, runtime->command.body_bytes};
        span_count = 2;
    }
    return tls_stream_start_write(&runtime->transport, ConstSlice<ConstSlice<u8>>{spans, span_count},
                                  request_plaintext_committed, runtime);
}

void prepare_response_metadata(RestClientRuntime* runtime,
                               const RestRateObservation& observation,
                               RestClientResponse* completed) noexcept {
    completed->rate = runtime->response.parsed_head.rate;
    completed->retry_at_ms = observation.retry_at_ms;
    completed->body_bytes = runtime->response.http.body_bytes;
    completed->status_code = runtime->response.parsed_head.head.status_code;
    completed->content_type = runtime->response.parsed_head.representation.content_type;
    completed->content_encoding = runtime->response.parsed_head.representation.content_encoding;
    completed->body_kind = rest_classify_body(
        ConstSlice<u8>{runtime->response_body, runtime->response.http.body_bytes});
    completed->rate_limited = observation.rate_limited;
    completed->global_rate_limit = observation.global;
    completed->reused_connection = runtime->transaction.reused_connection;
    completed->wire_body_bytes = runtime->wire_body_bytes;
    completed->decode_us = runtime->decode_us;
    completed->wire_content_encoding = runtime->wire_encoding;
}

bool finalize_response(RestClient* client, RestClientRuntime* runtime,
                       const RestResponseStep& step, u64 now_ms) noexcept {
    if (!step.ok() || step.state != RestResponseState::complete || step.extra_bytes != 0) {
        return false;
    }
    const auto& head = runtime->response.parsed_head;
    if (head.head.status_code == 503 && (head.rate.fields & rate_limit_field_retry_after) &&
        head.rate.retry_after_ms) {
        // A trusted delay remains authoritative even when decoding its body
        // fails. Terminal cleanup releases the reservation and wakes peers.
        RateLock guard(runtime);
        if (!endpoint_gate_observe(runtime->endpoint_gate, runtime->endpoint_lease,
                                   deadline_after(now_ms, head.rate.retry_after_ms)))
            return false;
    }
    struct SessionRejection {
        RestClient* client;
        bool pending;
        void commit() noexcept {
            if (!pending)
                return;
            InterlockedExchange(&client->authentication_rejected, 1);
            if (client->shared)
                InterlockedExchange(&client->shared->authentication_rejected, 1);
            pending = false;
        }
        ~SessionRejection() noexcept {
            commit();
        }
    } session_rejection{client, runtime->authorization == RestAuthorization::normal_user &&
                                    runtime->response.parsed_head.head.status_code == 401};
    runtime->wire_body_bytes = runtime->response.http.body_bytes;
    runtime->wire_encoding = runtime->response.parsed_head.representation.content_encoding;
    runtime->decode_us = 0;
    if (!runtime->user_auth_mode && runtime->wire_body_bytes != 0 &&
        runtime->command.route.method != RestMethod::head &&
        runtime->wire_encoding != RestContentEncoding::missing &&
        runtime->wire_encoding != RestContentEncoding::identity) {
        if (runtime->wire_encoding != RestContentEncoding::gzip)
            return false;
        // A complete response follows the final TLS handshake send. Reuse its
        // idle output as inflate scratch; copy/erase finishes on this worker
        // before close/reconnect can clear or reuse the handshake buffer.
        // Larger response capacities retain a separately owned scratch buffer.
        if (runtime->decompression && runtime->decompression_capacity < runtime->response_capacity)
            release_decompression(runtime);
        if (!runtime->decompression) {
            u8* scratch = runtime->handshake_output;
            if (runtime->response_capacity > kHandshakeOutputBytes) {
                scratch = static_cast<u8*>(VirtualAlloc(nullptr, runtime->response_capacity,
                                                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (!scratch)
                    return false;
            }
            runtime->decompression = scratch;
            runtime->decompression_capacity = scratch == runtime->handshake_output
                                                  ? kHandshakeOutputBytes : runtime->response_capacity;
        }
        const i64 begin = clock_now();
        const auto inflated =
            gzip_inflate({runtime->response_body, runtime->wire_body_bytes},
                         {runtime->decompression, runtime->response_capacity}, &runtime->inflate_workspace);
        if (!inflated.ok()) {
            secure_clear_bytes(runtime->decompression, runtime->response_capacity);
            return false;
        }
        copy_bytes(runtime->response_body, runtime->decompression, inflated.output_bytes);
        secure_clear_bytes(runtime->decompression, inflated.output_bytes);
        runtime->response.http.body_bytes = inflated.output_bytes;
        runtime->response.parsed_head.representation.content_encoding =
            RestContentEncoding::identity;
        runtime->decode_us = static_cast<u32>(clock_microseconds(runtime->clock, begin, clock_now()));
    }
    ApiRejection rejection{};
    if (!runtime->user_auth_mode && runtime->response.parsed_head.head.status_code >= 400)
        api_rejection_decode(
            {reinterpret_cast<const char*>(runtime->response_body), runtime->response.http.body_bytes},
            {runtime->rate_structural_scratch, kRateStructuralCount}, &rejection);
    if (session_rejection.pending && rejection.has_code && rejection.code == 60003 &&
        api_rejection_is_mfa_challenge(
            {reinterpret_cast<const char*>(runtime->response_body), runtime->response.http.body_bytes},
            {runtime->rate_structural_scratch, kRateStructuralCount}))
        session_rejection.pending = false;
    // Commit before publishing ordinary 401s. Early decompression/allocation
    // failures also commit through the guard; an existing peer rejection is
    // never cleared by a later valid elevation challenge.
    session_rejection.commit();
    const bool action_required = api_rejection_requires_action(rejection);
    const bool auth_limited =
        runtime->user_auth_mode && runtime->response.parsed_head.head.status_code == 429;
    RateLimitBodyResult rate_body{};
    const RateLimitBodyResult* rate_body_pointer = nullptr;
    if (runtime->response.parsed_head.head.status_code == 429) {
        rate_body =
            decode_rate_limit_body(Utf8View{reinterpret_cast<const char*>(runtime->response_body),
                                            runtime->response.http.body_bytes},
                                   Slice<u32>{runtime->rate_structural_scratch, kRateStructuralCount}, 8);
        if (!rate_body.ok() && !action_required && !auth_limited &&
            !(runtime->response.parsed_head.rate.fields & rate_limit_field_retry_after)) {
            return false;
        }
        if (rate_body.ok())
            rate_body_pointer = &rate_body;
    }
    RestRateObservation observation{};
    // A verification challenge is terminal even if labelled 429 without a
    // retry deadline. Preserve the actual HTTP response instead of hiding it
    // behind a transport error. Genuine rate-limit headers still feed the scheduler.
    if ((action_required || auth_limited) && runtime->response.parsed_head.head.status_code == 429 &&
        !rate_body_pointer && !(runtime->response.parsed_head.rate.fields & rate_limit_field_retry_after))
        cancel_rate_lease(runtime);
    else {
        RateLock guard(runtime);
        observation = rest_rate_observe(rate_scheduler(runtime), &runtime->transaction.lease,
                                        runtime->response.parsed_head, rate_body_pointer, now_ms,
                                        clock_unix_milliseconds());
        endpoint_gate_release(runtime->endpoint_gate, &runtime->endpoint_lease, now_ms);
        rate_changed(runtime);
    }
    if (!observation.ok() || !rest_transaction_complete(&runtime->transaction)) {
        return false;
    }
    RestClientResponse completed{};
    prepare_response_metadata(runtime, observation, &completed);
    // Auth challenge/error JSON need not have the rate-limit body schema.
    // Preserve a verified 429 even without a deadline, so its owner can hold
    // retries instead of interpreting a challenge or inventing a short delay.
    if (auth_limited)
        completed.rate_limited = true;
    completed.rejection = rejection;
    // Retry eligibility is shared by every request type, including typing,
    // edits, reactions and commands. Keep the raw rate headers for diagnostics.
    if (action_required) {
        completed.rate_limited = false;
        completed.retry_at_ms = 0;
    }
    ++runtime->connection_request_count;
    if (!runtime->response.http.connection_reusable) {
        close_transport(runtime);
    }
    publish_completed(client, runtime, completed);
    return true;
}

RestClientError client_transport_error(const RestClientRuntime* runtime, TlsStreamError error) noexcept {
    switch (error) {
    case TlsStreamError::resolve:
        return RestClientError::resolve;
    case TlsStreamError::connect:
        return RestClientError::connect;
    case TlsStreamError::tls:
        return RestClientError::tls;
    case TlsStreamError::send:
        return RestClientError::send;
    case TlsStreamError::receive:
        return RestClientError::receive;
    case TlsStreamError::capacity:
    case TlsStreamError::unexpected_data:
        return runtime->transaction.phase == RestTransactionPhase::receiving
                   ? rest_client_response_error(runtime->response_step.error)
                   : RestClientError::tls;
    case TlsStreamError::invalid_input:
    case TlsStreamError::none:
    default:
        return RestClientError::request;
    }
}

void publish_transport_phase(RestClient* client, RestClientRuntime* runtime) noexcept {
    switch (runtime->transport.state) {
    case TlsStreamState::resolving:
        publish_worker_phase(client, runtime, RestClientPhase::resolving);
        break;
    case TlsStreamState::connecting:
        publish_worker_phase(client, runtime, RestClientPhase::connecting);
        break;
    case TlsStreamState::handshake:
        publish_worker_phase(client, runtime, RestClientPhase::tls_handshake);
        break;
    case TlsStreamState::writing:
        publish_worker_phase(client, runtime, RestClientPhase::sending);
        break;
    case TlsStreamState::reading:
        publish_worker_phase(client, runtime, RestClientPhase::receiving);
        break;
    default:
        break;
    }
}

bool drive_transport_event(RestClient* client, RestClientRuntime* runtime,
                           TlsStreamEvent current, u64 now_ms) noexcept {
    for (;;) {
        if (!current.ok()) {
            begin_drain(client, runtime, client_transport_error(runtime, current.error),
                        current.native_error, false);
            return false;
        }
        if (current.kind == TlsStreamEventKind::waiting) {
            publish_transport_phase(client, runtime);
            return true;
        }
        if (current.kind == TlsStreamEventKind::ready) {
            runtime->transport_ready = true;
            runtime->connection_request_count = 0;
            InterlockedIncrement64(&client->connections);
            current = begin_request_send(client, runtime);
            continue;
        }
        if (current.kind == TlsStreamEventKind::write_complete) {
            if (runtime->write_commit_failed ||
                runtime->transaction.phase != RestTransactionPhase::receiving ||
                !rest_response_initialize(
                     &runtime->response, Slice<u8>{runtime->response_input, kRestMaximumHeaderBytes},
                     Slice<u8>{runtime->response_body, runtime->response_capacity},
                     runtime->command.route.method)) {
                begin_drain(client, runtime, RestClientError::request, 0, false);
                return false;
            }
            publish_worker_phase(client, runtime, RestClientPhase::receiving);
            runtime->receive_counter = clock_now();
            current = tls_stream_start_read(&runtime->transport, response_plaintext_received, runtime);
            continue;
        }
        if (current.kind == TlsStreamEventKind::read_complete) {
            if (runtime->response_callback_failed ||
                !finalize_response(client, runtime, runtime->response_step, now_ms)) {
                begin_drain(client, runtime, RestClientError::response, 0, false);
                return false;
            }
            return true;
        }
        if (current.kind == TlsStreamEventKind::drained) {
            return true;
        }
        begin_drain(client, runtime, RestClientError::request, 0, false);
        return false;
    }
}

bool acquire_request(RestClient* client, RestClientRuntime* runtime, NetworkLoop* loop,
                     u64 now_ms) noexcept {
    if (reject_restricted_request(client, runtime))
        return true;
    RestRateAcquireResult acquired{};
    bool endpoint_wait = false;
    {
        RateLock guard(runtime);
        auto* shared = client->shared;
        const bool mutation = runtime->command.route.method != RestMethod::get &&
                              runtime->command.route.method != RestMethod::head;
        const auto endpoint = endpoint_gate_acquire(runtime->endpoint_gate, runtime->command.endpoint_key,
                                                    now_ms, &runtime->endpoint_lease);
        if (endpoint.kind != EndpointGateKind::acquired) {
            endpoint_wait = true;
            acquired.kind = endpoint.kind == EndpointGateKind::waiting ? RestRateAcquireKind::waiting
                            : endpoint.kind == EndpointGateKind::busy  ? RestRateAcquireKind::busy
                                                                       : RestRateAcquireKind::error;
            acquired.wait_until_ms = endpoint.wait_until_ms;
        } else if (shared && mutation && shared->mutation_owner) {
            acquired.kind = RestRateAcquireKind::busy;
        } else {
            acquired = rest_rate_acquire(rate_scheduler(runtime), runtime->command.route, now_ms);
            if (shared && mutation && acquired.acquired_request())
                shared->mutation_owner = client;
        }
        // This reservation was never visible outside the locked admission
        // attempt, so releasing it needs no peer wake (which could ping-pong).
        if (!acquired.acquired_request())
            endpoint_gate_release(runtime->endpoint_gate, &runtime->endpoint_lease, now_ms, false);
    }
    const bool peer_wait = acquired.kind == RestRateAcquireKind::busy;
    if (g_trace && (peer_wait || acquired.kind == RestRateAcquireKind::waiting) &&
        (!runtime->rate_wait_until_ms || runtime->waiting_for_peer != peer_wait)) {
        TraceRecord line("gate_wait");
        trace_identity(line, runtime, runtime->trace_request_id);
        line.field("reason", endpoint_wait ? "endpoint" : peer_wait ? "peer_or_mutation" : "rate_limit");
        line.field("wait_ms", acquired.wait_until_ms > now_ms ? acquired.wait_until_ms - now_ms : 0);
        g_trace(line.text);
    }
    runtime->waiting_for_peer = peer_wait;
    if (runtime->waiting_for_peer) {
        // A completion/cancellation wakes this worker. The deadline is only a
        // safety timeout, never a periodic polling interval.
        runtime->rate_wait_until_ms = deadline_after(now_ms, runtime->command.timeout_ms);
        publish_worker_phase(client, runtime, RestClientPhase::rate_wait);
        return true;
    }
    if (acquired.kind == RestRateAcquireKind::waiting) {
        runtime->rate_wait_until_ms = acquired.wait_until_ms;
        publish_worker_phase(client, runtime, RestClientPhase::rate_wait);
        return true;
    }
    if (!acquired.acquired_request()) {
        publish_request_error(client, runtime, RestClientError::rate, 0);
        return true;
    }
    runtime->rate_wait_until_ms = 0;
    const u64 deadline = deadline_after(now_ms, runtime->command.timeout_ms);
    const bool reused = runtime->transport_ready && runtime->connection_request_count != 0;
    if (!rest_transaction_acquire(&runtime->transaction, runtime->command.request_id,
                                  acquired.lease, deadline, reused)) {
        RestRateLease lease = acquired.lease;
        {
            RateLock guard(runtime);
            rest_rate_cancel(rate_scheduler(runtime), &lease);
            endpoint_gate_release(runtime->endpoint_gate, &runtime->endpoint_lease, now_ms);
            rate_changed(runtime);
        }
        publish_request_error(client, runtime, RestClientError::request, 0);
        return true;
    }
    if (runtime->transport_ready) {
        drive_transport_event(client, runtime, begin_request_send(client, runtime), now_ms);
        return true;
    }
    drive_transport_event(client, runtime, begin_resolution(client, runtime, loop), now_ms);
    return true;
}

void activate_request(RestClient* client, RestClientRuntime* runtime, NetworkLoop* loop,
                      u64 now_ms) noexcept {
    const LONG prior =
        InterlockedCompareExchange(&client->request_state, request_state_active, request_state_pending);
    if (prior == request_state_cancel_pending) {
        runtime->phase_counter = clock_now();
        runtime->trace_request_id = runtime->command.request_id;
        runtime->trace_phase = RestClientPhase::queued;
        runtime->first_byte_counter = runtime->receive_counter = 0;
        runtime->request_started_counter = 0;
        reset_value<RestTransaction>(&runtime->transaction);
        publish_request_error(client, runtime, RestClientError::cancelled, ERROR_OPERATION_ABORTED);
        return;
    }
    if (prior != request_state_pending) {
        return;
    }
    runtime->request_started_counter = clock_now();
    runtime->phase_counter = runtime->request_started_counter;
    runtime->first_byte_counter = runtime->receive_counter = 0;
    runtime->trace_request_id = runtime->command.request_id;
    runtime->trace_phase = RestClientPhase::queued;
    if (g_trace) {
        TraceRecord line("start");
        trace_identity(line, runtime, runtime->trace_request_id);
        line.field("route", runtime->command.route.route_id);
        line.field("method", static_cast<u64>(runtime->command.route.method));
        line.field("queue_us", clock_microseconds(runtime->clock, runtime->command.submitted_counter,
                                                   runtime->request_started_counter));
        g_trace(line.text);
    }
    reset_value<RestTransaction>(&runtime->transaction);
    acquire_request(client, runtime, loop, now_ms);
}

void cancel_request(RestClient* client, RestClientRuntime* runtime) noexcept {
    if (InterlockedCompareExchange(&client->request_state, 0, 0) != request_state_cancel_active) {
        return;
    }
    if (runtime->transaction.phase == RestTransactionPhase::idle &&
        runtime->rate_wait_until_ms != 0) {
        runtime->rate_wait_until_ms = 0;
        publish_request_error(client, runtime, RestClientError::cancelled, 0);
        return;
    }
    begin_drain(client, runtime, RestClientError::cancelled, ERROR_OPERATION_ABORTED, true);
}

NetworkWorkerStep handle_completion(RestClient* client, RestClientRuntime* runtime,
                                    NetworkLoop* loop, const NetworkWorkerEvent& event) noexcept {
    const TlsStreamEvent transport_event =
        tls_stream_handle_completion(&runtime->transport, loop, event.poll.completion);
    if (runtime->stopping || runtime->draining) {
        return finish_drain(client, runtime);
    }
    drive_transport_event(client, runtime, transport_event, event.now_ms);
    if (runtime->draining) {
        return finish_drain(client, runtime);
    }
    return NetworkWorkerStep{next_wait(runtime, event.now_ms), false};
}

void begin_shutdown(RestClient* client, RestClientRuntime* runtime) noexcept {
    runtime->stopping = true;
    publish_worker_phase(client, runtime, RestClientPhase::stopping);
    cancel_rate_lease(runtime);
    rest_transaction_cancel(&runtime->transaction);
    close_transport(runtime);
}

NetworkWorkerStep worker_handler(void* opaque, NetworkLoop* loop,
                                 const NetworkWorkerEvent& event) noexcept {
    auto* client = static_cast<RestClient*>(opaque);
    auto* runtime = static_cast<RestClientRuntime*>(client->runtime);
    if (event.kind == NetworkWorkerEventKind::started) {
        publish_worker_phase(client, runtime, RestClientPhase::idle);
        return NetworkWorkerStep{INFINITE, false};
    }
    if (event.kind == NetworkWorkerEventKind::stop_requested) {
        begin_shutdown(client, runtime);
        return finish_drain(client, runtime);
    }
    if (event.kind == NetworkWorkerEventKind::poll_error) {
        publish_client_failure(client, RestClientError::worker,
                               static_cast<u32>(event.poll.completion.native_error));
        begin_shutdown(client, runtime);
        return finish_drain(client, runtime);
    }
    if (event.kind == NetworkWorkerEventKind::deadline) {
        if (runtime->rate_wait_until_ms != 0 && event.now_ms >= runtime->rate_wait_until_ms) {
            if (runtime->waiting_for_peer) {
                runtime->waiting_for_peer = false;
                runtime->rate_wait_until_ms = 0;
                publish_request_error(client, runtime, RestClientError::timeout, WAIT_TIMEOUT);
            } else
                acquire_request(client, runtime, loop, event.now_ms);
        }
        if (transaction_in_progress(runtime->transaction) &&
            event.now_ms >= runtime->transaction.deadline_ms) {
            begin_drain(client, runtime, RestClientError::timeout, WAIT_TIMEOUT, false);
            return finish_drain(client, runtime);
        }
        return NetworkWorkerStep{next_wait(runtime, event.now_ms), false};
    }
    if (event.poll.completion.kind == NetworkOperationKind::signal) {
        if (event.poll.completion.signal == &runtime->request_signal &&
            event.poll.completion.signal_value == kRequestSignalValue) {
            activate_request(client, runtime, loop, event.now_ms);
        } else if (event.poll.completion.signal == &runtime->cancel_signal &&
                   event.poll.completion.signal_value == kCancelSignalValue) {
            cancel_request(client, runtime);
        } else if (event.poll.completion.signal == &runtime->rate_signal &&
                   event.poll.completion.signal_value == kRateSignalValue && runtime->rate_wait_until_ms &&
                   runtime->transaction.phase == RestTransactionPhase::idle &&
                   InterlockedCompareExchange(&client->request_state, 0, 0) == request_state_active) {
            acquire_request(client, runtime, loop, event.now_ms);
        }
        if (runtime->draining) {
            return finish_drain(client, runtime);
        }
        return NetworkWorkerStep{next_wait(runtime, event.now_ms), false};
    }
    return handle_completion(client, runtime, loop, event);
}

NetworkWorkerResult start_rest_worker(RestClient* client, RestSharedState* shared) noexcept {
    auto result = network_worker_start(&client->worker, worker_handler, client);
    if (!result.ok() || !shared) return result;
    // No request can be submitted before start returns. Register only once the
    // IOCP is ready, without holding the scheduler lock during thread startup.
    AcquireSRWLockExclusive(shared->lock.native());
    result.error = NetworkWorkerError::invalid_input;
    for (auto*& slot : shared->clients) {
        if (slot) continue;
        if (!shared->scheduler.buckets.data)
            rest_rate_scheduler_initialize(&shared->scheduler, {shared->buckets, kRateBucketCount},
                                           {shared->bindings, kRateBindingCount});
        client->shared = shared;
        slot = client;
        static_cast<RestClientRuntime*>(client->runtime)->trace_lane =
            &slot == &shared->clients[0] ? 1 : 2;
        result.error = NetworkWorkerError::none;
        break;
    }
    ReleaseSRWLockExclusive(shared->lock.native());
    if (!result.ok()) {
        // A full scheduler rejects startup. This worker is idle and has never
        // owned a request or rate lease; join before releasing its storage.
        network_worker_request_stop(&client->worker);
        network_worker_join(&client->worker, INFINITE);
    }
    return result;
}

void destroy_client_storage(RestClient* client, SecureBuffer* restore) noexcept {
    if (!client || !client->runtime) {
        return;
    }
    auto* runtime = static_cast<RestClientRuntime*>(client->runtime);
    if (restore && runtime->credential.data) {
        secure_buffer_transfer(restore, &runtime->credential);
    } else {
        secure_buffer_release(&runtime->credential);
    }
    secure_buffer_release(&runtime->secret_storage);
    release_extended_body(runtime);
    release_decompression(runtime);
    const u32 bytes = runtime->allocation_bytes;
    secure_clear_bytes(runtime, bytes);
    VirtualFree(runtime, 0, MEM_RELEASE);
    client->runtime = nullptr;
}

// Releases the storage of a start that failed after allocating it and records the error.
RestClientResult fail_start(RestClient* client, RestClientError error, u32 native_error) noexcept {
    destroy_client_storage(client, nullptr);
    InterlockedExchange(&client->error, static_cast<LONG>(error));
    InterlockedExchange(&client->native_error, static_cast<LONG>(native_error));
    return client_result(client);
}

RestClientResult start_client(RestClient* client, SecureBuffer* credential,
                              RestAuthorization authorization,
                              const RestUserConfiguration* user_configuration,
                              u32 response_capacity, bool user_auth_mode,
                              RestSharedState* shared = nullptr,
                              u32 request_capacity = kUserAuthMaximumRequestBodyBytes) noexcept {
    const bool normal_user = authorization == RestAuthorization::normal_user;
    const u32 bytes = allocation_bytes(response_capacity, user_auth_mode, normal_user, shared != nullptr);
    if (!client || client->runtime || client->response_event || client->worker.thread || bytes == 0 ||
        (user_auth_mode &&
         (credential || user_configuration || authorization != RestAuthorization::none ||
          user_auth_secure_bytes(response_capacity, request_capacity) == 0)) ||
        (authorization == RestAuthorization::none && credential) ||
        (authorization != RestAuthorization::none && !credential) ||
        (normal_user != (user_configuration != nullptr)) ||
        (user_configuration && !valid_user_configuration(*user_configuration)) ||
        (credential && (!credential->data || !credential->locked || credential->bytes == 0 ||
                        credential->bytes > kMaximumCredentialBytes))) {
        return invalid_result();
    }
    if (credential) {
        const ConstSlice<u8> bytes_view = secure_buffer_view(*credential);
        for (u32 index = 0; index < bytes_view.count; ++index) {
            if (bytes_view[index] < 0x21 || bytes_view[index] > 0x7E)
                return invalid_result();
        }
    }
    reset_value<RestClient>(client);
    client->runtime = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!client->runtime) {
        InterlockedExchange(&client->error, static_cast<LONG>(RestClientError::memory));
        InterlockedExchange(&client->native_error, static_cast<LONG>(thread_last_error()));
        return client_result(client);
    }
    auto* runtime = static_cast<RestClientRuntime*>(client->runtime);
    runtime->allocation_bytes = bytes;
    runtime->owner = client;
    runtime->clock = clock_initialize();
    runtime->trace_worker_id = static_cast<u32>(InterlockedIncrement(&g_trace_serial));
    runtime->response_capacity = response_capacity;
    runtime->default_response_capacity = response_capacity;
    runtime->authorization = authorization;
    runtime->user_auth_mode = user_auth_mode;
    runtime->request_body_capacity = user_auth_mode ? request_capacity : kRestClientMaximumRequestBodyBytes;
    if (user_auth_mode) {
        const u32 secure_bytes = user_auth_secure_bytes(response_capacity, request_capacity);
        const SecureBufferResult initialized =
            secure_buffer_initialize(&runtime->secret_storage, secure_bytes);
        if (!initialized.ok())
            return fail_start(client, RestClientError::memory, initialized.native_error);
    }
    assign_storage(runtime, shared);
    runtime->default_body = runtime->request_body;
    if ((user_configuration && !copy_user_configuration(runtime, *user_configuration)) ||
        (user_auth_mode && !initialize_auth_user_configuration(runtime))) {
        destroy_client_storage(client, nullptr);
        return invalid_result();
    }
    if (!tls_stream_initialize(
            &runtime->transport, kApiHostWide,
            TlsStreamBuffers{Slice<u8>{runtime->encrypted_storage, user_auth_mode
                                                                       ? kUserAuthEncryptedInputBytes
                                                                       : kEncryptedInputBytes},
                             Slice<u8>{runtime->handshake_output, kHandshakeOutputBytes},
                             Slice<u8>{runtime->record_output, user_auth_mode ? kUserAuthRecordOutputBytes
                                                                              : kRecordOutputBytes}})) {
        destroy_client_storage(client, nullptr);
        return invalid_result();
    }
    client->response_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!client->response_event)
        return fail_start(client, RestClientError::event, thread_last_error());
    if (credential && !secure_buffer_transfer(&runtime->credential, credential)) {
        CloseHandle(client->response_event);
        client->response_event = nullptr;
        destroy_client_storage(client, nullptr);
        return invalid_result();
    }
    publish_phase(client, RestClientPhase::starting);
    const i64 worker_start = g_trace ? clock_now() : 0;
    const NetworkWorkerResult started = start_rest_worker(client, shared);
    if (!started.ok()) {
        if (credential && runtime->credential.data) {
            secure_buffer_transfer(credential, &runtime->credential);
        }
        CloseHandle(client->response_event);
        client->response_event = nullptr;
        return fail_start(client, RestClientError::worker, started.native_error);
    }
    if (g_trace) {
        TraceRecord line("worker_online");
        trace_identity(line, runtime, 0);
        line.field("startup_us", clock_microseconds(runtime->clock, worker_start, clock_now()));
        line.field("runtime_bytes", runtime->allocation_bytes);
        g_trace(line.text);
    }
    return client_result(client);
}

} // namespace

bool rest_transaction_acquire(RestTransaction* transaction, u64 request_id,
                              const RestRateLease& lease, u64 deadline_ms,
                              bool reused_connection) noexcept {
    if (!transaction || request_id == 0 || !lease.active || deadline_ms == 0 ||
        transaction->phase != RestTransactionPhase::idle) {
        return false;
    }
    reset_value<RestTransaction>(transaction);
    transaction->lease = lease;
    transaction->request_id = request_id;
    transaction->deadline_ms = deadline_ms;
    transaction->phase = RestTransactionPhase::acquired;
    transaction->reused_connection = reused_connection;
    return true;
}

bool rest_transaction_begin_send(RestTransaction* transaction, u32 request_bytes) noexcept {
    if (!transaction || request_bytes == 0 || transaction->phase != RestTransactionPhase::acquired) {
        return false;
    }
    transaction->request_bytes = request_bytes;
    transaction->committed_bytes = 0;
    transaction->phase = RestTransactionPhase::sending;
    return true;
}

bool rest_transaction_commit_plaintext(RestTransaction* transaction, u32 bytes) noexcept {
    if (!transaction || bytes == 0 || transaction->phase != RestTransactionPhase::sending ||
        bytes > transaction->request_bytes - transaction->committed_bytes) {
        return false;
    }
    transaction->committed_bytes += bytes;
    if (transaction->committed_bytes == transaction->request_bytes) {
        transaction->phase = RestTransactionPhase::receiving;
    }
    return true;
}

bool rest_transaction_complete(RestTransaction* transaction) noexcept {
    if (!transaction || transaction->lease.active ||
        transaction->phase != RestTransactionPhase::receiving ||
        transaction->committed_bytes != transaction->request_bytes) {
        return false;
    }
    transaction->phase = RestTransactionPhase::complete;
    return true;
}

bool rest_transaction_cancel(RestTransaction* transaction) noexcept {
    return end_transaction(transaction, RestTransactionPhase::cancelled);
}

bool rest_transaction_fail(RestTransaction* transaction) noexcept {
    return end_transaction(transaction, RestTransactionPhase::failed);
}

void rest_client_set_trace(RestTraceFunction trace) noexcept { g_trace = trace; }

RestClientResult rest_client_start_public(RestClient* client,
                                          u32 response_capacity, RestSharedState* shared) noexcept {
    return start_client(client, nullptr, RestAuthorization::none, nullptr, response_capacity, false, shared);
}

RestClientResult rest_client_start_user(RestClient* client, SecureBuffer* credential,
                                        const RestUserConfiguration* configuration,
                                        u32 response_capacity, RestSharedState* shared) noexcept {
    return start_client(client, credential, RestAuthorization::normal_user, configuration,
                        response_capacity, false, shared);
}

RestClientResult rest_client_start_user_auth(RestClient* client,
                                             u32 response_capacity) noexcept {
    return start_client(client, nullptr, RestAuthorization::none, nullptr, response_capacity, true);
}

void rest_client_set_required_action(RestClient* client,
                                     AccountRequirement action) noexcept {
    if (client) {
        InterlockedExchange(&client->required_action, static_cast<LONG>(action));
        if (client->shared)
            InterlockedExchange(&client->shared->required_action, static_cast<LONG>(action));
    }
}

static RestClientSubmitResult submit_request(RestClient* client, const RestClientRequest& request,
                                             u32 extended_response_capacity, bool large = false,
                                             u8** owned_body = nullptr) noexcept {
    auto* runtime = runtime_of(client);
    const bool logout = runtime && runtime->authorization == RestAuthorization::normal_user &&
                        account_logout_request_valid(request);
    if (runtime) {
        const auto reserved =
            static_cast<u64>(InterlockedCompareExchange64(&runtime->logout_reservation_id, 0, 0));
        if (reserved && (!logout || request.request_id != reserved))
            return {RestClientSubmitError::busy};
    }
    if (!runtime || runtime->user_auth_mode || !client->worker.thread ||
        (authentication_rejected(client) && !logout) || request.request_id == 0 ||
        !valid_route(request.route) || !valid_target(request.target) ||
        (!request.body.data && request.body.count != 0) ||
        (!request.json_body && request.body.count != 0) || request.timeout_ms > kMaximumRequestTimeoutMs ||
        !rest_audit_reason_valid(request.audit_reason) ||
        ((request.audit_reason.count || request.send_empty_audit_reason) &&
         request.route.method == RestMethod::get) ||
        !rest_extra_headers_valid(request.extra_headers) ||
        (!request.multipart_boundary.data && request.multipart_boundary.count) ||
        (request.multipart_boundary.count &&
         (!request.json_body || !rest_multipart_boundary_valid(request.multipart_boundary))) ||
        worker_ending(client)) {
        return RestClientSubmitResult{RestClientSubmitError::invalid_input};
    }
    const auto required = required_action(client);
    if (!logout && !account_request_allowed(required, request.route.method))
        return {RestClientSubmitError::account_restricted};
    if (request.target.count > kRestClientMaximumTargetBytes) {
        return RestClientSubmitResult{RestClientSubmitError::target_capacity};
    }
    const u32 body_limit = extended_response_capacity ? kRestUserSettingsMaximumBodyBytes
                           : large                    ? kRestClientMaximumLargeRequestBodyBytes
                                                      : kRestClientMaximumRequestBodyBytes;
    if (request.body.count > body_limit) {
        return RestClientSubmitResult{RestClientSubmitError::body_capacity};
    }
    if (InterlockedCompareExchange(&client->request_state, request_state_writing, request_state_empty) !=
        request_state_empty) {
        trace_busy(client, runtime, request);
        return RestClientSubmitResult{RestClientSubmitError::busy};
    }
    // Only an oversized body or an extended response (settings, large lists)
    // leaves the fixed arena; an oversized body's response keeps the configured
    // capacity and shares the bank after the upload is committed.
    const bool extended =
        extended_response_capacity || request.body.count > kRestClientMaximumRequestBodyBytes;
    bool adopted_body = false;
    if (extended) {
        const u32 response_capacity =
            extended_response_capacity ? extended_response_capacity : runtime->response_capacity;
        const u32 bank_bytes = maximum(request.body.count, response_capacity);
        adopted_body = owned_body && request.body.count >= response_capacity;
        auto* bank = adopted_body ? *owned_body : static_cast<u8*>(VirtualAlloc(nullptr, bank_bytes,
                                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!bank) {
            InterlockedExchange(&client->request_state, request_state_empty);
            return {RestClientSubmitError::memory};
        }
        runtime->extended_body = bank;
        runtime->extended_body_bytes = bank_bytes;
        runtime->request_body = runtime->response_body = bank;
        runtime->request_body_capacity = bank_bytes;
        runtime->response_capacity = response_capacity;
        // Ownership changes before pending publication. REST retains ownership
        // on every return path. Rollback or response/stop cleanup releases it;
        // a concurrent pending claimant can keep it after a failed signal.
        if (adopted_body) *owned_body = nullptr;
    }
    if (g_trace) {
        TraceRecord line("submit");
        trace_identity(line, runtime, request.request_id);
        line.field("route", request.route.route_id);
        line.field("busy_retries", runtime->busy_count);
        line.field("busy_episode_ms", runtime->busy_count ? clock_monotonic_milliseconds() - runtime->busy_since_ms : 0);
        g_trace(line.text);
    }
    runtime->busy_count = runtime->busy_since_ms = 0;
    runtime->command.submitted_counter = clock_now();
    runtime->command.route = request.route;
    runtime->command.endpoint_key = endpoint_key(request.target);
    runtime->command.request_id = request.request_id;
    runtime->command.target_bytes = request.target.count;
    runtime->command.body_bytes = request.body.count;
    runtime->command.timeout_ms =
        request.timeout_ms == 0 ? kRestClientDefaultTimeoutMs : request.timeout_ms;
    runtime->command.json_body = request.json_body;
    runtime->command.identity_response = request.identity_response;
    runtime->command.audit_reason_bytes = request.audit_reason.count;
    runtime->command.send_empty_audit_reason = request.send_empty_audit_reason;
    runtime->command.multipart_boundary_bytes = request.multipart_boundary.count;
    const bool armed = runtime->armed_request_id && runtime->armed_request_id == request.request_id;
    const u32 armed_bytes = armed ? runtime->armed_headers_bytes : 0;
    runtime->command.extra_headers_bytes = request.extra_headers.count + armed_bytes;
    if (request.extra_headers.count > sizeof(runtime->extra_headers) - armed_bytes ||
        !copy_bytes(runtime->extra_headers, sizeof(runtime->extra_headers), request.extra_headers.data,
                    request.extra_headers.count) ||
        !copy_bytes(runtime->extra_headers + request.extra_headers.count,
                    sizeof(runtime->extra_headers) - request.extra_headers.count, runtime->armed_headers,
                    armed_bytes) ||
        !copy_bytes(runtime->audit_reason, sizeof(runtime->audit_reason), request.audit_reason.data,
                    request.audit_reason.count) ||
        !copy_bytes(runtime->multipart_boundary, sizeof(runtime->multipart_boundary),
                    request.multipart_boundary.data, request.multipart_boundary.count) ||
        !copy_bytes(runtime->request_target, kRestClientMaximumTargetBytes, request.target.data,
                    request.target.count) ||
        (!adopted_body && !copy_bytes(runtime->request_body, runtime->request_body_capacity, request.body.data,
                    request.body.count))) {
        erase_command(runtime);
        release_extended_body(runtime);
        InterlockedExchange(&client->request_state, request_state_empty);
        return RestClientSubmitResult{RestClientSubmitError::invalid_input};
    }
    if (armed) {
        // One retry only: the solved challenge never outlives its request.
        secure_clear_bytes(runtime->armed_headers, runtime->armed_headers_bytes);
        runtime->armed_headers_bytes = 0;
        runtime->armed_request_id = 0;
    }
    InterlockedExchange64(&client->current_request_id, static_cast<LONG64>(request.request_id));
    InterlockedExchange64(&runtime->logout_request_id,
                          logout ? static_cast<LONG64>(request.request_id) : 0);
    InterlockedExchange(&client->request_state, request_state_pending);
    publish_phase(client, RestClientPhase::queued);
    const NetworkResult signalled =
        network_signal_post(&client->worker.loop, &runtime->request_signal, kRequestSignalValue);
    if (!signalled.ok()) {
        // A concurrent cancellation or an earlier queued signal may already
        // be handling this request. Keep its spans and request ID.
        if (InterlockedCompareExchange(&client->request_state, request_state_writing,
                                       request_state_pending) != request_state_pending)
            return {RestClientSubmitError::none};
        erase_command(runtime);
        release_extended_body(runtime);
        InterlockedExchange64(&client->current_request_id, 0);
        InterlockedExchange64(&runtime->logout_request_id, 0);
        publish_phase(client, RestClientPhase::idle);
        InterlockedExchange(&client->request_state, request_state_empty);
        return RestClientSubmitResult{RestClientSubmitError::signal};
    }
    return RestClientSubmitResult{RestClientSubmitError::none};
}

RestClientSubmitResult rest_client_submit(RestClient* client,
                                          const RestClientRequest& request) noexcept {
    return submit_request(client, request, 0);
}

RestClientSubmitResult rest_client_submit_large(RestClient* client,
                                                const RestClientRequest& request) noexcept {
    return submit_request(client, request, 0, true);
}

RestClientSubmitResult rest_client_submit_large_owned(
    RestClient* client, const RestClientRequest& request, u8** body_owner) noexcept {
    if (!body_owner || !*body_owner || *body_owner != request.body.data || !request.body.count)
        return {RestClientSubmitError::invalid_input};
    return submit_request(client, request, 0, true, body_owner);
}

RestClientSubmitResult rest_client_submit_large_response(RestClient* client,
                                                         const RestClientRequest& request,
                                                         u32 response_capacity) noexcept {
    const bool get = request.route.method == RestMethod::get && !request.body.count && !request.json_body;
    const bool post = request.route.method == RestMethod::post && request.json_body &&
                      request.body.count <= kRestClientMaximumRequestBodyBytes;
    if (response_capacity < kRestClientMinimumResponseBytes ||
        response_capacity > kRestClientMaximumResponseBytes || (!get && !post))
        return {RestClientSubmitError::invalid_input};
    return submit_request(client, request, response_capacity);
}

RestClientSubmitResult rest_client_submit_user_settings(
    RestClient* client, const RestClientRequest& request,
    u32 response_capacity) noexcept {
    static constexpr char target[] = "/api/v10/users/@me/settings-proto/2";
    const auto* runtime = client ? static_cast<const RestClientRuntime*>(client->runtime) : nullptr;
    if (!runtime || runtime->authorization != RestAuthorization::normal_user ||
        runtime->user_auth_mode || response_capacity < kRestClientMinimumResponseBytes ||
        response_capacity > kRestClientMaximumResponseBytes || !request.target.data ||
        request.target.count != sizeof(target) - 1 ||
        !bytes_equal(request.target.data, target, sizeof(target) - 1) ||
        request.route.route_id != kRestUserSettingsRouteId || request.route.major_key != 0 ||
        request.route.major != RestMajorParameter::none ||
        (request.route.method != RestMethod::get && request.route.method != RestMethod::patch) ||
        (request.route.method == RestMethod::get && (request.body.count != 0 || request.json_body)) ||
        (request.route.method == RestMethod::patch && (!request.body.count || !request.json_body)))
        return {RestClientSubmitError::invalid_input};
    return submit_request(client, request, response_capacity);
}

RestClientResult rest_client_start_user_auth_capacity(
    RestClient* client, u32 response_capacity, u32 request_capacity) noexcept {
    return start_client(client, nullptr, RestAuthorization::none, nullptr,
                        response_capacity, true, nullptr, request_capacity);
}

RestClientSubmitResult rest_client_submit_user_auth(
    RestClient* client, const RestUserAuthRequest& request) noexcept {
    auto* runtime = runtime_of(client);
    const RestRouteKey route = user_auth_route(request.operation);
    const bool has_captcha = request.captcha_key != nullptr;
    const bool handoff_headers = request.operation == UserAuthOperation::browser_handoff_exchange &&
        (request.captcha_key || request.captcha_rqtoken || request.captcha_session_id);
    if (!runtime || !runtime->user_auth_mode || !client->worker.thread ||
        request.request_id == 0 || !valid_route(route) || handoff_headers || !valid_locked_value(request.body, ~0u, true) ||
        !valid_locked_value(request.captcha_key, kUserAuthMaximumCaptchaKeyBytes, false) ||
        !valid_locked_value(request.captcha_rqtoken, kUserAuthMaximumCaptchaTokenBytes, false) ||
        !valid_locked_value(request.captcha_session_id, kUserAuthMaximumCaptchaSessionBytes, false) ||
        (!has_captcha && (request.captcha_rqtoken || request.captcha_session_id)) ||
        request.timeout_ms > kMaximumRequestTimeoutMs || worker_ending(client)) {
        return RestClientSubmitResult{RestClientSubmitError::invalid_input};
    }
    if (request.body->bytes > runtime->request_body_capacity) {
        return RestClientSubmitResult{RestClientSubmitError::body_capacity};
    }
    if (InterlockedCompareExchange(&client->request_state, request_state_writing, request_state_empty) !=
        request_state_empty) {
        return RestClientSubmitResult{RestClientSubmitError::busy};
    }

    const RestUserRequestHeaders user_headers{
        {runtime->user_super_properties, runtime->user_super_properties_bytes},
        {runtime->user_locale, runtime->user_locale_bytes},
        {runtime->user_timezone, runtime->user_timezone_bytes}};
    const UserAuthWriteResult head = user_auth_write_request_head(
        request.operation, request.body->bytes,
        UserAuthCaptchaHeaders{locked_utf8(request.captcha_key), locked_utf8(request.captcha_rqtoken),
                               locked_utf8(request.captcha_session_id)},
        Slice<u8>{runtime->request_head, runtime->request_head_capacity}, &user_headers);
    const ConstSlice<u8> body = secure_buffer_view(*request.body);
    if (!head.ok() || !copy_bytes(runtime->request_body, runtime->request_body_capacity, body.data,
                                  body.count)) {
        secure_clear_bytes(runtime->request_head, runtime->request_head_capacity);
        secure_clear_bytes(runtime->request_body, runtime->request_body_capacity);
        InterlockedExchange(&client->request_state, request_state_empty);
        return RestClientSubmitResult{head.error == UserAuthWriteError::output_too_small
                                                 ? RestClientSubmitError::target_capacity
                                                 : RestClientSubmitError::invalid_input};
    }

    runtime->command.route = route;
    const auto auth_target = user_auth_target(request.operation);
    runtime->command.endpoint_key =
        endpoint_key({reinterpret_cast<const u8*>(auth_target.data), auth_target.bytes});
    runtime->command.request_id = request.request_id;
    runtime->command.body_bytes = request.body->bytes;
    runtime->command.head_bytes = head.bytes;
    runtime->command.timeout_ms =
        request.timeout_ms == 0 ? kRestClientDefaultTimeoutMs : request.timeout_ms;
    runtime->command.json_body = true;
    runtime->command.submitted_counter = clock_now();
    InterlockedExchange64(&client->current_request_id, static_cast<LONG64>(request.request_id));
    InterlockedExchange(&client->request_state, request_state_pending);
    publish_phase(client, RestClientPhase::queued);
    const NetworkResult signalled =
        network_signal_post(&client->worker.loop, &runtime->request_signal, kRequestSignalValue);
    if (!signalled.ok()) {
        erase_command(runtime);
        InterlockedExchange64(&client->current_request_id, 0);
        InterlockedExchange(&client->request_state, request_state_empty);
        publish_phase(client, RestClientPhase::idle);
        return RestClientSubmitResult{RestClientSubmitError::signal};
    }
    return RestClientSubmitResult{RestClientSubmitError::none};
}

bool rest_client_arm_headers(RestClient* client, u64 request_id, ConstSlice<u8> headers) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || runtime->user_auth_mode || !rest_extra_headers_valid(headers))
        return false;
    secure_clear_bytes(runtime->armed_headers, runtime->armed_headers_bytes);
    runtime->armed_headers_bytes = 0;
    runtime->armed_request_id = 0;
    if (!request_id || !headers.count)
        return true;
    copy_bytes(runtime->armed_headers, headers.data, headers.count);
    runtime->armed_headers_bytes = headers.count;
    runtime->armed_request_id = request_id;
    return true;
}

bool rest_client_cancel(RestClient* client, u64 request_id) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || request_id == 0) {
        return false;
    }
    return network_client_cancel(client, &runtime->cancel_signal, kCancelSignalValue, request_id);
}

RestClientResult rest_client_wait_response(RestClient* client,
                                           u32 timeout_ms) noexcept {
    if (!client || !client->response_event)
        return invalid_result();
    const DWORD waited = WaitForSingleObject(client->response_event, timeout_ms);
    RestClientResult value = client_result(client);
    if (waited != WAIT_OBJECT_0 && value.error == RestClientError::none) {
        value.error =
            waited == WAIT_TIMEOUT ? RestClientError::timeout : RestClientError::worker;
        value.native_error = waited == WAIT_TIMEOUT ? WAIT_TIMEOUT : thread_last_error();
    }
    return value;
}

RestClientTakeResult rest_client_take_response(RestClient* client,
                                               RestClientResponse* response,
                                               Slice<u8> body_output) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || runtime->user_auth_mode || !response || (!body_output.data && body_output.count != 0)) {
        return RestClientTakeResult{0, RestClientTakeError::invalid_input};
    }
    if (InterlockedCompareExchange(&client->request_state, 0, 0) != request_state_ready) {
        return RestClientTakeResult{0, RestClientTakeError::empty};
    }
    if (runtime->completed.body_bytes > body_output.count) {
        return RestClientTakeResult{runtime->completed.body_bytes,
                                    RestClientTakeError::output_too_small};
    }
    for (u32 index = 0; index < runtime->completed.body_bytes; ++index) {
        body_output[index] = runtime->response_body[index];
    }
    *response = runtime->completed;
    const u32 body_bytes = runtime->completed.body_bytes;
    release_completed_response(client, runtime, body_bytes);
    return RestClientTakeResult{body_bytes, RestClientTakeError::none};
}

RestClientSubmitResult rest_client_prepare_logout(RestClient* client,
                                                  u64 request_id) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || !request_id || runtime->user_auth_mode ||
        runtime->authorization != RestAuthorization::normal_user || !client->worker.thread ||
        worker_ending(client))
        return {RestClientSubmitError::invalid_input};
    const auto reserved = static_cast<u64>(
        InterlockedCompareExchange64(&runtime->logout_reservation_id, static_cast<LONG64>(request_id), 0));
    if (reserved && reserved != request_id)
        return {RestClientSubmitError::busy};
    if (static_cast<u64>(InterlockedCompareExchange64(&runtime->logout_request_id, 0, 0)) == request_id)
        return {RestClientSubmitError::busy};
    if (runtime->logout_cancelled)
        return {RestClientSubmitError::invalid_input};
    const auto current = static_cast<u64>(InterlockedCompareExchange64(&client->current_request_id, 0, 0));
    const auto state = InterlockedCompareExchange(&client->request_state, 0, 0);
    if (!current && state == request_state_empty) {
        runtime->logout_preempt_id = 0;
        return {RestClientSubmitError::none};
    }
    if (!runtime->logout_preempt_id && current)
        runtime->logout_preempt_id = current;
    if (!current || current != runtime->logout_preempt_id)
        return {RestClientSubmitError::busy};
    if (state == request_state_ready && runtime->completed.request_id == current) {
        // The ready mailbox is only read by this UI-thread drain.
        // Discarding uses the same secure erasure as a normal response take,
        // without copying unrelated plaintext into a logout output bank.
        release_completed_response(client, runtime, runtime->completed.body_bytes);
        runtime->logout_preempt_id = 0;
        return {RestClientSubmitError::none};
    }
    rest_client_cancel(client, current);
    return {RestClientSubmitError::busy};
}

bool rest_client_cancel_logout(RestClient* client, u64 request_id) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || runtime->authorization != RestAuthorization::normal_user || !request_id ||
        static_cast<u64>(InterlockedCompareExchange64(&runtime->logout_reservation_id, 0, 0)) != request_id)
        return false;
    const auto admitted = static_cast<u64>(InterlockedCompareExchange64(&runtime->logout_request_id, 0, 0));
    if (!admitted) {
        runtime->logout_cancelled = true;
        if (runtime->logout_preempt_id)
            rest_client_cancel(client, runtime->logout_preempt_id);
        return true;
    }
    if (admitted != request_id)
        return false;
    return rest_client_cancel(client, request_id);
}

RestClientTakeResult rest_client_take_logout_response(
    RestClient* client, u64 request_id, RestClientResponse* response) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || !response || !request_id || runtime->user_auth_mode ||
        runtime->authorization != RestAuthorization::normal_user)
        return {0, RestClientTakeError::invalid_input};
    if (InterlockedCompareExchange(&client->request_state, 0, 0) != request_state_ready ||
        static_cast<u64>(InterlockedCompareExchange64(&runtime->logout_request_id, 0, 0)) != request_id ||
        runtime->completed.request_id != request_id ||
        runtime->completed.diagnostics.route_id != kAccountLogoutRouteId ||
        runtime->completed.diagnostics.method != RestMethod::post)
        return {0, RestClientTakeError::empty};
    *response = runtime->completed;
    const u32 bytes = runtime->completed.body_bytes;
    release_completed_response(client, runtime, bytes);
    return {bytes, RestClientTakeError::none};
}

RestClientTakeResult rest_client_consume_user_auth_response(
    RestClient* client, RestClientResponse* response,
    RestUserAuthConsumeFunction consumer, void* context) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || !runtime->user_auth_mode || !response || !consumer) {
        return RestClientTakeResult{0, RestClientTakeError::invalid_input};
    }
    if (InterlockedCompareExchange(&client->request_state, 0, 0) != request_state_ready) {
        return RestClientTakeResult{0, RestClientTakeError::empty};
    }
    const u32 body_bytes = runtime->completed.body_bytes;
    if (!consumer(context, runtime->completed, ConstSlice<u8>{runtime->response_body, body_bytes})) {
        return RestClientTakeResult{body_bytes, RestClientTakeError::consumer_rejected};
    }
    *response = runtime->completed;
    release_completed_response(client, runtime, body_bytes);
    return RestClientTakeResult{body_bytes, RestClientTakeError::none};
}

HANDLE rest_client_notification_handle(const RestClient* client) noexcept {
    return client ? client->response_event : nullptr;
}

RestClientResult rest_client_status(const RestClient* client) noexcept {
    return client_result(client);
}

static void capture_user_auth_stop_receipt(RestClient* client, RestClientResponse* receipt) noexcept {
    if (!receipt || !client || !client->runtime ||
        InterlockedCompareExchange(&client->request_state, 0, 0) != request_state_ready)
        return;
    const auto* runtime = static_cast<const RestClientRuntime*>(client->runtime);
    if (!runtime->user_auth_mode || !runtime->completed.transport_ok() ||
        runtime->completed.status_code != 429)
        return;
    receipt->request_id = runtime->completed.request_id;
    receipt->status_code = 429;
    receipt->retry_at_ms = runtime->completed.retry_at_ms;
}

static RestClientResult stop_client(RestClient* client, u32 timeout_ms,
                                    RestClientResponse* receipt) noexcept {
    if (receipt)
        reset_value(receipt);
    if (!client || !client->worker.thread)
        return invalid_result();
    if (client->shared) {
        AcquireSRWLockExclusive(client->shared->lock.native());
        for (auto*& peer : client->shared->clients)
            if (peer == client)
                peer = nullptr;
        ReleaseSRWLockExclusive(client->shared->lock.native());
    }
    if (WaitForSingleObject(client->worker.thread, 0) == WAIT_TIMEOUT) {
        network_worker_request_stop(&client->worker);
    }
    const NetworkWorkerResult joined = network_worker_join(&client->worker, timeout_ms);
    RestClientResult value = client_result(client);
    // The worker may finish publishing after Stop begins, while join waits.
    // Read only a release-published ready receipt, before any storage cleanup.
    capture_user_auth_stop_receipt(client, receipt);
    if (!joined.ok()) {
        value.error = RestClientError::worker;
        value.native_error = joined.native_error;
        return value;
    }
    if (client->response_event) {
        CloseHandle(client->response_event);
    }
    destroy_client_storage(client, nullptr);
    reset_value<RestClient>(client);
    value.phase = value.error == RestClientError::none ? RestClientPhase::stopped : RestClientPhase::failed;
    return value;
}

RestClientResult rest_client_stop(RestClient* client, u32 timeout_ms) noexcept {
    return stop_client(client, timeout_ms, nullptr);
}

RestClientResult rest_client_stop_user_auth(RestClient* client, u32 timeout_ms,
                                            RestClientResponse* receipt) noexcept {
    return stop_client(client, timeout_ms, receipt);
}

u32 rest_client_runtime_bytes(u32 response_capacity) noexcept {
    return allocation_bytes(response_capacity, false, false);
}

u32 rest_client_user_runtime_bytes(u32 response_capacity) noexcept {
    return allocation_bytes(response_capacity, false, true);
}

u32 rest_client_user_auth_capacity_runtime_bytes(u32 response_capacity, u32 request_capacity) noexcept {
    const u32 ordinary = allocation_bytes(response_capacity, true, false);
    const u32 secure = user_auth_secure_bytes(response_capacity, request_capacity);
    return ordinary != 0 && secure != 0 && secure <= ~0u - ordinary ? ordinary + secure : 0;
}

u32 rest_client_user_auth_runtime_bytes(u32 response_capacity) noexcept {
    return rest_client_user_auth_capacity_runtime_bytes(response_capacity,
                                                        kUserAuthMaximumRequestBodyBytes);
}

} // namespace maicord
