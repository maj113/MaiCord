// Actual shipping coordinator with a synchronous, non-network REST boundary.
#include "../src/app/user_auth_coordinator.cpp"
#include "test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::reset_coordinator_test {
struct Fixture {
    SecureBuffer request, response, headers;
    RestClientResponse metadata;
    UserAuthOperation operation;
    u64 request_id;
    u32 submissions, notification_reads, request_capacity;
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
RestClientResult rest_client_start_user_auth_capacity(RestClient* client, u32 response, u32 capacity) noexcept {
    reset_coordinator_test::fixture.request_capacity = capacity;
    return rest_client_start_user_auth(client, response);
}
RestClientResult rest_client_start_user_auth(RestClient* client, u32) noexcept {
    return reset_coordinator_test::start_client(client);
}
RestClientSubmitResult rest_client_submit_user_auth(
    RestClient*, const RestUserAuthRequest& value) noexcept {
    using namespace reset_coordinator_test;
    if (fixture.fail_submit) return {RestClientSubmitError::busy};
    if (!value.body || !value.body->locked || value.body->bytes > fixture.request_capacity || !secure_buffer_assign(&fixture.request, secure_buffer_view(*value.body)).ok())
        return {RestClientSubmitError::invalid_input};
    return record_request(value);
}
RestClientResult rest_client_status(const RestClient*) noexcept {
    return reset_coordinator_test::client_status();
}
RestClientTakeResult rest_client_consume_user_auth_response(
    RestClient*, RestClientResponse* response,
    RestUserAuthConsumeFunction consume, void* context) noexcept {
    return reset_coordinator_test::consume_response(response, consume, context);
}
HANDLE rest_client_notification_handle(const RestClient*) noexcept {
    ++reset_coordinator_test::fixture.notification_reads;
    return reinterpret_cast<HANDLE>(1);
}
RestClientResult rest_client_stop_user_auth(RestClient* client, u32 timeout_ms,
                                            RestClientResponse* receipt) noexcept {
    reset_coordinator_test::stop_receipt(receipt);
    return rest_client_stop(client, timeout_ms);
}
RestClientResult rest_client_stop(RestClient* client, u32) noexcept {
    return reset_coordinator_test::stop_client(client);
}
}

