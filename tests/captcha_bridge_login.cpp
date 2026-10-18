// Shipping coordinator and TU-local login owner with a synchronous fake REST
// boundary; the real loopback bridge carries the solution. No browser, no
// service request and no profile file is involved.
#pragma warning(disable : 4505) // shared TU-local login helpers unused here
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include "../src/app/user_auth_coordinator.cpp"
#include "account_login_captcha.hpp"
#include "maicord/platform/browser_sign_in_launcher.hpp"

namespace maicord::captcha_login_test {
struct Fixture {
    SecureBuffer request, response, headers, remote_solution;
    RestClientResponse metadata;
    UserAuthOperation operation;
    u64 request_id;
    u32 submissions, remote_submissions;
    bool ready;
    RemoteAuthClientResult remote;
} fixture{};
using Check = void (*)(bool, const char*) noexcept;
Check check_fn{};
void check(bool value, const char* label) noexcept { check_fn(value, label); }
bool body(Utf8View value) noexcept {
    return secure_buffer_assign(&fixture.response, {reinterpret_cast<const u8*>(value.data), value.bytes}).ok();
}
void response(u16 status) noexcept {
    fixture.metadata = {}; fixture.metadata.status_code = status; fixture.metadata.request_id = fixture.request_id;
    fixture.metadata.body_bytes = fixture.response.bytes; fixture.ready = true;
}
bool headers_contain(Utf8View value) noexcept {
    for (u32 i = 0; i + value.bytes <= fixture.headers.bytes; ++i)
        if (bytes_equal(fixture.headers.data + i, value.data, value.bytes)) return true;
    return false;
}
bool contains(Utf8View text, Utf8View value) noexcept {
    for (u32 i = 0; i + value.bytes <= text.bytes; ++i)
        if (bytes_equal(text.data + i, value.data, value.bytes))
            return true;
    return false;
}
} // namespace maicord::captcha_login_test

namespace maicord {
using namespace captcha_login_test;
u32 rest_client_user_auth_runtime_bytes(u32) noexcept { return 0; }
u32 rest_client_user_auth_capacity_runtime_bytes(u32, u32) noexcept { return 0; }
RestClientResult rest_client_start_user_auth(RestClient* client, u32) noexcept {
    client->runtime = &fixture; client->worker.thread = reinterpret_cast<HANDLE>(1);
    RestClientResult result{}; result.phase = RestClientPhase::idle; return result;
}
RestClientResult rest_client_start_user_auth_capacity(RestClient* client, u32 response, u32) noexcept {
    return rest_client_start_user_auth(client, response);
}
RestClientSubmitResult rest_client_submit_user_auth(RestClient*, const RestUserAuthRequest& value) noexcept {
    if (!value.body || !secure_buffer_assign(&fixture.request, secure_buffer_view(*value.body)).ok())
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
    result.phase = fixture.ready ? RestClientPhase::response_ready : RestClientPhase::idle;
    return result;
}
RestClientTakeResult rest_client_consume_user_auth_response(
    RestClient*, RestClientResponse* output, RestUserAuthConsumeFunction consume, void* context) noexcept {
    *output = fixture.metadata;
    if (!consume(context, fixture.metadata, secure_buffer_view(fixture.response)))
        return {0, RestClientTakeError::invalid_input};
    fixture.ready = false; secure_buffer_clear(&fixture.response);
    return {output->body_bytes, RestClientTakeError::none};
}
HANDLE rest_client_notification_handle(const RestClient*) noexcept { return reinterpret_cast<HANDLE>(1); }
bool rest_client_cancel(RestClient*, u64) noexcept { return true; }
bool rest_client_arm_headers(RestClient*, u64 request_id, ConstSlice<u8> headers) noexcept {
    return !request_id && !headers.count;
}
RestClientResult rest_client_stop_user_auth(RestClient* client, u32 timeout_ms,
                                            RestClientResponse* receipt) noexcept {
    using namespace captcha_login_test;
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
    *client = {};
    fixture.ready = false;
    RestClientResult result{};
    result.phase = RestClientPhase::stopped;
    return result;
}

// QR owner boundary: only the CAPTCHA continuation is exercised here.
RemoteAuthClientResult remote_auth_client_start(RemoteAuthClient*) noexcept { return fixture.remote; }
RemoteAuthClientResult remote_auth_client_cancel(RemoteAuthClient*) noexcept { return fixture.remote; }
RemoteAuthClientResult remote_auth_client_stop(RemoteAuthClient* client, u32) noexcept { *client = {}; return {}; }
bool remote_auth_client_pop_event(RemoteAuthClient*, RemoteAuthClientEvent*) noexcept { return false; }
bool remote_auth_client_copy_qr_url(const RemoteAuthClient&, Slice<char>, u32*) noexcept { return false; }
bool remote_auth_client_copy_user(const RemoteAuthClient&, Slice<char>, u32*) noexcept { return false; }
SecureBufferResult remote_auth_client_take_credential(RemoteAuthClient*, SecureBuffer*) noexcept {
    return {0, ERROR_INVALID_STATE, SecureBufferError::invalid_input};
}
SecureBufferResult remote_auth_client_copy_captcha_secret(const RemoteAuthClient*, CaptchaChallengeSecretKind kind,
                                                          SecureBuffer* output) noexcept {
    if (kind != CaptchaChallengeSecretKind::sitekey) return {0, ERROR_NOT_FOUND, SecureBufferError::invalid_input};
    constexpr auto sitekey = utf8_literal("0x4AAAAAAAsynthetic_turnstile");
    return secure_buffer_assign(output, {reinterpret_cast<const u8*>(sitekey.data), sitekey.bytes});
}
RemoteAuthClientResult remote_auth_client_submit_captcha(RemoteAuthClient*, const SecureBuffer* solution) noexcept {
    ++fixture.remote_submissions;
    secure_buffer_assign(&fixture.remote_solution, secure_buffer_view(*solution));
    fixture.remote.phase = RemoteAuthClientPhase::ticket_exchange;
    return fixture.remote;
}
HANDLE remote_auth_client_notification_handle(const RemoteAuthClient*) noexcept { return nullptr; }
RemoteAuthClientResult remote_auth_client_status(const RemoteAuthClient*) noexcept { return fixture.remote; }
} // namespace maicord

