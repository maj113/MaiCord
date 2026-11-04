#include "rest_client_internal.hpp"
#include "maicord/app/account/account_logout.hpp"
#include "maicord/app/account/user_client_configuration.hpp"
#include "maicord/app/account/client_session_metadata.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/protocol/auth/user_auth.hpp"

namespace maicord {
namespace rest_client_detail {

RestTraceFunction g_trace = nullptr;
volatile LONG g_trace_serial = 0;

} // namespace rest_client_detail
namespace {

inline constexpr u32 kRequestHeadBytes = 4 * 1024;
inline constexpr u32 kNormalUserRequestHeadBytes = 28 * 1024;
inline constexpr u32 kUserAuthRequestHeadBytes = 12 * 1024;
static_assert(kRequestHeadBytes <= kRestMaximumHeaderBytes &&
              kNormalUserRequestHeadBytes <= kRestMaximumHeaderBytes &&
              kUserAuthRequestHeadBytes <= kRestMaximumHeaderBytes);

inline constexpr u32 kUserClientMetadataBytes =
    kRestUserSuperPropertiesMaximumBytes + kRestUserLocaleMaximumBytes + kRestUserTimezoneMaximumBytes +
    kRestUserAcceptLanguageMaximumBytes;

// How long the client holds an endpoint after a 503 that names no usable Retry-After.
inline constexpr u64 kServiceUnavailableHoldMs = 5000;

} // namespace
namespace rest_client_detail {

RestEndpointKey endpoint_key(ConstSlice<u8> target) noexcept {
    if (!target.data || !target.count)
        return {};
    u32 bytes = 0;
    while (bytes < target.count && target[bytes] != '?')
        ++bytes;
    return {hash64_counted_bytes(kHash64Seed, target.data, bytes),
            hash64_counted_bytes(~kHash64Seed, target.data, bytes), bytes};
}

} // namespace rest_client_detail
namespace {

RestEndpointGateEntry* endpoint_entry(RestEndpointGate* gate, RestEndpointLease lease) noexcept {
    if (!gate || !lease.generation || lease.index >= kRestEndpointGateCapacity)
        return nullptr;
    auto& entry = gate->entries[lease.index];
    return entry.generation == lease.generation && entry.in_flight ? &entry : nullptr;
}

} // namespace
namespace rest_client_detail {

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

} // namespace rest_client_detail
namespace {

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

} // namespace
namespace rest_client_detail {

bool endpoint_gate_release(RestEndpointGate* gate, RestEndpointLease* lease, u64 now_ms,
                           bool settled) noexcept {
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

} // namespace rest_client_detail
namespace {

struct RestLocalRates {
    RestRateScheduler scheduler;
    RestRateBucket buckets[kRateBucketCount];
    RestRateBinding bindings[kRateBindingCount];
    RestEndpointGate endpoint_gate;
};

} // namespace
namespace rest_client_detail {

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
    return static_cast<AccountRequirement>(
        read_acquire(client->shared ? &client->shared->required_action : &client->required_action));
}

} // namespace rest_client_detail
namespace {

constexpr u32 ordinary_fixed_storage_bytes(bool normal_user) noexcept {
    return kRestClientMaximumTargetBytes + kEncryptedInputBytes + kHandshakeOutputBytes +
           kRecordOutputBytes + kRestMaximumHeaderBytes + (normal_user ? kUserClientMetadataBytes : 0);
}

constexpr u32 user_auth_secure_fixed_storage_bytes(u32 request_capacity) noexcept {
    return request_capacity + kUserAuthEncryptedInputBytes +
           kUserAuthRecordOutputBytes + kRestMaximumHeaderBytes;
}

inline u32 page_rounded_bytes(u64 raw) noexcept {
    const u64 rounded = (raw + 4095ull) & ~4095ull;
    return rounded <= ~0u ? static_cast<u32>(rounded) : 0;
}

} // namespace
namespace rest_client_detail {

u32 user_auth_secure_bytes(u32 response_capacity, u32 request_capacity) noexcept {
    if (response_capacity < kRestClientMinimumResponseBytes ||
        response_capacity > kUserAuthMaximumResponseBytes ||
        (request_capacity != kUserAuthMaximumRequestBodyBytes &&
         request_capacity != kUserAuthResetRequestBodyBytes)) {
        return 0;
    }
    return page_rounded_bytes(user_auth_secure_fixed_storage_bytes(request_capacity));
}

u32 allocation_bytes(u32 response_capacity, bool user_auth_mode, bool normal_user, bool shared) noexcept {
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

// The four user-header buffers, laid out back to back from cursor.
static void assign_user_metadata(RestClientRuntime* runtime, u8* cursor) noexcept {
    runtime->user_super_properties = cursor;
    cursor += kRestUserSuperPropertiesMaximumBytes;
    runtime->user_locale = reinterpret_cast<char*>(cursor);
    cursor += kRestUserLocaleMaximumBytes;
    runtime->user_timezone = reinterpret_cast<char*>(cursor);
    cursor += kRestUserTimezoneMaximumBytes;
    runtime->user_accept_language = reinterpret_cast<char*>(cursor);
}

void assign_storage(RestClientRuntime* runtime, RestSharedState* shared) noexcept {
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
        assign_user_metadata(runtime, cursor);
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
    if (runtime->authorization == RestAuthorization::normal_user)
        assign_user_metadata(runtime, cursor);
}

} // namespace rest_client_detail
namespace {

RestClientPhase load_phase(const RestClient* client) noexcept {
    return static_cast<RestClientPhase>(
        read_acquire(&client->phase));
}

RestClientError load_error(const RestClient* client) noexcept {
    return static_cast<RestClientError>(
        read_acquire(&client->error));
}

} // namespace
namespace rest_client_detail {

void publish_phase(RestClient* client, RestClientPhase phase) noexcept {
    InterlockedExchange(&client->phase, static_cast<LONG>(phase));
}

} // namespace rest_client_detail
namespace {

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

} // namespace
namespace rest_client_detail {

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
    line.field("blocking_request", static_cast<u64>(read_acquire(&client->current_request_id)));
    line.field("blocking_phase", phase_name(load_phase(client)));
    if (client->shared) {
        for (auto* peer : client->shared->clients) if (peer && peer != client) {
            line.field("peer_request", static_cast<u64>(read_acquire(&peer->current_request_id)));
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

bool valid_user_configuration(const RestUserConfiguration& configuration) noexcept {
    return rest_user_headers_valid(
        {configuration.super_properties, configuration.locale, configuration.timezone, configuration.accept_language});
}

bool copy_user_configuration(RestClientRuntime* runtime,
                             const RestUserConfiguration& configuration) noexcept {
    if (!runtime->user_super_properties || !runtime->user_locale || !runtime->user_timezone ||
        !valid_user_configuration(configuration) ||
        !copy_bytes(runtime->user_super_properties, kRestUserSuperPropertiesMaximumBytes,
                    configuration.super_properties.data, configuration.super_properties.count) ||
        !copy_bytes(runtime->user_locale, kRestUserLocaleMaximumBytes, configuration.locale.data,
                    configuration.locale.bytes) ||
        !copy_bytes(runtime->user_timezone, kRestUserTimezoneMaximumBytes, configuration.timezone.data,
                    configuration.timezone.bytes) ||
        !copy_bytes(runtime->user_accept_language, kRestUserAcceptLanguageMaximumBytes,
                    configuration.accept_language.data, configuration.accept_language.bytes)) {
        return false;
    }
    runtime->user_super_properties_bytes = configuration.super_properties.count;
    runtime->user_locale_bytes = configuration.locale.bytes;
    runtime->user_timezone_bytes = configuration.timezone.bytes;
    runtime->user_accept_language_bytes = configuration.accept_language.bytes;
    runtime->metadata = configuration.metadata;
    return true;
}

// The account's cookie jar: normal-user requests send and store cookies.
CookieJar* request_cookies(const RestClientRuntime* runtime) noexcept {
    return !runtime->user_auth_mode && runtime->metadata ? runtime->metadata->cookies : nullptr;
}

CookieRequest cookie_request(const RestClientRuntime* runtime) noexcept {
    return {utf8_literal("discord.com"),
            {reinterpret_cast<const char*>(runtime->request_target), runtime->command.target_bytes}, true};
}

} // namespace rest_client_detail
namespace {

// The super properties and locale as the live metadata reports them for this request.
void refresh_user_metadata(RestClientRuntime* runtime) noexcept {
    if (!runtime->metadata)
        return;
    const u32 bytes = client_session_metadata_request(
        runtime->metadata, clock_unix_milliseconds(), &runtime->metadata_revision,
        {runtime->user_super_properties, kRestUserSuperPropertiesMaximumBytes},
        {runtime->user_locale, kRestUserLocaleMaximumBytes}, &runtime->user_locale_bytes);
    if (bytes)
        runtime->user_super_properties_bytes = bytes;
}

} // namespace
namespace rest_client_detail {

bool initialize_auth_user_configuration(RestClientRuntime* runtime) noexcept {
    UserClientConfigurationStorage storage{};
    if (!user_client_configuration_initialize(&storage).ok())
        return false;
    // Sign-in happens in the focused window, before any heartbeat session.
    GatewayUserProperties signed_out = storage.configuration.properties;
    signed_out.client_app_state = utf8_literal("focused");
    u8 scratch[kRestUserSuperPropertiesMaximumBytes];
    const auto properties = write_rest_user_super_properties(
        signed_out, {scratch, sizeof(scratch)},
        {runtime->user_super_properties, kRestUserSuperPropertiesMaximumBytes});
    if (!properties.ok())
        return false;
    return copy_user_configuration(runtime, {{runtime->user_super_properties, properties.bytes},
                                             {storage.discord_locale, storage.discord_locale_bytes},
                                             {storage.timezone, storage.timezone_bytes},
                                             nullptr,
                                             {storage.accept_language, storage.accept_language_bytes}});
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

void erase_command(RestClientRuntime* runtime, bool erase_body) noexcept {
    secure_clear_bytes(runtime->request_target, runtime->command.target_bytes);
    if (erase_body)
        secure_clear_bytes(runtime->request_body, runtime->command.body_bytes);
    secure_clear_bytes(runtime->request_head, runtime->command.head_bytes);
    secure_clear_bytes(runtime->audit_reason, runtime->command.audit_reason_bytes);
    secure_clear_bytes(runtime->extra_headers, runtime->command.extra_headers_bytes);
    secure_clear_bytes(runtime->multipart_boundary, runtime->command.multipart_boundary_bytes);
    reset_value<RestClientCommand>(&runtime->command);
}

} // namespace rest_client_detail
namespace {

void erase_response_storage(RestClientRuntime* runtime) noexcept {
    secure_clear_bytes(runtime->response_input, kRestMaximumHeaderBytes);
    if (runtime->response.http.body_bytes != 0) {
        secure_clear_bytes(runtime->response_body, runtime->response.http.body_bytes);
    }
    reset_value<RestResponse>(&runtime->response);
}

} // namespace
namespace rest_client_detail {

void release_decompression(RestClientRuntime* runtime) noexcept {
    if (runtime->decompression) {
        secure_clear_bytes(runtime->decompression, runtime->decompression_capacity);
        if (runtime->decompression != runtime->handshake_output)
            pages_release(runtime->decompression);
    }
    runtime->decompression = nullptr;
    runtime->decompression_capacity = 0;
}

// Caller owns writing/ready state or has joined the worker. Never release a
// bank while TLS still borrows an outgoing span or while a response is ready.
void release_extended_body(RestClientRuntime* runtime) noexcept {
    if (!runtime->extended_body) return;
    secure_clear_bytes(runtime->extended_body, runtime->extended_body_bytes);
    pages_release(runtime->extended_body);
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
    const auto logout = read_acquire(&runtime->logout_request_id);
    if (logout && read_acquire(&runtime->logout_reservation_id) == logout) {
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

// Connects afresh once an idle connection is old enough for the edge to have
// closed it (see kTlsStreamIdleReuseMs).
void retire_idle_transport(RestClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->transport_ready &&
        now_ms >= deadline_after(runtime->transport_idle_since_ms, kTlsStreamIdleReuseMs))
        close_transport(runtime);
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

} // namespace rest_client_detail
namespace {

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
                                static_cast<u64>(read_acquire(&client->requests)),
                                static_cast<u64>(read_acquire(&client->responses)) +
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
        line.field("wire_bytes", runtime->wire_body_bytes);
        line.field("body_bytes", response.body_bytes);
        line.field("encoding", static_cast<u64>(runtime->wire_encoding));
        line.field("decode_us", runtime->decode_us);
        line.field("reused", static_cast<u64>(response.reused_connection));
        g_trace(line.text);
        runtime->wire_body_bytes = runtime->decode_us = 0;
        runtime->wire_encoding = RestContentEncoding::missing;
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
    increment_single_writer(&client->responses); // worker thread only
    publish_worker_phase(client, runtime, RestClientPhase::response_ready);
    runtime->trace_request_id = 0;
    runtime->ready_counter = clock_now();
    InterlockedExchange(&client->request_state, request_state_ready);
    SetEvent(client->response_event);
}

} // namespace
namespace rest_client_detail {

void publish_request_error(RestClient* client, RestClientRuntime* runtime,
                           RestClientError error, u32 native_error) noexcept {
    erase_response_storage(runtime);
    RestClientResponse response{};
    response.error = error;
    response.native_error = native_error;
    response.reused_connection = runtime->transaction.reused_connection;
    publish_completed(client, runtime, response);
}

} // namespace rest_client_detail
namespace {

bool drained(const RestClientRuntime* runtime) noexcept {
    return tls_stream_drained(const_cast<TlsStream*>(&runtime->transport));
}

} // namespace
namespace rest_client_detail {

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

} // namespace rest_client_detail
namespace {

// Retries a bodyless GET the way the client's `retries` option does: after 1 s, 2 s, 4 s, then 5 s (the mean of its
// jittered backoff from 500 ms capped at 5 s), counting the failed tries in X-Failed-Requests.
bool schedule_retry(RestClient* client, RestClientRuntime* runtime, u64 now_ms) noexcept {
    if (!runtime->command.retries_left || runtime->stopping)
        return false;
    --runtime->command.retries_left;
    ++runtime->command.failed_requests;
    erase_response_storage(runtime);
    reset_value<RestTransaction>(&runtime->transaction);
    runtime->waiting_for_peer = false;
    const u32 delay_ms = minimum(5000u, 500u << runtime->command.failed_requests);
    runtime->command.retry_not_before_ms = deadline_after(now_ms, delay_ms);
    runtime->rate_wait_until_ms = runtime->command.retry_not_before_ms;
    publish_worker_phase(client, runtime, RestClientPhase::rate_wait);
    return true;
}

// The failures the client retries: any but a cancellation, its own timeout or a local refusal.
bool retryable_transport_error(RestClientError error) noexcept {
    return error == RestClientError::resolve || error == RestClientError::connect || error == RestClientError::tls ||
           error == RestClientError::send || error == RestClientError::receive || error == RestClientError::response;
}

} // namespace
namespace rest_client_detail {

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
    const u64 now_ms = clock_monotonic_milliseconds();
    if (retryable_transport_error(error) && schedule_retry(client, runtime, now_ms))
        return NetworkWorkerStep{next_wait(runtime, now_ms), false};
    publish_request_error(client, runtime, error, native_error);
    return NetworkWorkerStep{INFINITE, false};
}

} // namespace rest_client_detail
namespace {

void request_plaintext_committed(void* opaque, ConstSlice<u8> committed) noexcept {
    auto* runtime = static_cast<RestClientRuntime*>(opaque);
    if (!rest_transaction_commit_plaintext(&runtime->transaction, committed.count)) {
        runtime->write_commit_failed = true;
    }
    if (committed.data && committed.count != 0) {
        secure_clear_bytes(const_cast<u8*>(committed.data), committed.count);
    }
}

} // namespace
namespace rest_client_detail {

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
        refresh_user_metadata(runtime);
        const ConstSlice<u8> token = runtime->authorization == RestAuthorization::none
                                         ? ConstSlice<u8>{nullptr, 0}
                                         : secure_buffer_view(runtime->credential);
        const RestUserRequestHeaders user_headers{
            ConstSlice<u8>{runtime->user_super_properties, runtime->user_super_properties_bytes},
            Utf8View{runtime->user_locale, runtime->user_locale_bytes},
            Utf8View{runtime->user_timezone, runtime->user_timezone_bytes},
            Utf8View{runtime->user_accept_language, runtime->user_accept_language_bytes}};
        const bool normal_user = runtime->authorization == RestAuthorization::normal_user;
        CookieJar* const cookies = normal_user ? request_cookies(runtime) : nullptr;
        const u32 cookie_bytes =
            cookies ? cookie_jar_header(cookies, cookie_request(runtime), clock_unix_milliseconds(),
                                        {runtime->cookie_header, kCookieHeaderMaximumBytes})
                    : 0;
        const RestRequestWriteResult written = write_rest_request_head(
            RestRequestHead{ConstSlice<u8>{runtime->request_target, runtime->command.target_bytes},
                            token, runtime->command.body_bytes, runtime->command.route.method,
                            runtime->authorization, runtime->command.json_body,
                            normal_user ? &user_headers : nullptr,
                            true,
                            {runtime->audit_reason, runtime->command.audit_reason_bytes},
                            {runtime->extra_headers, runtime->command.extra_headers_bytes},
                            {runtime->multipart_boundary, runtime->command.multipart_boundary_bytes},
                            runtime->command.send_empty_audit_reason,
                            {runtime->cookie_header, cookie_bytes}, runtime->command.failed_requests},
            Slice<u8>{runtime->request_head, runtime->request_head_capacity});
        secure_clear_bytes(runtime->cookie_header, cookie_bytes);
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
    increment_single_writer(&client->requests); // worker thread only
    if (runtime->transaction.reused_connection) {
        increment_single_writer(&client->reused_requests);
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

} // namespace rest_client_detail
namespace {

void prepare_response_metadata(RestClientRuntime* runtime,
                               const RestRateObservation& observation,
                               RestClientResponse* completed) noexcept {
    const RateLimitHeaders& rate = runtime->response.parsed_head.rate;
    completed->rate = {rate.reset_after_ms, rate.retry_after_ms, rate.fields, rate.remaining};
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
}

} // namespace
namespace rest_client_detail {

bool finalize_response(RestClient* client, RestClientRuntime* runtime,
                       const RestResponseStep& step, u64 now_ms) noexcept {
    if (!step.ok() || step.state != RestResponseState::complete || step.extra_bytes != 0) {
        return false;
    }
    const auto& head = runtime->response.parsed_head;
    if (head.head.status_code == 503) {
        // The client holds the endpoint for the Retry-After seconds, or five seconds without a usable one.
        const u64 hold_ms = (head.rate.fields & rate_limit_field_retry_after) && head.rate.retry_after_ms
                                ? head.rate.retry_after_ms
                                : kServiceUnavailableHoldMs;
        // A trusted delay remains authoritative even when decoding its body
        // fails. Terminal cleanup releases the reservation and wakes peers.
        RateLock guard(runtime);
        if (!endpoint_gate_observe(runtime->endpoint_gate, runtime->endpoint_lease, deadline_after(now_ms, hold_ms)))
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
                scratch = static_cast<u8*>(commit_pages(runtime->response_capacity));
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
    runtime->transport_idle_since_ms = now_ms;
    ++runtime->connection_request_count;
    if (!runtime->response.http.connection_reusable) {
        close_transport(runtime);
    }
    if (rest_retryable_status(completed.status_code) && schedule_retry(client, runtime, now_ms))
        return true;
    publish_completed(client, runtime, completed);
    return true;
}

} // namespace rest_client_detail
} // namespace maicord
