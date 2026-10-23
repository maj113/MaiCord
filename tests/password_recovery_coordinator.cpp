// Actual shipping coordinator with a synchronous, non-network REST boundary.
#include "../src/app/user_auth_coordinator.cpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::recovery_coordinator_test {
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
    return recovery_coordinator_test::start_client(client);
}
RestClientSubmitResult rest_client_submit_user_auth(
    RestClient*, const RestUserAuthRequest& value) noexcept {
    using namespace recovery_coordinator_test;
    if (fixture.fail_submit) return {RestClientSubmitError::busy};
    if (!value.body || !value.body->locked || !secure_buffer_assign(&fixture.request, secure_buffer_view(*value.body)).ok())
        return {RestClientSubmitError::invalid_input};
    return record_request(value);
}
RestClientResult rest_client_status(const RestClient*) noexcept {
    return recovery_coordinator_test::client_status();
}
RestClientTakeResult rest_client_consume_user_auth_response(
    RestClient*, RestClientResponse* response,
    RestUserAuthConsumeFunction consume, void* context) noexcept {
    return recovery_coordinator_test::consume_response(response, consume, context);
}
HANDLE rest_client_notification_handle(const RestClient*) noexcept {
    ++recovery_coordinator_test::fixture.notification_reads;
    return reinterpret_cast<HANDLE>(1);
}
RestClientResult rest_client_stop_user_auth(RestClient* client, u32 timeout_ms,
                                            RestClientResponse* receipt) noexcept {
    recovery_coordinator_test::stop_receipt(receipt);
    return rest_client_stop(client, timeout_ms);
}
RestClientResult rest_client_stop(RestClient* client, u32) noexcept {
    return recovery_coordinator_test::stop_client(client);
}
}

