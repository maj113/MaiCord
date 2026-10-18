// Actual loopback bridge worker driven by a synthetic native client. No
// browser is launched and nothing leaves 127.0.0.1.
#include "maicord/sys/winsock.hpp"
#include "maicord/app/captcha_bridge.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/platform/random.hpp"
namespace maicord::captcha_bridge_clock {
// The bridge's monotonic clock runs ahead of the real one by this offset, so
// the deadline test advances time instead of sleeping through it.
volatile LONG64 offset_ms{};
u64 now() noexcept {
    return GetTickCount64() + static_cast<u64>(ReadAcquire64(&offset_ms));
}
} // namespace maicord::captcha_bridge_clock
#define GetTickCount64 maicord::captcha_bridge_clock::now
#include "../src/app/captcha_bridge.cpp"
#undef GetTickCount64
#include "maicord/app/account_action_captcha.hpp"
#include "maicord/platform/browser_sign_in_launcher.hpp"

namespace maicord::captcha_bridge_test {
using Check = void (*)(bool, const char*) noexcept;

struct Reply {
    u8 data[24 * 1024];
    u32 bytes;
    bool closed;
};
Reply reply{};

u16 status_code() noexcept {
    if (reply.bytes < 12 || !bytes_equal(reply.data, "HTTP/1.1 ", 9)) return 0;
    return static_cast<u16>((reply.data[9] - '0') * 100 + (reply.data[10] - '0') * 10 + (reply.data[11] - '0'));
}
bool reply_contains(Utf8View needle) noexcept {
    for (u32 i = 0; i + needle.bytes <= reply.bytes; ++i)
        if (bytes_equal(reply.data + i, needle.data, needle.bytes)) return true;
    return false;
}
SOCKET connect_to(u16 port) noexcept {
    const SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) return handle;
    const DWORD timeout = 3000;
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    if (connect(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(handle); return INVALID_SOCKET;
    }
    return handle;
}
// Windows retries SYNs to a closed loopback port for seconds; binding the
// exact port proves the listener is gone without waiting.
bool port_released(u16 port) noexcept {
    const SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    const bool bound = handle != INVALID_SOCKET && bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    if (handle != INVALID_SOCKET) closesocket(handle);
    return bound;
}
// The worker closes its listener just after publishing a terminal phase.
bool port_released_within(u16 port, u32 milliseconds) noexcept {
    for (u32 waited = 0;; waited += 10) {
        if (port_released(port)) return true;
        if (waited >= milliseconds) return false;
        Sleep(10);
    }
}
bool send_all(SOCKET handle, const u8* data, u32 bytes) noexcept {
    return send(handle, reinterpret_cast<const char*>(data), static_cast<int>(bytes), 0) == static_cast<int>(bytes);
}
// Reads until the server closes; a timeout leaves closed=false.
void read_reply(SOCKET handle) noexcept {
    reply.bytes = 0; reply.closed = false;
    for (;;) {
        const int received = recv(handle, reinterpret_cast<char*>(reply.data + reply.bytes),
                                  static_cast<int>(sizeof(reply.data) - reply.bytes), 0);
        if (received > 0) { reply.bytes += static_cast<u32>(received); continue; }
        reply.closed = received == 0 || WSAGetLastError() == WSAECONNRESET;
        return;
    }
}
struct Request {
    u8 data[8 * 1024];
    u32 bytes;
    void add(Utf8View value) noexcept { copy_bytes(data + bytes, value.data, value.bytes); bytes += value.bytes; }
    void number(u32 value) noexcept {
        char digits[10]; u32 count = 0;
        do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
        while (count) data[bytes++] = static_cast<u8>(digits[--count]);
    }
};
Request request{};
void build(bool page, u16 port, Utf8View state, Utf8View body = {}, bool foreign_origin = false) noexcept {
    request.bytes = 0;
    request.add(page ? utf8_literal("GET /c/") : utf8_literal("POST /s/")); request.add(state);
    request.add(utf8_literal(" HTTP/1.1\r\nHost: 127.0.0.1:")); request.number(port);
    if (page) { request.add(utf8_literal("\r\nSec-Fetch-Site: none\r\n\r\n")); return; }
    request.add(foreign_origin ? utf8_literal("\r\nOrigin: http://127.0.0.1:1") : utf8_literal("\r\nOrigin: http://127.0.0.1:"));
    request.number(port);
    request.add(utf8_literal("\r\nContent-Type: application/json\r\nContent-Length: ")); request.number(body.bytes);
    request.add(utf8_literal("\r\n\r\n")); request.add(body);
}
bool exchange(u16 port) noexcept {
    const SOCKET handle = connect_to(port);
    if (handle == INVALID_SOCKET) return false;
    const bool sent = send_all(handle, request.data, request.bytes);
    if (sent) read_reply(handle);
    closesocket(handle);
    return sent;
}
bool wait_phase(CaptchaBridge* bridge, CaptchaBridgePhase phase, u32 timeout = 3000) noexcept {
    const u64 end = GetTickCount64() + timeout;
    while (captcha_bridge_status(bridge).phase != phase) {
        const u64 now = GetTickCount64();
        const HANDLE event = captcha_bridge_notification_handle(bridge);
        if (now >= end || !event || WaitForSingleObject(event, static_cast<DWORD>(end - now)) == WAIT_FAILED) return false;
    }
    return true;
}
bool secrets_erased(const CaptchaBridge& bridge) noexcept {
    const auto* runtime = static_cast<const Runtime*>(bridge.runtime);
    if (!runtime || runtime->token_bytes || runtime->sitekey_bytes || runtime->rqdata_bytes || runtime->user_flow_bytes)
        return false;
    for (u32 i = kTokenOffset; i < kStorageBytes; ++i) if (runtime->storage.data[i]) return false;
    return true;
}
constexpr Utf8View kTestSitekey = utf8_literal("10000000-ffff-ffff-ffff-000000000001");
constexpr Utf8View kSolution = utf8_literal("10000000-aaaa-bbbb-cccc-000000000001");
constexpr Utf8View kBody = utf8_literal("{\"token\":\"10000000-aaaa-bbbb-cccc-000000000001\"}");

// Parses port and state from the locked URL produced for the shell.
bool target(CaptchaBridge* bridge, u64 generation, u16* port, char* state) noexcept {
    SecureBuffer url{};
    if (!secure_buffer_initialize(&url, kCaptchaBridgeMaximumUrlBytes).ok()) return false;
    const bool copied = captcha_bridge_copy_url(bridge, generation, &url).ok() && captcha_bridge_url_valid(&url);
    if (copied) {
        u32 cursor = 17, value = 0;
        while (url.data[cursor] != '/') value = value * 10 + url.data[cursor++] - '0';
        *port = static_cast<u16>(value);
        copy_bytes(state, url.data + cursor + 3, kCaptchaBridgeStateBytes);
    }
    secure_buffer_release(&url);
    return copied;
}

void lifecycle(Check check) noexcept {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) { check(false, "bridge lifecycle: Winsock starts"); return; }
    CaptchaBridge bridge{};
    check(captcha_bridge_start(&bridge, 0, 5000, {kTestSitekey, {}, CaptchaProvider::hcaptcha, false}).error == CaptchaBridgeError::invalid_input &&
              captcha_bridge_start(&bridge, 1, 5000, {kTestSitekey, {}, CaptchaProvider::unknown, false}).error == CaptchaBridgeError::invalid_input &&
              captcha_bridge_start(&bridge, 1, 5000, {utf8_literal("a\"b"), {}, CaptchaProvider::hcaptcha, false}).error == CaptchaBridgeError::invalid_input &&
              captcha_bridge_start(&bridge, 1, 300001, {kTestSitekey, {}, CaptchaProvider::hcaptcha, false}).error == CaptchaBridgeError::invalid_input &&
              !bridge.runtime,
          "bridge lifecycle: invalid generation, provider, sitekey or deadline never opens a listener");
    check(captcha_bridge_start(&bridge, 11, 10000, {kTestSitekey, utf8_literal("rqdata_synthetic"), CaptchaProvider::hcaptcha, true}).ok() &&
              wait_phase(&bridge, CaptchaBridgePhase::waiting),
          "bridge lifecycle: event-driven listener binds an OS-assigned loopback port");
    u16 port = 0; char state[kCaptchaBridgeStateBytes]{};
    SecureBuffer other{};
    secure_buffer_initialize(&other, kCaptchaBridgeMaximumUrlBytes);
    check(target(&bridge, 11, &port, state) && port >= 1024 && !captcha_bridge_copy_url(&bridge, 12, &other).ok(),
          "bridge lifecycle: URL is released only to the exact generation and has the launcher shape");
    const Utf8View state_view{state, kCaptchaBridgeStateBytes};

