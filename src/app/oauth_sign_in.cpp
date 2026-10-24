#include "maicord/sys/winsock.hpp"
#include "maicord/app/oauth_sign_in.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/win32_process.hpp"

namespace maicord {
namespace {
using Phase = OAuthSignInPhase;
struct Runtime {
    SRWLOCK lock;
    OAuthSignInStatus status;
    OAuthAuthorization authorization;
    OAuthAuthorizationTransport transport;
    SecureBuffer proof, wire, tokens;
    HANDLE thread, stop, notification;
    SOCKET listener, client;
    WSAEVENT listener_event, client_event;
    u64 deadline, client_deadline;
    char client_id[kOAuthClientBytes];
    u32 client_bytes, wire_bytes;
    OAuthProvider provider;
};
Runtime* get(const OAuthSignIn* owner) noexcept { return owner ? static_cast<Runtime*>(owner->runtime) : nullptr; }
bool empty(const SecureBuffer* b) noexcept { return b && !b->data && !b->capacity && !b->bytes && !b->locked; }
bool external(const OAuthSignIn* owner, const Runtime& r, const SecureBuffer* b) noexcept {
    return empty(b) && !byte_ranges_overlap_or_wrap(b, sizeof(*b), owner, sizeof(*owner)) &&
        !byte_ranges_overlap_or_wrap(b, sizeof(*b), &r, sizeof(r)) &&
        !byte_ranges_overlap_or_wrap(b, sizeof(*b), r.proof.data, r.proof.capacity) &&
        !byte_ranges_overlap_or_wrap(b, sizeof(*b), r.wire.data, r.wire.capacity);
}
OAuthSignInStatus snapshot(Runtime& r) noexcept {
    AcquireSRWLockShared(&r.lock); const auto status = r.status; ReleaseSRWLockShared(&r.lock); return status;
}
void terminal(Runtime& r, Phase phase, OAuthError error = {}, u32 native = 0) noexcept {
    AcquireSRWLockExclusive(&r.lock);
    if (r.status.phase == Phase::starting || r.status.phase == Phase::waiting || r.status.phase == Phase::exchanging) {
        r.status.phase = phase; r.status.error = error; r.status.native_error = native;
    }
    ReleaseSRWLockExclusive(&r.lock); SetEvent(r.notification);
}
void close_client(Runtime& r) noexcept {
    if (r.client != INVALID_SOCKET) { closesocket(r.client); r.client = INVALID_SOCKET; }
    r.wire_bytes = 0; r.client_deadline = 0;
    if (r.wire.data) secure_clear_bytes(r.wire.data, 8192);
}
bool bind_listener(Runtime& r) noexcept {
    r.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const BOOL exclusive = TRUE;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(address);
    if (r.listener == INVALID_SOCKET ||
        setsockopt(r.listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR ||
        bind(r.listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        getsockname(r.listener, reinterpret_cast<sockaddr*>(&address), &size) == SOCKET_ERROR ||
        size != sizeof(address) || ntohs(address.sin_port) < 1024 || listen(r.listener, 2) == SOCKET_ERROR ||
        WSAEventSelect(r.listener, r.listener_event, FD_ACCEPT | FD_CLOSE) == SOCKET_ERROR) return false;
    AcquireSRWLockExclusive(&r.lock);
    if (r.status.phase == Phase::starting) { r.status.port = ntohs(address.sin_port); r.status.phase = Phase::waiting; }
    ReleaseSRWLockExclusive(&r.lock); SetEvent(r.notification);
    return true;
}
void accept_client(Runtime& r) noexcept {
    sockaddr_in peer{}; int size = sizeof(peer);
    const SOCKET client = accept(r.listener, reinterpret_cast<sockaddr*>(&peer), &size);
    if (client == INVALID_SOCKET) return;
    // A speculative browser connection with no request yields to its real GET.
    if ((r.client != INVALID_SOCKET && r.wire_bytes) || size != sizeof(peer) ||
        peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
        WSAEventSelect(client, r.client_event, FD_READ | FD_CLOSE | FD_WRITE) == SOCKET_ERROR) {
        closesocket(client); return;
    }
    close_client(r); r.client = client;
    r.client_deadline = earliest_deadline(r.deadline, deadline_after(GetTickCount64(), 5000));
}
void reply(Runtime& r) noexcept {
    constexpr char text[] = "HTTP/1.1 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "Content-Type: text/plain; charset=utf-8\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'\r\n\r\nReturn to MaiCord to finish signing in.";
    u32 sent = 0;
    const u64 deadline = earliest_deadline(r.deadline, deadline_after(GetTickCount64(), 1000));
    while (sent < sizeof(text) - 1 && !deadline_due(GetTickCount64(), deadline)) {
        const int bytes = send(r.client, text + sent, static_cast<int>(sizeof(text) - 1 - sent), 0);
        if (bytes > 0) { sent += static_cast<u32>(bytes); continue; }
        if (!bytes || WSAGetLastError() != WSAEWOULDBLOCK) break;
        HANDLE handles[]{r.stop, r.client_event};
        if (WaitForMultipleObjects(2, handles, FALSE, wait_milliseconds_until(GetTickCount64(), deadline)) != WAIT_OBJECT_0 + 1) break;
        WSANETWORKEVENTS events{};
        if (WSAEnumNetworkEvents(r.client, r.client_event, &events) == SOCKET_ERROR || (events.lNetworkEvents & FD_CLOSE)) break;
    }
    shutdown(r.client, SD_SEND);
}
bool receive(Runtime& r) noexcept {
    const int bytes = recv(r.client, reinterpret_cast<char*>(r.wire.data + r.wire_bytes), static_cast<int>(8192 - r.wire_bytes), 0);
    if (bytes <= 0) {
        if (!bytes || WSAGetLastError() != WSAEWOULDBLOCK) close_client(r);
        return false;
    }
    r.wire_bytes += static_cast<u32>(bytes);
    const auto& proof = *reinterpret_cast<OAuthProof*>(r.proof.data);
    const auto callback = oauth_callback(r.provider, {reinterpret_cast<char*>(r.wire.data), r.wire_bytes}, snapshot(r).port,
        {proof.state, kOAuthProofBytes}, {r.wire.data + 8192, kOAuthCodeBytes});
    if (!callback.complete) return false;
    if (callback.error != OAuthError::none && callback.error != OAuthError::denied) {
        close_client(r); return false;
    }
    AcquireSRWLockExclusive(&r.lock);
    const bool admitted = r.status.phase == Phase::waiting && !deadline_due(GetTickCount64(), r.deadline);
    if (admitted) r.status.phase = Phase::exchanging;
    ReleaseSRWLockExclusive(&r.lock);
    if (!admitted) return true;
    // Spending the state closes admission before any token exchange or reply.
    closesocket(r.listener); r.listener = INVALID_SOCKET;
    reply(r); close_client(r); SetEvent(r.notification);
    if (callback.error == OAuthError::denied) { terminal(r, Phase::failed, callback.error); return true; }
    SecureBuffer tokens{};
    const auto result = oauth_authorization_exchange(&r.authorization, {r.client_id, r.client_bytes}, snapshot(r).port,
        {reinterpret_cast<char*>(r.wire.data + 8192), callback.code_bytes}, {proof.verifier, kOAuthProofBytes}, &tokens, r.transport,
        r.provider);
    secure_clear_bytes(r.wire.data + 8192, kOAuthCodeBytes);
    AcquireSRWLockExclusive(&r.lock);
    if (r.status.phase == Phase::exchanging) {
        r.status.http_status = result.status;
        r.status.error = result.error;
        if (deadline_due(GetTickCount64(), r.deadline)) r.status.phase = Phase::expired;
        else if (result.ok() && secure_buffer_transfer(&r.tokens, &tokens)) r.status.phase = Phase::ready;
        else r.status.phase = Phase::failed;
    }
    ReleaseSRWLockExclusive(&r.lock);
    secure_buffer_release(&tokens); SetEvent(r.notification);
    return true;
}
DWORD WINAPI run(void* opaque) noexcept {
    auto& r = *static_cast<Runtime*>(opaque);
    WSADATA data{}; const int started = WSAStartup(MAKEWORD(2, 2), &data);
    if (started) terminal(r, Phase::failed, OAuthError::unavailable, static_cast<u32>(started));
    else {
        r.listener_event = WSACreateEvent(); r.client_event = WSACreateEvent();
        if (r.listener_event == WSA_INVALID_EVENT || r.client_event == WSA_INVALID_EVENT || !bind_listener(r))
            terminal(r, Phase::failed, OAuthError::unavailable, static_cast<u32>(WSAGetLastError()));
        while (snapshot(r).phase == Phase::waiting) {
            const u64 now = GetTickCount64();
            if (deadline_due(now, r.deadline)) { terminal(r, Phase::expired); break; }
            if (deadline_due(now, r.client_deadline)) close_client(r);
            HANDLE handles[]{r.stop, r.client_event, r.listener_event};
            const auto waited = WaitForMultipleObjects(3, handles, FALSE,
                wait_milliseconds_until(now, earliest_deadline(r.deadline, r.client_deadline)));
            if (waited == WAIT_OBJECT_0) break;
            if (waited == WAIT_TIMEOUT) continue;
            WSANETWORKEVENTS events{};
            if (waited == WAIT_OBJECT_0 + 1) {
                if (r.client == INVALID_SOCKET) { WSAResetEvent(r.client_event); continue; }
                if (WSAEnumNetworkEvents(r.client, r.client_event, &events) == SOCKET_ERROR) { close_client(r); continue; }
                if ((events.lNetworkEvents & FD_READ) && receive(r)) break;
                if (events.lNetworkEvents & FD_CLOSE) close_client(r);
            } else if (waited == WAIT_OBJECT_0 + 2) {
                if (WSAEnumNetworkEvents(r.listener, r.listener_event, &events) == SOCKET_ERROR || (events.lNetworkEvents & FD_CLOSE)) {
                    terminal(r, Phase::failed, OAuthError::unavailable); break;
                }
                if (events.lNetworkEvents & FD_ACCEPT) accept_client(r);
            } else { terminal(r, Phase::failed, OAuthError::unavailable, thread_last_error()); break; }
        }
        close_client(r);
        if (r.listener != INVALID_SOCKET) { closesocket(r.listener); r.listener = INVALID_SOCKET; }
        if (r.listener_event != WSA_INVALID_EVENT) WSACloseEvent(r.listener_event);
        if (r.client_event != WSA_INVALID_EVENT) WSACloseEvent(r.client_event);
        WSACleanup();
    }
    AcquireSRWLockExclusive(&r.lock);
    secure_buffer_release(&r.proof); secure_buffer_release(&r.wire);
    ReleaseSRWLockExclusive(&r.lock);
    SetEvent(r.notification);
    return 0;
}
void destroy(OAuthSignIn* owner) noexcept {
    auto& r = *get(owner);
    if (r.thread) CloseHandle(r.thread);
    if (r.stop) CloseHandle(r.stop);
    if (r.notification) CloseHandle(r.notification);
    secure_buffer_release(&r.proof); secure_buffer_release(&r.wire); secure_buffer_release(&r.tokens);
    oauth_authorization_release(&r.authorization);
    secure_clear_bytes(&r, sizeof(r)); VirtualFree(&r, 0, MEM_RELEASE); owner->runtime = nullptr;
}
}
bool oauth_sign_in_start(OAuthSignIn* owner, Utf8View client, u64 generation, u32 timeout_ms,
                           OAuthAuthorizationTransport transport, OAuthProvider provider) noexcept {
    if (!owner || owner->runtime || !generation || timeout_ms < 1000 || timeout_ms > 300000 || !oauth_client_valid(provider, client)) return false;
    auto* r = static_cast<Runtime*>(VirtualAlloc(nullptr, sizeof(Runtime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!r) return false;
    owner->runtime = r;
    r->listener = r->client = INVALID_SOCKET;
    r->listener_event = r->client_event = WSA_INVALID_EVENT;
    r->transport = transport; r->status = {generation, Phase::starting}; r->provider = provider;
    r->deadline = deadline_after(GetTickCount64(), timeout_ms);
    copy_bytes(r->client_id, client.data, client.bytes); r->client_bytes = client.bytes;
    r->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    r->notification = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!r->stop || !r->notification || !oauth_authorization_proof(&r->proof) ||
        !secure_buffer_initialize(&r->wire, 8192 + kOAuthCodeBytes).ok()) { destroy(owner); return false; }
    r->thread = CreateThread(nullptr, 0, run, r, 0, nullptr);
    if (!r->thread) { destroy(owner); return false; }
    return true;
}
OAuthSignInStatus oauth_sign_in_status(const OAuthSignIn* owner) noexcept { auto* r = get(owner); return r ? snapshot(*r) : OAuthSignInStatus{}; }
HANDLE oauth_sign_in_notification(const OAuthSignIn* owner) noexcept {
    auto* r = get(owner);
    if (!r) return nullptr;
    // After cancellation, wait on actual worker completion rather than polling
    // a join or losing the wake between its final notification and thread exit.
    return snapshot(*r).phase == Phase::cancelled ? r->thread : r->notification;
}
bool oauth_sign_in_url(const OAuthSignIn* owner, u64 generation, SecureBuffer* output) noexcept {
    auto* r = get(owner); if (!r || !empty(output)) return false;
    SecureBuffer url{};
    if (!secure_buffer_initialize(&url, 1024).ok()) return false;
    AcquireSRWLockShared(&r->lock);
    if (external(owner, *r, output) && r->status.generation == generation && r->status.phase == Phase::waiting) {
        const auto& proof = *reinterpret_cast<OAuthProof*>(r->proof.data);
        url.bytes = oauth_authorize(r->provider, {r->client_id, r->client_bytes}, r->status.port,
            {proof.state, kOAuthProofBytes}, {proof.challenge, kOAuthProofBytes}, {url.data, url.capacity});
    }
    ReleaseSRWLockShared(&r->lock);
    const bool copied = url.bytes && secure_buffer_transfer(output, &url);
    secure_buffer_release(&url); return copied;
}
bool oauth_sign_in_take(OAuthSignIn* owner, u64 generation, SecureBuffer* output) noexcept {
    auto* r = get(owner); if (!r) return false;
    AcquireSRWLockExclusive(&r->lock);
    const bool taken = external(owner, *r, output) && r->status.generation == generation && r->status.phase == Phase::ready &&
                       secure_buffer_transfer(output, &r->tokens);
    if (taken) r->status.phase = Phase::complete;
    ReleaseSRWLockExclusive(&r->lock); return taken;
}
bool oauth_sign_in_stop(OAuthSignIn* owner, u32 timeout_ms) noexcept {
    if (!owner || timeout_ms > 5000) return false;
    auto* r = get(owner); if (!r) return true;
    AcquireSRWLockExclusive(&r->lock);
    r->status.phase = Phase::cancelled;
    secure_buffer_release(&r->tokens);
    oauth_authorization_cancel(&r->authorization);
    ReleaseSRWLockExclusive(&r->lock); SetEvent(r->stop);
    if (WaitForSingleObject(r->thread, timeout_ms) != WAIT_OBJECT_0) return false;
    destroy(owner); return true;
}
} // namespace maicord
