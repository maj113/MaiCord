// Actual shipping coordinator with a synchronous, non-network REST boundary.
#include "../src/app/user_auth_coordinator.cpp"
#include "test_output.hpp"

namespace maicord::phone_coordinator_test {
struct Fixture {
    SecureBuffer request, response, headers;
    RestClientResponse metadata;
    UserAuthOperation operation;
    u64 request_id;
    u32 submissions, notification_reads;
    bool ready, fail_stop, fail_submit, joined_error;
    RestClientPhase phase;
    RestClientError error;
} fixture{};
#include "user_auth_coordinator_test_support.inc"
}

namespace maicord {
// The production size-query entry shares the REST object with the transport.
// This fixture never measures that owner; keep its unused link boundary mocked
// so the real networking implementation cannot be pulled into the test.
u32 rest_client_user_auth_runtime_bytes(u32) noexcept { return 0; }
u32 rest_client_user_auth_capacity_runtime_bytes(u32, u32) noexcept { return 0; }
RestClientResult rest_client_start_user_auth_capacity(RestClient* client, u32 response, u32) noexcept {
    return rest_client_start_user_auth(client, response);
}
RestClientResult rest_client_start_user_auth(RestClient* client, u32) noexcept {
    client->runtime = &phone_coordinator_test::fixture;
    client->worker.thread = reinterpret_cast<HANDLE>(1);
    phone_coordinator_test::fixture.phase = RestClientPhase::idle;
    phone_coordinator_test::fixture.error = RestClientError::none;
    RestClientResult result{}; result.phase = RestClientPhase::idle; return result;
}
RestClientSubmitResult rest_client_submit_user_auth(
    RestClient*, const RestUserAuthRequest& value) noexcept {
    using namespace phone_coordinator_test;
    if (fixture.fail_submit) return {RestClientSubmitError::busy};
    if (!value.body || !value.body->locked || !secure_buffer_assign(&fixture.request, secure_buffer_view(*value.body)).ok())
        return {RestClientSubmitError::invalid_input};
    fixture.operation = value.operation; fixture.request_id = value.request_id; ++fixture.submissions;
    secure_buffer_clear(&fixture.headers);
    if (value.captcha_key) {
        const auto utf8 = [](const SecureBuffer* buffer) noexcept -> Utf8View {
            return buffer ? Utf8View{reinterpret_cast<const char*>(buffer->data), buffer->bytes} : Utf8View{};
        };
        const auto written = user_auth_write_captcha_headers(
            {utf8(value.captcha_key), utf8(value.captcha_rqtoken), utf8(value.captcha_session_id)},
            {fixture.headers.data, fixture.headers.capacity});
        if (!written.ok()) return {RestClientSubmitError::invalid_input};
        fixture.headers.bytes = written.bytes;
    }
    fixture.ready = false;
    return {RestClientSubmitError::none};
}
RestClientResult rest_client_status(const RestClient*) noexcept {
    RestClientResult result{};
    result.phase = phone_coordinator_test::fixture.ready ? RestClientPhase::response_ready : phone_coordinator_test::fixture.phase;
    result.error = phone_coordinator_test::fixture.error;
    return result;
}
RestClientTakeResult rest_client_consume_user_auth_response(
    RestClient*, RestClientResponse* response,
    RestUserAuthConsumeFunction consume, void* context) noexcept {
    using namespace phone_coordinator_test;
    *response = fixture.metadata;
    if (!consume(context, fixture.metadata, secure_buffer_view(fixture.response)))
        return {0, RestClientTakeError::invalid_input};
    fixture.ready = false;
    secure_buffer_clear(&fixture.response);
    return {response->body_bytes, RestClientTakeError::none};
}
HANDLE rest_client_notification_handle(const RestClient*) noexcept {
    ++phone_coordinator_test::fixture.notification_reads;
    return reinterpret_cast<HANDLE>(1);
}
RestClientResult rest_client_stop_user_auth(RestClient* client, u32 timeout_ms,
                                            RestClientResponse* receipt) noexcept {
    using namespace phone_coordinator_test;
    if (receipt) {
        *receipt = {};
        if (fixture.ready && fixture.metadata.transport_ok() && fixture.metadata.status_code == 429) {
            receipt->request_id = fixture.metadata.request_id;
            receipt->status_code = 429;
            receipt->retry_at_ms = fixture.metadata.retry_at_ms;
        }
    }
    return rest_client_stop(client, timeout_ms);
}
RestClientResult rest_client_stop(RestClient* client, u32) noexcept {
    using namespace phone_coordinator_test;
    RestClientResult result{};
    result.phase = fixture.fail_stop      ? RestClientPhase::draining
                   : fixture.joined_error ? RestClientPhase::failed
                                          : RestClientPhase::stopped;
    result.error = fixture.joined_error ? RestClientError::worker : RestClientError::none;
    if (!fixture.fail_stop) {
        *client = {}; fixture.ready = false;
        secure_buffer_clear(&fixture.request); secure_buffer_clear(&fixture.response); secure_buffer_clear(&fixture.headers);
    }
    return result;
}
void measure(const char* label, u32 value) noexcept {
    test_write_metric(label, value);
}
}