    // An active connection keeps the slot; a second connection is refused.
    const SOCKET active = connect_to(port);
    build(true, port, state_view);
    const bool partial = active != INVALID_SOCKET && send_all(active, request.data, 10);
    Sleep(50);
    const SOCKET second = connect_to(port);
    if (second != INVALID_SOCKET) { build(true, port, state_view); send_all(second, request.data, request.bytes); read_reply(second); closesocket(second); }
    check(partial && reply.bytes == 0, "bridge lifecycle: one connection at a time; a concurrent client gets no response");
    if (active != INVALID_SOCKET) {
        send_all(active, request.data + 10, request.bytes - 10);
        read_reply(active);
        closesocket(active);
    }
    check(status_code() == 200 && reply.closed && reply_contains(utf8_literal("Content-Security-Policy: default-src 'none'")) &&
              reply_contains(utf8_literal("rqdata_synthetic")) && reply_contains(utf8_literal("https://js.hcaptcha.com/1/api.js")) &&
              captcha_bridge_status(&bridge).page_served,
          "bridge lifecycle: the page is served once with its CSP and challenge data");
    build(true, port, state_view);
    check(exchange(port) && status_code() == 410, "bridge lifecycle: the page is single-use; a reload is gone");
    // An idle speculative socket yields to the real request.
    const SOCKET idle = connect_to(port);
    Sleep(50);
    build(false, port, state_view, kBody, true);
    check(exchange(port) && reply.bytes == 0 && reply.closed && captcha_bridge_status(&bridge).phase == CaptchaBridgePhase::waiting,
          "bridge lifecycle: a foreign Origin POST is dropped without spending the state");
    if (idle != INVALID_SOCKET) closesocket(idle);
    char wrong[kCaptchaBridgeStateBytes];
    copy_bytes(wrong, state, sizeof(wrong));
    wrong[0] = wrong[0] == '0' ? '1' : '0';
    build(false, port, {wrong, kCaptchaBridgeStateBytes}, kBody);
    check(exchange(port) && reply.bytes == 0 && captcha_bridge_status(&bridge).phase == CaptchaBridgePhase::waiting,
          "bridge lifecycle: a guessed state is refused");
    build(false, port, state_view, kBody);
    check(exchange(port) && status_code() == 204 && !reply_contains(utf8_literal("Access-Control")) &&
              wait_phase(&bridge, CaptchaBridgePhase::token_ready),
          "bridge lifecycle: the same-origin solution is accepted once");
    const auto* runtime = static_cast<const Runtime*>(bridge.runtime);
    check(runtime && runtime->storage.locked && runtime->token_bytes == kSolution.bytes &&
              bytes_equal(runtime->storage.data + kTokenOffset, kSolution.data, kSolution.bytes),
          "bridge lifecycle: the token lives only in the locked arena");
    build(false, port, state_view, kBody);
    check(exchange(port) && status_code() == 410, "bridge lifecycle: replaying the spent state is gone");
    SecureBuffer token{};
    secure_buffer_initialize(&token, kCaptchaBridgeMaximumTokenBytes);
    check(!captcha_bridge_take_token(&bridge, 12, &token).ok() && token.bytes == 0 &&
              captcha_bridge_take_token(&bridge, 11, &token).ok() && token.bytes == kSolution.bytes &&
              bytes_equal(token.data, kSolution.data, kSolution.bytes) && secrets_erased(bridge) &&
              captcha_bridge_status(&bridge).phase == CaptchaBridgePhase::complete,
          "bridge lifecycle: take is exact-generation, copies the token and wipes every secret");
    secure_buffer_clear(&token);
    check(!captcha_bridge_take_token(&bridge, 11, &token).ok() && !captcha_bridge_copy_url(&bridge, 11, &other).ok(),
          "bridge lifecycle: the token and URL are one-shot");
    check(captcha_bridge_stop(&bridge, 5000).phase == CaptchaBridgePhase::complete && !bridge.runtime &&
              port_released(port),
          "bridge lifecycle: stop joins the worker and closes the listener");

