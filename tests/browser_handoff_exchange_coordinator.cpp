// Actual shipping coordinator with a synchronous, non-network REST boundary.
#include "maicord/sys/winsock.hpp"
#include "maicord/app/browser_login.hpp"
#include "maicord/app/account_profile_promotion.hpp"
#include "../src/app/user_auth_coordinator.cpp"
#include "test_output.hpp"

namespace maicord::browser_exchange_coordinator_test {
struct Fixture {
    SecureBuffer request, response, headers;
    RestClientResponse metadata;
    UserAuthOperation operation;
    u64 request_id;
    u32 submissions, notification_reads;
    bool ready, fail_stop, fail_submit, joined_error, any_captcha;
    RestClientPhase phase;
    RestClientError error;
    RestClientResponse stop_receipt;
    bool publish_during_stop;
    u32 body_visits;
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
    client->runtime = &browser_exchange_coordinator_test::fixture;
    client->worker.thread = reinterpret_cast<HANDLE>(1);
    browser_exchange_coordinator_test::fixture.phase = RestClientPhase::idle;
    browser_exchange_coordinator_test::fixture.error = RestClientError::none;
    RestClientResult result{}; result.phase = RestClientPhase::idle; return result;
}
RestClientSubmitResult rest_client_submit_user_auth(
    RestClient*, const RestUserAuthRequest& value) noexcept {
    using namespace browser_exchange_coordinator_test;
    fixture.any_captcha = value.captcha_key || value.captcha_rqtoken || value.captcha_session_id;
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
    result.phase = browser_exchange_coordinator_test::fixture.ready ? RestClientPhase::response_ready : browser_exchange_coordinator_test::fixture.phase;
    result.error = browser_exchange_coordinator_test::fixture.error;
    return result;
}
RestClientTakeResult rest_client_consume_user_auth_response(
    RestClient*, RestClientResponse* response,
    RestUserAuthConsumeFunction consume, void* context) noexcept {
    using namespace browser_exchange_coordinator_test;
    *response = fixture.metadata;
    ++fixture.body_visits;
    if (!consume(context, fixture.metadata, secure_buffer_view(fixture.response)))
        return {0, RestClientTakeError::invalid_input};
    fixture.ready = false;
    secure_buffer_clear(&fixture.response);
    return {response->body_bytes, RestClientTakeError::none};
}
HANDLE rest_client_notification_handle(const RestClient*) noexcept {
    ++browser_exchange_coordinator_test::fixture.notification_reads;
    return reinterpret_cast<HANDLE>(1);
}
RestClientResult rest_client_stop(RestClient* client, u32) noexcept {
    using namespace browser_exchange_coordinator_test;
    RestClientResult result{};
    result.phase = fixture.fail_stop ? RestClientPhase::draining :
        fixture.joined_error ? RestClientPhase::failed : RestClientPhase::stopped;
    result.error = fixture.joined_error ? RestClientError::worker : RestClientError::none;
    if (!fixture.fail_stop) {
        *client = {}; fixture.ready = false;
        secure_buffer_clear(&fixture.request); secure_buffer_clear(&fixture.response); secure_buffer_clear(&fixture.headers);
    }
    return result;
}
}

