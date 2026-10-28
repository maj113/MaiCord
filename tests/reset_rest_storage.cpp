// Offline allocation fixture. It inspects the actual REST owner implementation
// (included by rest_client_probe.hpp) for its private allocation/layout and
// exercises its real erasure helpers. Part of rest_client_fixtures.cpp.
#include "rest_client_probe.hpp"
#include "maicord/sys/process_info.hpp"
#include "maicord/sys/winsock.hpp"

namespace maicord {
namespace {
u32 storage_failures;
u32 storage_normal_owner_commit;
u32 storage_normal_secret_commit;
RestClient storage_restart_owner_snapshot;
// Snapshot the complete page-rounded owner, including its immutable metadata.
u8 storage_restart_runtime_snapshot[(sizeof(RestClientRuntime) + sizeof(RestLocalRates) +
                                     kHandshakeOutputBytes + kUserClientMetadataBytes + 4095) &
                                    ~usize{4095}];
u8 storage_restart_secret_snapshot[user_auth_secure_fixed_storage_bytes(kUserAuthResetRequestBodyBytes)];
u8 storage_metadata_snapshot[kUserClientMetadataBytes];

void storage_check(bool condition, const char* name) noexcept {
    if (condition) return;
    ++storage_failures;
    DWORD written = 0;
    const u32 bytes = c_string_length(name);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), name, bytes, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &written, nullptr);
}

bool storage_zero(const u8* data, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes; ++i)
        if (data[i]) return false;
    return true;
}

void storage_fill(u8* data, u32 bytes, u8 value) noexcept {
    for (u32 i = 0; i < bytes; ++i) data[i] = value;
}

u32 storage_committed(const void* address, u32 expected) noexcept {
    const usize base = reinterpret_cast<usize>(address);
    u32 observed = 0;
    while (observed < expected) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(base + observed), &info, sizeof(info)) != sizeof(info) ||
            info.AllocationBase != address || info.State != MEM_COMMIT ||
            info.Protect != PAGE_READWRITE || info.Type != MEM_PRIVATE ||
            reinterpret_cast<usize>(info.BaseAddress) != base + observed ||
            !info.RegionSize || info.RegionSize > expected - observed) return 0;
        observed += static_cast<u32>(info.RegionSize);
    }
    return observed;
}

bool storage_locked(const u8* data, u32 bytes) noexcept {
    for (u32 offset = 0; offset < bytes; offset += 4096) {
        PSAPI_WORKING_SET_EX_INFORMATION page{};
        page.VirtualAddress = const_cast<u8*>(data + offset);
        if (!QueryWorkingSetEx(GetCurrentProcess(), &page, sizeof(page)) ||
            !page.VirtualAttributes.Valid || !page.VirtualAttributes.Locked) return false;
    }
    return true;
}

bool storage_freed(usize base, u32 bytes) noexcept {
    for (u32 offset = 0; offset < bytes; offset += 4096) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(base + offset), &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_FREE || info.AllocationBase) return false;
    }
    return true;
}

bool storage_offline(const RestClient& client, const RestClientRuntime& runtime) noexcept {
    return InterlockedCompareExchange64(const_cast<volatile LONG64*>(&client.connections), 0, 0) == 0 &&
        InterlockedCompareExchange64(const_cast<volatile LONG64*>(&client.requests), 0, 0) == 0 &&
        !runtime.transport.resolver_started && !runtime.transport.resolver.pending &&
        !runtime.transport.tls_initialized && runtime.transport.outstanding_operations == 0 &&
        runtime.transport.connection.socket == INVALID_SOCKET;
}

