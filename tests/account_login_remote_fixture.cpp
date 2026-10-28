// Actual QR owner implementation with a synthetic, held terminal worker.
// No DNS, socket, service request, or user credential is involved.
#include "../src/app/remote_auth_client.cpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::login_handoff_test {
struct HeldWorker { RemoteAuthClient* client; HANDLE release; } held{};
DWORD WINAPI held_authenticated_worker(void* opaque) noexcept {
    auto* owner = static_cast<HeldWorker*>(opaque);
    auto* runtime = static_cast<RemoteAuthClientRuntime*>(owner->client->runtime);
    AcquireSRWLockExclusive(owner->client->snapshot_lock.native());
    publish_phase(owner->client, RemoteAuthClientPhase::authenticated);
    publish_event(owner->client, runtime, RemoteAuthClientEventKind::authenticated);
    ReleaseSRWLockExclusive(owner->client->snapshot_lock.native());
    SetEvent(owner->client->ready_event);
    WaitForSingleObject(owner->release, INFINITE);
    return 0;
}
// The synthetic runtime, its events and the protected credential both fixtures hold.
bool prepare_runtime(RemoteAuthClient* client) noexcept {
    auto* runtime = static_cast<RemoteAuthClientRuntime*>(VirtualAlloc(nullptr,
        sizeof(RemoteAuthClientRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime || !remote_auth_ticket_exchange_initialize(&runtime->ticket_exchange).ok()) return false;
    client->runtime = runtime;
    client->ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client->notification_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client->cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client->captcha_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    constexpr auto token = utf8_literal("synthetic-session");
    return client->ready_event && client->notification_event && client->cancel_event &&
        client->captcha_event && secure_buffer_assign(&runtime->ticket_exchange.credential,
            {reinterpret_cast<const u8*>(token.data), token.bytes}).ok();
}
bool prepare_remote(RemoteAuthClient* client, HANDLE* release) noexcept {
    *release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!prepare_runtime(client) || !*release) return false;
    store_user(client, utf8_literal("42:0:0:synthetic"));
    held = {client, *release};
    client->thread = CreateThread(nullptr, 0, held_authenticated_worker, &held, 0, nullptr);
    return client->thread && WaitForSingleObject(client->ready_event, 3000) == WAIT_OBJECT_0;
}
bool remote_credential_erased(const RemoteAuthClient& client) noexcept {
    const auto* runtime = static_cast<const RemoteAuthClientRuntime*>(client.runtime);
    if (!runtime) return true;
    const auto& credential = runtime->ticket_exchange.credential;
    if (credential.bytes) return false;
    for (u32 i = 0; i < credential.capacity; ++i) if (credential.data[i]) return false;
    return true;
}
DWORD WINAPI publish_limited_during_stop(void* opaque) noexcept {
    auto* client = static_cast<RemoteAuthClient*>(opaque);
    auto* runtime = static_cast<RemoteAuthClientRuntime*>(client->runtime);
    SetEvent(client->ready_event);
    // A deterministic late receipt: no status poll can observe it before the
    // actual production stop requests cancellation and joins this worker.
    WaitForSingleObject(client->cancel_event, INFINITE);
    AcquireSRWLockExclusive(client->snapshot_lock.native());
    RemoteAuthTicketExchangeResult receipt{};
    receipt.http_status = 429;
    receipt.retry_at_ms = 900'000'321;
    receipt.error = RemoteAuthTicketExchangeError::cancelled;
    publish_ticket_status(client, receipt);
    publish_phase(client, RemoteAuthClientPhase::cancelled);
    publish_event(client, runtime, RemoteAuthClientEventKind::cancelled);
    ReleaseSRWLockExclusive(client->snapshot_lock.native());
    return 0;
}
bool prepare_remote_stop_limit(RemoteAuthClient* client) noexcept {
    if (!prepare_runtime(client)) return false;
    publish_phase(client, RemoteAuthClientPhase::qr_ready);
    client->thread = CreateThread(nullptr, 0, publish_limited_during_stop, client, 0, nullptr);
    return client->thread && WaitForSingleObject(client->ready_event, 3000) == WAIT_OBJECT_0;
}
} // namespace maicord::login_handoff_test