    // Timeout: the worker expires the state and erases secrets itself. Jump the
    // bridge clock past the deadline; a loopback connection wakes the worker.
    check(captcha_bridge_start(&bridge, 21, 1000, {kTestSitekey, {}, CaptchaProvider::turnstile, false}).ok() &&
              wait_phase(&bridge, CaptchaBridgePhase::waiting) && target(&bridge, 21, &port, state),
          "bridge lifecycle: deadline fixture starts");
    InterlockedAdd64(&captcha_bridge_clock::offset_ms, 1000);
    const SOCKET wake = connect_to(port);
    check(wake != INVALID_SOCKET && wait_phase(&bridge, CaptchaBridgePhase::expired) && secrets_erased(bridge),
          "bridge lifecycle: the bounded deadline expires the state and wipes it");
    if (wake != INVALID_SOCKET) closesocket(wake);
    check(port_released_within(port, 2000), "bridge lifecycle: an expired state closes its listener");
    captcha_bridge_stop(&bridge, 5000);

    // Cancel: stop invalidates first, then joins.
    check(captcha_bridge_start(&bridge, 31, 10000, {kTestSitekey, {}, CaptchaProvider::recaptcha, false}).ok() &&
              wait_phase(&bridge, CaptchaBridgePhase::waiting) && target(&bridge, 31, &port, state),
          "bridge lifecycle: cancellation fixture starts");
    const auto cancelled = captcha_bridge_stop(&bridge, 5000);
    check(cancelled.phase == CaptchaBridgePhase::cancelled && !bridge.runtime && port_released(port),
          "bridge lifecycle: cancel invalidates the state and joins the listener");