namespace maicord::phone_coordinator_test {
bool zero_range(const u8* data, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i) if (data[i]) return false;
    return true;
}
bool ready_phone(UserAuthCoordinator* coordinator, SecureBuffer* identifier) noexcept {
    constexpr auto original = utf8_literal("+1\"\\\xc3\xa9\n");
    if (!secure_buffer_assign(identifier, {reinterpret_cast<const u8*>(original.data), original.bytes}).ok() ||
        !user_auth_coordinator_start(coordinator).ok() ||
        !user_auth_coordinator_submit_recovery(coordinator, identifier).ok()) return false;
    secure_buffer_clear(identifier);
    auto* runtime = runtime_of(coordinator);
    if (!runtime || !body(utf8_literal("{\"code\":70007}"))) return false;
    response(400, runtime->active_request_id);
    UserAuthCoordinatorEvent event{};
    return user_auth_coordinator_pump(coordinator, &event).ok() &&
        event.kind == UserAuthCoordinatorEventKind::recovery_phone_required && runtime->phone_context_active &&
        runtime->continuation_base_bytes && !runtime->active_request_id && !runtime->token_bytes;
}
void run() noexcept {
    check(secure_buffer_initialize(&fixture.request, 32768).ok() &&
        secure_buffer_initialize(&fixture.response, 16384).ok() && secure_buffer_initialize(&fixture.headers, 16384).ok(),
        "phone coordinator: bounded locked synchronous transport fixture initializes");
    SecureBuffer identifier{}, code{}, solution{}, token{};
    check(secure_buffer_initialize(&identifier, 512).ok() && secure_buffer_initialize(&code, 16).ok() &&
        secure_buffer_initialize(&solution, 512).ok() && secure_buffer_initialize(&token, 4096).ok(),
        "phone coordinator: separate locked caller buffers initialize");
    constexpr auto digits = utf8_literal("001234");
    constexpr auto base = utf8_literal("{\"phone\":\"+1\\\"\\\\\xc3\xa9\\n\"}");
    constexpr auto exact = utf8_literal("{\"phone\":\"+1\\\"\\\\\xc3\xa9\\n\",\"code\":\"001234\"}");
    constexpr auto challenge = utf8_literal("{\"captcha_key\":[\"required\"],\"captcha_service\":\"recaptcha\",\"captcha_sitekey\":\"site\",\"captcha_rqtoken\":\"opaque\",\"captcha_session_id\":\"session\",\"should_serve_invisible\":true}");
    secure_buffer_assign(&code, {reinterpret_cast<const u8*>(digits.data), digits.bytes});
    constexpr auto solved = utf8_literal("synthetic-provider-solution");
    secure_buffer_assign(&solution, {reinterpret_cast<const u8*>(solved.data), solved.bytes});
    UserAuthCoordinator coordinator{}; UserAuthCoordinatorEvent event{}; AccountProfile profile{};
    check(ready_phone(&coordinator, &identifier), "phone coordinator: actual 70007 enters editable phone flow after caller identifier erased");
    auto* runtime = runtime_of(&coordinator);
    if (!runtime) return;
    MEMORY_BASIC_INFORMATION owner_pages{}, secret_pages{};
    check(VirtualQuery(runtime, &owner_pages, sizeof(owner_pages)) == sizeof(owner_pages) &&
        owner_pages.AllocationBase == runtime && owner_pages.State == MEM_COMMIT &&
        owner_pages.RegionSize == page_round(sizeof(UserAuthCoordinatorRuntime)) &&
        VirtualQuery(runtime->secrets.data, &secret_pages, sizeof(secret_pages)) == sizeof(secret_pages) &&
        secret_pages.AllocationBase == runtime->secrets.data && secret_pages.State == MEM_COMMIT &&
        secret_pages.RegionSize == kUserAuthCoordinatorSecretBytes,
        "phone coordinator: actual native metadata/locked arena commitment matches ordinary owner capacities");
    measure("MEASURE phone_coordinator_owner_bytes=", static_cast<u32>(sizeof(UserAuthCoordinatorRuntime)));
    measure("MEASURE phone_coordinator_owner_committed_bytes=", static_cast<u32>(owner_pages.RegionSize));
    measure("MEASURE phone_coordinator_secret_committed_bytes=", static_cast<u32>(secret_pages.RegionSize));
    measure("MEASURE phone_coordinator_request_capacity=", runtime->request_capacity);
    check(runtime->secrets.capacity == kUserAuthCoordinatorSecretBytes && runtime->request_capacity == 32768 &&
        runtime->continuation_base_bytes == base.bytes && bytes_equal(runtime->secrets.data, base.data, base.bytes) &&
        zero_range(runtime->secrets.data + base.bytes, kPhoneTokenOffset - base.bytes) &&
        zero_range(runtime->secrets.data + kPhoneTokenOffset, 4096),
        "phone coordinator: original exact encoded phone resides in existing 32 KiB request with reserved empty reset stage");
    u8 snapshot[65536]{}; copy_bytes(snapshot, runtime->secrets.data, sizeof(snapshot));
    const auto initial_generation = runtime->flow.generation;
    SecureBuffer alias{runtime->secrets.data + 10, 6, 6, true};
    check(!user_auth_coordinator_submit_phone_verify(&coordinator, &alias).ok() &&
        bytes_equal(snapshot, runtime->secrets.data, sizeof(snapshot)) && runtime->flow.generation == initial_generation,
        "phone coordinator: code alias rejection preserves every locked byte and generation");
    fixture.fail_submit = true;
    check(!user_auth_coordinator_submit_phone_verify(&coordinator, &code).ok() &&
        bytes_equal(snapshot, runtime->secrets.data, sizeof(snapshot)) && runtime->flow.generation == initial_generation,
        "phone coordinator: failed verify admission erases suffix and retains exact editable phone");
    fixture.fail_submit = false;
    check(user_auth_coordinator_submit_phone_verify(&coordinator, &code).ok() &&
        fixture.operation == UserAuthOperation::phone_verify && fixture.request.bytes == exact.bytes &&
        bytes_equal(fixture.request.data, exact.data, exact.bytes), "phone coordinator: exact verify body is copied synchronously");
    const auto old_request = runtime->active_request_id;
    body(challenge); response(400, old_request);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::captcha_required &&
        runtime->flow.captcha_return_phase == UserAuthPhase::phone_pending && runtime->retained_request_bytes == exact.bytes,
        "phone coordinator: verify CAPTCHA retains immutable phone and exact code operation");
    const auto verify_captcha = user_auth_coordinator_captcha_snapshot(&coordinator);
    check(verify_captcha.active && verify_captcha.provider == CaptchaProvider::recaptcha && verify_captcha.invisible &&
        verify_captcha.sitekey_bytes == 4 && verify_captcha.rqtoken_bytes == 6,
        "phone coordinator: verify challenge exposes its actual provider and invisible hint without copying secrets");
    check(user_auth_coordinator_submit_captcha(&coordinator, &solution).ok() && fixture.operation == UserAuthOperation::phone_verify &&
        fixture.request.bytes == exact.bytes && bytes_equal(fixture.request.data, exact.data, exact.bytes) &&
        runtime->active_request_id != old_request && fixture.headers.bytes && !runtime->captcha_rqtoken_bytes,
        "phone coordinator: challenge replay preserves exact verify route body with admitted new generation");
    check(!user_auth_coordinator_captcha_snapshot(&coordinator).active && !runtime->captcha_invisible,
        "phone coordinator: verify replay clears old provider projection and retained hint");
    const auto admitted = runtime->active_request_id;
    copy_bytes(snapshot, runtime->secrets.data, sizeof(snapshot));
    const auto saved_metadata = coordinator.state;
    body(utf8_literal("{\"token\":\"stale-never-stage\"}")); response(200, old_request);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::none &&
        bytes_equal(snapshot, runtime->secrets.data, sizeof(snapshot)) && same_metadata(saved_metadata, coordinator.state),
        "phone coordinator: stale request admission rejects before decoder or metadata writes");
    body(utf8_literal("{\"code\":50035}")); response(400, admitted);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::phone_rejected &&
        coordinator.state.phase == UserAuthCoordinatorPhase::phone_rejected && runtime->continuation_base_bytes == base.bytes &&
        bytes_equal(runtime->secrets.data, base.data, base.bytes) && zero_range(runtime->secrets.data + base.bytes, 32768 - base.bytes),
        "phone coordinator: code rejection restores phone and erases code challenge and token stage");
    const auto before_resend = fixture.submissions;
    check(user_auth_coordinator_submit_phone_resend(&coordinator).ok() && fixture.operation == UserAuthOperation::phone_resend &&
        coordinator.state.phase == UserAuthCoordinatorPhase::phone_resend_pending && fixture.request.bytes == base.bytes &&
        bytes_equal(fixture.request.data, base.data, base.bytes), "phone coordinator: only explicit resend submits original phone without code");
    const auto resend_request = runtime->active_request_id;
    body(utf8_literal(R"({"captcha_key":["required"],"captcha_service":"future-provider","captcha_sitekey":"resend","captcha_rqtoken":"new-opaque"})"));
    response(400, resend_request);
    user_auth_coordinator_pump(&coordinator, &event);
    const auto resend_captcha = user_auth_coordinator_captcha_snapshot(&coordinator);
    check(resend_captcha.active && resend_captcha.provider == CaptchaProvider::unknown && !resend_captcha.invisible &&
        resend_captcha.sitekey_bytes == 6 && resend_captcha.rqtoken_bytes == 10 && !resend_captcha.session_id_bytes,
        "phone coordinator: resend replaces verify metadata and missing invisible hint defaults false for unknown provider");
    check(user_auth_coordinator_submit_captcha(&coordinator, &solution).ok() && fixture.operation == UserAuthOperation::phone_resend &&
        fixture.request.bytes == base.bytes && bytes_equal(fixture.request.data, base.data, base.bytes),
        "phone coordinator: resend CAPTCHA replays resend rather than verify");
    body(utf8_literal("{}")); const u64 due = GetTickCount64() + 60000;
    response(429, runtime->active_request_id, due);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && coordinator.state.phase == UserAuthCoordinatorPhase::phone_rate_limited &&
        coordinator.state.retry_at_ms == due && runtime->continuation_retry_at_ms == due &&
        !user_auth_coordinator_submit_phone_resend(&coordinator).ok() && !user_auth_coordinator_submit_phone_verify(&coordinator, &code).ok() &&
        fixture.submissions == before_resend + 2, "phone coordinator: admitted 429 deadline gates explicit attempts without automatic resend");
    check(!user_auth_coordinator_captcha_snapshot(&coordinator).active && !runtime->captcha_invisible,
        "phone coordinator: rate-limited editing cannot project the cleared resend challenge");
    runtime->continuation_retry_at_ms = GetTickCount64();
    check(user_auth_coordinator_submit_phone_resend(&coordinator).ok(), "phone coordinator: explicit resend admitted after deadline");
    secure_buffer_clear(&fixture.response); response(204, runtime->active_request_id);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::phone_code_sent &&
        coordinator.state.phase == UserAuthCoordinatorPhase::recovery_phone_required && !runtime->token_bytes,
        "phone coordinator: empty resend completion stays editable and cannot authenticate");
    check(user_auth_coordinator_submit_phone_verify(&coordinator, &code).ok(), "phone coordinator: retry verify admitted");
    char full_token[4108]{}; copy_bytes(full_token, "{\"token\":\"", 10);
    for (u32 i = 0; i < 4096; ++i) full_token[10 + i] = 'x';
    copy_bytes(full_token + 4106, "\"}", 2);
    // An account-slot sentinel must be erased, never replaced by phone data.
    for (u32 i = 0; i < kTokenCapacity; ++i) secret(runtime, kTokenOffset)[i] = 0xa5;
    body({full_token, sizeof(full_token)}); response(200, runtime->active_request_id);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::phone_verified_reset &&
        coordinator.state.phase == UserAuthCoordinatorPhase::phone_token_ready && runtime->continuation_token_bytes == 4096 &&
        zero_range(runtime->secrets.data, kPhoneTokenOffset) && !runtime->continuation_base_bytes &&
        zero_range(secret(runtime, kTokenOffset), kTokenCapacity) && !runtime->token_bytes &&
        runtime->secrets.data[32767] == 'x' && !user_auth_coordinator_take_profile(&coordinator, 1, &profile, 1).ok() && !profile.credential.data,
        "phone coordinator: full reset token wipes phone body and cannot enter account slot or profile promotion");
    copy_bytes(snapshot, runtime->secrets.data, sizeof(snapshot));
    const auto token_generation = runtime->flow.generation;
    const auto unchanged = [&]() noexcept { return runtime->flow.generation == token_generation &&
        bytes_equal(snapshot, runtime->secrets.data, sizeof(snapshot)) && runtime->continuation_token_bytes == 4096; };
    check(!user_auth_coordinator_take_phone_reset_token(&coordinator, token_generation - 1, &token).ok() && unchanged(),
        "phone coordinator: wrong-generation take preserves complete stage");
    SecureBuffer staged_alias{runtime->secrets.data + kPhoneTokenOffset, 4096, 0, true};
    SecureBuffer partial_alias{runtime->secrets.data + kPhoneTokenOffset - 1, 4096, 0, true};
    SecureBuffer runtime_alias{reinterpret_cast<u8*>(runtime), 4096, 0, true};
    check(!user_auth_coordinator_take_phone_reset_token(&coordinator, token_generation, &staged_alias).ok() && unchanged() &&
        !user_auth_coordinator_take_phone_reset_token(&coordinator, token_generation, &partial_alias).ok() && unchanged() &&
        !user_auth_coordinator_take_phone_reset_token(&coordinator, token_generation, &runtime_alias).ok() && unchanged(),
        "phone coordinator: exact stage partial arena and runtime aliases are rejected before destination clearing");
    token.capacity = 4095;
    check(!user_auth_coordinator_take_phone_reset_token(&coordinator, token_generation, &token).ok() && unchanged(),
        "phone coordinator: too-small take leaves staging and generation intact");
    token.capacity = 4096; token.locked = false;
    check(!user_auth_coordinator_take_phone_reset_token(&coordinator, token_generation, &token).ok() && unchanged(),
        "phone coordinator: unlocked take is rejected atomically");
    token.locked = true; token.bytes = 1; token.data[0] = 'z';
    check(!user_auth_coordinator_take_phone_reset_token(&coordinator, token_generation, &token).ok() && unchanged() && token.data[0] == 'z',
        "phone coordinator: nonempty take output is rejected without wiping caller bytes");
    secure_buffer_clear(&token);
    check(user_auth_coordinator_take_phone_reset_token(&coordinator, token_generation, &token).ok() && token.bytes == 4096 &&
        token.data[4095] == 'x' && erased(*runtime) && runtime->flow.phase == UserAuthPhase::phone_token_taken &&
        coordinator.state.phase == UserAuthCoordinatorPhase::phone_token_taken && runtime->flow.generation != token_generation &&
        !user_auth_coordinator_take_phone_reset_token(&coordinator, token_generation, &token).ok(),
        "phone coordinator: one-shot transfer clears complete arena and invalidates token generation");
    secure_buffer_clear(&token);
    fixture.phase = RestClientPhase::stopping; fixture.error = RestClientError::worker;
    check(!user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::transport_error &&
        erased(*runtime), "phone coordinator: fatal worker is admitted after zero-ID one-shot taken phase");
    user_auth_coordinator_stop(&coordinator, 1);

    check(ready_phone(&coordinator, &identifier) && user_auth_coordinator_submit_phone_verify(&coordinator, &code).ok(),
        "phone coordinator: over-limit reset fixture admits verify");
    runtime = runtime_of(&coordinator);
    char oversized[4109]{}; copy_bytes(oversized, "{\"token\":\"", 10);
    for (u32 i = 0; i < 4097; ++i) oversized[10 + i] = 'y';
    copy_bytes(oversized + 4107, "\"}", 2);
    body({oversized, sizeof(oversized)}); response(200, runtime->active_request_id);
    check(!user_auth_coordinator_pump(&coordinator, &event).ok() && event.decode_error == UserAuthDecodeError::value_too_large &&
        event.kind == UserAuthCoordinatorEventKind::protocol_error && erased(*runtime),
        "phone coordinator: over-limit 4097 byte response cannot stage or promote a token");
    user_auth_coordinator_stop(&coordinator, 1);

    char largest_phone[512]; for (auto& byte : largest_phone) byte = 1;
    secure_buffer_assign(&identifier, {reinterpret_cast<const u8*>(largest_phone), sizeof(largest_phone)});
    check(user_auth_coordinator_start(&coordinator).ok() && user_auth_coordinator_submit_recovery(&coordinator, &identifier).ok(),
        "phone coordinator: worst case escaped original identifier admits");
    runtime = runtime_of(&coordinator); secure_buffer_clear(&identifier);
    body(utf8_literal("{\"code\":70007}")); response(400, runtime->active_request_id); user_auth_coordinator_pump(&coordinator, &event);
    check(runtime->continuation_base_bytes == 3084 && user_auth_coordinator_submit_phone_verify(&coordinator, &code).ok() &&
        fixture.request.bytes == 3100 && bytes_equal(fixture.request.data, "{\"phone\":\"", 10) &&
        bytes_equal(fixture.request.data + 10, "\\u0001", 6) &&
        bytes_equal(fixture.request.data + 10 + 511 * 6, "\\u0001", 6),
        "phone coordinator: exact 512 byte phone encoding survives caller erasure and verify suffix");
    user_auth_coordinator_stop(&coordinator, 1);

    for (u32 mode = 0; mode < 4; ++mode) {
        check(ready_phone(&coordinator, &identifier), "phone coordinator: isolated fatal/stop fixture enters phone editing");
        runtime = runtime_of(&coordinator);
        if (mode == 1) {
            user_auth_coordinator_submit_phone_verify(&coordinator, &code); body(challenge); response(400, runtime->active_request_id);
            user_auth_coordinator_pump(&coordinator, &event);
        } else if (mode >= 2) {
            user_auth_coordinator_submit_phone_verify(&coordinator, &code); body(utf8_literal("{\"token\":\"private-reset\"}"));
            response(200, runtime->active_request_id); user_auth_coordinator_pump(&coordinator, &event);
        }
        if (mode < 3) {
            fixture.phase = RestClientPhase::stopping; fixture.error = RestClientError::worker;
            check(!user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::transport_error &&
                erased(*runtime) && !runtime->phone_context_active && !user_auth_coordinator_notification_handle(&coordinator),
                "phone coordinator: fatal worker with zero active ID wipes editing challenge and staged-token lifetimes");
        }
        fixture.fail_stop = true;
        check(user_auth_coordinator_stop(&coordinator, 1).error == UserAuthCoordinatorError::stop && coordinator.runtime == runtime &&
            coordinator.transport.runtime && coordinator.transport.worker.thread && erased(*runtime),
            "phone coordinator: failed join retains quarantined owner after every phone secret is erased");
        fixture.fail_stop = false; fixture.joined_error = true;
        check(user_auth_coordinator_stop(&coordinator, 1).phase == UserAuthCoordinatorPhase::stopped && !coordinator.runtime &&
            !coordinator.transport.runtime && !coordinator.transport.worker.thread,
            "phone coordinator: explicit joined-error retry releases only after actual transport ownership ends");
        fixture.joined_error = false;
    }
    check(ready_phone(&coordinator, &identifier) && user_auth_coordinator_submit_phone_resend(&coordinator).ok(),
        "phone coordinator: auth-shaped resend fixture admits");
    runtime = runtime_of(&coordinator); body(utf8_literal("{\"token\":\"never-stage\"}")); response(200, runtime->active_request_id);
    check(!user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::protocol_error && erased(*runtime),
        "phone coordinator: resend auth-shaped success fails and wipes full continuation");
    user_auth_coordinator_stop(&coordinator, 1);
    secure_buffer_release(&identifier); secure_buffer_release(&code); secure_buffer_release(&solution); secure_buffer_release(&token);
    secure_buffer_release(&fixture.request); secure_buffer_release(&fixture.response); secure_buffer_release(&fixture.headers);
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_phone_recovery_coordinator_entry() noexcept {
    maicord::phone_coordinator_test::run();
    ExitProcess(maicord::phone_coordinator_test::failures ? 1 : 0);
}

