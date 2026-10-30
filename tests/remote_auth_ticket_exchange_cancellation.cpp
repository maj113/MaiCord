// Production ticket/CAPTCHA owners and real CNG, with a synchronous REST boundary.
#include "maicord/app/remote_auth_ticket_exchange.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/sys/crypto.hpp"
// Include the actual client owner so the quarantine regression can construct
// an already-joined outer thread without opening any network connection.
#include "../src/app/remote_auth_client.cpp"
#include "test_output.hpp"

namespace maicord::remote_auth_cancellation_test {
enum class Mode : u8 { success, captcha_success, pre_cancel, response_cancel, consume_cancel, join_cancel,
                       captcha_cancel, retry_cancel, join_failure, limited_challenge, limited_malformed,
                       limited_response_cancel };
constexpr u64 kServerRetryAt = 900'000'123;
struct Fixture {
    SecureBuffer response;
    HANDLE ready, cancel, captcha;
    Mode mode;
    u32 starts, submissions, consumes, cancels, joins, challenges, retries;
    u64 request_id;
    bool decoded_credential;
    bool limited_stop_receipt;
} fixture{};
u32 failures{};
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
bool erased(const SecureBuffer& value) noexcept {
    if (value.bytes) return false;
    for (u32 i = 0; i < value.capacity; ++i) if (value.data[i]) return false;
    return true;
}
bool challenge_mode() noexcept {
    return fixture.submissions == 1 && (fixture.mode == Mode::captcha_success ||
        fixture.mode == Mode::captcha_cancel || fixture.mode == Mode::retry_cancel ||
        fixture.mode == Mode::limited_challenge);
}
constexpr auto token = utf8_literal("synthetic-session");
bool encrypted_receipt(const RemoteAuthHandshake& handshake) noexcept {
    u8 cipher[kRemoteAuthRsaBytes]{}; char encoded[512]{}; char body[600]{};
    BCRYPT_OAEP_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM, nullptr, 0}; ULONG count = 0;
    const auto status = BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(handshake.crypto.rsa_key),
        reinterpret_cast<PUCHAR>(const_cast<char*>(token.data)), token.bytes, &padding,
        nullptr, 0, cipher, sizeof(cipher), &count, BCRYPT_PAD_OAEP);
    const auto encoded_result = base64_encode({cipher, count}, {encoded, sizeof(encoded)});
    constexpr auto prefix = utf8_literal("{\"encrypted_token\":\"");
    constexpr auto suffix = utf8_literal("\"}");
    const u32 bytes = prefix.bytes + encoded_result.bytes + suffix.bytes;
    copy_bytes(body, prefix.data, prefix.bytes);
    copy_bytes(body + prefix.bytes, encoded, encoded_result.bytes);
    copy_bytes(body + prefix.bytes + encoded_result.bytes, suffix.data, suffix.bytes);
    const bool ok = status >= 0 && count == kRemoteAuthRsaBytes && encoded_result.ok() &&
        secure_buffer_assign(&fixture.response, {reinterpret_cast<const u8*>(body), bytes}).ok();
    SecureZeroMemory(cipher, sizeof(cipher)); SecureZeroMemory(encoded, sizeof(encoded));
    SecureZeroMemory(body, sizeof(body));
    return ok;
}
void event(void* context, const RemoteAuthTicketExchangeEvent& value) noexcept {
    auto* exchange = static_cast<RemoteAuthTicketExchange*>(context);
    if (value.kind == RemoteAuthTicketExchangeEventKind::captcha_required) {
        ++fixture.challenges;
        SecureBuffer solution{};
        constexpr auto text = utf8_literal("synthetic-provider-solution");
        bool ready = secure_buffer_initialize(&solution, 128).ok();
        if (fixture.mode == Mode::captcha_success) {
            constexpr auto expected = utf8_literal("6LeYqFcqAAAAAD6iZesmNgVulsO4PkpBdr6NVG6M");
            const auto copied = remote_auth_ticket_exchange_copy_captcha_secret(
                exchange, CaptchaChallengeSecretKind::sitekey, &solution);
            check(ready && value.status.captcha_provider == CaptchaProvider::recaptcha_enterprise &&
                      copied.ok() && solution.locked && solution.bytes == expected.bytes &&
                      bytes_equal(solution.data, expected.data, expected.bytes),
                  "remote auth: actual QR exchange decodes nullable enterprise sitekey into its protected "
                  "owner");
        }
        ready = ready &&
                secure_buffer_assign(&solution, {reinterpret_cast<const u8*>(text.data), text.bytes}).ok();
        check(ready &&
                  remote_auth_ticket_exchange_submit_captcha(exchange, &solution, fixture.captcha).ok(),
              "remote auth: protected synthetic CAPTCHA solution admits");
        secure_buffer_release(&solution);
        if (fixture.mode == Mode::captcha_cancel) SetEvent(fixture.cancel);
    } else {
        ++fixture.retries;
        if (fixture.mode == Mode::retry_cancel) SetEvent(fixture.cancel);
    }
}
DWORD WINAPI completed_outer_thread(void*) noexcept { return 0; }
} // namespace maicord::remote_auth_cancellation_test