    // A malformed POST with the right state spends it and reports rejection.
    check(
        captcha_bridge_start(
            &bridge, 41, 10000,
            {kTestSitekey, {}, CaptchaProvider::recaptcha_enterprise, false, utf8_literal("message/send")})
                .ok() &&
            wait_phase(&bridge, CaptchaBridgePhase::waiting) && target(&bridge, 41, &port, state),
        "bridge lifecycle: malformed-solution fixture starts");
    build(false, port, state_view, utf8_literal("{\"token\":\"<script>alert(1)</script>\"}"));
    const bool exchanged = exchange(port);
    const auto rejected = captcha_bridge_status(&bridge);
    check(exchanged && status_code() == 410 && rejected.phase == CaptchaBridgePhase::failed &&
              rejected.error == CaptchaBridgeError::rejected && secrets_erased(bridge),
          "bridge lifecycle: the first POST invalidates the state even when its token is refused");
    captcha_bridge_stop(&bridge, 5000);
    check(captcha_bridge_runtime_bytes() <= 72 * 1024, "bridge lifecycle: one bounded locked arena");

    AccountActionCaptcha action{};
    AccountActionCaptchaRequest action_request{};
    action_request.body = utf8_literal(
        R"({"captcha_key":["captcha-required"],"captcha_service":"recaptcha_enterprise","user_flow":"friend/request"})");
    action_request.generation = 51;
    action_request.timeout_ms = 10000;
    action_request.status_code = 400;
    action_request.operation = AccountActionCaptchaOperation::start;
    check(account_action_captcha(&action, nullptr, nullptr, action_request).ok() &&
              wait_phase(&action.bridge, CaptchaBridgePhase::waiting) &&
              target(&action.bridge, 51, &port, state),
          "action captcha: omitted enterprise key starts the existing bridge owner");
    action_request.operation = AccountActionCaptchaOperation::copy_url;
    action_request.url = &other;
    const bool copied = account_action_captcha(&action, nullptr, nullptr, action_request).ok() &&
                        captcha_bridge_url_valid(&other);
    build(true, port, state_view);
    check(copied && exchange(port) && status_code() == 200 &&
              reply_contains(utf8_literal("6LeYqFcqAAAAAD6iZesmNgVulsO4PkpBdr6NVG6M")) &&
              reply_contains(utf8_literal("\"a\":\"friend/request\"")) &&
              reply_contains(utf8_literal("enterprise.js?render=")) &&
              !reply_contains(utf8_literal("a.render(")),
          "action captcha: resolved enterprise key and action reach the score page");
    action_request.operation = AccountActionCaptchaOperation::cancel;
    check(account_action_captcha(&action, nullptr, nullptr, action_request).ok() &&
              !action.bridge.runtime && !action.challenge_ready && !action.token.data &&
              port_released(port),
          "action captcha: cancelling joins the listener and erases the action challenge");
    account_action_captcha_release(&action, nullptr, nullptr);
    secure_buffer_release(&token);
    secure_buffer_release(&other);
    WSACleanup();
}
} // namespace maicord::captcha_bridge_test

namespace maicord {
void test_captcha_bridge_runtime(void (*check)(bool, const char*) noexcept) noexcept {
    captcha_bridge_test::lifecycle(check);
}
} // namespace maicord
