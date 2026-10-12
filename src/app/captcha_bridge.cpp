#include <WinSock2.h>
#include "maicord/app/captcha_bridge.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/platform/random.hpp"

namespace maicord {
namespace {
constexpr u32 kWireBytes = kCaptchaBridgeMaximumHeaderBytes + kCaptchaBridgeMaximumBodyBytes;
constexpr u32 kResponseOffset = kWireBytes;
constexpr u32 kResponseBytes = kCaptchaBridgeMaximumResponseBytes;
constexpr u32 kTokenOffset = kResponseOffset + kResponseBytes;
constexpr u32 kSitekeyOffset = kTokenOffset + kCaptchaBridgeMaximumTokenBytes;
constexpr u32 kRqdataOffset = kSitekeyOffset + kCaptchaBridgeMaximumSitekeyBytes;
constexpr u32 kStateOffset = kRqdataOffset + kCaptchaBridgeMaximumRqdataBytes;
constexpr u32 kNonceOffset = kStateOffset + kCaptchaBridgeStateBytes;
constexpr u32 kStorageBytes = kNonceOffset + kCaptchaBridgeNonceBytes;
constexpr u32 kClientTimeoutMs = 5000;

struct Runtime {
    SRWLOCK lock;
    SecureBuffer storage;
    CaptchaBridgeStatus status;
    HANDLE stop_event, notification, thread;
    WSAEVENT listener_event, client_event;
    SOCKET listener, client;
    u64 client_deadline;
    u32 wire_bytes, response_bytes, response_sent, token_bytes, sitekey_bytes, rqdata_bytes;
    CaptchaBridgeProvider provider;
    bool invisible, spent;
};
Runtime* runtime_of(const CaptchaBridge* bridge) noexcept {
    return bridge ? static_cast<Runtime*>(bridge->runtime) : nullptr;
}
u32 owner_bytes() noexcept { return static_cast<u32>((sizeof(Runtime) + 4095) & ~usize{4095}); }
u8* at(Runtime* runtime, u32 offset) noexcept { return runtime->storage.data + offset; }
Utf8View view(Runtime* runtime, u32 offset, u32 bytes) noexcept {
    return {reinterpret_cast<const char*>(runtime->storage.data + offset), bytes};
}
bool live(CaptchaBridgePhase phase) noexcept {
    return phase == CaptchaBridgePhase::starting || phase == CaptchaBridgePhase::waiting ||
           phase == CaptchaBridgePhase::token_ready;
}
// Called with the exclusive lock held.
void erase_secrets(Runtime* runtime) noexcept {
    SecureZeroMemory(at(runtime, kTokenOffset), kStorageBytes - kTokenOffset);
    runtime->token_bytes = runtime->sitekey_bytes = runtime->rqdata_bytes = 0;
}
void notify(Runtime* runtime) noexcept { SetEvent(runtime->notification); }
CaptchaBridgeStatus snapshot(Runtime* runtime) noexcept {
    AcquireSRWLockShared(&runtime->lock); const auto status = runtime->status; ReleaseSRWLockShared(&runtime->lock);
    return status;
}
void terminal(Runtime* runtime, CaptchaBridgePhase phase, CaptchaBridgeError error, u32 native = 0) noexcept {
    AcquireSRWLockExclusive(&runtime->lock);
    if (live(runtime->status.phase)) {
        runtime->status.phase = phase; runtime->status.error = error; runtime->status.native_error = native;
        erase_secrets(runtime);
    }
    ReleaseSRWLockExclusive(&runtime->lock);
    notify(runtime);
}
void close_client(Runtime* runtime) noexcept {
    if (runtime->client != INVALID_SOCKET) { closesocket(runtime->client); runtime->client = INVALID_SOCKET; }
    runtime->wire_bytes = runtime->response_bytes = runtime->response_sent = 0;
    runtime->client_deadline = 0;
    // The worker exclusively owns the wire and response partitions.
    SecureZeroMemory(at(runtime, 0), kTokenOffset);
}
bool external_output(const CaptchaBridge* bridge, Runtime* runtime, const SecureBuffer* output) noexcept {
    if (!output || !output->data || !output->locked || !output->capacity || output->bytes > output->capacity)
        return false;
    const ConstSlice<u8> data{output->data, output->capacity};
    const ConstSlice<u8> owner{reinterpret_cast<const u8*>(output), sizeof(*output)};
    const ConstSlice<u8> storage{runtime->storage.data, runtime->storage.capacity};
    const ConstSlice<u8> metadata{reinterpret_cast<const u8*>(runtime), owner_bytes()};
    const ConstSlice<u8> handle{reinterpret_cast<const u8*>(bridge), sizeof(*bridge)};
    return byte_range_valid(data) && !byte_ranges_overlap(data, storage) && !byte_ranges_overlap(data, metadata) &&
           !byte_ranges_overlap(data, handle) && !byte_ranges_overlap(data, owner) &&
           !byte_ranges_overlap(owner, storage) && !byte_ranges_overlap(owner, metadata) &&
           !byte_ranges_overlap(owner, handle);
}
void destroy(CaptchaBridge* bridge) noexcept {
    auto* runtime = runtime_of(bridge);
    if (!runtime) return;
    if (runtime->thread) CloseHandle(runtime->thread);
    if (runtime->notification) CloseHandle(runtime->notification);
    if (runtime->stop_event) CloseHandle(runtime->stop_event);
    secure_buffer_release(&runtime->storage);
    SecureZeroMemory(runtime, owner_bytes()); VirtualFree(runtime, 0, MEM_RELEASE); bridge->runtime = nullptr;
}
bool bind_listener(Runtime* runtime) noexcept {
    runtime->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (runtime->listener == INVALID_SOCKET) {
        terminal(runtime, CaptchaBridgePhase::failed, CaptchaBridgeError::network, static_cast<u32>(WSAGetLastError()));
        return false;
    }
    const BOOL exclusive = TRUE;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_bytes = sizeof(address);
    if (setsockopt(runtime->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR ||
        bind(runtime->listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        getsockname(runtime->listener, reinterpret_cast<sockaddr*>(&address), &address_bytes) == SOCKET_ERROR ||
        address_bytes != sizeof(address) || ntohs(address.sin_port) < 1024 ||
        listen(runtime->listener, 2) == SOCKET_ERROR ||
        WSAEventSelect(runtime->listener, runtime->listener_event, FD_ACCEPT | FD_CLOSE) == SOCKET_ERROR) {
        terminal(runtime, CaptchaBridgePhase::failed, CaptchaBridgeError::network, static_cast<u32>(WSAGetLastError()));
        return false;
    }
    AcquireSRWLockExclusive(&runtime->lock);
    if (runtime->status.phase == CaptchaBridgePhase::starting) {
        runtime->status.port = ntohs(address.sin_port);
        runtime->status.phase = CaptchaBridgePhase::waiting;
    }
    ReleaseSRWLockExclusive(&runtime->lock);
    notify(runtime);
    return true;
}
void accept_client(Runtime* runtime, u64 now) noexcept {
    sockaddr_in peer{}; int peer_bytes = sizeof(peer);
    const SOCKET accepted = accept(runtime->listener, reinterpret_cast<sockaddr*>(&peer), &peer_bytes);
    if (accepted == INVALID_SOCKET) {
        const u32 error = static_cast<u32>(WSAGetLastError());
        if (error != WSAEWOULDBLOCK) terminal(runtime, CaptchaBridgePhase::failed, CaptchaBridgeError::network, error);
        return;
    }
    const auto current = snapshot(runtime);
    // Browsers may open an idle speculative socket first. One connection is
    // served at a time; an idle one yields to the next, an active one never.
    const bool idle = runtime->client != INVALID_SOCKET && !runtime->wire_bytes && !runtime->response_bytes;
    if ((runtime->client != INVALID_SOCKET && !idle) || !live(current.phase) || peer_bytes != sizeof(peer) ||
        peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        closesocket(accepted); return;
    }
    if (idle) close_client(runtime);
    if (WSAEventSelect(accepted, runtime->client_event, FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR) {
        closesocket(accepted); return;
    }
    runtime->client = accepted;
    runtime->client_deadline = earliest_deadline(current.deadline_ms, deadline_after(now, kClientTimeoutMs));
}
void flush_response(Runtime* runtime) noexcept {
    if (runtime->client == INVALID_SOCKET || !runtime->response_bytes) return;
    while (runtime->response_sent < runtime->response_bytes) {
        const int sent = send(runtime->client, reinterpret_cast<const char*>(at(runtime, kResponseOffset) + runtime->response_sent),
            static_cast<int>(runtime->response_bytes - runtime->response_sent), 0);
        if (sent > 0) runtime->response_sent += static_cast<u32>(sent);
        else {
            if (sent == 0 || WSAGetLastError() != WSAEWOULDBLOCK) close_client(runtime);
            return;
        }
    }
    shutdown(runtime->client, SD_SEND);
    close_client(runtime);
}
CaptchaBridgeProvider protocol_provider(CaptchaProvider provider) noexcept {
    switch (provider) {
    case CaptchaProvider::hcaptcha: return CaptchaBridgeProvider::hcaptcha;
    case CaptchaProvider::recaptcha: return CaptchaBridgeProvider::recaptcha;
    case CaptchaProvider::recaptcha_enterprise: return CaptchaBridgeProvider::recaptcha_enterprise;
    case CaptchaProvider::turnstile: return CaptchaBridgeProvider::turnstile;
    default: return CaptchaBridgeProvider::unknown;
    }
}
// Builds the response for a complete request; false closes without one.
bool respond(Runtime* runtime, const CaptchaBridgeParseResult& parsed) noexcept {
    const Slice<u8> response{at(runtime, kResponseOffset), kResponseBytes};
    CaptchaBridgeWriteResult written{0, CaptchaBridgeProtocolError::invalid_input};
    bool candidate = false;
    AcquireSRWLockExclusive(&runtime->lock);
    const bool admitted = runtime->status.phase == CaptchaBridgePhase::waiting && !runtime->spent &&
                          !deadline_due(GetTickCount64(), runtime->status.deadline_ms);
    if (!parsed.ok()) {
        // A well-routed POST with the exact state spends it even when malformed.
        if (admitted && (parsed.error == CaptchaBridgeProtocolError::json ||
                         parsed.error == CaptchaBridgeProtocolError::token)) {
            runtime->spent = true;
            runtime->status.phase = CaptchaBridgePhase::failed;
            runtime->status.error = CaptchaBridgeError::rejected;
            erase_secrets(runtime);
            candidate = true;
            written = captcha_bridge_write_response(CaptchaBridgeResponse::gone, response);
        }
    } else if (parsed.request.kind == CaptchaBridgeRequestKind::page) {
        if (admitted && !runtime->status.page_served) {
            u8 random[16];
            const bool nonce = secure_random_fill({random, sizeof(random)}).ok() &&
                captcha_bridge_write_hex({random, sizeof(random)}, {at(runtime, kNonceOffset), kCaptchaBridgeNonceBytes}).ok();
            SecureZeroMemory(random, sizeof(random));
            if (nonce) {
                written = captcha_bridge_write_page({view(runtime, kSitekeyOffset, runtime->sitekey_bytes),
                    view(runtime, kRqdataOffset, runtime->rqdata_bytes), view(runtime, kStateOffset, kCaptchaBridgeStateBytes),
                    view(runtime, kNonceOffset, kCaptchaBridgeNonceBytes), runtime->status.port, runtime->provider,
                    runtime->invisible}, response);
                runtime->status.page_served = written.ok();
            }
            SecureZeroMemory(at(runtime, kNonceOffset), kCaptchaBridgeNonceBytes);
        } else {
            written = captcha_bridge_write_response(CaptchaBridgeResponse::gone, response);
        }
    } else if (admitted) {
        runtime->spent = true;
        copy_bytes(at(runtime, kTokenOffset), parsed.request.token.data, parsed.request.token.bytes);
        runtime->token_bytes = parsed.request.token.bytes;
        runtime->status.phase = CaptchaBridgePhase::token_ready;
        candidate = true;
        written = captcha_bridge_write_response(CaptchaBridgeResponse::accepted, response);
    } else {
        written = captcha_bridge_write_response(CaptchaBridgeResponse::gone, response);
    }
    ReleaseSRWLockExclusive(&runtime->lock);
    if (candidate) notify(runtime);
    if (!written.ok()) return false;
    runtime->response_bytes = written.bytes;
    return true;
}
void receive_request(Runtime* runtime) noexcept {
    if (runtime->client == INVALID_SOCKET || runtime->response_bytes) return;
    const int received = recv(runtime->client, reinterpret_cast<char*>(at(runtime, runtime->wire_bytes)),
                              static_cast<int>(kWireBytes - runtime->wire_bytes), 0);
    if (received <= 0) {
        if (received == 0 || WSAGetLastError() != WSAEWOULDBLOCK) close_client(runtime);
        return;
    }
    runtime->wire_bytes += static_cast<u32>(received);
    // The state slot is stable while live; stop erases it only under the lock.
    AcquireSRWLockShared(&runtime->lock);
    u8 state[kCaptchaBridgeStateBytes];
    copy_bytes(state, at(runtime, kStateOffset), kCaptchaBridgeStateBytes);
    const u16 port = runtime->status.port;
    ReleaseSRWLockShared(&runtime->lock);
    const auto parsed = captcha_bridge_parse_request({at(runtime, 0), runtime->wire_bytes}, port,
                                                     {reinterpret_cast<const char*>(state), kCaptchaBridgeStateBytes});
    SecureZeroMemory(state, sizeof(state));
    if (parsed.state == CaptchaBridgeParseState::need_input) {
        if (runtime->wire_bytes == kWireBytes) close_client(runtime);
        return;
    }
    const bool answered = respond(runtime, parsed);
    SecureZeroMemory(at(runtime, 0), kWireBytes);
    if (!answered) { close_client(runtime); return; }
    flush_response(runtime);
}
DWORD WINAPI bridge_main(void* opaque) noexcept {
    auto* runtime = static_cast<Runtime*>(opaque);
    WSADATA winsock{};
    const int started = WSAStartup(MAKEWORD(2, 2), &winsock);
    if (started) {
        terminal(runtime, CaptchaBridgePhase::failed, CaptchaBridgeError::network, static_cast<u32>(started));
        return 1;
    }
    runtime->listener_event = WSACreateEvent(); runtime->client_event = WSACreateEvent();
    if (runtime->listener_event == WSA_INVALID_EVENT || runtime->client_event == WSA_INVALID_EVENT)
        terminal(runtime, CaptchaBridgePhase::failed, CaptchaBridgeError::event, static_cast<u32>(WSAGetLastError()));
    else if (live(snapshot(runtime).phase) && bind_listener(runtime)) {
        for (;;) {
            const auto current = snapshot(runtime); const u64 now = GetTickCount64();
            if (!live(current.phase) && current.phase != CaptchaBridgePhase::complete &&
                current.phase != CaptchaBridgePhase::failed) break;
            const bool admitting = current.phase == CaptchaBridgePhase::starting || current.phase == CaptchaBridgePhase::waiting;
            if (admitting && deadline_due(now, current.deadline_ms)) {
                terminal(runtime, CaptchaBridgePhase::expired, CaptchaBridgeError::none);
                break;
            }
            if (deadline_due(now, runtime->client_deadline)) close_client(runtime);
            HANDLE waits[] = {runtime->stop_event, runtime->client_event, runtime->listener_event};
            const u64 deadline = earliest_deadline(admitting ? current.deadline_ms : 0, runtime->client_deadline);
            const DWORD waited = WaitForMultipleObjects(3, waits, FALSE,
                deadline ? wait_milliseconds_until(now, deadline) : INFINITE);
            if (waited == WAIT_OBJECT_0) break;
            if (waited == WAIT_TIMEOUT) continue;
            if (waited == WAIT_OBJECT_0 + 1) {
                WSANETWORKEVENTS events{};
                if (runtime->client == INVALID_SOCKET) { WSAResetEvent(runtime->client_event); continue; }
                if (WSAEnumNetworkEvents(runtime->client, runtime->client_event, &events) == SOCKET_ERROR) { close_client(runtime); continue; }
                if (events.lNetworkEvents & FD_READ) receive_request(runtime);
                if (events.lNetworkEvents & FD_WRITE) flush_response(runtime);
                if (events.lNetworkEvents & FD_CLOSE) close_client(runtime);
            } else if (waited == WAIT_OBJECT_0 + 2) {
                WSANETWORKEVENTS events{};
                if (WSAEnumNetworkEvents(runtime->listener, runtime->listener_event, &events) == SOCKET_ERROR ||
                    (events.lNetworkEvents & FD_CLOSE)) {
                    terminal(runtime, CaptchaBridgePhase::failed, CaptchaBridgeError::network, static_cast<u32>(WSAGetLastError()));
                    break;
                }
                if (events.lNetworkEvents & FD_ACCEPT) accept_client(runtime, GetTickCount64());
            } else {
                terminal(runtime, CaptchaBridgePhase::failed, CaptchaBridgeError::event, thread_last_error());
                break;
            }
        }
    }
    close_client(runtime);
    if (runtime->listener != INVALID_SOCKET) { closesocket(runtime->listener); runtime->listener = INVALID_SOCKET; }
    if (runtime->listener_event != WSA_INVALID_EVENT) { WSACloseEvent(runtime->listener_event); runtime->listener_event = WSA_INVALID_EVENT; }
    if (runtime->client_event != WSA_INVALID_EVENT) { WSACloseEvent(runtime->client_event); runtime->client_event = WSA_INVALID_EVENT; }
    WSACleanup();
    return 0;
}
CaptchaBridgeStatus invalid(CaptchaBridgeError error = CaptchaBridgeError::invalid_input, u32 native = ERROR_INVALID_PARAMETER) noexcept {
    CaptchaBridgeStatus result{}; result.error = error; result.native_error = native; return result;
}
SecureBufferResult output_error(SecureBufferError error = SecureBufferError::invalid_input, u32 bytes = 0) noexcept {
    return {bytes, ERROR_INVALID_PARAMETER, error};
}
} // namespace

CaptchaBridgeStatus captcha_bridge_start(CaptchaBridge* bridge, u64 generation, u32 timeout_ms,
                                         const CaptchaBridgeChallenge& challenge) noexcept {
    const CaptchaBridgeProvider provider = protocol_provider(challenge.provider);
    if (!bridge || bridge->runtime || !generation || timeout_ms < 1000 || timeout_ms > kCaptchaBridgeDefaultTimeoutMs ||
        provider == CaptchaBridgeProvider::unknown || !captcha_bridge_sitekey_valid(challenge.sitekey) ||
        !captcha_bridge_rqdata_valid(challenge.rqdata))
        return invalid();
    auto* runtime = static_cast<Runtime*>(VirtualAlloc(nullptr, owner_bytes(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime) return invalid(CaptchaBridgeError::memory, thread_last_error());
    bridge->runtime = runtime;
    runtime->listener = runtime->client = INVALID_SOCKET;
    runtime->listener_event = runtime->client_event = WSA_INVALID_EVENT;
    runtime->status.generation = generation;
    runtime->status.deadline_ms = deadline_after(GetTickCount64(), timeout_ms);
    runtime->status.phase = CaptchaBridgePhase::starting;
    runtime->provider = provider;
    runtime->invisible = challenge.invisible;
    const auto secured = secure_buffer_initialize(&runtime->storage, kStorageBytes);
    if (!secured.ok()) { const u32 native = secured.native_error; destroy(bridge); return invalid(CaptchaBridgeError::memory, native); }
    copy_bytes(at(runtime, kSitekeyOffset), challenge.sitekey.data, challenge.sitekey.bytes);
    runtime->sitekey_bytes = challenge.sitekey.bytes;
    if (challenge.rqdata.bytes) copy_bytes(at(runtime, kRqdataOffset), challenge.rqdata.data, challenge.rqdata.bytes);
    runtime->rqdata_bytes = challenge.rqdata.bytes;
    u8 random[16];
    const bool state = secure_random_fill({random, sizeof(random)}).ok() &&
        captcha_bridge_write_hex({random, sizeof(random)}, {at(runtime, kStateOffset), kCaptchaBridgeStateBytes}).ok();
    SecureZeroMemory(random, sizeof(random));
    if (!state) { destroy(bridge); return invalid(CaptchaBridgeError::random); }
    runtime->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    runtime->notification = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!runtime->stop_event || !runtime->notification) {
        const u32 native = thread_last_error(); destroy(bridge); return invalid(CaptchaBridgeError::event, native);
    }
    runtime->thread = CreateThread(nullptr, 256 * 1024, bridge_main, runtime, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!runtime->thread) { const u32 native = thread_last_error(); destroy(bridge); return invalid(CaptchaBridgeError::thread, native); }
    return snapshot(runtime);
}
CaptchaBridgeStatus captcha_bridge_status(const CaptchaBridge* bridge) noexcept {
    auto* runtime = runtime_of(bridge); return runtime ? snapshot(runtime) : CaptchaBridgeStatus{};
}
HANDLE captcha_bridge_notification_handle(const CaptchaBridge* bridge) noexcept {
    auto* runtime = runtime_of(bridge); return runtime ? runtime->notification : nullptr;
}
SecureBufferResult captcha_bridge_copy_url(const CaptchaBridge* bridge, u64 expected_generation, SecureBuffer* output) noexcept {
    auto* runtime = runtime_of(bridge);
    if (!runtime || !external_output(bridge, runtime, output)) return output_error();
    AcquireSRWLockShared(&runtime->lock);
    CaptchaBridgeWriteResult written{0, CaptchaBridgeProtocolError::invalid_input};
    if (expected_generation && runtime->status.generation == expected_generation &&
        runtime->status.phase == CaptchaBridgePhase::waiting && !runtime->spent &&
        !deadline_due(GetTickCount64(), runtime->status.deadline_ms))
        written = captcha_bridge_write_url(runtime->status.port, view(runtime, kStateOffset, kCaptchaBridgeStateBytes),
                                           {output->data, output->capacity});
    if (written.ok()) output->bytes = written.bytes;
    ReleaseSRWLockShared(&runtime->lock);
    return written.ok() ? SecureBufferResult{written.bytes, 0, SecureBufferError::none}
        : output_error(written.error == CaptchaBridgeProtocolError::capacity ? SecureBufferError::output_too_small
                                                                             : SecureBufferError::invalid_input, written.bytes);
}
SecureBufferResult captcha_bridge_take_token(CaptchaBridge* bridge, u64 expected_generation, SecureBuffer* output) noexcept {
    auto* runtime = runtime_of(bridge);
    if (!runtime || !external_output(bridge, runtime, output)) return output_error();
    AcquireSRWLockExclusive(&runtime->lock);
    SecureBufferResult result = output_error();
    if (expected_generation && runtime->status.generation == expected_generation &&
        runtime->status.phase == CaptchaBridgePhase::token_ready && runtime->token_bytes) {
        result = secure_buffer_assign(output, {at(runtime, kTokenOffset), runtime->token_bytes});
        if (result.ok()) {
            runtime->status.phase = CaptchaBridgePhase::complete;
            erase_secrets(runtime);
        }
    }
    ReleaseSRWLockExclusive(&runtime->lock);
    return result;
}
CaptchaBridgeStatus captcha_bridge_stop(CaptchaBridge* bridge, u32 timeout_ms) noexcept {
    if (!bridge || timeout_ms > 5000) return invalid();
    auto* runtime = runtime_of(bridge);
    if (!runtime) return {};
    AcquireSRWLockExclusive(&runtime->lock);
    if (live(runtime->status.phase) || runtime->status.phase == CaptchaBridgePhase::stopping) {
        runtime->status.phase = CaptchaBridgePhase::cancelled; runtime->status.error = CaptchaBridgeError::none;
    }
    runtime->spent = true;
    erase_secrets(runtime);
    ReleaseSRWLockExclusive(&runtime->lock);
    SetEvent(runtime->stop_event);
    const DWORD joined = WaitForSingleObject(runtime->thread, timeout_ms);
    if (joined != WAIT_OBJECT_0) {
        AcquireSRWLockExclusive(&runtime->lock);
        runtime->status.phase = CaptchaBridgePhase::stopping; runtime->status.error = CaptchaBridgeError::stop;
        runtime->status.native_error = joined == WAIT_TIMEOUT ? WAIT_TIMEOUT : thread_last_error();
        const auto status = runtime->status;
        ReleaseSRWLockExclusive(&runtime->lock);
        return status;
    }
    const auto status = snapshot(runtime);
    destroy(bridge);
    return status;
}
u32 captcha_bridge_runtime_bytes() noexcept { return owner_bytes() + kStorageBytes; }
} // namespace maicord