namespace maicord {
RestClientResult rest_client_start_user_auth(RestClient* client, u32) noexcept {
    using namespace remote_auth_cancellation_test;
    ++fixture.starts; client->runtime = &fixture; client->worker.thread = reinterpret_cast<HANDLE>(1);
    return {};
}
RestClientSubmitResult rest_client_submit_user_auth(
    RestClient*, const RestUserAuthRequest& request) noexcept {
    using namespace remote_auth_cancellation_test;
    ++fixture.submissions; fixture.request_id = request.request_id;
    check(request.operation == UserAuthOperation::remote_auth_ticket && request.body && request.body->locked,
          "remote auth: actual exchange submits bounded protected ticket body");
    if (fixture.submissions == 2) {
        constexpr auto solution = utf8_literal("synthetic-provider-solution");
        constexpr auto rqtoken = utf8_literal("synthetic-rqtoken");
        constexpr auto session = utf8_literal("synthetic-session-id");
        const auto exact = [](const SecureBuffer* value, Utf8View expected) noexcept {
            return value && value->locked && value->bytes == expected.bytes &&
                bytes_equal(value->data, expected.data, expected.bytes);
        };
        check(exact(request.captcha_key, solution) && exact(request.captcha_rqtoken, rqtoken) &&
            exact(request.captcha_session_id, session),
            "remote auth: actual CAPTCHA retry submits exact protected solution and replay tuple");
    }
    SetEvent(fixture.ready);
    if (fixture.mode == Mode::response_cancel || fixture.mode == Mode::limited_response_cancel)
        SetEvent(fixture.cancel);
    return {};
}
HANDLE rest_client_notification_handle(const RestClient*) noexcept {
    return remote_auth_cancellation_test::fixture.ready;
}
bool rest_client_cancel(RestClient*, u64 request_id) noexcept {
    using namespace remote_auth_cancellation_test;
    ++fixture.cancels;
    check(request_id == fixture.request_id, "remote auth: cancellation targets admitted request");
    // Real cancellation cannot claim a response already published as ready.
    return WaitForSingleObject(fixture.ready, 0) != WAIT_OBJECT_0;
}
RestClientTakeResult rest_client_consume_user_auth_response(
    RestClient*, RestClientResponse* response,
    RestUserAuthConsumeFunction consume, void* context) noexcept {
    using namespace remote_auth_cancellation_test;
    ++fixture.consumes; *response = {}; response->request_id = fixture.request_id;
    const bool limited = fixture.mode == Mode::limited_challenge || fixture.mode == Mode::limited_malformed;
    response->status_code = limited ? 429 : challenge_mode() ? 400 : 200;
    response->rate_limited = limited;
    response->retry_at_ms = limited ? kServerRetryAt : 0;
    constexpr auto challenge =
        utf8_literal("{\"captcha_key\":[\"challenge\"],\"captcha_service\":\"hcaptcha\",\"captcha_"
                     "sitekey\":\"synthetic-sitekey\",\"captcha_rqtoken\":\"synthetic-rqtoken\",\"captcha_"
                     "session_id\":\"synthetic-session-id\"}");
    constexpr auto enterprise = utf8_literal(
        R"({"captcha_key":["challenge"],"captcha_service":"recaptcha_enterprise","captcha_sitekey":null,"captcha_rqtoken":"synthetic-rqtoken","captcha_session_id":"synthetic-session-id"})");
    const auto challenge_body = fixture.mode == Mode::captcha_success ? enterprise : challenge;
    constexpr auto malformed = utf8_literal("{not-json");
    const ConstSlice<u8> body =
        fixture.mode == Mode::limited_malformed
            ? ConstSlice<u8>{reinterpret_cast<const u8*>(malformed.data), malformed.bytes}
        : challenge_mode()
            ? ConstSlice<u8>{reinterpret_cast<const u8*>(challenge_body.data), challenge_body.bytes}
            : secure_buffer_view(fixture.response);
    response->body_bytes = body.count;
    const bool accepted = consume(context, *response, body);
    // The callback is the production decode/decrypt boundary. Signaling here
    // forces the late-cancel gate to erase a successfully decrypted token.
    fixture.decoded_credential = accepted && response->status_code >= 200 && response->status_code < 300;
    if (fixture.mode == Mode::consume_cancel) SetEvent(fixture.cancel);
    ResetEvent(fixture.ready);
    return {body.count, accepted ? RestClientTakeError::none : RestClientTakeError::consumer_rejected};
}
RestClientResult rest_client_stop(RestClient* client, u32) noexcept {
    using namespace remote_auth_cancellation_test;
    ++fixture.joins;
    if (fixture.mode == Mode::join_cancel) SetEvent(fixture.cancel);
    RestClientResult result{};
    result.phase = fixture.mode == Mode::join_failure ? RestClientPhase::stopping : RestClientPhase::stopped;
    if (fixture.mode != Mode::join_failure) *client = {};
    return result;
}
RestClientResult rest_client_stop_user_auth(RestClient* client, u32 timeout_ms,
                                           RestClientResponse* receipt) noexcept {
    using namespace remote_auth_cancellation_test;
    *receipt = {};
    if (WaitForSingleObject(fixture.ready, 0) == WAIT_OBJECT_0 &&
        (fixture.mode == Mode::limited_response_cancel || fixture.limited_stop_receipt)) {
        receipt->request_id = fixture.request_id;
        receipt->status_code = 429;
        receipt->retry_at_ms = kServerRetryAt;
    }
    return rest_client_stop(client, timeout_ms);
}
} // namespace maicord