namespace maicord::captcha_login_test {
constexpr auto kChallenge = utf8_literal(
    R"({"captcha_key":["captcha-required"],"captcha_sitekey":"10000000-ffff-ffff-ffff-000000000001",)"
    R"("captcha_service":"hcaptcha","captcha_rqdata":"synthetic_rqdata","captcha_rqtoken":"synthetic.rqtoken",)"
    R"("captcha_session_id":"session-1"})");
constexpr auto kEnterpriseChallenge = utf8_literal(
    R"({"captcha_key":["captcha-required"],"captcha_sitekey":null,"captcha_service":"recaptcha_enterprise",)"
    R"("captcha_rqtoken":"synthetic.rqtoken","captcha_session_id":"session-1","user_flow":"login/password"})");

AccountLoginRuntime* new_owner() noexcept {
    auto* owner = static_cast<AccountLoginRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountLoginRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner) return nullptr;
    constexpr wchar_t path[] = L"unused-synthetic-profile";
    copy_bytes(owner->profile_path, path, sizeof(path));
    owner->coordinator_started = user_auth_coordinator_start(&owner->coordinator).ok();
    return owner;
}
bool wait_bridge(AccountLoginRuntime* owner, CaptchaBridgePhase phase) noexcept {
    const u64 end = GetTickCount64() + 3000;
    while (module_login_captcha_bridge_status(owner).phase != phase) {
        const u64 now = GetTickCount64();
        const HANDLE event = module_login_notification_handle_with_captcha(owner);
        if (now >= end || !event || WaitForSingleObject(event, static_cast<DWORD>(end - now)) == WAIT_FAILED) return false;
    }
    return true;
}
// Plays the browser: fetches the page, then posts the solution.
bool solve(AccountLoginRuntime* owner, u64 generation, Utf8View solution, u16* used_port = nullptr,
           bool enterprise = false) noexcept {
    SecureBuffer url{};
    if (!secure_buffer_initialize(&url, kCaptchaBridgeMaximumUrlBytes).ok()) return false;
    bool okay = module_login_copy_captcha_bridge_url(owner, generation, &url).ok() && captcha_bridge_url_valid(&url);
    u32 cursor = 17, port = 0;
    char state[kCaptchaBridgeStateBytes]{};
    if (okay) {
        while (url.data[cursor] != '/') port = port * 10 + url.data[cursor++] - '0';
        copy_bytes(state, url.data + cursor + 3, sizeof(state));
    }
    secure_buffer_release(&url);
    if (used_port) *used_port = static_cast<u16>(port);
    for (u32 round = 0; okay && round < 2; ++round) {
        static u8 wire[8192];
        u32 bytes = 0;
        const auto add = [&](Utf8View value) noexcept { copy_bytes(wire + bytes, value.data, value.bytes); bytes += value.bytes; };
        const auto number = [&](u32 value) noexcept {
            char digits[10]; u32 count = 0;
            do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
            while (count) wire[bytes++] = static_cast<u8>(digits[--count]);
        };
        char body[4200]; u32 body_bytes = 0;
        copy_bytes(body, "{\"token\":\"", 10); body_bytes = 10;
        copy_bytes(body + body_bytes, solution.data, solution.bytes); body_bytes += solution.bytes;
        copy_bytes(body + body_bytes, "\"}", 2); body_bytes += 2;
        add(round ? utf8_literal("POST /s/") : utf8_literal("GET /c/")); add({state, sizeof(state)});
        add(utf8_literal(" HTTP/1.1\r\nHost: 127.0.0.1:")); number(port);
        if (!round) add(utf8_literal("\r\n\r\n"));
        else {
            add(utf8_literal("\r\nOrigin: http://127.0.0.1:")); number(port);
            add(utf8_literal("\r\nSec-Fetch-Site: same-origin\r\nContent-Type: application/json\r\nContent-Length: "));
            number(body_bytes); add(utf8_literal("\r\n\r\n")); add({body, body_bytes});
        }
        const SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        const DWORD timeout = 3000;
        setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<u16>(port));
        okay = connect(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
               send(handle, reinterpret_cast<const char*>(wire), static_cast<int>(bytes), 0) == static_cast<int>(bytes);
        static char answer[24 * 1024];
        int received = 0, total = 0;
        while (okay && (received = recv(handle, answer + total, static_cast<int>(sizeof(answer)) - total, 0)) > 0) total += received;
        okay = okay && total > 12 && answer[9] == (round ? '2' : '2') && answer[10] == '0' && answer[11] == (round ? '4' : '0');
        if (okay && !round && enterprise) {
            const Utf8View page{answer, static_cast<u32>(total)};
            okay = contains(page, utf8_literal("\"k\":\"6LeYqFcqAAAAAD6iZesmNgVulsO4PkpBdr6NVG6M\"")) &&
                   contains(page, utf8_literal("enterprise.js?render=")) &&
                   contains(page, utf8_literal("encodeURIComponent(c.k)")) &&
                   contains(page, utf8_literal("\"a\":\"login/password\"")) &&
                   !contains(page, utf8_literal("render=explicit")) &&
                   !contains(page, utf8_literal("a.render("));
        }
        SecureZeroMemory(wire, sizeof(wire)); SecureZeroMemory(answer, sizeof(answer)); SecureZeroMemory(body, sizeof(body));
        closesocket(handle);
    }
    return okay && wait_bridge(owner, CaptchaBridgePhase::token_ready);
}