void storage_invalid_start(RestClient* client, u32 response, u32 request) noexcept {
    auto* runtime = static_cast<RestClientRuntime*>(client->runtime);
    const auto old_runtime = client->runtime;
    const auto old_thread = client->worker.thread;
    const auto old_event = client->response_event;
    const auto old_secret = runtime->secret_storage.data;
    const u32 old_capacity = runtime->secret_storage.capacity;
    runtime->request_body[0] = 0xA7;
    const auto result = rest_client_start_user_auth_capacity(client, response, request);
    storage_check(result.error == RestClientError::invalid_input, "invalid profile must reject");
    storage_check(client->runtime == old_runtime && client->worker.thread == old_thread &&
        client->response_event == old_event, "invalid profile must preserve active handles");
    if (client->runtime == old_runtime) {
        storage_check(runtime->secret_storage.data == old_secret &&
            runtime->secret_storage.capacity == old_capacity && runtime->request_body[0] == 0xA7,
            "invalid profile must preserve active storage");
        runtime->request_body[0] = 0;
    }
}

void storage_valid_restart(RestClient* client, u32 request_capacity) noexcept {
    auto* runtime = static_cast<RestClientRuntime*>(client->runtime);
    const u32 owner_bytes = runtime->allocation_bytes;
    const u32 secret_bytes = runtime->secret_storage.capacity;
    storage_check(owner_bytes <= sizeof(storage_restart_runtime_snapshot) &&
        secret_bytes <= sizeof(storage_restart_secret_snapshot), "restart snapshot capacity covers actual allocation");
    if (owner_bytes > sizeof(storage_restart_runtime_snapshot) || secret_bytes > sizeof(storage_restart_secret_snapshot))
        return;
    runtime->request_body[0] = 0xA7;
    runtime->request_body[runtime->request_body_capacity - 1] = 0x4B;
    runtime->secret_storage.data[secret_bytes - 1] = 0xD9;
    copy_bytes(&storage_restart_owner_snapshot, client, sizeof(*client));
    copy_bytes(storage_restart_runtime_snapshot, runtime, owner_bytes);
    copy_bytes(storage_restart_secret_snapshot, runtime->secret_storage.data, secret_bytes);
    const LONG serial_before = InterlockedCompareExchange(&g_trace_serial, 0, 0);
    const auto result = rest_client_start_user_auth_capacity(client,
        kUserAuthMaximumResponseBytes, request_capacity);
    storage_check(result.error == RestClientError::invalid_input,
        "valid-capacity restart on live owner must reject");
    storage_check(bytes_equal(&storage_restart_owner_snapshot, client, sizeof(*client)),
        "valid-capacity restart preserves complete owner and all handles");
    storage_check(InterlockedCompareExchange(&g_trace_serial, 0, 0) == serial_before,
        "valid-capacity restart creates no second runtime or worker identity");
    // Exit on an ownership regression before dereferencing possibly replaced
    // storage or allowing the old worker to outlive this stack owner.
    if (client->runtime != storage_restart_owner_snapshot.runtime ||
        client->worker.thread != storage_restart_owner_snapshot.worker.thread) ExitProcess(1);
    storage_check(bytes_equal(storage_restart_runtime_snapshot, runtime, owner_bytes) &&
        bytes_equal(storage_restart_secret_snapshot, runtime->secret_storage.data, secret_bytes) &&
        storage_offline(*client, *runtime),
        "valid-capacity restart preserves entire runtime and protected storage without network work");
    runtime->request_body[0] = 0;
    runtime->request_body[runtime->request_body_capacity - 1] = 0;
    runtime->secret_storage.data[secret_bytes - 1] = 0;
    SecureZeroMemory(storage_restart_runtime_snapshot, owner_bytes);
    SecureZeroMemory(storage_restart_secret_snapshot, secret_bytes);
    SecureZeroMemory(&storage_restart_owner_snapshot, sizeof(storage_restart_owner_snapshot));
}

bool storage_consume(void* context, const RestClientResponse& response,
                     ConstSlice<u8> body) noexcept {
    const auto* runtime = static_cast<RestClientRuntime*>(context);
    return response.request_id == 101 && body.data == runtime->request_body && body.count == 19 &&
        body.data[0] == 0x37 && body.data[18] == 0x37;
}

