#include "maicord/sys/winsock.hpp"
#include "maicord/app/oauth_sign_in.hpp"
#include "maicord/protocol/writer.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
struct Fake { volatile LONG calls; HANDLE entered, finish; };
UpdateTransportError exchange(void* context, const UpdateHttpRequest& request,
    const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
    auto& f = *static_cast<Fake*>(context);
    check(options.read_error_body && options.body.count && options.headers.count == 2,
          "PKCE exchange uses the bounded token POST transport");
    InterlockedIncrement(&f.calls);
    if (f.entered) { SetEvent(f.entered); WaitForSingleObject(f.finish, 2000); }
    constexpr auto json = utf8_literal(R"({"access_token":"test-access","refresh_token":"test-refresh","expires_in":3600,"token_type":"Bearer"})");
    *response = {}; response->status = 200;
    return request.sink(request.sink_context, {reinterpret_cast<const u8*>(json.data), json.bytes})
        ? UpdateTransportError::none : UpdateTransportError::sink;
}
bool wait_phase(OAuthSignIn& owner, OAuthSignInPhase phase) noexcept {
    for (u32 i = 0; i < 6; ++i) {
        if (oauth_sign_in_status(&owner).phase == phase) return true;
        WaitForSingleObject(oauth_sign_in_notification(&owner), 500);
    }
    return oauth_sign_in_status(&owner).phase == phase;
}
Utf8View parameter(const SecureBuffer& url, Utf8View name) noexcept {
    for (u32 i = 0; i + name.bytes <= url.bytes; ++i) {
        if (!bytes_equal(url.data + i, name.data, name.bytes)) continue;
        const u32 first = i + name.bytes;
        u32 end = first;
        while (end < url.bytes && url.data[end] != '&') ++end;
        return {reinterpret_cast<const char*>(url.data + first), end - first};
    }
    return {};
}
SOCKET connect_to(u16 port) noexcept {
    const SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) return client;
    const DWORD timeout = 1000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    if (connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(client); return INVALID_SOCKET;
    }
    return client;
}
bool callback(u16 port, Utf8View state, bool denied = false, bool expect_reply = true) noexcept {
    u8 wire[1024]; ProtocolWriter writer{{wire, sizeof(wire)}, 0};
    auto add = [&](Utf8View text) noexcept { return protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(text.data), text.bytes); };
    if (!add(utf8_literal("GET /oauth2callback?state=")) || !add(state) ||
        !add(denied ? utf8_literal("&error=access_denied") : utf8_literal("&code=test-code")) ||
        !add(utf8_literal(" HTTP/1.1\r\nHost: 127.0.0.1:")) || !protocol_writer_append_u64(&writer, port) ||
        !add(utf8_literal("\r\n\r\n"))) return false;
    const SOCKET client = connect_to(port);
    if (client == INVALID_SOCKET) return false;
    bool ok = send(client, reinterpret_cast<char*>(wire), 19, 0) == 19 &&
        send(client, reinterpret_cast<char*>(wire + 19), static_cast<int>(writer.used - 19), 0) == static_cast<int>(writer.used - 19);
    char response[512]; u32 received = 0;
    while (ok && received < sizeof(response)) {
        const int bytes = recv(client, response + received, static_cast<int>(sizeof(response) - received), 0);
        if (!bytes) break;
        if (bytes < 0) { ok = !expect_reply; break; }
        received += static_cast<u32>(bytes);
    }
    closesocket(client);
    return ok && (!expect_reply || (received >= 15 && bytes_equal(response, "HTTP/1.1 200 OK", 15)));
}
void receiver(OAuthProvider provider) noexcept {
    const auto client = utf8_literal("0123456789abcdef0123456789abcdef");
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) { check(false, "test loopback Winsock"); return; }
    OAuthSignIn owner{}; Fake fake{};
    const OAuthAuthorizationTransport transport{&fake, exchange};
    const bool started = oauth_sign_in_start(&owner, client, 41, 10000, transport, provider);
    check(started &&
          wait_phase(owner, OAuthSignInPhase::waiting), "sign-in starts an exclusive ephemeral loopback listener");
    SecureBuffer url{}, tokens{};
    check(!oauth_sign_in_url(&owner, 40, &url) && !url.data && oauth_sign_in_url(&owner, 41, &url),
          "authorization URL is available only to the current attempt");
    if (url.data) {
        check(!parameter(url, utf8_literal("client_secret=")).bytes, "browser authorization excludes the client secret");
        const auto state = parameter(url, utf8_literal("&state="));
        const auto status = oauth_sign_in_status(&owner);
        const SOCKET speculative = connect_to(status.port);
        check(state.bytes == 43 && status.port >= 1024 && callback(status.port, utf8_literal("wrong-state"), false, false) &&
              InterlockedCompareExchange(&fake.calls, 0, 0) == 0 && oauth_sign_in_status(&owner).phase == OAuthSignInPhase::waiting,
              "foreign state closes only its request and speculative browser sockets cannot block the real callback");
        if (speculative != INVALID_SOCKET) closesocket(speculative);
        check(callback(status.port, state) && wait_phase(owner, OAuthSignInPhase::ready) &&
              InterlockedCompareExchange(&fake.calls, 0, 0) == 1,
              "fragmented real callback reaches exactly one native token exchange");
        const SOCKET replay = connect_to(status.port);
        check(replay == INVALID_SOCKET, "listener closes before credentials can be replayed");
        if (replay != INVALID_SOCKET) closesocket(replay);
        check(!oauth_sign_in_take(&owner, 40, &tokens) && oauth_sign_in_take(&owner, 41, &tokens) &&
              tokens.locked && tokens.bytes == sizeof(OAuthTokens), "credentials transfer exactly once to the matching attempt");
        secure_buffer_release(&tokens);
        check(!oauth_sign_in_take(&owner, 41, &tokens), "repeated credential take cannot replay a completed attempt");
    }
    secure_buffer_release(&url);
    check(oauth_sign_in_stop(&owner, 1000) && !owner.runtime, "completed sign-in joins and releases its temporary runtime");
    check(oauth_sign_in_start(&owner, client, 42, 10000, transport, provider) &&
          wait_phase(owner, OAuthSignInPhase::waiting) && oauth_sign_in_url(&owner, 42, &url), "new sign-in gets fresh attempt state");
    if (url.data) {
        const auto state = parameter(url, utf8_literal("&state="));
        check(callback(oauth_sign_in_status(&owner).port, state, true) && wait_phase(owner, OAuthSignInPhase::failed) &&
              oauth_sign_in_status(&owner).error == OAuthError::denied && fake.calls == 1,
              "user denial completes without token exchange");
    }
    secure_buffer_release(&url);
    check(oauth_sign_in_stop(&owner, 1000), "denied attempt joins");
    fake.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    fake.finish = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(oauth_sign_in_start(&owner, client, 43, 10000, transport, provider) &&
          wait_phase(owner, OAuthSignInPhase::waiting) && oauth_sign_in_url(&owner, 43, &url), "cancellable sign-in attempt");
    if (url.data) {
        check(callback(oauth_sign_in_status(&owner).port, parameter(url, utf8_literal("&state="))) &&
              WaitForSingleObject(fake.entered, 1000) == WAIT_OBJECT_0, "cancellation fixture enters its transport");
        check(!oauth_sign_in_stop(&owner, 0) && owner.runtime && !oauth_sign_in_take(&owner, 43, &tokens),
              "failed join retains the running worker and cancelled attempt cannot publish credentials");
        SetEvent(fake.finish);
        check(WaitForSingleObject(oauth_sign_in_notification(&owner), 1000) == WAIT_OBJECT_0,
              "cancelled attempt wakes on joined worker completion without a polling timer");
    }
    secure_buffer_release(&url);
    check(oauth_sign_in_stop(&owner, 1000) && !owner.runtime && !tokens.data, "retrying joined stop releases the cancelled exchange");
    CloseHandle(fake.entered); CloseHandle(fake.finish);
    WSACleanup();
}
}
}
extern "C" void maicord_oauth_sign_in_entry() noexcept {
    maicord::receiver(maicord::OAuthProvider::spotify); ExitProcess(maicord::failures ? 1 : 0);
}