namespace maicord::reset_coordinator_test {
constexpr auto initial = utf8_literal(R"({"token":"reset-link","password":"new-password","source":"/reset"})");
constexpr auto mfa = utf8_literal(R"({"mfa":true,"totp":true,"sms":true,"backup":true,"webauthn":"challenge","ticket":"mfa-ticket"})");
constexpr auto challenge = utf8_literal(R"({"captcha_key":["required"],"captcha_service":"hcaptcha","captcha_sitekey":"site","captcha_rqtoken":"rq-token","captcha_session_id":"session"})");
constexpr auto assertion = utf8_literal(R"({"id":"AQID","rawId":"AQID","response":{"authenticatorData":"AQID","clientDataJSON":"AQID","signature":"AQID","userHandle":null},"type":"public-key","clientExtensionResults":{},"authenticatorAttachment":null})");
void copy_bytes(u8* output, const char* source, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes; ++i) output[i] = static_cast<u8>(source[i]);
}
bool assign(SecureBuffer* output, Utf8View value) noexcept {
    return secure_buffer_assign(output, {reinterpret_cast<const u8*>(value.data), value.bytes}).ok();
}
bool exact(Utf8View value) noexcept {
    return fixture.request.bytes == value.bytes && bytes_equal(fixture.request.data, value.data, value.bytes);
}
bool base_intact(const UserAuthCoordinatorRuntime& runtime) noexcept {
    if (runtime.continuation_base_bytes != initial.bytes || !bytes_equal(runtime.secrets.data, initial.data, initial.bytes)) return false;
    for (u32 i = initial.bytes; i < runtime.request_capacity; ++i) if (runtime.secrets.data[i]) return false;
    return runtime.ticket_bytes == 10 && bytes_equal(secret(&runtime, kTicketOffset), "mfa-ticket", 10);
}
void deliver(UserAuthCoordinator* coordinator, UserAuthCoordinatorEvent* event, u16 status, Utf8View value,
    u64 request_id = 0, u64 retry_at = 0, RestClientError error = RestClientError::none) noexcept {
    check(body(value), "reset coordinator: response stored only in locked mock memory");
    response(status, request_id ? request_id : fixture.request_id, retry_at, error);
    user_auth_coordinator_pump(coordinator, event);
}
void diagnostic_number(const char* name, u32 value) noexcept {
    DWORD written = 0; u32 name_bytes = 0; while (name[name_bytes]) ++name_bytes;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), name, name_bytes, &written, nullptr);
    char digits[10]{}; u32 count = 0;
    do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    for (u32 i = 0; i < count / 2; ++i) { const char a = digits[i]; digits[i] = digits[count - i - 1]; digits[count - i - 1] = a; }
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), digits, count, &written, nullptr);
}
bool start_fixture(UserAuthCoordinator* coordinator) noexcept {
    const auto result = user_auth_coordinator_start_reset(coordinator);
    const auto* runtime = runtime_of(coordinator);
    if (result.ok() && runtime && runtime->secrets.data && runtime->secrets.locked) return true;
    check(false, "reset coordinator: actual reset owner starts");
    diagnostic_number("start_error=", static_cast<u32>(result.error));
    diagnostic_number(" native_error=", result.native_error);
    diagnostic_number(" phase=", static_cast<u32>(result.phase));
    diagnostic_number(" caller_and_fixture_locked_bytes=", fixture.request.capacity + fixture.response.capacity + fixture.headers.capacity + 32 * 1024);
    DWORD written = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
    return false;
}
void run() noexcept {
    const bool initialized = secure_buffer_initialize(&fixture.request, kUserAuthResetCoordinatorRequestBytes).ok() &&
        secure_buffer_initialize(&fixture.response, 32 * 1024).ok() && secure_buffer_initialize(&fixture.headers, 12 * 1024).ok();
    check(initialized, "reset coordinator: synchronous locked transport initializes");
    if (!initialized) return;
    SecureBuffer token{}, password{}, code{}, solution{};
    const bool callers_initialized = secure_buffer_initialize(&token, 4096).ok() && secure_buffer_initialize(&password, 4096).ok() &&
        secure_buffer_initialize(&code, 20 * 1024).ok() && secure_buffer_initialize(&solution, 4096).ok();
    check(callers_initialized, "reset coordinator: caller locked buffers initialize");
    if (!callers_initialized) return;
    assign(&token, utf8_literal("reset-link")); assign(&password, utf8_literal("new-password"));
    assign(&solution, utf8_literal("provider-solution"));
    UserAuthCoordinator coordinator{}; UserAuthCoordinatorEvent event{}; AccountProfile profile{};
    constexpr UserAuthMfaType methods[]{UserAuthMfaType::totp, UserAuthMfaType::backup, UserAuthMfaType::sms, UserAuthMfaType::webauthn};
    constexpr Utf8View codes[]{utf8_literal("123456"), utf8_literal("ab-cd-efgh"), utf8_literal("654321"), assertion};
    constexpr Utf8View expected[]{
        utf8_literal(R"({"token":"reset-link","password":"new-password","source":"/reset","code":"123456","ticket":"mfa-ticket","method":"totp"})"),
        utf8_literal(R"({"token":"reset-link","password":"new-password","source":"/reset","code":"abcdefgh","ticket":"mfa-ticket","method":"backup"})"),
        utf8_literal(R"({"token":"reset-link","password":"new-password","source":"/reset","code":"654321","ticket":"mfa-ticket","method":"sms"})"),
        utf8_literal(R"({"token":"reset-link","password":"new-password","source":"/reset","code":"{\"id\":\"AQID\",\"rawId\":\"AQID\",\"response\":{\"authenticatorData\":\"AQID\",\"clientDataJSON\":\"AQID\",\"signature\":\"AQID\",\"userHandle\":null},\"type\":\"public-key\",\"clientExtensionResults\":{},\"authenticatorAttachment\":null}","ticket":"mfa-ticket","method":"webauthn"})")};
    for (u32 method = 0; method < 4; ++method) {
        if (!start_fixture(&coordinator)) return;
        auto* runtime = runtime_of(&coordinator);
        check(runtime && runtime->secrets.capacity == 104 * 1024 && runtime->request_capacity == 72 * 1024 &&
            fixture.request_capacity == 72 * 1024, "reset coordinator: exactly one 104 KiB arena and scoped 72 KiB transport body");
        check(user_auth_coordinator_submit_reset(&coordinator, &token, &password).ok() && exact(initial) &&
            fixture.operation == UserAuthOperation::reset_password, "reset coordinator: initial exact wire body synchronously copied");
        const auto old_request = fixture.request_id;
        deliver(&coordinator, &event, 200, mfa);
        check(event.kind == UserAuthCoordinatorEventKind::mfa_required && base_intact(*runtime) && !runtime->token_bytes,
            "reset coordinator: MFA retains base and challenge without authenticating");
        check(!user_auth_coordinator_take_profile(&coordinator, 1, &profile, 1).ok() && !profile.credential.data,
            "reset coordinator: initial MFA cannot promote a profile");
        assign(&code, codes[method]);
        check(user_auth_coordinator_submit_mfa(&coordinator, {&code, nullptr, methods[method]}).ok() && exact(expected[method]) &&
            fixture.operation == UserAuthOperation::reset_mfa && coordinator.state.phase == UserAuthCoordinatorPhase::reset_mfa_pending,
            "reset coordinator: all four methods append exact reset suffix and use reset endpoint");
        const auto admitted = fixture.request_id;
        const auto prior = coordinator.state;
        deliver(&coordinator, &event, 200, utf8_literal(R"({"token":"stale-token"})"), old_request);
        check(event.kind == UserAuthCoordinatorEventKind::none && same_metadata(prior, coordinator.state) &&
            runtime->active_request_id == admitted && !runtime->token_bytes, "reset coordinator: stale request cannot decode or change secrets");
        deliver(&coordinator, &event, 400, utf8_literal(R"({"code":60008,"ticket":"untrusted-new-ticket"})"));
        check(event.kind == UserAuthCoordinatorEventKind::rejected && coordinator.state.phase == UserAuthCoordinatorPhase::mfa_required &&
            base_intact(*runtime) && runtime->mfa_capabilities == 15 && runtime->webauthn_bytes == 9,
            "reset coordinator: invalid code erases suffix and preserves original base ticket capabilities and challenge");
        const auto after_rejection = coordinator.state;
        deliver(&coordinator, &event, 200, utf8_literal(R"({"token":"duplicate-token"})"), admitted);
        check(event.kind == UserAuthCoordinatorEventKind::none && same_metadata(after_rejection, coordinator.state) && base_intact(*runtime),
            "reset coordinator: duplicate completion cannot authenticate or erase context");
        check(user_auth_coordinator_submit_mfa(&coordinator, {&code, nullptr, methods[method]}).ok() && exact(expected[method]),
            "reset coordinator: explicit retry retains exact original password and token");
        deliver(&coordinator, &event, 200, utf8_literal(R"({"token":"session-token"})"));
        check(event.kind == UserAuthCoordinatorEventKind::authenticated && !runtime->reset_context_active && !runtime->continuation_base_bytes,
            "reset coordinator: final authenticated response erases reset context");
        for (u32 i = 0; i < runtime->request_capacity; ++i) check(!runtime->secrets.data[i], "reset coordinator: initial and transient secret bytes wiped");
        check(user_auth_coordinator_take_profile(&coordinator, 1, &profile, 1).ok() && profile.credential.bytes == 13 &&
            bytes_equal(profile.credential.data, "session-token", 13), "reset coordinator: only final session enters protected profile handoff");
        secure_buffer_release(&profile.credential); profile = {};
        user_auth_coordinator_stop(&coordinator, 1);
    }
    // The CAPTCHA path retains exactly the admitted body for each route/stage.
    const Utf8View stage_challenges[]{
        utf8_literal(R"({"captcha_key":["required"],"captcha_service":"hcaptcha","captcha_sitekey":"site","captcha_rqtoken":"rq-token","captcha_session_id":"session","should_serve_invisible":true})"),
        utf8_literal(R"({"captcha_key":["required"],"captcha_service":"ReCAPTCHA_ENTERPRISE","captcha_sitekey":"site","captcha_rqtoken":"rq-token","captcha_session_id":"session"})"),
        utf8_literal(R"({"captcha_key":["required"],"captcha_service":"turnstile","captcha_sitekey":"site","captcha_rqtoken":"rq-token","captcha_session_id":"session","should_serve_invisible":false})")};
    const CaptchaProvider stage_providers[]{CaptchaProvider::hcaptcha, CaptchaProvider::recaptcha_enterprise, CaptchaProvider::turnstile};
    for (u32 stage = 0; stage < 3; ++stage) {
        if (!start_fixture(&coordinator)) return; user_auth_coordinator_submit_reset(&coordinator, &token, &password);
        auto* runtime = runtime_of(&coordinator);
        if (stage) deliver(&coordinator, &event, 200, mfa);
        if (stage == 1) { assign(&code, codes[0]); user_auth_coordinator_submit_mfa(&coordinator, {&code, nullptr, methods[0]}); }
        if (stage == 2) check(user_auth_coordinator_submit_sms_send(&coordinator).ok() && runtime->active_body_offset == initial.bytes &&
            exact(utf8_literal(R"({"ticket":"mfa-ticket"})")), "reset coordinator: SMS writes a protected tail while preserving reset base");
        const auto old_request = fixture.request_id;
        const auto operation = fixture.operation;
        const auto expected_body = stage == 0 ? initial : stage == 1 ? expected[0] : utf8_literal(R"({"ticket":"mfa-ticket"})");
        deliver(&coordinator, &event, 400, stage_challenges[stage]);
        check(event.kind == UserAuthCoordinatorEventKind::captcha_required && runtime->retained_request_bytes == expected_body.bytes,
            "reset coordinator: challenged body retained for initial final and SMS operations");
        const auto captcha = user_auth_coordinator_captcha_snapshot(&coordinator);
        check(captcha.active && captcha.provider == stage_providers[stage] && captcha.invisible == (stage == 0) &&
            captcha.sitekey_bytes == 4 && captcha.rqtoken_bytes == 8 && captcha.session_id_bytes == 7,
            "reset coordinator: initial final and SMS challenges project actual provider optional hint and retained lengths");
        check(user_auth_coordinator_submit_captcha(&coordinator, &solution).ok() && fixture.operation == operation &&
            fixture.request_id != old_request && exact(expected_body) && fixture.headers.bytes != 0,
            "reset coordinator: CAPTCHA synchronously replays exact body operation and protected headers");
        check(!user_auth_coordinator_captcha_snapshot(&coordinator).active && !runtime->captcha_invisible,
            "reset coordinator: admitted replay clears invisible hint and CAPTCHA projection without losing reset context");
        if (stage == 2) {
            deliver(&coordinator, &event, 200, utf8_literal("{}"));
            check(event.kind == UserAuthCoordinatorEventKind::sms_code_sent && base_intact(*runtime) && !runtime->token_bytes,
                "reset coordinator: solved SMS returns to editing and cannot promote session");
            user_auth_coordinator_submit_sms_send(&coordinator);
            deliver(&coordinator, &event, 200, utf8_literal(R"({"token":"not-a-session"})"));
            check(event.kind == UserAuthCoordinatorEventKind::protocol_error && erased(*runtime),
                "reset coordinator: auth-shaped SMS success wipes context without promotion");
        } else {
            deliver(&coordinator, &event, 200, stage == 0 ? mfa : utf8_literal(R"({"token":"session-token"})"));
            check(event.kind == (stage == 0 ? UserAuthCoordinatorEventKind::mfa_required : UserAuthCoordinatorEventKind::authenticated),
                "reset coordinator: challenged initial or final request returns to correct stage");
        }
        check(!user_auth_coordinator_captcha_snapshot(&coordinator).active && !runtime->captcha_invisible,
            "reset coordinator: MFA editing authenticated or failed completion cannot project a prior challenge");
        user_auth_coordinator_stop(&coordinator, 1);
    }
    // Exercise the real 72 KiB request admission with every maximum expanded
    // secret, while the ordinary transport profile remains 32 KiB.
    if (!start_fixture(&coordinator)) return;
    secure_clear_bytes(token.data, token.capacity); token.bytes = kLoginResetMaximumTokenBytes;
    secure_clear_bytes(password.data, password.capacity); password.bytes = kUserAuthMaximumPasswordBytes;
    check(user_auth_coordinator_submit_reset(&coordinator, &token, &password).ok(),
        "reset coordinator: maximum expanded initial token and password fit protected request arena");
    assign(&fixture.response, utf8_literal(R"({"mfa":true,"totp":true,"sms":true,"backup":true,"webauthn":"challenge","ticket":")"));
    for (u32 i = 0; i < kUserAuthMaximumTicketBytes; ++i) {
        copy_bytes(fixture.response.data + fixture.response.bytes, "\\u0000", 6); fixture.response.bytes += 6;
    }
    copy_bytes(fixture.response.data + fixture.response.bytes, "\"}", 2); fixture.response.bytes += 2;
    response(200, fixture.request_id); user_auth_coordinator_pump(&coordinator, &event);
    auto* large_runtime = runtime_of(&coordinator);
    const auto large_base = large_runtime->continuation_base_bytes;
    constexpr auto assertion_head = utf8_literal(R"({"id":"AQID","rawId":"AQID","response":{"authenticatorData":"AQID","clientDataJSON":"AQID","signature":")");
    constexpr auto assertion_tail = utf8_literal(R"(","userHandle":null},"type":"public-key","clientExtensionResults":{},"authenticatorAttachment":null})");
    assign(&code, assertion_head);
    u32 binary_bytes = kUserAuthMaximumWebauthnCredentialBytes - assertion_head.bytes - assertion_tail.bytes;
    if ((binary_bytes & 3u) == 1) --binary_bytes;
    for (u32 i = 0; i < binary_bytes; ++i) code.data[code.bytes++] = 'A';
    copy_bytes(code.data + code.bytes, assertion_tail.data, assertion_tail.bytes); code.bytes += assertion_tail.bytes;
    check(user_auth_coordinator_submit_mfa(&coordinator, {&code, nullptr, UserAuthMfaType::webauthn}).ok() &&
        fixture.request.bytes > 32 * 1024 && fixture.request.bytes <= 72 * 1024 &&
        fixture.operation == UserAuthOperation::reset_mfa && large_runtime->continuation_base_bytes == large_base,
        "reset coordinator: actual maximum native assertion expanded context fits scoped 72 KiB synchronous transport");
    deliver(&coordinator, &event, 400, utf8_literal(R"({"code":60008})"));
    check(large_runtime->continuation_base_bytes == large_base && large_runtime->ticket_bytes == kUserAuthMaximumTicketBytes,
        "reset coordinator: maximum request rejection preserves original protected context");
    const auto large_generation = large_runtime->flow.generation;
    code.bytes = kUserAuthMaximumWebauthnCredentialBytes + 1;
    check(!user_auth_coordinator_submit_mfa(&coordinator, {&code, nullptr, UserAuthMfaType::webauthn}).ok() &&
        large_runtime->flow.generation == large_generation && large_runtime->continuation_base_bytes == large_base,
        "reset coordinator: oversized assertion rejects before generation or base changes");
    user_auth_coordinator_stop(&coordinator, 1);
    assign(&token, utf8_literal("reset-link")); assign(&password, utf8_literal("new-password"));
    if (!start_fixture(&coordinator)) return; user_auth_coordinator_submit_reset(&coordinator, &token, &password);
    auto* runtime = runtime_of(&coordinator);
    const auto prefix = runtime->continuation_token_bytes;
    deliver(&coordinator, &event, 400, utf8_literal(R"({"code":50035})"));
    check(coordinator.state.phase == UserAuthCoordinatorPhase::reset_rejected && runtime->reset_prefix_retry &&
        prefix == runtime->continuation_token_bytes, "reset coordinator: explicit rejection retains only encoded token prefix");
    for (u32 i = prefix; i < runtime->secrets.capacity; ++i) check(!runtime->secrets.data[i], "reset coordinator: rejected password and challenges erased");
    const auto retry_generation = runtime->flow.generation;
    secure_clear_bytes(password.data, password.capacity); password.bytes = kUserAuthMaximumPasswordBytes + 1;
    check(!user_auth_coordinator_submit_reset(&coordinator, nullptr, &password).ok() && runtime->flow.generation == retry_generation &&
        runtime->continuation_token_bytes == prefix, "reset coordinator: oversized retry rejected atomically before usable prefix changes");
    assign(&password, utf8_literal("new-password")); fixture.fail_submit = true;
    check(!user_auth_coordinator_submit_reset(&coordinator, nullptr, &password).ok() && runtime->reset_prefix_retry &&
        runtime->continuation_base_bytes == 0, "reset coordinator: failed synchronous admission restores prefix-only retry");
    fixture.fail_submit = false;
    check(user_auth_coordinator_submit_reset(&coordinator, nullptr, &password).ok() && exact(initial),
        "reset coordinator: explicit password retry needs no second token bank");
    deliver(&coordinator, &event, 429, utf8_literal("{"), 0, GetTickCount64() + 60'000);
    const auto submissions = fixture.submissions;
    check(coordinator.state.phase == UserAuthCoordinatorPhase::reset_rate_limited && runtime->reset_prefix_retry &&
        !user_auth_coordinator_submit_reset(&coordinator, nullptr, &password).ok() && fixture.submissions == submissions,
        "reset coordinator: authoritative 429 deadline prevents premature mutation retry even with malformed body");
    user_auth_coordinator_stop(&coordinator, 1);
    for (u32 terminal = 0; terminal < 4; ++terminal) {
        if (!start_fixture(&coordinator)) return; user_auth_coordinator_submit_reset(&coordinator, &token, &password);
        runtime = runtime_of(&coordinator);
        if (terminal == 0) deliver(&coordinator, &event, 0, utf8_literal("{}"), 0, 0, RestClientError::receive);
        else if (terminal == 1) deliver(&coordinator, &event, 200, utf8_literal("{"));
        else if (terminal == 2) deliver(&coordinator, &event, 403, utf8_literal(R"({"code":50013})"));
        else deliver(&coordinator, &event, 429, utf8_literal("{}"));
        check(erased(*runtime) && !user_auth_coordinator_submit_reset(&coordinator, &token, &password).ok(),
            "reset coordinator: ambiguous malformed terminal or deadline-free response wipes and forbids repeat");
        user_auth_coordinator_stop(&coordinator, 1);
    }
    if (!start_fixture(&coordinator)) return; user_auth_coordinator_submit_reset(&coordinator, &token, &password);
    runtime = runtime_of(&coordinator); const auto cancelled = fixture.request_id;
    deliver(&coordinator, &event, 400, challenge);
    fixture.fail_stop = true;
    check(user_auth_coordinator_stop(&coordinator, 1).phase == UserAuthCoordinatorPhase::stopping && erased(*runtime) &&
        !runtime->active_request_id && !user_auth_coordinator_notification_handle(&coordinator),
        "reset coordinator: failed join quarantines transport after early admission invalidation and full erasure");
    deliver(&coordinator, &event, 200, utf8_literal(R"({"token":"late-token"})"), cancelled);
    check(event.kind == UserAuthCoordinatorEventKind::none && erased(*runtime) && !profile.credential.data,
        "reset coordinator: canceled CAPTCHA owner cannot adopt late completion or spin notifications");
    fixture.fail_stop = false;
    check(user_auth_coordinator_stop(&coordinator, 1).phase == UserAuthCoordinatorPhase::stopped && !coordinator.runtime,
        "reset coordinator: explicit successful teardown retry releases quarantine");
    secure_buffer_release(&token); secure_buffer_release(&password); secure_buffer_release(&code); secure_buffer_release(&solution);
    secure_buffer_release(&fixture.request); secure_buffer_release(&fixture.response); secure_buffer_release(&fixture.headers);
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_password_reset_coordinator_entry() noexcept {
    maicord::reset_coordinator_test::run();
    ExitProcess(maicord::reset_coordinator_test::failures ? 1 : 0);
}