namespace maicord {
RestClientResult rest_client_stop_user_auth(RestClient* client, u32 timeout_ms,
                                            RestClientResponse* receipt) noexcept {
    using namespace browser_exchange_coordinator_test;
    if (fixture.publish_during_stop) {
        fixture.metadata = fixture.stop_receipt;
        fixture.ready = true;
        fixture.publish_during_stop = false;
    }
    *receipt = {};
    if (fixture.ready && fixture.metadata.transport_ok() && fixture.metadata.status_code == 429) {
        receipt->request_id = fixture.metadata.request_id;
        receipt->status_code = 429;
        receipt->retry_at_ms = fixture.metadata.retry_at_ms;
    }
    return rest_client_stop(client, timeout_ms);
}
void test_browser_handoff_exchange_protocol(void (*check)(bool, const char*) noexcept) noexcept;
} // namespace maicord
namespace maicord::browser_exchange_coordinator_test {
bool wait_browser(BrowserLogin* owner, BrowserSignInReceiverPhase phase, u32 timeout = 3000) noexcept {
    const u64 end = GetTickCount64() + timeout;
    while (browser_login_status(owner).phase != phase) {
        const u64 now = GetTickCount64();
        if (now >= end) return false;
        const auto event = browser_sign_in_receiver_notification_handle(&owner->receiver);
        if (!event || WaitForSingleObject(event, static_cast<DWORD>(end - now)) != WAIT_OBJECT_0) return false;
    }
    return true;
}
bool browser_candidate(BrowserLogin* owner) noexcept {
    const auto status = browser_login_status(owner);
    const SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) return false;
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(status.port);
    const DWORD timeout = 2000;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    bool okay = connect(socket_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    constexpr auto payload = utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{\"handoffToken\":\"synthetic-handoff\"},\"nonce\":\"fixture\"}");
    u8 wire[512]{}; u32 cursor = 0;
    const auto append = [&](Utf8View value) noexcept { copy_bytes(wire + cursor, value.data, value.bytes); cursor += value.bytes; };
    const auto decimal = [&](u32 value) noexcept {
        u32 divisor = 1; while (value / divisor >= 10) divisor *= 10;
        do { wire[cursor++] = static_cast<u8>('0' + value / divisor); value %= divisor; divisor /= 10; } while (divisor);
    };
    append(utf8_literal("POST /rpc?v=1 HTTP/1.1\r\nHost: 127.0.0.1:")); decimal(status.port);
    append(utf8_literal("\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\nContent-Length: "));
    decimal(payload.bytes); append(utf8_literal("\r\n\r\n")); append(payload);
    if (okay) okay = send(socket_handle, reinterpret_cast<const char*>(wire), cursor, 0) == static_cast<int>(cursor);
    u8 answer[1024]{};
    if (okay) okay = recv(socket_handle, reinterpret_cast<char*>(answer), sizeof(answer), 0) > 0;
    SecureZeroMemory(wire, sizeof(wire)); SecureZeroMemory(answer, sizeof(answer)); closesocket(socket_handle);
    return okay && wait_browser(owner, BrowserSignInReceiverPhase::candidate_ready);
}
void run_browser_owner() noexcept {
    BrowserLogin owner{}; UserAuthCoordinator coordinator{}; SecureBuffer url{}, expected{};
    struct Cleanup {
        BrowserLogin& owner; UserAuthCoordinator& coordinator; SecureBuffer& url; SecureBuffer& expected;
        ~Cleanup() noexcept {
            browser_login_stop(&owner, 5000); user_auth_coordinator_stop(&coordinator, 5000);
            secure_buffer_release(&url); secure_buffer_release(&expected);
        }
    } cleanup{owner, coordinator, url, expected};
    bool ready = user_auth_coordinator_start(&coordinator).ok() && secure_buffer_initialize(&url, 256).ok() &&
        secure_buffer_initialize(&expected, 8192).ok() && browser_login_start(&owner, 701, 10000).ok() &&
        wait_browser(&owner, BrowserSignInReceiverPhase::waiting);
    check(ready, "browser owner: genuine bounded loopback receiver and protected fake-REST coordinator start");
    if (!ready) return;
    check(browser_sign_in_receiver_copy_url(&owner.receiver, 701, &url).ok() &&
        !browser_sign_in_receiver_copy_url(&owner.receiver, 700, &expected).ok(),
        "browser owner: locked generated URL is available only to exact attempt");
    check(browser_candidate(&owner), "browser owner: controlled native loopback client publishes candidate");
    const auto first = browser_login_status(&owner).candidate_id;
    auto pumped = browser_login_pump(&owner, &coordinator);
    const auto written = browser_sign_in_write_exchange({reinterpret_cast<const char*>(url.data + url.bytes - 36), 36},
        utf8_literal("synthetic-handoff"), {expected.data, expected.capacity});
    check(!pumped.failed && !pumped.accepted && written.ok() && fixture.request.bytes == written.bytes &&
        bytes_equal(fixture.request.data, expected.data, written.bytes) && !fixture.any_captcha &&
        browser_login_status(&owner).phase == BrowserSignInReceiverPhase::exchange_pending,
        "browser owner: exact key/candidate snapshot reaches fake protected exchange with no CAPTCHA replay");
    check(body(utf8_literal("{\"message\":\"Invalid handoff\",\"code\":50035}")), "browser owner: rejection fixture prepares");
    response(400, fixture.request_id);
    pumped = browser_login_pump(&owner, &coordinator);
    check(!pumped.accepted && !pumped.failed && !owner.authenticated &&
        browser_login_status(&owner).phase == BrowserSignInReceiverPhase::waiting,
        "browser owner: rejected candidate never promotes and preserves same bounded listener");
    check(browser_candidate(&owner) && browser_login_status(&owner).candidate_id > first,
        "browser owner: replacement candidate gets a fresh identity");
    browser_login_pump(&owner, &coordinator);
    check(body(utf8_literal("{\"user\":{\"id\":\"123456789012345678\"},\"token\":\"synthetic-session\"}")),
        "browser owner: valid synthetic receipt prepares");
    response(200, fixture.request_id);
    pumped = browser_login_pump(&owner, &coordinator);
    check(pumped.accepted && !pumped.failed && owner.authenticated &&
        browser_login_status(&owner).phase == BrowserSignInReceiverPhase::complete &&
        !browser_sign_in_receiver_copy_url(&owner.receiver, 701, &expected).ok(),
        "browser owner: validated exact receipt closes admission before protected profile transfer");
    AccountProfile profile{}; AccountProfilePromotion promotion{};
    fixture.fail_stop = true;
    check(!user_auth_coordinator_take_profile(&coordinator, pumped.event.response.user_id, &profile, 1).ok() &&
        !profile.credential.data && coordinator.transport.runtime,
        "browser owner: failed exchange join quarantines token and prevents DPAPI profile creation");
    fixture.fail_stop = false;
    const auto taken = user_auth_coordinator_take_profile(&coordinator, pumped.event.response.user_id, &profile, 5000);
    check(taken.ok() && account_profile_promotion_stage(&promotion, &profile).ok() && !profile.credential.data,
        "browser owner: joined normal session transfers into sole protected profile transaction");
    account_profile_release(&profile);
    check(!account_profile_promotion_commit(&promotion, L"").ok() && account_profile_promotion_has_credential(promotion),
        "browser owner: failed profile commit preserves exact staged credential for save retry");
    wchar_t folder[MAX_PATH]{}, path[MAX_PATH]{};
    const bool temporary = GetTempPathW(MAX_PATH, folder) && GetTempFileNameW(folder, L"mcb", 0, path);
    bool committed = temporary && account_profile_promotion_commit(&promotion, path).ok();
    AccountProfile loaded{};
    const bool reloaded = committed && account_profile_load(path, &loaded).ok() && loaded.credential.locked &&
        loaded.metadata.profile_id == pumped.event.response.user_id && loaded.credential.bytes == 17 &&
        bytes_equal(loaded.credential.data, "synthetic-session", 17);
    check(reloaded && !account_profile_promotion_has_credential(promotion),
        "browser owner: synthetic receipt completes atomic DPAPI transaction and reloads normal-user session");
    account_profile_release(&loaded); account_profile_promotion_release(&promotion);
    if (temporary) DeleteFileW(path);
    check(browser_login_stop(&owner, 5000).ok() && !owner.receiver.runtime,
        "browser owner: completed receiver joins and releases native handles");
    user_auth_coordinator_stop(&coordinator, 5000); owner = {};
    ready = user_auth_coordinator_start(&coordinator).ok() && browser_login_start(&owner, 702, 1000).ok() &&
        wait_browser(&owner, BrowserSignInReceiverPhase::waiting) && browser_candidate(&owner);
    check(ready, "browser owner: deadline case starts a fresh isolated attempt");
    if (!ready) return;
    browser_login_pump(&owner, &coordinator);
    check(wait_browser(&owner, BrowserSignInReceiverPhase::expired), "browser owner: actual receiver deadline expires pending exchange");
    body(utf8_literal("{\"user\":{\"id\":\"123456789012345678\"},\"token\":\"synthetic-session\"}"));
    response(200, fixture.request_id);
    pumped = browser_login_pump(&owner, &coordinator);
    check(!pumped.accepted && !owner.authenticated &&
        !user_auth_coordinator_take_profile(&coordinator, 1, &profile, 1).ok() && !profile.credential.data,
        "browser owner: late successful receipt after deadline never reaches profile admission");
    browser_login_stop(&owner, 5000); user_auth_coordinator_stop(&coordinator, 5000); owner = {};
    ready = user_auth_coordinator_start(&coordinator).ok() && browser_login_start(&owner, 703, 10000).ok() &&
        wait_browser(&owner, BrowserSignInReceiverPhase::waiting) && browser_candidate(&owner);
    check(ready, "browser owner: cancellation case starts a fresh isolated attempt");
    if (!ready) return;
    browser_login_pump(&owner, &coordinator);
    body(utf8_literal("{\"user\":{\"id\":\"123456789012345678\"},\"token\":\"synthetic-session\"}"));
    response(200, fixture.request_id);
    browser_login_stop(&owner, 0);
    pumped = browser_login_pump(&owner, &coordinator);
    check(!pumped.accepted && !owner.authenticated && !profile.credential.data,
        "browser owner: cancellation invalidates pending candidate before queued success can be consumed");
}
bool request_erased(const UserAuthCoordinatorRuntime& runtime) noexcept {
    for (u32 i = 0; i < runtime.request_capacity; ++i) if (runtime.secrets.data[i]) return false;
    return !runtime.retained_request_bytes && !runtime.active_body_offset;
}
void run_mfa_retry() noexcept {
    SecureBuffer code{};
    check(secure_buffer_initialize(&code, 1024).ok(), "ordinary MFA: protected code fixture initializes");
    if (!code.data) return;
    constexpr auto initial = utf8_literal(R"({"mfa":true,"totp":true,"sms":true,"backup":true,"webauthn":"original-challenge","ticket":"original-ticket","login_instance_id":"original-instance"})");
    constexpr UserAuthMfaType methods[]{UserAuthMfaType::totp, UserAuthMfaType::sms,
        UserAuthMfaType::backup, UserAuthMfaType::webauthn};
    constexpr u8 all_methods = user_auth_mfa_capability_totp | user_auth_mfa_capability_sms |
        user_auth_mfa_capability_backup | user_auth_mfa_capability_webauthn;
    constexpr Utf8View answers[]{utf8_literal("123456"), utf8_literal("654321"),
        utf8_literal("ab-cd-efgh"), utf8_literal(R"({"id":"synthetic-assertion"})")};
    for (u32 i = 0; i < 4; ++i) {
        UserAuthCoordinator coordinator{}; UserAuthCoordinatorEvent event{};
        check(user_auth_coordinator_start(&coordinator).ok() &&
            secure_buffer_assign(&code, {reinterpret_cast<const u8*>(answers[i].data), answers[i].bytes}).ok() &&
            user_auth_coordinator_submit_password(&coordinator, {&code, &code, nullptr, false}).ok() && body(initial),
            "ordinary MFA: password receipt establishes isolated method context");
        auto* runtime = runtime_of(&coordinator);
        if (!runtime) { secure_buffer_release(&code); return; }
        response(200, fixture.request_id);
        check(user_auth_coordinator_pump(&coordinator, &event).phase == UserAuthCoordinatorPhase::mfa_required &&
            user_auth_coordinator_submit_mfa(&coordinator, {&code, nullptr, methods[i]}).ok(),
            "ordinary MFA: original method answer is admitted");
        const auto generation = runtime->flow.generation;
        const auto submissions = fixture.submissions;
        const auto failed_request = fixture.request_id;
        check(body(utf8_literal(R"({"code":60008,"ticket":"foreign-ticket","login_instance_id":"foreign-instance","webauthn":"foreign-challenge","user_id":"999","totp":false})")),
            "ordinary MFA: synthetic invalid-code receipt with unrelated fields prepares");
        response(400, fixture.request_id);
        const auto invalid = user_auth_coordinator_pump(&coordinator, &event);
        check(invalid.ok() && invalid.phase == UserAuthCoordinatorPhase::mfa_required &&
            event.kind == UserAuthCoordinatorEventKind::rejected && event.response.error_code == 60008 &&
            !event.response.has_user_id && event.response.mfa_capabilities == all_methods &&
            runtime->flow.generation == generation && !runtime->active_request_id &&
            runtime->ticket_bytes == 15 && bytes_equal(secret(runtime, kTicketOffset), "original-ticket", 15) &&
            runtime->login_instance_bytes == 17 && bytes_equal(secret(runtime, kLoginInstanceOffset), "original-instance", 17) &&
            runtime->webauthn_bytes == 18 && bytes_equal(secret(runtime, kWebauthnOffset), "original-challenge", 18) &&
            request_erased(*runtime) && !runtime->token_bytes && fixture.submissions == submissions,
            "ordinary MFA: invalid code erases attempted request but retains original ticket identity challenge and capabilities");
        check(user_auth_coordinator_pump(&coordinator, &event).phase == UserAuthCoordinatorPhase::mfa_required &&
            event.kind == UserAuthCoordinatorEventKind::none && fixture.submissions == submissions,
            "ordinary MFA: repeated pump does not replay an invalid answer");
        // A retry can select a different advertised method on the same owner.
        secure_buffer_assign(&code, {reinterpret_cast<const u8*>(answers[(i + 1) % 4].data), answers[(i + 1) % 4].bytes});
        check(user_auth_coordinator_submit_mfa(&coordinator, {&code, nullptr, methods[(i + 1) % 4]}).ok() &&
            fixture.submissions == submissions + 1 && runtime->active_request_id &&
            runtime->flow.generation != generation,
            "ordinary MFA: explicit alternate-method retry starts a fresh request without password resubmission");
        const auto retry_request = runtime->active_request_id;
        response(400, failed_request);
        check(user_auth_coordinator_pump(&coordinator, &event).phase == UserAuthCoordinatorPhase::mfa_pending &&
            event.kind == UserAuthCoordinatorEventKind::none && runtime->active_request_id == retry_request,
            "ordinary MFA: late invalid receipt from the prior answer cannot replace the new pending request");
        if (i == 0) {
            check(body(utf8_literal(R"({"token":"synthetic-session"})")), "ordinary MFA: successful explicit retry prepares");
            response(200, fixture.request_id);
            AccountProfile profile{};
            check(user_auth_coordinator_pump(&coordinator, &event).phase == UserAuthCoordinatorPhase::authenticated &&
                event.kind == UserAuthCoordinatorEventKind::authenticated && !runtime->ticket_bytes &&
                !runtime->login_instance_bytes && !runtime->webauthn_bytes &&
                user_auth_coordinator_take_profile(&coordinator, 42, &profile, 1000).ok() &&
                profile.credential.locked && profile.credential.bytes == 17 &&
                bytes_equal(profile.credential.data, "synthetic-session", 17) && !runtime->token_bytes,
                "ordinary MFA: valid explicit retry promotes one protected session and clears original method context");
            account_profile_release(&profile);
        } else {
            check(body(utf8_literal(R"({"code":50013})")), "ordinary MFA: unrelated terminal rejection prepares");
            response(403, fixture.request_id);
            check(user_auth_coordinator_pump(&coordinator, &event).phase == UserAuthCoordinatorPhase::rejected &&
                !runtime->ticket_bytes && !runtime->login_instance_bytes && !runtime->webauthn_bytes &&
                !user_auth_coordinator_submit_mfa(&coordinator, {&code, nullptr, methods[i]}).ok(),
                "ordinary MFA: unrelated rejection clears context and cannot reuse the MFA owner");
        }
        check(user_auth_coordinator_stop(&coordinator, 1000).ok(), "ordinary MFA: isolated owner releases");
    }
    // The same numeric code on the password endpoint is still terminal.
    UserAuthCoordinator coordinator{}; UserAuthCoordinatorEvent event{};
    check(user_auth_coordinator_start(&coordinator).ok() &&
        user_auth_coordinator_submit_password(&coordinator, {&code, &code, nullptr, false}).ok() &&
        body(utf8_literal(R"({"code":60008})")), "ordinary MFA: endpoint scope fixture prepares");
    response(400, fixture.request_id);
    check(user_auth_coordinator_pump(&coordinator, &event).phase == UserAuthCoordinatorPhase::rejected,
        "ordinary MFA: invalid-code number outside an admitted MFA request does not create retry context");
    user_auth_coordinator_stop(&coordinator, 1000);
    constexpr u16 terminal_statuses[]{403, 400};
    constexpr Utf8View terminal_receipts[]{utf8_literal(R"({"code":60008})"),
        utf8_literal(R"({"code":60008,"suspended_user_token":"synthetic-suspended"})")};
    secure_buffer_assign(&code, {reinterpret_cast<const u8*>(answers[0].data), answers[0].bytes});
    for (u32 i = 0; i < 2; ++i) {
        coordinator = {};
        check(user_auth_coordinator_start(&coordinator).ok() &&
            user_auth_coordinator_submit_password(&coordinator, {&code, &code, nullptr, false}).ok() && body(initial),
            "ordinary MFA: non-reusable receipt fixture establishes original context");
        response(200, fixture.request_id); user_auth_coordinator_pump(&coordinator, &event);
        check(user_auth_coordinator_submit_mfa(&coordinator, {&code, nullptr, UserAuthMfaType::totp}).ok() &&
            body(terminal_receipts[i]), "ordinary MFA: non-reusable rejection prepares");
        response(terminal_statuses[i], fixture.request_id);
        const auto rejected = user_auth_coordinator_pump(&coordinator, &event);
        const auto* runtime = runtime_of(&coordinator);
        check(rejected.phase == UserAuthCoordinatorPhase::rejected && runtime && !runtime->ticket_bytes &&
            !runtime->login_instance_bytes && !runtime->webauthn_bytes,
            "ordinary MFA: forbidden status or suspended-account receipt never preserves invalid-code retry context");
        user_auth_coordinator_stop(&coordinator, 1000);
    }
    secure_buffer_release(&code);
}
void run() noexcept {
    test_browser_handoff_exchange_protocol(check);
    check(secure_buffer_initialize(&fixture.request, kBrowserSignInMaximumBodyBytes).ok() &&
        secure_buffer_initialize(&fixture.response, kUserAuthBrowserExchangeMaximumResponseBytes).ok() &&
        secure_buffer_initialize(&fixture.headers, 1024).ok(), "browser coordinator: bounded locked REST fixture initializes");
    if (!fixture.request.data || !fixture.response.data || !fixture.headers.data) return;
    SecureBuffer input{}, solution{};
    check(secure_buffer_initialize(&input, kBrowserSignInMaximumBodyBytes).ok() &&
        secure_buffer_initialize(&solution, 512).ok(), "browser coordinator: independent locked inputs initialize");
    if (!input.data || !solution.data) return;
    constexpr auto exact = utf8_literal("{\"key\":\"00000000-0000-4000-8000-000000000000\",\"handoff_token\":\"synthetic-handoff\"}");
    const auto assign = [&]() noexcept { return secure_buffer_assign(&input,
        {reinterpret_cast<const u8*>(exact.data), exact.bytes}).ok(); };
    const auto caller_intact = [&]() noexcept { return input.bytes == exact.bytes && bytes_equal(input.data, exact.data, exact.bytes); };
    constexpr auto success = utf8_literal("{\"user\":{\"id\":\"123456789012345678\"},\"token\":\"synthetic-session\"}");
    constexpr auto solved = utf8_literal("synthetic-solution");
    secure_buffer_assign(&solution, {reinterpret_cast<const u8*>(solved.data), solved.bytes});
    UserAuthCoordinator coordinator{}; UserAuthCoordinatorEvent event{};
    check(assign() && user_auth_coordinator_start(&coordinator).ok(), "browser coordinator: native lazy owner starts");
    auto* runtime = runtime_of(&coordinator); if (!runtime) return;
    const auto initial_generation = runtime->flow.generation;
    SecureBuffer unlocked = input; unlocked.locked = false;
    check(!user_auth_coordinator_submit_browser_handoff_exchange(&coordinator, &unlocked).ok() &&
        !fixture.submissions && runtime->flow.generation == initial_generation && caller_intact(),
        "browser coordinator: unlocked input cannot reach REST and caller owner remains intact");
    SecureBuffer own_arena = partition(runtime, kScratchOffset, runtime->request_capacity, exact.bytes);
    check(!user_auth_coordinator_submit_browser_handoff_exchange(&coordinator, &own_arena).ok() &&
        !fixture.submissions && request_erased(*runtime), "browser coordinator: aliased input cannot escape coordinator arena");
    fixture.fail_submit = true;
    check(!user_auth_coordinator_submit_browser_handoff_exchange(&coordinator, &input).ok() &&
        !runtime->active_request_id && runtime->flow.generation == initial_generation && erased(*runtime) && caller_intact(),
        "browser coordinator: failed admission erases staging, preserves caller and leaves generation uncommitted");
    fixture.fail_submit = false;
    check(user_auth_coordinator_submit_browser_handoff_exchange(&coordinator, &input).ok() &&
        fixture.operation == UserAuthOperation::browser_handoff_exchange && fixture.submissions == 1 &&
        !fixture.any_captcha && !fixture.headers.bytes && caller_intact() && request_erased(*runtime) &&
        fixture.request.bytes == exact.bytes && bytes_equal(fixture.request.data, exact.data, exact.bytes) &&
        coordinator.state.phase == UserAuthCoordinatorPhase::browser_exchange_pending,
        "browser coordinator: exact protected handoff snapshot copies synchronously without retained replay or CAPTCHA tuple");
    const auto active_request = runtime->active_request_id, generation = runtime->flow.generation;
    const auto pending_metadata = coordinator.state;
    check(!user_auth_coordinator_submit_browser_handoff_exchange(&coordinator, &input).ok() &&
        !user_auth_coordinator_submit_captcha(&coordinator, &solution).ok() &&
        !user_auth_coordinator_submit_password(&coordinator, {&solution, &solution, nullptr, false}).ok() &&
        fixture.submissions == 1 && runtime->active_request_id == active_request &&
        runtime->flow.generation == generation && same_metadata(coordinator.state, pending_metadata) && caller_intact(),
        "browser coordinator: busy duplicate, password and CAPTCHA attempts preserve admitted owner and caller");
    check(body(success), "browser coordinator: synthetic strict receipt prepares");
    response(200, active_request + 1);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::none &&
        runtime->active_request_id == active_request && !runtime->token_bytes && fixture.submissions == 1,
        "browser coordinator: mismatched completion cannot adopt a token or replay");
    check(body(success), "browser coordinator: cancelled receipt prepares");
    user_auth_state_cancel(&runtime->flow); response(200, active_request);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::none &&
        !runtime->token_bytes && fixture.submissions == 1, "browser coordinator: cancelled generation ignores late completion");
    fixture.fail_stop = true;
    const auto stop = user_auth_coordinator_stop(&coordinator, 1);
    check(stop.phase == UserAuthCoordinatorPhase::stopping && coordinator.runtime && coordinator.transport.runtime &&
        coordinator.transport.worker.thread && erased(*runtime) && !user_auth_coordinator_notification_handle(&coordinator) &&
        !user_auth_coordinator_submit_browser_handoff_exchange(&coordinator, &input).ok(),
        "browser coordinator: failed join erases credentials and retains native owners in quarantine");
    fixture.fail_stop = false;
    check(user_auth_coordinator_stop(&coordinator, 1).phase == UserAuthCoordinatorPhase::stopped && !coordinator.runtime,
        "browser coordinator: explicit stop retry releases quarantined owners");

    const u16 statuses[]{400, 429, 400, 200, 200, 200};
    const Utf8View results[]{utf8_literal("{\"code\":50035,\"message\":\"invalid\"}"), utf8_literal("{}"),
        utf8_literal("{\"captcha_key\":[\"challenge\"]}"), utf8_literal("{\"token\":\"foreign\"}"),
        utf8_literal("{\"user\":{\"id\":\"1\",\"id\":\"2\"},\"token\":\"foreign\"}"), success};
    AccountProfile profile{};
    for (u32 i = 0; i < 6; ++i) {
        check(user_auth_coordinator_start(&coordinator).ok() && assign() &&
            user_auth_coordinator_submit_browser_handoff_exchange(&coordinator, &input).ok() && body(results[i]),
            "browser coordinator: isolated response case prepares");
        runtime = runtime_of(&coordinator); if (!runtime) return;
        const u32 submissions = fixture.submissions;
        const auto request_id = runtime->active_request_id;
        response(statuses[i], request_id, i == 1 ? 123456 : 0,
            i == 5 ? RestClientError::worker : RestClientError::none);
        const auto consumed = user_auth_coordinator_pump(&coordinator, &event);
        check(erased(*runtime) && !runtime->active_request_id && fixture.submissions == submissions &&
            (i < 2 ? consumed.ok() && event.kind == UserAuthCoordinatorEventKind::rejected &&
                consumed.phase == UserAuthCoordinatorPhase::rejected :
             i == 5 ? event.kind == UserAuthCoordinatorEventKind::transport_error :
                event.kind == UserAuthCoordinatorEventKind::protocol_error) &&
            !user_auth_coordinator_take_profile(&coordinator, 1, &profile, 1).ok() && !profile.credential.data,
            "browser coordinator: rejection, rate-limit, challenge, malformed and transport failure erase secrets without promotion or replay");
        check(user_auth_coordinator_pump(&coordinator, &event).phase == consumed.phase &&
            event.kind == UserAuthCoordinatorEventKind::none && fixture.submissions == submissions &&
            !user_auth_coordinator_submit_captcha(&coordinator, &solution).ok(),
            "browser coordinator: repeated pump and CAPTCHA cannot replay terminal handoff credential");
        check(user_auth_coordinator_stop(&coordinator, 1).phase == UserAuthCoordinatorPhase::stopped,
            "browser coordinator: terminal fixture releases actual owner");
    }

    check(user_auth_coordinator_start(&coordinator).ok() && assign() &&
        user_auth_coordinator_submit_browser_handoff_exchange(&coordinator, &input).ok() && body(success),
        "browser coordinator: successful profile case prepares");
    runtime = runtime_of(&coordinator); if (!runtime) return;
    response(200, runtime->active_request_id);
    check(user_auth_coordinator_pump(&coordinator, &event).ok() && event.kind == UserAuthCoordinatorEventKind::authenticated &&
        event.response.user_id == 123456789012345678ull && event.response.has_user_id && runtime->token_bytes == 17 &&
        !runtime->active_request_id && request_erased(*runtime) && !runtime->ticket_bytes &&
        !runtime->captcha_rqtoken_bytes && !runtime->suspended_bytes,
        "browser coordinator: valid dedicated receipt retains only bounded session token and nested identity metadata");
    fixture.fail_stop = true;
    check(user_auth_coordinator_take_profile(&coordinator, 77, &profile, 1).phase == UserAuthCoordinatorPhase::stopping &&
        coordinator.transport.runtime && runtime->token_bytes == 17 && !profile.credential.data,
        "browser coordinator: failed promotion join preserves protected session and creates no profile");
    fixture.fail_stop = false;
    check(user_auth_coordinator_take_profile(&coordinator, 77, &profile, 1).ok() &&
        profile.metadata.profile_id == 77 && profile.credential.locked && profile.credential.bytes == 17 &&
        bytes_equal(profile.credential.data, "synthetic-session", 17) && !runtime->token_bytes &&
        !coordinator.transport.runtime && !coordinator.transport.worker.thread,
        "browser coordinator: joined transport transfers session to independent locked normal-user profile");
    check(!user_auth_coordinator_take_profile(&coordinator, 78, &profile, 1).ok(),
        "browser coordinator: profile promotion is one shot");
    account_profile_release(&profile);
    user_auth_coordinator_stop(&coordinator, 1);

    check(user_auth_coordinator_start_reset(&coordinator).ok() && assign(), "browser coordinator: reset owner isolation prepares");
    runtime = runtime_of(&coordinator);
    const auto reset_submissions = fixture.submissions;
    check(!user_auth_coordinator_submit_browser_handoff_exchange(&coordinator, &input).ok() && runtime && runtime->reset_owner &&
        erased(*runtime) && fixture.submissions == reset_submissions && caller_intact(),
        "browser coordinator: dedicated reset arena never adopts handoff input");
    user_auth_coordinator_stop(&coordinator, 1);
    check(user_auth_coordinator_start(&coordinator).ok() &&
        user_auth_coordinator_submit_password(&coordinator, {&solution, &solution, nullptr, false}).ok(),
        "browser coordinator: ordinary password owner prepares");
    runtime = runtime_of(&coordinator); const auto ordinary_request = runtime ? runtime->active_request_id : 0;
    const auto ordinary_submissions = fixture.submissions;
    check(!user_auth_coordinator_submit_browser_handoff_exchange(&coordinator, &input).ok() && runtime &&
        runtime->active_request_id == ordinary_request && fixture.submissions == ordinary_submissions &&
        fixture.operation == UserAuthOperation::password && caller_intact(),
        "browser coordinator: ordinary admitted password request retains operation and protected ownership");
    user_auth_coordinator_stop(&coordinator, 1);
    for (u32 scenario = 0; scenario < 8; ++scenario) {
        UserAuthCoordinator stopping{};
        check(user_auth_coordinator_start(&stopping).ok() &&
                  user_auth_coordinator_submit_password(&stopping, {&solution, &solution, nullptr, false})
                      .ok(),
              "ordinary Stop: exact protected password request prepares");
        auto* pending = runtime_of(&stopping);
        if (!pending)
            continue;
        const u64 request_id = pending->active_request_id;
        fixture.stop_receipt = {};
        fixture.stop_receipt.request_id = request_id + (scenario == 4 ? 1 : 0);
        fixture.stop_receipt.status_code = scenario == 3 ? 200 : 429;
        fixture.stop_receipt.retry_at_ms = scenario == 2 ? 0 : 900'000'123;
        fixture.stop_receipt.error = scenario == 5 ? RestClientError::receive : RestClientError::none;
        check(body(utf8_literal(R"({"token":"synthetic-late-session"})")),
              "ordinary Stop: untrusted body remains protected until destruction");
        fixture.publish_during_stop = scenario != 0 && scenario != 7;
        if (scenario == 0) {
            fixture.metadata = fixture.stop_receipt;
            fixture.ready = true;
        }
        fixture.fail_stop = scenario == 6 || scenario == 7;
        const u32 visits = fixture.body_visits, submissions = fixture.submissions;
        auto stopped = user_auth_coordinator_stop(&stopping, 1);
        if (scenario == 6 || scenario == 7) {
            if (scenario == 7) {
                check(
                    stopped.phase == UserAuthCoordinatorPhase::stopping && !stopped.http_status &&
                        stopped.request_id == request_id && erased(*pending),
                    "ordinary Stop: empty failed join retains the admitted request identity for its retry");
                fixture.publish_during_stop = true;
            } else {
                check(stopped.phase == UserAuthCoordinatorPhase::stopping && stopped.http_status == 429 &&
                          stopped.retry_at_ms == 900'000'123 && stopping.runtime && erased(*pending) &&
                          !pending->active_request_id,
                      "ordinary Stop: failed join retains only matching cooldown and quarantined empty "
                      "secret owner");
            }
            fixture.fail_stop = false;
            fixture.ready = false;
            stopped = user_auth_coordinator_stop(&stopping, 1);
        }
        const bool limited = scenario != 3 && scenario != 4 && scenario != 5;
        check(stopped.ok() && stopped.phase == UserAuthCoordinatorPhase::stopped && !stopping.runtime &&
                  stopped.http_status == (limited ? 429 : 0) &&
                  stopped.retry_at_ms == (limited && scenario != 2 ? 900'000'123 : 0) &&
                  fixture.body_visits == visits && fixture.submissions == submissions,
              "ordinary Stop: preserves exact ready/join-time429 including missing deadline, never visits "
              "body or resubmits");
    }
    run_mfa_retry();
    run_browser_owner();
    secure_buffer_release(&input); secure_buffer_release(&solution);
    secure_buffer_release(&fixture.request); secure_buffer_release(&fixture.response); secure_buffer_release(&fixture.headers);
}
} // namespace maicord::browser_exchange_coordinator_test
extern "C" __declspec(noreturn) void WINAPI maicord_browser_handoff_exchange_coordinator_entry() noexcept {
    maicord::browser_exchange_coordinator_test::run();
    ExitProcess(maicord::browser_exchange_coordinator_test::failures ? 1 : 0);
}