void password_flow() noexcept {
    SecureBuffer credential{};
    auto* owner = new_owner();
    check(owner && owner->coordinator_started && secure_buffer_initialize(&credential, 256).ok(), "captcha login: owner starts");
    if (!owner || !credential.data) return;
    constexpr auto login = utf8_literal("native@example.com");
    secure_buffer_assign(&credential, {reinterpret_cast<const u8*>(login.data), login.bytes});
    check(user_auth_coordinator_submit_password(&owner->coordinator,
                                                {&credential, &credential, nullptr, false})
                  .ok() &&
              body(kEnterpriseChallenge),
          "captcha login: password request admitted");
    SecureBuffer original{};
    secure_buffer_initialize(&original, kUserAuthCoordinatorRequestBytes);
    secure_buffer_assign(&original, secure_buffer_view(fixture.request));
    response(400);
    auto status = module_login_pump_with_captcha(owner);
    check(status.phase == AccountLoginPhase::captcha_required &&
              status.captcha_provider == CaptchaProvider::recaptcha_enterprise &&
              !module_login_copy_captcha_bridge_url(owner, 5, &credential).ok(),
          "captcha login: password challenge waits for an explicit bridge start");
    check(module_login_start_captcha_bridge(owner, 0, 60000).error == AccountLoginError::invalid_input &&
              !owner->captcha_bridge.runtime, "captcha login: zero generation is refused");
    status = module_login_start_captcha_bridge(owner, 5, 60000);
    check(status.ok() && owner->captcha_bridge.runtime && wait_bridge(owner, CaptchaBridgePhase::waiting) &&
              module_login_start_captcha_bridge(owner, 6, 60000).error == AccountLoginError::invalid_input,
          "captcha login: one bridge per challenge starts from the locked challenge");
    constexpr auto solution = utf8_literal("P1_synthetic.solution-0001");
    u16 first_port = 0;
    const u32 submissions = fixture.submissions;
    check(
        solve(owner, 5, solution, &first_port, true),
        "captcha login: nullable enterprise key resolves and action reaches the score page before replay");
    status = module_login_pump_with_captcha(owner);
    check(status.phase == AccountLoginPhase::password_pending && fixture.submissions == submissions + 1 &&
              fixture.operation == UserAuthOperation::password && fixture.request.bytes == original.bytes &&
              bytes_equal(fixture.request.data, original.data, original.bytes) &&
              headers_contain(utf8_literal("X-Captcha-Key: P1_synthetic.solution-0001\r\n")) &&
              headers_contain(utf8_literal("X-Captcha-Rqtoken: synthetic.rqtoken\r\n")) &&
              headers_contain(utf8_literal("X-Captcha-Session-Id: session-1\r\n")) && !owner->captcha_bridge.runtime &&
              module_login_captcha_bridge_status(owner).phase == CaptchaBridgePhase::complete,
          "captcha login: the retained password request replays once with the solution and replay headers; listener joined");
    // An invalid solution yields a fresh challenge and a fresh single-use state.
    body(kChallenge); response(400);
    status = module_login_pump_with_captcha(owner);
    check(status.phase == AccountLoginPhase::captcha_required && status.event == UserAuthCoordinatorEventKind::captcha_required,
          "captcha login: a refused solution presents a fresh challenge");
    u16 second_port = 0;
    check(module_login_start_captcha_bridge(owner, 7, 60000).ok() && wait_bridge(owner, CaptchaBridgePhase::waiting) &&
              solve(owner, 7, utf8_literal("P1_synthetic.solution-0002"), &second_port) && second_port != 0,
          "captcha login: the retry restarts the bridge with a new state");
    status = module_login_pump_with_captcha(owner);
    check(status.phase == AccountLoginPhase::password_pending && headers_contain(utf8_literal("X-Captcha-Key: P1_synthetic.solution-0002\r\n")),
          "captcha login: the second solution replays the same request");
    body(utf8_literal(R"({"mfa":true,"totp":true,"sms":true,"ticket":"mfa-ticket","login_instance_id":"instance"})"));
    response(200);
    status = module_login_pump_with_captcha(owner);
    check(status.phase == AccountLoginPhase::mfa_required, "captcha login: solved password challenge continues to MFA");
    // MFA submit challenged.
    secure_buffer_assign(&credential, {reinterpret_cast<const u8*>("123456"), 6});
    check(user_auth_coordinator_submit_mfa(&owner->coordinator, {&credential, nullptr, UserAuthMfaType::totp}).ok() && body(kChallenge),
          "captcha login: MFA request admitted");
    secure_buffer_assign(&original, secure_buffer_view(fixture.request));
    response(400);
    check(module_login_pump_with_captcha(owner).phase == AccountLoginPhase::captcha_required &&
              module_login_start_captcha_bridge(owner, 8, 60000).ok() && wait_bridge(owner, CaptchaBridgePhase::waiting) &&
              solve(owner, 8, utf8_literal("P1_synthetic.solution-mfa")),
          "captcha login: MFA challenge is solved through the bridge");
    status = module_login_pump_with_captcha(owner);
    check(status.phase == AccountLoginPhase::mfa_pending && fixture.operation == UserAuthOperation::mfa_totp &&
              fixture.request.bytes == original.bytes && bytes_equal(fixture.request.data, original.data, original.bytes) &&
              headers_contain(utf8_literal("X-Captcha-Key: P1_synthetic.solution-mfa\r\n")),
          "captcha login: the MFA request replays with the solution");
    body(utf8_literal(R"({"code":60008})")); response(400);
    check(module_login_pump_with_captcha(owner).phase == AccountLoginPhase::mfa_required, "captcha login: MFA context retained");
    // SMS send challenged.
    check(user_auth_coordinator_submit_sms_send(&owner->coordinator).ok() && body(kChallenge), "captcha login: SMS request admitted");
    secure_buffer_assign(&original, secure_buffer_view(fixture.request));
    response(400);
    check(module_login_pump_with_captcha(owner).phase == AccountLoginPhase::captcha_required &&
              module_login_start_captcha_bridge(owner, 9, 60000).ok() && wait_bridge(owner, CaptchaBridgePhase::waiting) &&
              solve(owner, 9, utf8_literal("P1_synthetic.solution-sms")),
          "captcha login: SMS challenge is solved through the bridge");
    status = module_login_pump_with_captcha(owner);
    check(status.phase == AccountLoginPhase::mfa_pending && fixture.operation == UserAuthOperation::mfa_sms_send &&
              fixture.request.bytes == original.bytes && headers_contain(utf8_literal("X-Captcha-Key: P1_synthetic.solution-sms\r\n")),
          "captcha login: the SMS send replays with the solution");
    // Cancel keeps the challenge; a stop tears the bridge down with the owner.
    body(kChallenge); response(400);
    check(module_login_pump_with_captcha(owner).phase == AccountLoginPhase::captcha_required &&
              module_login_start_captcha_bridge(owner, 10, 60000).ok() && wait_bridge(owner, CaptchaBridgePhase::waiting),
          "captcha login: cancellation fixture starts");
    status = module_login_cancel_captcha_bridge(owner);
    check(status.ok() && status.phase == AccountLoginPhase::captcha_required && !owner->captcha_bridge.runtime &&
              module_login_captcha_bridge_status(owner).phase == CaptchaBridgePhase::cancelled,
          "captcha login: cancelling the bridge keeps the challenge for another attempt");
    check(module_login_start_captcha_bridge(owner, 11, 60000).ok() && wait_bridge(owner, CaptchaBridgePhase::waiting),
          "captcha login: stop fixture starts");
    void* instance = owner;
    status = module_login_stop(&instance, 5000);
    check(status.phase == AccountLoginPhase::stopped && !instance, "captcha login: login stop joins the bridge and releases the owner");
    secure_buffer_release(&original);
    secure_buffer_release(&credential);
}