namespace maicord::recovery_coordinator_test {
void run() noexcept {
    const bool initialized = secure_buffer_initialize(&fixture.request, 4096).ok() &&
        secure_buffer_initialize(&fixture.response, 4096).ok() && secure_buffer_initialize(&fixture.headers, 1024).ok();
    check(initialized, "recovery coordinator: locked mock transport storage initializes");
    if (!initialized) return;
    SecureBuffer identifier{}, solution{};
    check(secure_buffer_initialize(&identifier, 512).ok() && secure_buffer_initialize(&solution, 512).ok(),
        "recovery coordinator: caller locked inputs initialize");
    constexpr auto login = utf8_literal("synthetic@example.test");
    constexpr auto exact = utf8_literal("{\"login\":\"synthetic@example.test\"}");
    secure_buffer_assign(&identifier, {reinterpret_cast<const u8*>(login.data), login.bytes});
    constexpr auto solved = utf8_literal("synthetic-provider-solution");
    secure_buffer_assign(&solution, {reinterpret_cast<const u8*>(solved.data), solved.bytes});
    UserAuthCoordinator coordinator{};
    check(user_auth_coordinator_start(&coordinator).ok(), "recovery coordinator: actual locked owner starts without network");
    auto* runtime = runtime_of(&coordinator);
    if (!runtime) return;
    fixture.fail_submit = true;
    check(!user_auth_coordinator_submit_recovery(&coordinator, &identifier).ok() &&
        runtime->flow.phase == UserAuthPhase::idle && !runtime->active_request_id && erased(*runtime),
        "recovery coordinator: failed transport admission erases staging and does not commit generation");
    fixture.fail_submit = false;
    check(user_auth_coordinator_submit_recovery(&coordinator, &identifier).ok() &&
        fixture.operation == UserAuthOperation::recovery && fixture.submissions == 1 &&
        fixture.request.bytes == exact.bytes && bytes_equal(fixture.request.data, exact.data, exact.bytes),
        "recovery coordinator: admitted operation copies only exact identifier body into transport");
    secure_buffer_clear(&identifier);
    const auto first_generation = runtime->flow.generation, first_request = runtime->active_request_id;
    check(body(utf8_literal("{\"captcha_key\":[\"required\"],\"captcha_service\":\"hcaptcha\",\"captcha_sitekey\":\"site\",\"captcha_rqtoken\":\"opaque\",\"captcha_session_id\":\"session\"}")),
        "recovery coordinator: CAPTCHA response prepared in locked storage");
    response(400, first_request);
    UserAuthCoordinatorEvent event{};
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::captcha_required &&
        runtime->flow.captcha_return_phase == UserAuthPhase::recovery_pending && runtime->retained_request_bytes == exact.bytes &&
        bytes_equal(secret(runtime, kScratchOffset), exact.data, exact.bytes) && !runtime->token_bytes,
        "recovery coordinator: actual consumer retains exact recovery request and challenge without caller identifier");
    const auto recovery_captcha = user_auth_coordinator_captcha_snapshot(&coordinator);
    check(recovery_captcha.active && recovery_captcha.provider == CaptchaProvider::hcaptcha && !recovery_captcha.invisible &&
        recovery_captcha.rqtoken_bytes == 6 && recovery_captcha.session_id_bytes == 7,
        "recovery coordinator: non-QR recovery challenge projects its retained provider with absent invisible hint defaulted false");
    constexpr auto exact_headers = utf8_literal("X-Captcha-Key: synthetic-provider-solution\r\nX-Captcha-Rqtoken: opaque\r\nX-Captcha-Session-Id: session\r\n");
    check(user_auth_coordinator_submit_captcha(&coordinator, &solution).ok() && fixture.operation == UserAuthOperation::recovery &&
        runtime->active_request_id != first_request && runtime->flow.generation != first_generation &&
        fixture.request.bytes == exact.bytes && bytes_equal(fixture.request.data, exact.data, exact.bytes) &&
        fixture.headers.bytes == exact_headers.bytes && bytes_equal(fixture.headers.data, exact_headers.data, exact_headers.bytes) &&
        !runtime->captcha_rqtoken_bytes && !runtime->captcha_session_bytes,
        "recovery coordinator: solved CAPTCHA replays exact operation body with new generation and copied retry headers");
    const auto retry_request = runtime->active_request_id;
    check(body(utf8_literal("{}")), "recovery coordinator: stale body prepares");
    response(200, first_request);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::none &&
        runtime->active_request_id == retry_request && runtime->retained_request_bytes == exact.bytes,
        "recovery coordinator: stale old completion cannot erase newer replay or adopt a result");
    check(body(utf8_literal("{\"method\":\"unverified\"}")), "recovery coordinator: sent body prepares");
    response(200, retry_request, GetTickCount64() + 60000);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::recovery_sent &&
        coordinator.state.phase == UserAuthCoordinatorPhase::recovery_sent && erased(*runtime) &&
        !event.retry_at_ms && !coordinator.state.retry_at_ms,
        "recovery coordinator: neutral sent result erases secrets without turning success Retry-After into a cooldown");
    AccountProfile profile{};
    check(!user_auth_coordinator_take_profile(&coordinator, 1, &profile, 1).ok() && !profile.credential.data,
        "recovery coordinator: sent request cannot produce or promote a profile credential");
    check(user_auth_coordinator_stop(&coordinator, 1).phase == UserAuthCoordinatorPhase::stopped, "recovery coordinator: sent owner drains");
    const u16 statuses[]{400, 429, 200, 429, 429};
    const Utf8View bodies[]{utf8_literal("{\"code\":70007}"), utf8_literal("{}"), utf8_literal("{\"token\":\"never-adopt\"}"),
        utf8_literal(""), utf8_literal("{")};
    for (u32 i = 0; i < 5; ++i) {
        secure_buffer_assign(&identifier, {reinterpret_cast<const u8*>(login.data), login.bytes});
        check(user_auth_coordinator_start(&coordinator).ok() && user_auth_coordinator_submit_recovery(&coordinator, &identifier).ok() &&
            body(bodies[i]), "recovery coordinator: isolated terminal request prepares");
        runtime = runtime_of(&coordinator);
        const u64 due = i == 0 ? GetTickCount64() + 60000 : i == 1 ? 21'600'000 : 0;
        response(statuses[i], runtime->active_request_id, due);
        const auto result = user_auth_coordinator_pump(&coordinator, &event);
        check((i == 0 ? runtime->phone_context_active && runtime->continuation_base_bytes &&
                       !runtime->token_bytes && !runtime->ticket_bytes : erased(*runtime)) && !profile.credential.data &&
            (i == 0 ? event.kind == UserAuthCoordinatorEventKind::recovery_phone_required &&
                       !event.retry_at_ms && !result.retry_at_ms && !runtime->continuation_retry_at_ms &&
                       user_auth_coordinator_submit_phone_resend(&coordinator).ok() :
             i == 1 || i >= 3 ? event.retry_at_ms == due && result.retry_at_ms == due &&
                                result.phase == UserAuthCoordinatorPhase::recovery_rate_limited :
                      event.kind == UserAuthCoordinatorEventKind::protocol_error),
            "recovery coordinator: only 429 publishes a cooldown while phone continuation stays manually editable");
        user_auth_coordinator_stop(&coordinator, 1);
    }
    for (u32 i = 0; i < 2; ++i) {
        secure_buffer_assign(&identifier, {reinterpret_cast<const u8*>(login.data), login.bytes});
        check(user_auth_coordinator_start(&coordinator).ok() && user_auth_coordinator_submit_recovery(&coordinator, &identifier).ok(),
            "recovery coordinator: fatal worker fixture admits request");
        runtime = runtime_of(&coordinator);
        if (i) {
            body(utf8_literal("{\"captcha_key\":[\"required\"],\"captcha_rqtoken\":\"private-replay\"}"));
            response(400, runtime->active_request_id);
            check(user_auth_coordinator_pump(&coordinator, &event).ok() && runtime->retained_request_bytes &&
                runtime->captcha_rqtoken_bytes && !runtime->active_request_id,
                "recovery coordinator: awaiting CAPTCHA owns exact request and challenge without active ID");
        }
        const auto notification_reads = fixture.notification_reads;
        check(user_auth_coordinator_notification_handle(&coordinator) == reinterpret_cast<HANDLE>(1) &&
            fixture.notification_reads == notification_reads + 1,
            "recovery coordinator: live nonnull notification boundary is admitted before fatal worker pump");
        fixture.phase = RestClientPhase::stopping; fixture.error = RestClientError::worker;
        check(!user_auth_coordinator_pump(&coordinator, &event).ok() &&
            event.kind == UserAuthCoordinatorEventKind::transport_error && erased(*runtime) &&
            !runtime->active_request_id && !user_auth_coordinator_notification_handle(&coordinator) &&
            fixture.notification_reads == notification_reads + 1 &&
            runtime->flow.phase == UserAuthPhase::idle,
            "recovery coordinator: real stopping worker-error state erases pending or CAPTCHA ownership and suppresses terminal wake");
        user_auth_coordinator_pump(&coordinator, &event);
        check(event.kind == UserAuthCoordinatorEventKind::none &&
            !user_auth_coordinator_notification_handle(&coordinator) && fixture.notification_reads == notification_reads + 1 &&
            !user_auth_coordinator_submit_captcha(&coordinator, &solution).ok() && !profile.credential.data,
            "recovery coordinator: fatal worker result emits once and cannot replay or adopt a profile");
        fixture.joined_error = true;
        check(user_auth_coordinator_stop(&coordinator, 1).phase == UserAuthCoordinatorPhase::stopped && !coordinator.runtime &&
            !coordinator.transport.runtime && !coordinator.transport.worker.thread,
            "recovery coordinator: joined-error stop releases owner when both transport storage and thread are absent");
        fixture.joined_error = false;
    }
    const Utf8View login_results[]{
        utf8_literal("{\"captcha_key\":[\"required\"],\"captcha_service\":\"hcaptcha\",\"should_serve_invisible\":true,\"captcha_rqtoken\":\"private-login-replay\"}"),
        utf8_literal("{\"mfa\":true,\"totp\":true,\"ticket\":\"private-mfa-ticket\"}"),
        utf8_literal("{\"token\":\"synthetic-session-token\"}")};
    for (u32 i = 0; i < 3; ++i) {
        secure_buffer_assign(&identifier, {reinterpret_cast<const u8*>(login.data), login.bytes});
        check(user_auth_coordinator_start(&coordinator).ok() &&
            user_auth_coordinator_submit_password(&coordinator, {&identifier, &solution, nullptr, false}).ok() && body(login_results[i]),
            "auth coordinator: actual password request prepares waiting or authenticated ownership");
        runtime = runtime_of(&coordinator);
        const auto ordinary_request = runtime->active_request_id;
        response(i ? 200 : 400, ordinary_request);
        check(user_auth_coordinator_pump(&coordinator, &event).ok() && !runtime->active_request_id &&
            (i == 0 ? runtime->flow.phase == UserAuthPhase::captcha_required && runtime->retained_request_bytes && runtime->captcha_rqtoken_bytes :
             i == 1 ? runtime->flow.phase == UserAuthPhase::mfa_required && runtime->ticket_bytes :
                      runtime->flow.phase == UserAuthPhase::authenticated && runtime->token_bytes),
            "auth coordinator: actual consumer owns ordinary CAPTCHA replay MFA ticket or authenticated credential at zero active ID");
        const auto captcha = user_auth_coordinator_captcha_snapshot(&coordinator);
        check(i == 0 ? captcha.active && captcha.provider == CaptchaProvider::hcaptcha && captcha.invisible && captcha.rqtoken_bytes :
            !captcha.active && captcha.provider == CaptchaProvider::unknown && !captcha.invisible,
            "auth coordinator: only actual ordinary CAPTCHA wait projects its provider and invisible hint");
        const auto notification_reads = fixture.notification_reads;
        check(user_auth_coordinator_notification_handle(&coordinator) == reinterpret_cast<HANDLE>(1),
            "auth coordinator: ordinary waiting notification is admitted before fatal worker status");
        fixture.phase = RestClientPhase::stopping; fixture.error = RestClientError::worker;
        const auto fatal = user_auth_coordinator_pump(&coordinator, &event);
        if (i < 2) {
            check(!fatal.ok() && event.kind == UserAuthCoordinatorEventKind::transport_error && erased(*runtime) &&
                runtime->flow.phase == UserAuthPhase::idle && !user_auth_coordinator_notification_handle(&coordinator) &&
                fixture.notification_reads == notification_reads + 1,
                "auth coordinator: ordinary zero-ID MFA and CAPTCHA fatal worker clears protected ownership and terminal wake");
            const auto cleared_captcha = user_auth_coordinator_captcha_snapshot(&coordinator);
            check(!cleared_captcha.active && !cleared_captcha.invisible && !runtime->captcha_invisible,
                "auth coordinator: fatal cancellation clears the retained invisible hint and public challenge metadata");
            user_auth_coordinator_pump(&coordinator, &event);
            check(event.kind == UserAuthCoordinatorEventKind::none && erased(*runtime),
                "auth coordinator: ordinary waiting fatal event is emitted only once");
            const auto terminal_metadata = coordinator.state;
            body(utf8_literal("{\"token\":\"never-adopt-late\"}")); response(200, ordinary_request, 12345);
            user_auth_coordinator_pump(&coordinator, &event);
            check(event.kind == UserAuthCoordinatorEventKind::none && erased(*runtime) &&
                same_metadata(coordinator.state, terminal_metadata),
                "auth coordinator: late ordinary response after fatal cancellation is ignored before metadata or secret mutation");
        } else {
            check(fatal.ok() && event.kind == UserAuthCoordinatorEventKind::none && runtime->token_bytes &&
                runtime->flow.phase == UserAuthPhase::authenticated,
                "auth coordinator: already-authenticated credential is preserved across later worker failure");
            fixture.joined_error = true;
            constexpr auto session_token = utf8_literal("synthetic-session-token");
            check(user_auth_coordinator_take_profile(&coordinator, 1, &profile, 1).ok() && profile.credential.bytes == session_token.bytes &&
                bytes_equal(profile.credential.data, session_token.data, session_token.bytes) && !runtime->token_bytes &&
                !user_auth_coordinator_notification_handle(&coordinator),
                "auth coordinator: joined worker error still permits authenticated credential handoff without waiting-secret loss");
            account_profile_release(&profile);
            fixture.joined_error = false;
        }
        check(user_auth_coordinator_stop(&coordinator, 1).phase == UserAuthCoordinatorPhase::stopped,
            "auth coordinator: ordinary waiting or promoted owner releases after drain");
    }
    secure_buffer_assign(&identifier, {reinterpret_cast<const u8*>(login.data), login.bytes});
    check(user_auth_coordinator_start(&coordinator).ok() &&
        user_auth_coordinator_submit_password(&coordinator, {&identifier, &solution, nullptr, false}).ok(),
        "auth coordinator: ordinary replay fixture admits password request");
    runtime = runtime_of(&coordinator);
    const auto old_password_request = runtime->active_request_id;
    body(login_results[0]); response(400, old_password_request);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() &&
        user_auth_coordinator_submit_captcha(&coordinator, &solution).ok(),
        "auth coordinator: ordinary CAPTCHA response admits new exact replay");
    check(!user_auth_coordinator_captcha_snapshot(&coordinator).active && !runtime->captcha_invisible,
        "auth coordinator: submitted ordinary CAPTCHA clears its hint while preserving exact replay ownership");
    const auto replay_request = runtime->active_request_id;
    const auto replay_metadata = coordinator.state;
    const auto replay_bytes = runtime->retained_request_bytes;
    body(utf8_literal("{\"token\":\"never-adopt-old\"}")); response(200, old_password_request, 12345);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::none &&
        replay_request != old_password_request && runtime->active_request_id == replay_request &&
        runtime->retained_request_bytes == replay_bytes && fixture.request.bytes == replay_bytes &&
        bytes_equal(secret(runtime, kScratchOffset), fixture.request.data, replay_bytes) &&
        same_metadata(coordinator.state, replay_metadata) && !runtime->token_bytes,
        "auth coordinator: stale ordinary response cannot mutate metadata erase newer replay or adopt old credential");
    body(login_results[2]); response(200, replay_request);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::authenticated,
        "auth coordinator: current ordinary replay completion remains admissible");
    user_auth_coordinator_stop(&coordinator, 1);

    // A newly requested challenge replaces the previous provider and hint;
    // an older completion at zero active ID cannot replace that new snapshot.
    check(user_auth_coordinator_start(&coordinator).ok() &&
        !user_auth_coordinator_captcha_snapshot(&coordinator).active &&
        user_auth_coordinator_submit_password(&coordinator, {&identifier, &solution, nullptr, false}).ok(),
        "auth coordinator: challenge replacement starts with no projected metadata");
    runtime = runtime_of(&coordinator);
    const auto replaced_request = runtime->active_request_id;
    body(login_results[0]); response(400, replaced_request);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() &&
        user_auth_coordinator_submit_captcha(&coordinator, &solution).ok(),
        "auth coordinator: replacement journey submits the original challenge replay");
    body(utf8_literal(R"({"captcha_key":["required"],"captcha_service":"future-provider","captcha_sitekey":"replacement","should_serve_invisible":true})"));
    response(400, runtime->active_request_id);
    check(user_auth_coordinator_pump(&coordinator, &event).ok(),
        "auth coordinator: current replay requests a replacement provider");
    const auto replacement = user_auth_coordinator_captcha_snapshot(&coordinator);
    check(replacement.active && replacement.provider == CaptchaProvider::unknown && replacement.invisible &&
        replacement.service_bytes == 15 && replacement.sitekey_bytes == 11 && !replacement.rqtoken_bytes,
        "auth coordinator: unknown replacement remains an active challenge without stale provider or replay lengths");
    body(login_results[2]); response(200, replaced_request);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::none &&
        user_auth_coordinator_captcha_snapshot(&coordinator).active &&
        user_auth_coordinator_captcha_snapshot(&coordinator).provider == CaptchaProvider::unknown && !runtime->token_bytes,
        "auth coordinator: stale completion cannot replace current unknown-provider challenge or authenticate");
    check(user_auth_coordinator_submit_captcha(&coordinator, &solution).ok(),
        "auth coordinator: unknown provider metadata does not alter existing replay admission");
    body(utf8_literal("{}")); response(400, runtime->active_request_id);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::rejected &&
        !user_auth_coordinator_captcha_snapshot(&coordinator).active && !runtime->captcha_invisible && !runtime->captcha_service_bytes,
        "auth coordinator: ordinary rejection clears replacement challenge and invisible hint");
    user_auth_coordinator_stop(&coordinator, 1);
    check(!user_auth_coordinator_captcha_snapshot(&coordinator).active,
        "auth coordinator: stopped owner has no CAPTCHA projection");

    check(user_auth_coordinator_start(&coordinator).ok() &&
        user_auth_coordinator_submit_password(&coordinator, {&identifier, &solution, nullptr, false}).ok(),
        "auth coordinator: SMS generation fixture admits password request");
    runtime = runtime_of(&coordinator);
    const auto sms_password_request = runtime->active_request_id;
    body(utf8_literal("{\"mfa\":true,\"sms\":true,\"ticket\":\"private-sms-ticket\"}")); response(200, sms_password_request);
    check(user_auth_coordinator_pump(&coordinator, &event).ok(), "auth coordinator: actual MFA SMS capability consumed");
    const auto sms_generation = runtime->flow.generation;
    check(user_auth_coordinator_submit_sms_send(&coordinator).ok() && runtime->active_generation == sms_generation &&
        runtime->flow.generation == sms_generation && runtime->ticket_bytes,
        "auth coordinator: SMS send deliberately retains current MFA flow generation while admitting a fresh request ID");
    const auto sms_request = runtime->active_request_id;
    const auto sms_metadata = coordinator.state;
    body(login_results[2]); response(200, sms_password_request);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::none &&
        runtime->active_request_id == sms_request && runtime->ticket_bytes &&
        same_metadata(coordinator.state, sms_metadata),
        "auth coordinator: same-generation SMS ignores older password response by request ID before decode");
    body(utf8_literal("{}")); response(200, sms_request);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::sms_code_sent &&
        runtime->flow.generation == sms_generation && runtime->flow.phase == UserAuthPhase::mfa_required &&
        !runtime->active_request_id && runtime->ticket_bytes,
        "auth coordinator: universal admission guard preserves valid same-generation SMS completion and retained MFA ticket");
    const auto sms_completed_metadata = coordinator.state;
    body(utf8_literal("{}")); response(200, sms_request, 12345);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::none && runtime->ticket_bytes &&
        same_metadata(coordinator.state, sms_completed_metadata),
        "auth coordinator: duplicate SMS completion at zero active ID is ignored without metadata change");
    user_auth_coordinator_stop(&coordinator, 1);

    secure_buffer_assign(&identifier, {reinterpret_cast<const u8*>(login.data), login.bytes});
    check(user_auth_coordinator_start(&coordinator).ok() && user_auth_coordinator_submit_recovery(&coordinator, &identifier).ok(),
        "recovery coordinator: cancellation fixture submits");
    runtime = runtime_of(&coordinator); const auto cancelled_request = runtime->active_request_id;
    body(login_results[0]); response(400, cancelled_request);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() &&
        user_auth_coordinator_captcha_snapshot(&coordinator).active && runtime->captcha_invisible,
        "recovery coordinator: cancellation fixture owns a server challenge and invisible hint");
    fixture.fail_stop = true;
    check(user_auth_coordinator_stop(&coordinator, 1).error == UserAuthCoordinatorError::stop &&
        coordinator.runtime == runtime && erased(*runtime) && !runtime->active_request_id && !runtime->captcha_invisible &&
        !user_auth_coordinator_captcha_snapshot(&coordinator).active,
        "recovery coordinator: failed stop quarantines owner after invalidating admission and erasing retained bytes");
    body(utf8_literal("{}")); response(200, cancelled_request);
    user_auth_coordinator_pump(&coordinator, &event);
    check(event.kind == UserAuthCoordinatorEventKind::none && erased(*runtime) &&
        runtime->flow.phase == UserAuthPhase::idle, "recovery coordinator: late canceled sent event is ignored even if consumer is called");
    fixture.fail_stop = false;
    check(user_auth_coordinator_stop(&coordinator, 1).phase == UserAuthCoordinatorPhase::stopped && !coordinator.runtime,
        "recovery coordinator: explicit retry releases quarantined owner");
    secure_buffer_release(&identifier); secure_buffer_release(&solution);
    secure_buffer_release(&fixture.request); secure_buffer_release(&fixture.response); secure_buffer_release(&fixture.headers);
}
}

extern "C" __declspec(noreturn) void WINAPI maicord_password_recovery_coordinator_entry() noexcept {
    maicord::recovery_coordinator_test::run();
    ExitProcess(maicord::recovery_coordinator_test::failures ? 1 : 0);
}
