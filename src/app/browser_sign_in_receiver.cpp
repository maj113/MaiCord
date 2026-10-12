#include <WinSock2.h>
#include "maicord/app/browser_sign_in_receiver.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/platform/random.hpp"

namespace maicord {
namespace {
constexpr u32 kWireBytes = kBrowserSignInMaximumHeaderBytes + kBrowserSignInMaximumBodyBytes;
constexpr u32 kQueryOffset = kWireBytes;
constexpr u32 kTokenOffset = kQueryOffset + kBrowserSignInMaximumBodyBytes;
constexpr u32 kKeyOffset = kTokenOffset + kBrowserSignInMaximumTokenBytes;
constexpr u32 kIndexOffset = kKeyOffset + 256;
constexpr u32 kResponseOffset = 38 * 1024;
constexpr u32 kStorageBytes = 40 * 1024;
constexpr u32 kResponseBytes = kStorageBytes - kResponseOffset;
constexpr u32 kClientTimeoutMs = 5000;
static_assert(kIndexOffset + kBrowserSignInStructuralCount * sizeof(u32) <= kResponseOffset);

struct Runtime {
    SRWLOCK lock;
    SecureBuffer storage;
    BrowserSignInReceiverStatus status;
    HANDLE stop_event, command_event, notification, thread;
    WSAEVENT listener_event, client_event;
    SOCKET listener, client;
    u64 client_deadline;
    u32 wire_bytes, response_bytes, response_sent, token_bytes;
    bool winsock_started;
};
Runtime* runtime_of(const BrowserSignInReceiver* receiver) noexcept {
    return receiver ? static_cast<Runtime*>(receiver->runtime) : nullptr;
}
u32 owner_bytes() noexcept { return static_cast<u32>((sizeof(Runtime) + 4095) & ~usize{4095}); }
void notify(Runtime* runtime) noexcept { SetEvent(runtime->notification); }
bool live(BrowserSignInReceiverPhase phase) noexcept {
    return phase == BrowserSignInReceiverPhase::starting || phase == BrowserSignInReceiverPhase::waiting ||
        phase == BrowserSignInReceiverPhase::candidate_ready || phase == BrowserSignInReceiverPhase::exchange_pending;
}
void clear_candidate(Runtime* runtime) noexcept {
    SecureZeroMemory(runtime->storage.data + kTokenOffset, kBrowserSignInMaximumTokenBytes);
    runtime->token_bytes = 0;
}
void clear_key(Runtime* runtime) noexcept { SecureZeroMemory(runtime->storage.data + kKeyOffset, kBrowserSignInKeyBytes); }
void close_client(Runtime* runtime) noexcept {
    if (runtime->client != INVALID_SOCKET) { closesocket(runtime->client); runtime->client = INVALID_SOCKET; }
    runtime->wire_bytes = runtime->response_bytes = runtime->response_sent = 0;
    runtime->client_deadline = 0;
    // The worker exclusively owns these partitions. UI cancellation erases
    // key/token under the lock, without racing an outstanding socket read.
    SecureZeroMemory(runtime->storage.data, kTokenOffset);
    SecureZeroMemory(runtime->storage.data + kIndexOffset, kBrowserSignInStructuralCount * sizeof(u32));
    SecureZeroMemory(runtime->storage.data + kResponseOffset, kResponseBytes);
}
void terminal(Runtime* runtime, BrowserSignInReceiverPhase phase, BrowserSignInReceiverError error, u32 native = 0) noexcept {
    AcquireSRWLockExclusive(&runtime->lock);
    if (live(runtime->status.phase)) {
        runtime->status.phase = phase; runtime->status.error = error; runtime->status.native_error = native;
        clear_candidate(runtime); clear_key(runtime);
    }
    ReleaseSRWLockExclusive(&runtime->lock);
    notify(runtime);
}
BrowserSignInReceiverStatus snapshot(Runtime* runtime) noexcept {
    AcquireSRWLockShared(&runtime->lock); const auto status = runtime->status; ReleaseSRWLockShared(&runtime->lock); return status;
}
bool external_output(const BrowserSignInReceiver* receiver, Runtime* runtime, const SecureBuffer* output) noexcept {
    return output && output->data && output->locked && output->capacity && output->bytes <= output->capacity &&
        byte_range_valid({output->data, output->capacity}) &&
        !byte_ranges_overlap({output->data, output->capacity}, {runtime->storage.data, runtime->storage.capacity}) &&
        !byte_ranges_overlap({output->data, output->capacity}, {reinterpret_cast<const u8*>(runtime), owner_bytes()}) &&
        !byte_ranges_overlap({output->data, output->capacity}, {reinterpret_cast<const u8*>(receiver), sizeof(*receiver)}) &&
        !byte_ranges_overlap({output->data, output->capacity}, {reinterpret_cast<const u8*>(output), sizeof(*output)}) &&
        !byte_ranges_overlap({reinterpret_cast<const u8*>(output), sizeof(*output)}, {runtime->storage.data, runtime->storage.capacity}) &&
        !byte_ranges_overlap({reinterpret_cast<const u8*>(output), sizeof(*output)}, {reinterpret_cast<const u8*>(runtime), owner_bytes()}) &&
        !byte_ranges_overlap({reinterpret_cast<const u8*>(output), sizeof(*output)}, {reinterpret_cast<const u8*>(receiver), sizeof(*receiver)});
}
void destroy(BrowserSignInReceiver* receiver) noexcept {
    auto* runtime = runtime_of(receiver);
    if (!runtime) return;
    if (runtime->thread) CloseHandle(runtime->thread);
    if (runtime->notification) CloseHandle(runtime->notification);
    if (runtime->command_event) CloseHandle(runtime->command_event);
    if (runtime->stop_event) CloseHandle(runtime->stop_event);
    secure_buffer_release(&runtime->storage);
    SecureZeroMemory(runtime, owner_bytes()); VirtualFree(runtime, 0, MEM_RELEASE); receiver->runtime = nullptr;
}
bool bind_listener(Runtime* runtime) noexcept {
    runtime->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (runtime->listener == INVALID_SOCKET) { terminal(runtime, BrowserSignInReceiverPhase::failed, BrowserSignInReceiverError::network, static_cast<u32>(WSAGetLastError())); return false; }
    const BOOL exclusive = TRUE;
    if (setsockopt(runtime->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
        terminal(runtime, BrowserSignInReceiverPhase::failed, BrowserSignInReceiverError::network, static_cast<u32>(WSAGetLastError())); return false;
    }
    for (u16 offset = 0; offset < kBrowserSignInPortCount; ++offset) {
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<u16>(kBrowserSignInFirstPort + offset));
        if (bind(runtime->listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            if (listen(runtime->listener, 1) == SOCKET_ERROR ||
                WSAEventSelect(runtime->listener, runtime->listener_event, FD_ACCEPT | FD_CLOSE) == SOCKET_ERROR) {
                terminal(runtime, BrowserSignInReceiverPhase::failed, BrowserSignInReceiverError::network, static_cast<u32>(WSAGetLastError())); return false;
            }
            AcquireSRWLockExclusive(&runtime->lock);
            if (runtime->status.phase == BrowserSignInReceiverPhase::starting) {
                runtime->status.port = static_cast<u16>(kBrowserSignInFirstPort + offset);
                runtime->status.phase = BrowserSignInReceiverPhase::waiting;
            }
            ReleaseSRWLockExclusive(&runtime->lock); notify(runtime); return true;
        }
        const u32 error = static_cast<u32>(WSAGetLastError());
        if (error != WSAEADDRINUSE && error != WSAEACCES) { terminal(runtime, BrowserSignInReceiverPhase::failed, BrowserSignInReceiverError::network, error); return false; }
    }
    terminal(runtime, BrowserSignInReceiverPhase::failed, BrowserSignInReceiverError::ports_busy, WSAEADDRINUSE); return false;
}
void accept_client(Runtime* runtime, u64 now) noexcept {
    sockaddr_in peer{}; int peer_bytes = sizeof(peer);
    const SOCKET accepted = accept(runtime->listener, reinterpret_cast<sockaddr*>(&peer), &peer_bytes);
    if (accepted == INVALID_SOCKET) {
        const u32 error = static_cast<u32>(WSAGetLastError());
        if (error != WSAEWOULDBLOCK) terminal(runtime, BrowserSignInReceiverPhase::failed, BrowserSignInReceiverError::network, error);
        return;
    }
    const auto current = snapshot(runtime);
    if (runtime->client != INVALID_SOCKET || current.phase != BrowserSignInReceiverPhase::waiting ||
        peer_bytes != sizeof(peer) || peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) { closesocket(accepted); return; }
    if (WSAEventSelect(accepted, runtime->client_event, FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR) { closesocket(accepted); return; }
    runtime->client = accepted; runtime->client_deadline = earliest_deadline(current.deadline_ms, deadline_after(now, kClientTimeoutMs));
}
void flush_response(Runtime* runtime) noexcept {
    if (runtime->client == INVALID_SOCKET || !runtime->response_bytes) return;
    // Drain this bounded response to completion or actual backpressure.
    // FD_WRITE is rearmed by WSAEWOULDBLOCK, not by a successful short send.
    while (runtime->response_sent < runtime->response_bytes) {
        const int sent = send(runtime->client, reinterpret_cast<const char*>(runtime->storage.data + kResponseOffset + runtime->response_sent),
            static_cast<int>(runtime->response_bytes - runtime->response_sent), 0);
        if (sent > 0) runtime->response_sent += static_cast<u32>(sent);
        else {
            if (sent == 0 || WSAGetLastError() != WSAEWOULDBLOCK) close_client(runtime);
            return;
        }
    }
    close_client(runtime);
}
void receive_request(Runtime* runtime) noexcept {
    if (runtime->client == INVALID_SOCKET || runtime->response_bytes) return;
    const int received = recv(runtime->client, reinterpret_cast<char*>(runtime->storage.data + runtime->wire_bytes),
        static_cast<int>(kWireBytes - runtime->wire_bytes), 0);
    if (received <= 0) { if (received == 0 || WSAGetLastError() != WSAEWOULDBLOCK) close_client(runtime); return; }
    runtime->wire_bytes += static_cast<u32>(received);
    const auto current = snapshot(runtime);
    const auto parsed = browser_sign_in_parse_request({runtime->storage.data, runtime->wire_bytes}, current.port,
        {runtime->storage.data + kQueryOffset, kBrowserSignInMaximumBodyBytes},
        {reinterpret_cast<u32*>(runtime->storage.data + kIndexOffset), kBrowserSignInStructuralCount});
    if (parsed.state == BrowserSignInParseState::need_input) { if (runtime->wire_bytes == kWireBytes) close_client(runtime); return; }
    if (!parsed.ok()) { close_client(runtime); return; }
    const auto response = browser_sign_in_write_response(parsed.request, {runtime->storage.data + kResponseOffset, kResponseBytes});
    if (!response.ok()) { close_client(runtime); return; }
    bool candidate = false;
    if (parsed.request.has_token) {
        AcquireSRWLockExclusive(&runtime->lock);
        if (runtime->status.phase == BrowserSignInReceiverPhase::waiting && !deadline_due(GetTickCount64(), runtime->status.deadline_ms) && runtime->status.candidate_id != ~u64{0}) {
            const auto decoded = json_string_decode_utf8(parsed.request.token,
                {reinterpret_cast<char*>(runtime->storage.data + kTokenOffset), kBrowserSignInMaximumTokenBytes});
            if (decoded.ok() && decoded.bytes) {
                runtime->token_bytes = decoded.bytes; ++runtime->status.candidate_id;
                runtime->status.phase = BrowserSignInReceiverPhase::candidate_ready; candidate = true;
            } else clear_candidate(runtime);
        }
        ReleaseSRWLockExclusive(&runtime->lock);
        if (!candidate) { close_client(runtime); return; }
    }
    // No-token callbacks are acknowledged but cannot change native admission.
    runtime->response_bytes = response.bytes;
    SecureZeroMemory(runtime->storage.data, kTokenOffset);
    SecureZeroMemory(runtime->storage.data + kIndexOffset, kBrowserSignInStructuralCount * sizeof(u32));
    if (candidate) notify(runtime);
    flush_response(runtime);
}
DWORD WINAPI receiver_main(void* opaque) noexcept {
    auto* runtime = static_cast<Runtime*>(opaque);
    WSADATA winsock{};
    const int started = WSAStartup(MAKEWORD(2, 2), &winsock);
    if (started) { terminal(runtime, BrowserSignInReceiverPhase::failed, BrowserSignInReceiverError::network, static_cast<u32>(started)); return 1; }
    runtime->winsock_started = true;
    runtime->listener_event = WSACreateEvent(); runtime->client_event = WSACreateEvent();
    if (runtime->listener_event == WSA_INVALID_EVENT || runtime->client_event == WSA_INVALID_EVENT) terminal(runtime, BrowserSignInReceiverPhase::failed, BrowserSignInReceiverError::event, static_cast<u32>(WSAGetLastError()));
    else if (live(snapshot(runtime).phase) && bind_listener(runtime)) {
        for (;;) {
            const auto current = snapshot(runtime); const u64 now = GetTickCount64();
            if (!live(current.phase)) break;
            if (deadline_due(now, current.deadline_ms)) { terminal(runtime, BrowserSignInReceiverPhase::expired, BrowserSignInReceiverError::none); break; }
            if (deadline_due(now, runtime->client_deadline)) close_client(runtime);
            HANDLE waits[] = {runtime->stop_event, runtime->command_event, runtime->client_event, runtime->listener_event};
            const u64 deadline = earliest_deadline(current.deadline_ms, runtime->client_deadline);
            const DWORD waited = WaitForMultipleObjects(4, waits, FALSE, wait_milliseconds_until(now, deadline));
            if (waited == WAIT_OBJECT_0) break;
            if (waited == WAIT_OBJECT_0 + 1 || waited == WAIT_TIMEOUT) continue;
            if (waited == WAIT_OBJECT_0 + 2) {
                WSANETWORKEVENTS events{};
                if (runtime->client == INVALID_SOCKET) { WSAResetEvent(runtime->client_event); continue; }
                if (WSAEnumNetworkEvents(runtime->client, runtime->client_event, &events) == SOCKET_ERROR) { close_client(runtime); continue; }
                if (events.lNetworkEvents & FD_READ) receive_request(runtime);
                if (events.lNetworkEvents & FD_WRITE) flush_response(runtime);
                if (events.lNetworkEvents & FD_CLOSE) close_client(runtime);
            } else if (waited == WAIT_OBJECT_0 + 3) {
                WSANETWORKEVENTS events{};
                if (WSAEnumNetworkEvents(runtime->listener, runtime->listener_event, &events) == SOCKET_ERROR ||
                    (events.lNetworkEvents & FD_CLOSE)) { terminal(runtime, BrowserSignInReceiverPhase::failed, BrowserSignInReceiverError::network, static_cast<u32>(WSAGetLastError())); break; }
                if (events.lNetworkEvents & FD_ACCEPT) accept_client(runtime, GetTickCount64());
            } else { terminal(runtime, BrowserSignInReceiverPhase::failed, BrowserSignInReceiverError::event, thread_last_error()); break; }
        }
    }
    close_client(runtime);
    if (runtime->listener != INVALID_SOCKET) { closesocket(runtime->listener); runtime->listener = INVALID_SOCKET; }
    if (runtime->listener_event != WSA_INVALID_EVENT) { WSACloseEvent(runtime->listener_event); runtime->listener_event = WSA_INVALID_EVENT; }
    if (runtime->client_event != WSA_INVALID_EVENT) { WSACloseEvent(runtime->client_event); runtime->client_event = WSA_INVALID_EVENT; }
    WSACleanup(); runtime->winsock_started = false; return 0;
}
BrowserSignInReceiverStatus invalid(BrowserSignInReceiverError error = BrowserSignInReceiverError::invalid_input) noexcept {
    BrowserSignInReceiverStatus result{}; result.error = error; result.native_error = ERROR_INVALID_PARAMETER; return result;
}
SecureBufferResult output_error(SecureBufferError error = SecureBufferError::invalid_input, u32 bytes = 0) noexcept {
    return {bytes, ERROR_INVALID_PARAMETER, error};
}
bool admit(Runtime* runtime, u64 generation, BrowserSignInReceiverPhase phase, u64 candidate = 0) noexcept {
    return generation && runtime->status.generation == generation && runtime->status.phase == phase &&
        (!candidate || runtime->status.candidate_id == candidate) && !deadline_due(GetTickCount64(), runtime->status.deadline_ms);
}
} // namespace

BrowserSignInReceiverStatus browser_sign_in_receiver_start(BrowserSignInReceiver* receiver, u64 generation, u32 timeout_ms) noexcept {
    if (!receiver || receiver->runtime || !generation || timeout_ms < 1000 || timeout_ms > 5 * 60 * 1000) return invalid();
    auto* runtime = static_cast<Runtime*>(VirtualAlloc(nullptr, owner_bytes(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime) return invalid(BrowserSignInReceiverError::memory);
    receiver->runtime = runtime; runtime->listener = runtime->client = INVALID_SOCKET;
    runtime->listener_event = runtime->client_event = WSA_INVALID_EVENT;
    runtime->status.generation = generation; runtime->status.deadline_ms = deadline_after(GetTickCount64(), timeout_ms);
    runtime->status.phase = BrowserSignInReceiverPhase::starting;
    auto secured = secure_buffer_initialize(&runtime->storage, kStorageBytes);
    if (!secured.ok()) { const u32 native = secured.native_error; destroy(receiver); auto status = invalid(BrowserSignInReceiverError::memory); status.native_error = native; return status; }
    const auto random = secure_random_fill({runtime->storage.data, 16});
    const auto key = random.ok() ? browser_sign_in_write_key({runtime->storage.data, 16}, {runtime->storage.data + kKeyOffset, kBrowserSignInKeyBytes}) : BrowserSignInWriteResult{0, BrowserSignInError::invalid_input};
    SecureZeroMemory(runtime->storage.data, 16);
    if (!key.ok()) { destroy(receiver); return invalid(BrowserSignInReceiverError::random); }
    runtime->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    runtime->command_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    runtime->notification = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!runtime->stop_event || !runtime->command_event || !runtime->notification) { const u32 native = thread_last_error(); destroy(receiver); auto status = invalid(BrowserSignInReceiverError::event); status.native_error = native; return status; }
    runtime->thread = CreateThread(nullptr, 256 * 1024, receiver_main, runtime, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!runtime->thread) { const u32 native = thread_last_error(); destroy(receiver); auto status = invalid(BrowserSignInReceiverError::thread); status.native_error = native; return status; }
    return snapshot(runtime);
}
BrowserSignInReceiverStatus browser_sign_in_receiver_status(const BrowserSignInReceiver* receiver) noexcept {
    auto* runtime = runtime_of(receiver); return runtime ? snapshot(runtime) : BrowserSignInReceiverStatus{};
}
HANDLE browser_sign_in_receiver_notification_handle(const BrowserSignInReceiver* receiver) noexcept {
    auto* runtime = runtime_of(receiver); return runtime ? runtime->notification : nullptr;
}
SecureBufferResult browser_sign_in_receiver_copy_url(const BrowserSignInReceiver* receiver, u64 expected_generation, SecureBuffer* output) noexcept {
    auto* runtime = runtime_of(receiver);
    if (!runtime || !external_output(receiver, runtime, output)) return output_error();
    AcquireSRWLockShared(&runtime->lock);
    BrowserSignInWriteResult written{0, BrowserSignInError::invalid_input};
    if (admit(runtime, expected_generation, BrowserSignInReceiverPhase::waiting)) written = browser_sign_in_write_url(runtime->status.port,
        {reinterpret_cast<const char*>(runtime->storage.data + kKeyOffset), kBrowserSignInKeyBytes}, {output->data, output->capacity});
    if (written.ok()) output->bytes = written.bytes;
    ReleaseSRWLockShared(&runtime->lock);
    return written.ok() ? SecureBufferResult{written.bytes, 0, SecureBufferError::none} : output_error(written.error == BrowserSignInError::capacity ? SecureBufferError::output_too_small : SecureBufferError::invalid_input, written.bytes);
}
SecureBufferResult browser_sign_in_receiver_take_exchange(BrowserSignInReceiver* receiver, u64 expected_generation, u64 expected_candidate_id, SecureBuffer* output) noexcept {
    auto* runtime = runtime_of(receiver);
    if (!runtime || !expected_candidate_id || !external_output(receiver, runtime, output)) return output_error();
    AcquireSRWLockExclusive(&runtime->lock);
    BrowserSignInWriteResult written{0, BrowserSignInError::invalid_input};
    if (admit(runtime, expected_generation, BrowserSignInReceiverPhase::candidate_ready, expected_candidate_id)) written = browser_sign_in_write_exchange(
        {reinterpret_cast<const char*>(runtime->storage.data + kKeyOffset), kBrowserSignInKeyBytes},
        {reinterpret_cast<const char*>(runtime->storage.data + kTokenOffset), runtime->token_bytes}, {output->data, output->capacity});
    if (written.ok()) { output->bytes = written.bytes; runtime->status.phase = BrowserSignInReceiverPhase::exchange_pending; clear_candidate(runtime); }
    ReleaseSRWLockExclusive(&runtime->lock);
    return written.ok() ? SecureBufferResult{written.bytes, 0, SecureBufferError::none} : output_error(written.error == BrowserSignInError::capacity ? SecureBufferError::output_too_small : SecureBufferError::invalid_input, written.bytes);
}
BrowserSignInReceiverStatus browser_sign_in_receiver_finish_exchange(BrowserSignInReceiver* receiver, u64 expected_generation, u64 expected_candidate_id, bool accepted) noexcept {
    auto* runtime = runtime_of(receiver); if (!runtime || !expected_candidate_id) return invalid();
    AcquireSRWLockExclusive(&runtime->lock);
    const bool pending = admit(runtime, expected_generation, BrowserSignInReceiverPhase::exchange_pending, expected_candidate_id);
    const bool discard = !accepted && admit(runtime, expected_generation, BrowserSignInReceiverPhase::candidate_ready, expected_candidate_id);
    if (!pending && !discard) { ReleaseSRWLockExclusive(&runtime->lock); return invalid(BrowserSignInReceiverError::state); }
    clear_candidate(runtime); runtime->status.phase = accepted ? BrowserSignInReceiverPhase::complete : BrowserSignInReceiverPhase::waiting;
    if (accepted) clear_key(runtime);
    const auto status = runtime->status;
    ReleaseSRWLockExclusive(&runtime->lock); SetEvent(runtime->command_event); notify(runtime); return status;
}
BrowserSignInReceiverStatus browser_sign_in_receiver_stop(BrowserSignInReceiver* receiver, u32 timeout_ms) noexcept {
    if (!receiver || timeout_ms > 5000) return invalid(); auto* runtime = runtime_of(receiver); if (!runtime) return {};
    AcquireSRWLockExclusive(&runtime->lock);
    if (live(runtime->status.phase) || runtime->status.phase == BrowserSignInReceiverPhase::stopping) {
        runtime->status.phase = BrowserSignInReceiverPhase::cancelled; runtime->status.error = BrowserSignInReceiverError::none;
    }
    clear_candidate(runtime); clear_key(runtime);
    ReleaseSRWLockExclusive(&runtime->lock);
    SetEvent(runtime->stop_event);
    const DWORD joined = WaitForSingleObject(runtime->thread, timeout_ms);
    if (joined != WAIT_OBJECT_0) {
        AcquireSRWLockExclusive(&runtime->lock);
        runtime->status.phase = BrowserSignInReceiverPhase::stopping; runtime->status.error = BrowserSignInReceiverError::stop;
        runtime->status.native_error = joined == WAIT_TIMEOUT ? WAIT_TIMEOUT : thread_last_error();
        const auto status = runtime->status; ReleaseSRWLockExclusive(&runtime->lock); return status;
    }
    const auto status = snapshot(runtime); destroy(receiver); return status;
}
u32 browser_sign_in_receiver_runtime_bytes() noexcept { return owner_bytes() + kStorageBytes; }
} // namespace maicord