void remote_flow() noexcept {
    auto* owner = static_cast<AccountLoginRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountLoginRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner) return;
    owner->remote_mode = true;
    fixture.remote = {};
    fixture.remote.phase = RemoteAuthClientPhase::captcha_required;
    fixture.remote.captcha_provider = CaptchaProvider::turnstile;
    owner->remote_status = fixture.remote;
    check(module_login_start_captcha_bridge(owner, 21, 60000).ok() && wait_bridge(owner, CaptchaBridgePhase::waiting) &&
              solve(owner, 21, utf8_literal("0.synthetic-turnstile_token")),
          "captcha login: QR ticket challenge is solved through the bridge");
    const auto status = module_login_pump_with_captcha(owner);
    check(fixture.remote_submissions == 1 && fixture.remote_solution.bytes == 27 &&
              bytes_equal(fixture.remote_solution.data, "0.synthetic-turnstile_token", 27) &&
              status.phase == AccountLoginPhase::remote_ticket_exchange && !owner->captcha_bridge.runtime,
          "captcha login: the QR ticket exchange receives the solution exactly once");
    void* instance = owner;
    module_login_stop(&instance, 5000);
}

void run() noexcept {
    secure_buffer_initialize(&fixture.request, kUserAuthCoordinatorRequestBytes);
    secure_buffer_initialize(&fixture.response, 16 * 1024);
    secure_buffer_initialize(&fixture.headers, 8 * 1024);
    secure_buffer_initialize(&fixture.remote_solution, 4096);
    WSADATA winsock{};
    if (!fixture.request.data || !fixture.response.data || !fixture.headers.data || !fixture.remote_solution.data ||
        WSAStartup(MAKEWORD(2, 2), &winsock)) {
        check(false, "captcha login: fixture initializes");
        return;
    }
    password_flow();
    remote_flow();
    WSACleanup();
    secure_buffer_release(&fixture.request); secure_buffer_release(&fixture.response);
    secure_buffer_release(&fixture.headers); secure_buffer_release(&fixture.remote_solution);
}
} // namespace maicord::captcha_login_test

namespace maicord {
void test_captcha_bridge_protocol(void (*check)(bool, const char*) noexcept) noexcept;
void test_captcha_bridge_runtime(void (*check)(bool, const char*) noexcept) noexcept;
}
namespace {
maicord::u32 failures{};
void report(bool value, const char* label) noexcept {
    if (value) return;
    ++failures; DWORD written = 0; maicord::u32 bytes = 0; while (label[bytes]) ++bytes;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), label, bytes, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_captcha_bridge_tests_entry() noexcept {
    maicord::captcha_login_test::check_fn = report;
    maicord::test_captcha_bridge_protocol(report);
    maicord::test_captcha_bridge_runtime(report);
    maicord::captcha_login_test::run();
    ExitProcess(failures ? 1 : 0);
}