namespace maicord::remote_auth_cancellation_test {
void run() noexcept {
    fixture.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    fixture.cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    fixture.captcha = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool initialized = fixture.ready && fixture.cancel && fixture.captcha &&
        secure_buffer_initialize(&fixture.response, 1024).ok();
    check(initialized, "remote auth: isolated synthetic fixture initializes");
    if (!initialized) return;
    constexpr Mode modes[]{Mode::success, Mode::captcha_success, Mode::pre_cancel, Mode::response_cancel,
        Mode::consume_cancel, Mode::join_cancel, Mode::captcha_cancel, Mode::retry_cancel, Mode::join_failure,
        Mode::limited_challenge, Mode::limited_malformed, Mode::limited_response_cancel};
    for (Mode mode : modes) {
        fixture.mode = mode;
        fixture.starts = fixture.submissions = fixture.consumes = fixture.cancels = fixture.joins = 0;
        fixture.challenges = fixture.retries = 0; fixture.decoded_credential = false;
        fixture.limited_stop_receipt = false;
        ResetEvent(fixture.ready); ResetEvent(fixture.cancel); ResetEvent(fixture.captcha);
        RemoteAuthTicketExchange exchange{}; RemoteAuthHandshake handshake{};
        const bool prepared = remote_auth_ticket_exchange_initialize(&exchange).ok() &&
            remote_auth_handshake_initialize(&handshake).ok() && encrypted_receipt(handshake);
        check(prepared, "remote auth: actual protected owners and synthetic encrypted receipt initialize");
        if (!prepared) break;
        handshake.phase = RemoteAuthHandshakePhase::ticket_ready;
        if (mode == Mode::pre_cancel) SetEvent(fixture.cancel);
        const auto result = remote_auth_ticket_exchange_run(&exchange, &handshake,
            utf8_literal("synthetic-ticket"), fixture.captcha, fixture.cancel, event, &exchange);
        check(erased(exchange.request) && erased(exchange.captcha_solution) &&
            !captcha_challenge_snapshot(&exchange.captcha).active,
            "remote auth: terminal exchange erases ticket, solution and challenge");
        if (mode == Mode::success || mode == Mode::captcha_success) {
            SecureBuffer output{};
            const bool owned = secure_buffer_initialize(&output, 128).ok();
            const u32 expected_requests = mode == Mode::captcha_success ? 2u : 1u;
            check(result.ok() && fixture.submissions == expected_requests && fixture.consumes == expected_requests &&
                fixture.joins == 1 && handshake.phase == RemoteAuthHandshakePhase::authenticated &&
                owned && remote_auth_ticket_exchange_take_credential(&exchange, &output).ok() &&
                output.bytes == token.bytes && bytes_equal(output.data, token.data, token.bytes) &&
                erased(exchange.credential), "remote auth: uncancelled real CNG receipt transfers once");
            secure_buffer_release(&output);
        } else if (mode == Mode::limited_challenge || mode == Mode::limited_malformed) {
            check(result.error == RemoteAuthTicketExchangeError::response && result.http_status == 429 &&
                result.retry_at_ms == kServerRetryAt && fixture.submissions == 1 && fixture.consumes == 1 &&
                fixture.joins == 1 && !fixture.challenges && !fixture.retries && !fixture.decoded_credential &&
                erased(exchange.credential),
                "remote auth: server deadline survives CAPTCHA-shaped or malformed429 and the one-time ticket never replays");
            if (mode == Mode::limited_malformed)
                check(result.decode_error != UserAuthDecodeError::none,
                    "remote auth: malformed rate-limit body retains decode failure independently of server floor");
            RemoteAuthClient published{};
            published.notification_event = fixture.ready;
            RemoteAuthClientRuntime runtime{};
            publish_ticket_status(&published, result);
            const auto status = remote_auth_client_status(&published);
            publish_event(&published, &runtime, RemoteAuthClientEventKind::failed);
            RemoteAuthClientEvent event{};
            check(status.retry_at_ms == kServerRetryAt && status.http_status == 429 &&
                spsc_queue_pop(&runtime.events, &event) && event.retry_at_ms == kServerRetryAt && event.http_status == 429,
                "remote auth: QR snapshot and terminal event preserve exact server retry floor");
        } else {
            check(result.error == (mode == Mode::join_failure ? RemoteAuthTicketExchangeError::stop :
                        RemoteAuthTicketExchangeError::cancelled) && erased(exchange.credential),
                  "remote auth: cancelled or unjoined receipt cannot retain a credential");
            if (mode == Mode::pre_cancel)
                check(!fixture.starts && !fixture.submissions && !fixture.consumes,
                      "remote auth: pre-cancel starts no transport and submits no ticket");
            if (mode == Mode::response_cancel)
                check(fixture.submissions == 1 && !fixture.consumes && fixture.cancels == 1,
                      "remote auth: simultaneous response and cancel chooses cancel before decode");
            if (mode == Mode::limited_response_cancel)
                check(result.http_status == 429 && result.retry_at_ms == kServerRetryAt &&
                    fixture.submissions == 1 && !fixture.consumes && fixture.cancels == 1 &&
                    fixture.joins == 1 && !fixture.challenges && !fixture.retries &&
                    !fixture.decoded_credential && handshake.phase == RemoteAuthHandshakePhase::ticket_ready,
                    "remote auth: dual-ready cancellation preserves server floor without decoding or replaying the ticket");
            if (mode == Mode::consume_cancel || mode == Mode::join_cancel || mode == Mode::join_failure)
                check(fixture.decoded_credential && handshake.phase == RemoteAuthHandshakePhase::authenticated,
                      "remote auth: post-decode and join gates erase a successfully decrypted credential");
            if (mode == Mode::captcha_cancel || mode == Mode::retry_cancel)
                check(fixture.submissions == 1 && fixture.challenges == 1 &&
                    fixture.retries == (mode == Mode::retry_cancel ? 1u : 0u),
                    "remote auth: simultaneous CAPTCHA cancel and retry cancel never replay the ticket");
        }
        if (mode == Mode::join_failure) {
            const auto request = exchange.request.data, credential = exchange.credential.data;
            const auto solution = exchange.captcha_solution.data, challenge = exchange.captcha.secrets.data;
            remote_auth_ticket_exchange_release(&exchange);
            check(exchange.transport.worker.thread && exchange.transport.runtime &&
                exchange.request.data == request && exchange.credential.data == credential &&
                exchange.captcha_solution.data == solution && exchange.captcha.secrets.data == challenge &&
                erased(exchange.credential),
                "remote auth: failed release join preserves worker, storage and protected owners for retry");
        }
        fixture.mode = Mode::success; // Permit explicit retry to join a quarantined mock worker.
        remote_auth_ticket_exchange_stop(&exchange, 1000);
        remote_auth_ticket_exchange_release(&exchange); remote_auth_handshake_release(&handshake);
        check(!exchange.transport.worker.thread && !exchange.transport.runtime && !exchange.request.data &&
            !exchange.credential.data && !exchange.captcha_solution.data && !exchange.captcha.secrets.data,
            "remote auth: successful release retry frees all quarantined owners");
    }
    // Exercise the actual enclosing client's stop path with an outer thread
    // that has already exited and a nested REST worker whose join fails.
    RemoteAuthClient client{};
    auto* runtime = static_cast<RemoteAuthClientRuntime*>(VirtualAlloc(nullptr,
        sizeof(RemoteAuthClientRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const bool owner_ready = runtime && remote_auth_ticket_exchange_initialize(&runtime->ticket_exchange).ok();
    check(owner_ready, "remote auth: enclosing native client quarantine fixture initializes");
    if (owner_ready) {
        client.runtime = runtime;
        client.thread = CreateThread(nullptr, 0, completed_outer_thread, nullptr, 0, nullptr);
        client.ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        client.notification_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        client.cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        client.captcha_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const bool handles_ready = client.thread && client.ready_event && client.notification_event &&
            client.cancel_event && client.captcha_event && WaitForSingleObject(client.thread, 1000) == WAIT_OBJECT_0;
        check(handles_ready, "remote auth: synthetic outer client thread joins without network");
        if (handles_ready) {
            rest_client_start_user_auth(&runtime->ticket_exchange.transport, 1024);
            runtime->ticket_exchange.admitted_request_id = 19;
            fixture.request_id = 20;
            fixture.limited_stop_receipt = true;
            SetEvent(fixture.ready);
            fixture.mode = Mode::join_failure;
            const auto thread = client.thread, notification = client.notification_event;
            const auto mismatched = remote_auth_client_stop(&client, 1);
            check(mismatched.error == RemoteAuthClientError::stop && !mismatched.http_status &&
                !mismatched.retry_at_ms && runtime->ticket_exchange.admitted_request_id == 19,
                "remote auth: stop ignores a rate-limit receipt for another request");
            fixture.request_id = 19;
            const auto stopped = remote_auth_client_stop(&client, 1);
            check(stopped.error == RemoteAuthClientError::stop && stopped.phase == RemoteAuthClientPhase::stopping &&
                client.runtime == runtime && client.thread == thread && client.notification_event == notification &&
                runtime->ticket_exchange.transport.worker.thread && runtime->ticket_exchange.transport.runtime &&
                stopped.http_status == 429 && stopped.retry_at_ms == kServerRetryAt &&
                runtime->ticket_exchange.admitted_request_id == 19 &&
                WaitForSingleObject(thread, 0) == WAIT_OBJECT_0,
                "remote auth: joined outer thread cannot release a still-live nested worker or client handles");
            fixture.mode = Mode::success;
            fixture.limited_stop_receipt = false;
            ResetEvent(fixture.ready);
            const auto retried = remote_auth_client_stop(&client, 1000);
            check(retried.ok() && retried.phase == RemoteAuthClientPhase::stopped && !client.runtime &&
                !client.thread && !client.notification_event && !client.cancel_event && !client.captcha_event &&
                retried.http_status == 429 && retried.retry_at_ms == kServerRetryAt,
                "remote auth: enclosing client stop retry releases nested worker and every native owner");
        }
    }
    secure_buffer_release(&fixture.response);
    CloseHandle(fixture.ready); CloseHandle(fixture.cancel); CloseHandle(fixture.captcha);
}
} // namespace maicord::remote_auth_cancellation_test
extern "C" __declspec(noreturn) void WINAPI maicord_remote_auth_cancellation_entry() noexcept {
    maicord::remote_auth_cancellation_test::run();
    ExitProcess(maicord::remote_auth_cancellation_test::failures ? 1 : 0);
}
