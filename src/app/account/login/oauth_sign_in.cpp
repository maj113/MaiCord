#include "maicord/platform/net/socket.hpp"
#include "maicord/app/account/login/oauth_sign_in.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"

namespace maicord {
namespace {
// This worker keeps its own loopback listener instead of LoopbackServer: after the callback it spends
// the state, closes the listener, replies synchronously and only then runs the blocking token exchange
// on the same thread. LoopbackServer flushes the reply after its request hook returns, so the exchange
// would precede the reply, and it has no hook that runs after the reply.
using Phase = OAuthSignInPhase;
struct Runtime {
    SRWLOCK lock;
    OAuthSignInStatus status;
    OAuthAuthorization authorization;
    OAuthAuthorizationTransport transport;
    SecureBuffer proof, wire, tokens;
    HANDLE thread, stop, notification, finished;
    Socket listener, client;
    SocketWatch listener_watch, client_watch;
    u64 deadline, client_deadline;
    char client_id[kOAuthClientBytes];
    u32 client_bytes, wire_bytes;
    OAuthProvider provider;
};
Runtime* get(const OAuthSignIn* owner) noexcept { return owner ? static_cast<Runtime*>(owner->runtime) : nullptr; }
bool empty(const SecureBuffer* b) noexcept { return b && !b->data && !b->capacity && !b->bytes && !b->locked; }
OAuthSignInStatus snapshot(Runtime& r) noexcept {
    AcquireSRWLockShared(&r.lock); const auto status = r.status; ReleaseSRWLockShared(&r.lock); return status;
}
void terminal(Runtime& r, Phase phase, OAuthError error = {}, u32 native = 0) noexcept {
    AcquireSRWLockExclusive(&r.lock);
    if (r.status.phase == Phase::starting || r.status.phase == Phase::waiting || r.status.phase == Phase::exchanging) {
        r.status.phase = phase; r.status.error = error; r.status.native_error = native;
    }
    ReleaseSRWLockExclusive(&r.lock); event_set(r.notification);
}
void close_client(Runtime& r) noexcept {
    socket_close(&r.client);
    r.wire_bytes = 0; r.client_deadline = 0;
    if (r.wire.data) secure_clear_bytes(r.wire.data, 8192);
}
bool bind_listener(Runtime& r) noexcept {
    const SocketListenResult listening = socket_listen_loopback(&r.listener, 0, 0, 2);
    if (!listening.ok() || !socket_watch_attach(&r.listener_watch, &r.listener, kSocketAccept | kSocketClose)) return false;
    AcquireSRWLockExclusive(&r.lock);
    if (r.status.phase == Phase::starting) { r.status.port = listening.port; r.status.phase = Phase::waiting; }
    ReleaseSRWLockExclusive(&r.lock); event_set(r.notification);
    return true;
}
void accept_client(Runtime& r) noexcept {
    Socket client{};
    bool from_loopback = false;
    i32 error = 0;
    if (!socket_accept(&r.listener, &client, &from_loopback, &error)) return;
    // A speculative browser connection with no request yields to its real GET.
    if ((socket_valid(r.client) && r.wire_bytes) || !from_loopback ||
        !socket_watch_attach(&r.client_watch, &client, kSocketRead | kSocketClose | kSocketWrite)) {
        socket_close(&client); return;
    }
    close_client(r); r.client = client;
    r.client_deadline = earliest_deadline(r.deadline, deadline_after(clock_monotonic_milliseconds(), 5000));
}
void reply(Runtime& r) noexcept {
    static constexpr char text[] = "HTTP/1.1 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "Content-Type: text/plain; charset=utf-8\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'\r\n\r\nReturn to MaiCord to finish signing in.";
    u32 sent = 0;
    const u64 deadline = earliest_deadline(r.deadline, deadline_after(clock_monotonic_milliseconds(), 1000));
    while (sent < sizeof(text) - 1 && !deadline_due(clock_monotonic_milliseconds(), deadline)) {
        const i32 bytes = socket_send(r.client, text + sent, sizeof(text) - 1 - sent);
        if (bytes > 0) { sent += static_cast<u32>(bytes); continue; }
        if (!bytes || network_last_error() != WSAEWOULDBLOCK) break;
        Waitable handles[]{r.stop, socket_watch_waitable(r.client_watch)};
        if (wait_any(handles, 2, wait_milliseconds_until(clock_monotonic_milliseconds(), deadline)) != WAIT_OBJECT_0 + 1) break;
        u32 events = 0;
        if (!socket_watch_poll(&r.client_watch, r.client, &events) || (events & kSocketClose)) break;
    }
    socket_shutdown_send(r.client);
}
bool receive(Runtime& r) noexcept {
    const i32 bytes = socket_receive(r.client, r.wire.data + r.wire_bytes, 8192 - r.wire_bytes);
    if (bytes <= 0) {
        if (!bytes || network_last_error() != WSAEWOULDBLOCK) close_client(r);
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
    const bool admitted = r.status.phase == Phase::waiting && !deadline_due(clock_monotonic_milliseconds(), r.deadline);
    if (admitted) r.status.phase = Phase::exchanging;
    ReleaseSRWLockExclusive(&r.lock);
    if (!admitted) return true;
    // Spending the state closes admission before any token exchange or reply.
    socket_close(&r.listener);
    reply(r); close_client(r); event_set(r.notification);
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
        if (deadline_due(clock_monotonic_milliseconds(), r.deadline)) r.status.phase = Phase::expired;
        else if (result.ok() && secure_buffer_transfer(&r.tokens, &tokens)) r.status.phase = Phase::ready;
        else r.status.phase = Phase::failed;
    }
    ReleaseSRWLockExclusive(&r.lock);
    secure_buffer_release(&tokens); event_set(r.notification);
    return true;
}
u32 run(void* opaque) noexcept {
    auto& r = *static_cast<Runtime*>(opaque);
    const i32 started = socket_startup();
    if (started) terminal(r, Phase::failed, OAuthError::unavailable, static_cast<u32>(started));
    else {
        if (!socket_watch_open(&r.listener_watch) || !socket_watch_open(&r.client_watch) || !bind_listener(r))
            terminal(r, Phase::failed, OAuthError::unavailable, static_cast<u32>(network_last_error()));
        while (snapshot(r).phase == Phase::waiting) {
            const u64 now = clock_monotonic_milliseconds();
            if (deadline_due(now, r.deadline)) { terminal(r, Phase::expired); break; }
            if (deadline_due(now, r.client_deadline)) close_client(r);
            Waitable handles[]{r.stop, socket_watch_waitable(r.client_watch), socket_watch_waitable(r.listener_watch)};
            const auto waited = wait_any(handles, 3,
                wait_milliseconds_until(now, earliest_deadline(r.deadline, r.client_deadline)));
            if (waited == WAIT_OBJECT_0) break;
            if (waited == WAIT_TIMEOUT) continue;
            u32 events = 0;
            if (waited == WAIT_OBJECT_0 + 1) {
                if (!socket_valid(r.client)) { socket_watch_reset(&r.client_watch); continue; }
                if (!socket_watch_poll(&r.client_watch, r.client, &events)) { close_client(r); continue; }
                if ((events & kSocketRead) && receive(r)) break;
                if (events & kSocketClose) close_client(r);
            } else if (waited == WAIT_OBJECT_0 + 2) {
                if (!socket_watch_poll(&r.listener_watch, r.listener, &events) || (events & kSocketClose)) {
                    terminal(r, Phase::failed, OAuthError::unavailable); break;
                }
                if (events & kSocketAccept) accept_client(r);
            } else { terminal(r, Phase::failed, OAuthError::unavailable, thread_last_error()); break; }
        }
        close_client(r);
        socket_close(&r.listener);
        socket_watch_close(&r.listener_watch);
        socket_watch_close(&r.client_watch);
        socket_cleanup();
    }
    AcquireSRWLockExclusive(&r.lock);
    secure_buffer_release(&r.proof); secure_buffer_release(&r.wire);
    ReleaseSRWLockExclusive(&r.lock);
    event_set(r.notification);
    event_set(r.finished);
    return 0;
}
void destroy(OAuthSignIn* owner) noexcept {
    auto& r = *get(owner);
    if (r.thread) thread_close(static_cast<Thread*>(r.thread));
    if (r.stop) waitable_close(r.stop);
    if (r.notification) waitable_close(r.notification);
    if (r.finished) waitable_close(r.finished);
    secure_buffer_release(&r.proof); secure_buffer_release(&r.wire); secure_buffer_release(&r.tokens);
    oauth_authorization_release(&r.authorization);
    secure_clear_bytes(&r, sizeof(r)); pages_release(&r); owner->runtime = nullptr;
}
}
bool oauth_sign_in_start(OAuthSignIn* owner, Utf8View client, u64 generation, u32 timeout_ms,
                           OAuthAuthorizationTransport transport, OAuthProvider provider) noexcept {
    if (!owner || owner->runtime || !generation || timeout_ms < 1000 || timeout_ms > 300000 || !oauth_client_valid(provider, client)) return false;
    auto* r = commit_pages<Runtime>();
    if (!r) return false;
    owner->runtime = r;
    r->listener = r->client = Socket{};
    r->listener_watch = r->client_watch = SocketWatch{};
    r->transport = transport; r->status = {generation, Phase::starting}; r->provider = provider;
    r->deadline = deadline_after(clock_monotonic_milliseconds(), timeout_ms);
    copy_bytes(r->client_id, client.data, client.bytes); r->client_bytes = client.bytes;
    r->stop = event_create(true, false);
    r->notification = event_create(false, false);
    r->finished = event_create(true, false);
    if (!r->stop || !r->notification || !r->finished || !oauth_authorization_proof(&r->proof) ||
        !secure_buffer_initialize(&r->wire, 8192 + kOAuthCodeBytes).ok()) { destroy(owner); return false; }
    r->thread = thread_start(run, r, 0, nullptr);
    if (!r->thread) { destroy(owner); return false; }
    return true;
}
OAuthSignInStatus oauth_sign_in_status(const OAuthSignIn* owner) noexcept { auto* r = get(owner); return r ? snapshot(*r) : OAuthSignInStatus{}; }
HANDLE oauth_sign_in_notification(const OAuthSignIn* owner) noexcept {
    auto* r = get(owner);
    if (!r) return nullptr;
    // After cancellation, wait on actual worker completion rather than polling
    // a join or losing the wake between its final notification and its exit.
    return snapshot(*r).phase == Phase::cancelled ? r->finished : r->notification;
}
bool oauth_sign_in_url(const OAuthSignIn* owner, u64 generation, SecureBuffer* output) noexcept {
    auto* r = get(owner); if (!r || !empty(output)) return false;
    SecureBuffer url{};
    if (!secure_buffer_initialize(&url, 1024).ok()) return false;
    AcquireSRWLockShared(&r->lock);
    if (r->status.generation == generation && r->status.phase == Phase::waiting) {
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
    const bool taken = empty(output) && r->status.generation == generation && r->status.phase == Phase::ready &&
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
    ReleaseSRWLockExclusive(&r->lock); event_set(r->stop);
    if (!thread_wait(static_cast<Thread*>(r->thread), timeout_ms)) return false;
    destroy(owner); return true;
}
} // namespace maicord