void storage_erasure(RestClient* client, RestClientRuntime* runtime) noexcept {
    // Exercise a full profile-sized plaintext commit with the real callback.
    // This is an offline synthetic transaction, never a submitted request.
    storage_fill(runtime->request_body, runtime->request_body_capacity, 0x5D);
    runtime->transaction.phase = RestTransactionPhase::sending;
    runtime->transaction.request_bytes = runtime->request_body_capacity;
    request_plaintext_committed(runtime, {runtime->request_body, runtime->request_body_capacity});
    storage_check(!runtime->write_commit_failed &&
        runtime->transaction.committed_bytes == runtime->request_body_capacity &&
        storage_zero(runtime->request_body, runtime->request_body_capacity), "entire committed request prefix erased");
    runtime->transaction = {};

    // The ready-response publication must preserve the request/response alias.
    storage_fill(runtime->response_body, 19, 0x37);
    storage_fill(runtime->request_head, runtime->request_head_capacity, 0x6B);
    runtime->command.request_id = 101;
    runtime->command.body_bytes = runtime->request_body_capacity;
    runtime->command.head_bytes = runtime->request_head_capacity;
    InterlockedExchange64(&client->current_request_id, 101);
    InterlockedExchange(&client->request_state, request_state_active);
    RestClientResponse complete{};
    complete.body_bytes = 19;
    complete.status_code = 200;
    publish_completed(client, runtime, complete);
    storage_check(runtime->response_body[0] == 0x37 && runtime->response_body[18] == 0x37 &&
        storage_zero(runtime->request_head, runtime->request_head_capacity), "publication preserves aliased response and erases head");
    storage_check(!rest_client_cancel(client, 101) && runtime->response_body[0] == 0x37,
        "ready response cannot be reclaimed by cancellation");
    RestClientResponse response{};
    const auto consumed = rest_client_consume_user_auth_response(client, &response, storage_consume, runtime);
    storage_check(consumed.ok() && consumed.body_bytes == 19 &&
        storage_zero(runtime->response_body, runtime->request_body_capacity) &&
        storage_zero(runtime->response_input, kRestMaximumHeaderBytes) &&
        InterlockedCompareExchange(&client->request_state, 0, 0) == request_state_empty &&
        InterlockedCompareExchange64(&client->current_request_id, 0, 0) == 0,
        "consumption erases response and releases request state");

    // A cancellation claim wins over a synthetic successful publication.
    storage_fill(runtime->request_body, runtime->request_body_capacity, 0x7E);
    runtime->command.request_id = 102;
    runtime->command.body_bytes = runtime->request_body_capacity;
    InterlockedExchange64(&client->current_request_id, 102);
    InterlockedExchange(&client->request_state, request_state_pending);
    storage_check(rest_client_cancel(client, 102) &&
        InterlockedCompareExchange(&client->request_state, 0, 0) == request_state_cancel_pending,
        "public cancellation atomically claims synthetic pending state without a network signal");
    publish_completed(client, runtime, complete);
    storage_check(runtime->completed.error == RestClientError::cancelled &&
        runtime->completed.request_id == 102 && runtime->completed.body_bytes == 0 &&
        storage_zero(runtime->request_body, runtime->request_body_capacity),
        "cancellation publication erases full unsent request");
    release_completed_response(client, runtime, 0);

    storage_fill(runtime->secret_storage.data, runtime->secret_storage.capacity, 0xC3);
    secure_buffer_clear(&runtime->secret_storage);
    storage_check(storage_zero(runtime->secret_storage.data, runtime->secret_storage.capacity),
        "clear erases every protected byte including unused tail");
}

