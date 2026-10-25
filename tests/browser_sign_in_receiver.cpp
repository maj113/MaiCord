#include "maicord/sys/winsock.hpp"
#include "maicord/app/browser_sign_in_receiver.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
// The live fixture talks only to the receiver's own loopback listener.
constexpr u32 kPatienceMs = 10000;
bool wait_phase(const BrowserSignInReceiver* receiver, BrowserSignInReceiverPhase phase) noexcept {
    const u64 end = GetTickCount64() + kPatienceMs;
    while (browser_sign_in_receiver_status(receiver).phase != phase) {
        const u64 now = GetTickCount64();
        const HANDLE event = browser_sign_in_receiver_notification_handle(receiver);
        if (now >= end || !event || WaitForSingleObject(event, static_cast<DWORD>(end - now)) == WAIT_FAILED) return false;
    }
    return true;
}
bool contains(const u8* data, u32 bytes, Utf8View needle) noexcept {
    for (u32 i = 0; i + needle.bytes <= bytes; ++i)
        if (bytes_equal(data + i, needle.data, needle.bytes)) return true;
    return false;
}
struct Wire {
    u8 data[2048];
    u32 bytes;
    void add(Utf8View value) noexcept { copy_bytes(data + bytes, value.data, value.bytes); bytes += value.bytes; }
    void number(u32 value) noexcept { bytes += decimal_write(data + bytes, value); }
};
// One POST /rpc; returns the reply length (0: the server closed without one).
u32 post(u16 port, Utf8View host_suffix, u8* reply, u32 capacity) noexcept {
    constexpr Utf8View body = utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{\"handoffToken\":\"synthetic-token\"},\"nonce\":\"n1\"}");
    Wire wire{};
    wire.add(utf8_literal("POST /rpc?v=1 HTTP/1.1\r\nHost: 127.0.0.1:")); wire.number(port); wire.add(host_suffix);
    wire.add(utf8_literal("\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\nContent-Length: ")); wire.number(body.bytes);
    wire.add(utf8_literal("\r\n\r\n")); wire.add(body);
    const SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) return 0;
    const DWORD timeout = kPatienceMs;
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    u32 received = 0;
    if (connect(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
        send(handle, reinterpret_cast<const char*>(wire.data), static_cast<int>(wire.bytes), 0) == static_cast<int>(wire.bytes)) {
        for (;;) {
            const int count = recv(handle, reinterpret_cast<char*>(reply + received), static_cast<int>(capacity - received), 0);
            if (count <= 0) break;
            received += static_cast<u32>(count);
        }
    }
    closesocket(handle);
    return received;
}
void live_worker(void (*check)(bool, const char*) noexcept) noexcept {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) { check(false, "receiver worker: Winsock starts"); return; }
    BrowserSignInReceiver receiver{};
    check(browser_sign_in_receiver_start(&receiver, 7, 20000).ok(), "receiver worker: a bounded attempt starts");
    if (!wait_phase(&receiver, BrowserSignInReceiverPhase::waiting)) {
        // Every port of the fixed range may be held by another program.
        check(browser_sign_in_receiver_status(&receiver).error == BrowserSignInReceiverError::ports_busy,
            "receiver worker: only a fully occupied port range prevents listening");
        browser_sign_in_receiver_stop(&receiver, 5000);
        WSACleanup();
        return;
    }
    const u16 port = browser_sign_in_receiver_status(&receiver).port;
    check(port >= kBrowserSignInFirstPort && port < kBrowserSignInFirstPort + kBrowserSignInPortCount,
        "receiver worker: listens on the first free port of the fixed loopback range");
    u8 reply[4096];
    check(post(port, utf8_literal("0"), reply, sizeof(reply)) == 0 &&
            browser_sign_in_receiver_status(&receiver).phase == BrowserSignInReceiverPhase::waiting,
        "receiver worker: a request naming another host is dropped without a reply or a candidate");
    const u32 bytes = post(port, {}, reply, sizeof(reply));
    check(bytes > 12 && bytes_equal(reply, "HTTP/1.1 200", 12) && wait_phase(&receiver, BrowserSignInReceiverPhase::candidate_ready),
        "receiver worker: the official callback is answered and published as one candidate");
    SecureBuffer exchange{};
    if (secure_buffer_initialize(&exchange, kBrowserSignInMaximumBodyBytes).ok()) {
        const u64 candidate = browser_sign_in_receiver_status(&receiver).candidate_id;
        check(browser_sign_in_receiver_take_exchange(&receiver, 7, candidate, &exchange).ok() &&
                contains(exchange.data, exchange.bytes, utf8_literal("\"handoff_token\":\"synthetic-token\"")) &&
                browser_sign_in_receiver_finish_exchange(&receiver, 7, candidate, true).phase == BrowserSignInReceiverPhase::complete,
            "receiver worker: the candidate leaves once, bound to the attempt key");
        secure_buffer_release(&exchange);
    }
    check(browser_sign_in_receiver_stop(&receiver, 5000).phase == BrowserSignInReceiverPhase::complete && !receiver.runtime,
        "receiver worker: stop joins the worker and releases the runtime");
    WSACleanup();
}
} // namespace

// The shared worker is covered live here and in the captcha bridge fixture.
void test_browser_sign_in_receiver(void (*check)(bool, const char*) noexcept) noexcept {
    live_worker(check);
    BrowserSignInReceiver receiver{};
    check(browser_sign_in_receiver_status(&receiver).phase == BrowserSignInReceiverPhase::empty &&
        !browser_sign_in_receiver_notification_handle(&receiver), "empty browser receiver has no handles, retained credentials or published candidate");
    check(browser_sign_in_receiver_start(&receiver, 0, 1000).error == BrowserSignInReceiverError::invalid_input &&
        browser_sign_in_receiver_start(&receiver, 1, 999).error == BrowserSignInReceiverError::invalid_input &&
        browser_sign_in_receiver_start(&receiver, 1, 300001).error == BrowserSignInReceiverError::invalid_input && !receiver.runtime,
        "invalid generations/deadlines fail before creating a worker/listener");
    SecureBuffer output{};
    const auto initialized = secure_buffer_initialize(&output, 256);
    check(initialized.ok(), "browser fixture uses externally owned locked snapshot output");
    if (initialized.ok()) {
        constexpr Utf8View marker = utf8_literal("synthetic-preserved");
        secure_buffer_assign(&output, {reinterpret_cast<const u8*>(marker.data), marker.bytes});
        check(!browser_sign_in_receiver_copy_url(&receiver, 1, &output).ok() &&
            !browser_sign_in_receiver_take_exchange(&receiver, 1, 1, &output).ok() &&
            output.bytes == marker.bytes && bytes_equal(output.data, marker.data, marker.bytes),
            "absent/stale owners reject secret snapshots without modifying caller output");
        secure_buffer_release(&output);
    }
    check(browser_sign_in_receiver_finish_exchange(&receiver, 1, 1, true).error == BrowserSignInReceiverError::invalid_input &&
        browser_sign_in_receiver_finish_exchange(&receiver, 1, 1, false).error == BrowserSignInReceiverError::invalid_input,
        "an absent owner cannot accept or reject an exchange as an authenticated session");
    check(browser_sign_in_receiver_stop(&receiver, 0).ok() && !receiver.runtime,
        "stopping an empty receiver is idempotent and does not allocate");
    check(browser_sign_in_receiver_runtime_bytes() >= 40 * 1024 && browser_sign_in_receiver_runtime_bytes() <= 48 * 1024,
        "receiver has one bounded locked request/credential arena and a page-sized metadata owner");
}
} // namespace maicord