void storage_profile(u32 request_capacity, bool legacy, bool invalid_cases) noexcept {
    RestClient client{};
    const u32 response_capacity = kUserAuthMaximumResponseBytes;
    const auto started = legacy ? rest_client_start_user_auth(&client, response_capacity) :
        rest_client_start_user_auth_capacity(&client, response_capacity, request_capacity);
    storage_check(started.ok() && client.runtime && client.worker.thread, "real offline owner startup");
    if (!started.ok() || !client.runtime || !client.worker.thread) return;
    const u64 idle_deadline = GetTickCount64() + 5000;
    while (load_phase(&client) == RestClientPhase::starting && GetTickCount64() < idle_deadline)
        Sleep(1);
    storage_check(load_phase(&client) == RestClientPhase::idle, "worker completed idle startup before helper cases");
    if (load_phase(&client) != RestClientPhase::idle) {
        rest_client_stop(&client, 5000);
        return;
    }
    auto* runtime = static_cast<RestClientRuntime*>(client.runtime);
    const u32 secure_expected = user_auth_secure_bytes(response_capacity, request_capacity);
    const u32 owner_observed = storage_committed(runtime, runtime->allocation_bytes);
    const u32 secure_observed = storage_committed(runtime->secret_storage.data, secure_expected);
    const u32 reported = rest_client_user_auth_capacity_runtime_bytes(response_capacity, request_capacity);
    const u32 prior_owner_bytes = page_rounded_bytes(
        static_cast<u64>(sizeof(RestClientRuntime)) + sizeof(RestLocalRates) + kHandshakeOutputBytes);
    const u32 metadata_owner_bytes = page_rounded_bytes(
        static_cast<u64>(sizeof(RestClientRuntime)) + sizeof(RestLocalRates) +
        kUserClientMetadataBytes + kHandshakeOutputBytes);
    storage_check(owner_observed && secure_observed && reported == owner_observed + secure_observed,
        "reported runtime bytes equal actual VirtualQuery committed bytes");
    storage_check(owner_observed == metadata_owner_bytes && owner_observed >= prior_owner_bytes &&
        owner_observed - prior_owner_bytes <= 4096,
        "auth metadata reuses ordinary tail padding with at most one boundary page and no new allocation");
    if (request_capacity == kUserAuthMaximumRequestBodyBytes && !legacy) {
        storage_normal_owner_commit = owner_observed;
        storage_normal_secret_commit = secure_observed;
    } else {
        storage_check(owner_observed == storage_normal_owner_commit &&
            secure_observed == storage_normal_secret_commit +
                (legacy ? 0u : kUserAuthResetRequestBodyBytes - kUserAuthMaximumRequestBodyBytes),
            "observed reset growth is scoped and subsequent legacy owner stays unchanged");
    }
    storage_check(runtime->request_body_capacity == request_capacity && runtime->response_capacity == response_capacity &&
        runtime->secret_storage.locked && runtime->secret_storage.capacity == secure_expected &&
        storage_zero(runtime->secret_storage.data, secure_expected) && storage_locked(runtime->secret_storage.data, secure_expected),
        "correct capacity and every initial protected page is zero and locked");
    const u8* base = runtime->secret_storage.data;
    storage_check(runtime->request_body == base && runtime->response_body == base &&
        runtime->encrypted_storage == base + request_capacity &&
        runtime->record_output == base + request_capacity + kUserAuthEncryptedInputBytes &&
        runtime->request_head == base + request_capacity + kUserAuthEncryptedInputBytes + kUserAuthRecordOutputBytes &&
        runtime->response_input == runtime->request_head &&
        runtime->request_head + kRestMaximumHeaderBytes == base + secure_expected &&
        runtime->request_head_capacity == kUserAuthRequestHeadBytes,
        "real locked offsets are contiguous nonoverlapping with intentional aliases");
    const usize owner_begin = reinterpret_cast<usize>(runtime);
    const usize handshake_begin = reinterpret_cast<usize>(runtime->handshake_output);
    storage_check(handshake_begin >= owner_begin + sizeof(RestClientRuntime) + sizeof(RestLocalRates) &&
        handshake_begin + kHandshakeOutputBytes == owner_begin + owner_observed &&
        (handshake_begin & 4095u) == 0 &&
        runtime->transport.buffers.encrypted_input.data == runtime->encrypted_storage &&
        runtime->transport.buffers.encrypted_input.count == kUserAuthEncryptedInputBytes &&
        runtime->transport.buffers.record_output.data == runtime->record_output &&
        runtime->transport.buffers.record_output.count == kUserAuthRecordOutputBytes &&
        runtime->transport.buffers.handshake_output.data == runtime->handshake_output,
        "real TLS buffers point to their protected and ordinary partitions");
    storage_check(runtime->user_super_properties == reinterpret_cast<const u8*>(runtime) +
            sizeof(RestClientRuntime) + sizeof(RestLocalRates) &&
        runtime->user_locale == reinterpret_cast<const char*>(runtime->user_super_properties +
            kRestUserSuperPropertiesMaximumBytes) &&
        runtime->user_timezone == runtime->user_locale + kRestUserLocaleMaximumBytes &&
        runtime->user_accept_language == runtime->user_timezone + kRestUserTimezoneMaximumBytes &&
        reinterpret_cast<usize>(runtime->user_accept_language + kRestUserAcceptLanguageMaximumBytes) <=
            handshake_begin &&
        rest_user_headers_valid({{runtime->user_super_properties, runtime->user_super_properties_bytes},
                                 {runtime->user_locale, runtime->user_locale_bytes},
                                 {runtime->user_timezone, runtime->user_timezone_bytes},
                                 {runtime->user_accept_language, runtime->user_accept_language_bytes}}) &&
        runtime->user_locale_bytes && runtime->user_timezone_bytes <= kRestUserTimezoneMaximumBytes,
        "nonsecret metadata fits before aligned final handshake output outside locked credentials");
    copy_bytes(storage_metadata_snapshot, runtime->user_super_properties, sizeof(storage_metadata_snapshot));
    UserClientConfigurationStorage launch{};
    u8 property_scratch[kRestUserSuperPropertiesMaximumBytes]{};
    u8 expected_properties[kRestUserSuperPropertiesMaximumBytes]{};
    const bool launch_ready = user_client_configuration_initialize(&launch).ok();
    GatewayUserProperties signed_out = launch.configuration.properties;
    signed_out.client_app_state = utf8_literal("focused");
    const auto projected = write_rest_user_super_properties(signed_out,
        {property_scratch, sizeof(property_scratch)}, {expected_properties, sizeof(expected_properties)});
    storage_check(launch_ready && projected.ok() && projected.bytes == runtime->user_super_properties_bytes &&
        bytes_equal(expected_properties, runtime->user_super_properties, projected.bytes) &&
        utf8_equal({launch.discord_locale, launch.discord_locale_bytes},
            {runtime->user_locale, runtime->user_locale_bytes}) &&
        utf8_equal({launch.accept_language, launch.accept_language_bytes},
            {runtime->user_accept_language, runtime->user_accept_language_bytes}) &&
        utf8_equal({launch.timezone, launch.timezone_bytes}, {runtime->user_timezone, runtime->user_timezone_bytes}),
        "auth startup caches the same immutable launch properties locale and timezone as normal REST");
    // A bad cached projection must reject at the actual submit boundary before
    // queue publication. No valid POST enters this offline worker.
    const u32 locale_bytes = runtime->user_locale_bytes;
    runtime->user_locale_bytes = 0;
    runtime->request_body[0] = '{';
    runtime->request_body[1] = '}';
    SecureBuffer tiny_body{runtime->request_body, runtime->secret_storage.capacity, 2, true};
    const auto metadata_rejected = rest_client_submit_user_auth(&client,
        {&tiny_body, nullptr, nullptr, nullptr, 98, 0, UserAuthOperation::password});
    runtime->user_locale_bytes = locale_bytes;
    storage_check(metadata_rejected.error == RestClientSubmitError::invalid_input &&
        InterlockedCompareExchange(&client.request_state, 0, 0) == request_state_empty &&
        InterlockedCompareExchange64(&client.current_request_id, 0, 0) == 0 &&
        runtime->command.request_id == 0 &&
        storage_zero(runtime->request_body, runtime->request_body_capacity) &&
        storage_zero(runtime->request_head, runtime->request_head_capacity),
        "actual auth submit consumes cached metadata and rejects invalid projection without publication");
    if (invalid_cases) {
        char key[kUserAuthMaximumCaptchaKeyBytes];
        char rqtoken[kUserAuthMaximumCaptchaTokenBytes];
        char session[kUserAuthMaximumCaptchaSessionBytes];
        u8 properties[kRestUserSuperPropertiesMaximumBytes];
        char locale[kRestUserLocaleMaximumBytes];
        char timezone[kRestUserTimezoneMaximumBytes];
        char languages[kRestUserAcceptLanguageMaximumBytes];
        storage_fill(reinterpret_cast<u8*>(key), sizeof(key), 'A');
        storage_fill(reinterpret_cast<u8*>(rqtoken), sizeof(rqtoken), 'B');
        storage_fill(reinterpret_cast<u8*>(session), sizeof(session), 'C');
        storage_fill(properties, sizeof(properties), 'A');
        storage_fill(reinterpret_cast<u8*>(locale), sizeof(locale), 'a');
        storage_fill(reinterpret_cast<u8*>(timezone), sizeof(timezone), 'b');
        storage_fill(reinterpret_cast<u8*>(languages), sizeof(languages), 'c');
        const RestUserRequestHeaders maximum_metadata{{properties, sizeof(properties)},
            {locale, sizeof(locale)}, {timezone, sizeof(timezone)}, {languages, sizeof(languages)}};
        const UserAuthCaptchaHeaders maximum_captcha{{key, sizeof(key)}, {rqtoken, sizeof(rqtoken)},
            {session, sizeof(session)}};
        const auto maximum_head = user_auth_write_request_head(UserAuthOperation::remote_auth_ticket,
            kUserAuthResetRequestBodyBytes, maximum_captcha,
            {runtime->request_head, runtime->request_head_capacity}, &maximum_metadata);
        storage_check(maximum_head.ok() && maximum_head.bytes <= kUserAuthRequestHeadBytes &&
            runtime->request_head_capacity == 12 * 1024,
            "maximum metadata and complete CAPTCHA tuple fit the unchanged locked 12 KiB request head");
        SecureZeroMemory(runtime->request_head, runtime->request_head_capacity);
    }
    storage_check(storage_offline(client, *runtime), "startup performed no resolve connect TLS or REST request");
    storage_valid_restart(&client, kUserAuthMaximumRequestBodyBytes);
    storage_valid_restart(&client, kUserAuthResetRequestBodyBytes);
    // This public submit is rejected before publication; no valid POST is ever
    // queued. Its body view is wholly inside the real locked allocation.
    SecureBuffer oversized{runtime->secret_storage.data, runtime->secret_storage.capacity, request_capacity + 1, true};
    const auto rejected = rest_client_submit_user_auth(&client,
        RestUserAuthRequest{&oversized, nullptr, nullptr, nullptr, 99, 0, UserAuthOperation::reset_password});
    storage_check(rejected.error == RestClientSubmitError::body_capacity &&
        InterlockedCompareExchange(&client.request_state, 0, 0) == request_state_empty &&
        InterlockedCompareExchange64(&client.current_request_id, 0, 0) == 0 && runtime->command.request_id == 0,
        "oversized public submit rejects before publishing or copying");
    if (invalid_cases) {
        constexpr u32 invalid_requests[]{0, 1, 32 * 1024 - 1, 32 * 1024 + 1, 64 * 1024,
            72 * 1024 - 1, 72 * 1024 + 1, 128 * 1024, ~0u};
        for (u32 capacity : invalid_requests) {
            storage_check(rest_client_user_auth_capacity_runtime_bytes(response_capacity, capacity) == 0,
                "invalid request runtime query must be zero");
            storage_invalid_start(&client, response_capacity, capacity);
        }
        constexpr u32 invalid_responses[]{0, kRestClientMinimumResponseBytes - 1,
            kUserAuthMaximumResponseBytes + 1, ~0u};
        for (u32 capacity : invalid_responses) {
            storage_check(rest_client_user_auth_capacity_runtime_bytes(capacity, request_capacity) == 0,
                "invalid response runtime query must be zero");
            storage_invalid_start(&client, capacity, request_capacity);
        }
    }
    storage_erasure(&client, runtime);
    storage_check(bytes_equal(storage_metadata_snapshot, runtime->user_super_properties,
        sizeof(storage_metadata_snapshot)), "request erasure preserves immutable nonsecret auth metadata");
    storage_check(storage_offline(client, *runtime), "offline helper cases performed no network requests");
    TraceRecord measured("reset_rest_storage");
    measured.field("request_capacity", request_capacity);
    measured.field("owner_commit", owner_observed);
    measured.field("owner_before_metadata", prior_owner_bytes);
    measured.field("metadata_reserved", kUserClientMetadataBytes);
    measured.field("owner_metadata_growth", owner_observed - prior_owner_bytes);
    measured.field("secure_commit", secure_observed);
    measured.field("runtime_query", reported);
    DWORD written = 0;
    u32 text_bytes = 0;
    while (measured.text[text_bytes]) ++text_bytes;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), measured.text, text_bytes, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &written, nullptr);
    const usize secret_begin = reinterpret_cast<usize>(runtime->secret_storage.data);
    const auto stopped = rest_client_stop(&client, 5000);
    storage_check(stopped.ok() && !client.runtime && !client.worker.thread && !client.response_event,
        "real owner stop joins and releases handles");
    // A regressed stop must not return a stack owner while its thread is live.
    if (client.worker.thread) ExitProcess(1);
    storage_check(storage_freed(owner_begin, owner_observed) && storage_freed(secret_begin, secure_observed),
        "every former allocation page is MEM_FREE after real stop");
}
} // namespace

u32 reset_rest_storage_fixture() noexcept {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    storage_check(kUserAuthMaximumRequestBodyBytes == 32 * 1024 &&
        kUserAuthResetRequestBodyBytes == 72 * 1024, "normal and reset profiles stay scoped");
    const auto phone_verify = user_auth_route(UserAuthOperation::phone_verify);
    const auto phone_resend = user_auth_route(UserAuthOperation::phone_resend);
    storage_check(phone_verify.route_id && phone_resend.route_id && phone_verify.route_id != phone_resend.route_id &&
        phone_verify.route_id != user_auth_route(UserAuthOperation::mfa_sms).route_id &&
        phone_resend.route_id != user_auth_route(UserAuthOperation::mfa_sms_send).route_id &&
        phone_verify.route_id != user_auth_route(UserAuthOperation::recovery).route_id &&
        phone_resend.route_id != user_auth_route(UserAuthOperation::reset_password).route_id,
        "phone verify/resend keep distinct local rate identities from recovery/reset/MFA operations");
    const u32 baseline = rest_client_user_auth_runtime_bytes(kUserAuthMaximumResponseBytes);
    storage_check(baseline == rest_client_user_auth_capacity_runtime_bytes(
        kUserAuthMaximumResponseBytes, kUserAuthMaximumRequestBodyBytes), "legacy query retains 32 KiB profile");
    storage_check(rest_client_user_auth_capacity_runtime_bytes(kUserAuthMaximumResponseBytes,
        kUserAuthResetRequestBodyBytes) == baseline + 40 * 1024, "reset grows only its protected request partition");
    storage_profile(kUserAuthMaximumRequestBodyBytes, false, true);
    storage_profile(kUserAuthResetRequestBodyBytes, false, false);
    storage_profile(kUserAuthMaximumRequestBodyBytes, true, false);
    storage_check(baseline == rest_client_user_auth_runtime_bytes(kUserAuthMaximumResponseBytes),
        "reset startup leaves normal allocation query unchanged");
    return storage_failures;
}
} // namespace maicord
