#include "loopback_server.hpp"
#include "maicord/app/browser_sign_in_receiver.hpp"
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
static_assert(kIndexOffset + kBrowserSignInStructuralCount * sizeof(u32) <= kResponseOffset);

struct Runtime : LoopbackServer {
    BrowserSignInReceiverStatus status;
    u32 token_bytes;
};
Runtime* runtime_of(const BrowserSignInReceiver* receiver) noexcept {
    return receiver ? static_cast<Runtime*>(receiver->runtime) : nullptr;
}
u32 owner_bytes() noexcept { return static_cast<u32>((sizeof(Runtime) + 4095) & ~usize{4095}); }
bool live(BrowserSignInReceiverPhase phase) noexcept {
    return phase == BrowserSignInReceiverPhase::starting || phase == BrowserSignInReceiverPhase::waiting ||
        phase == BrowserSignInReceiverPhase::candidate_ready || phase == BrowserSignInReceiverPhase::exchange_pending;
}
void clear_candidate(Runtime* runtime) noexcept {
    SecureZeroMemory(runtime->storage.data + kTokenOffset, kBrowserSignInMaximumTokenBytes);
    runtime->token_bytes = 0;
}
void clear_key(Runtime* runtime) noexcept { SecureZeroMemory(runtime->storage.data + kKeyOffset, kBrowserSignInKeyBytes); }
// The worker exclusively owns these partitions. UI cancellation erases
// key/token under the lock, without racing an outstanding socket read.
void wipe(LoopbackServer* server) noexcept {
    auto* runtime = static_cast<Runtime*>(server);
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
    loopback_notify(runtime);
}
BrowserSignInReceiverStatus snapshot(Runtime* runtime) noexcept {
    AcquireSRWLockShared(&runtime->lock); const auto status = runtime->status; ReleaseSRWLockShared(&runtime->lock); return status;
}
void destroy(BrowserSignInReceiver* receiver) noexcept {
    auto* runtime = runtime_of(receiver);
    if (!runtime) return;
    loopback_destroy(runtime, owner_bytes()); receiver->runtime = nullptr;
}
bool step(LoopbackServer* server, LoopbackState* state) noexcept {
    auto* runtime = static_cast<Runtime*>(server);
    const auto current = snapshot(runtime);
    const u64 now = GetTickCount64();
    if (!live(current.phase)) return false;
    if (deadline_due(now, current.deadline_ms)) { terminal(runtime, BrowserSignInReceiverPhase::expired, BrowserSignInReceiverError::none); return false; }
    *state = {now, current.deadline_ms, true, current.phase == BrowserSignInReceiverPhase::waiting};
    return true;
}
void fail(LoopbackServer* server, LoopbackFailure failure, u32 native) noexcept {
    terminal(static_cast<Runtime*>(server), BrowserSignInReceiverPhase::failed,
        failure == LoopbackFailure::event ? BrowserSignInReceiverError::event :
        failure == LoopbackFailure::ports_busy ? BrowserSignInReceiverError::ports_busy : BrowserSignInReceiverError::network, native);
}
void bound(LoopbackServer* server, u16 port) noexcept {
    auto* runtime = static_cast<Runtime*>(server);
    AcquireSRWLockExclusive(&runtime->lock);
    if (runtime->status.phase == BrowserSignInReceiverPhase::starting) {
        runtime->status.port = port;
        runtime->status.phase = BrowserSignInReceiverPhase::waiting;
    }
    ReleaseSRWLockExclusive(&runtime->lock); loopback_notify(runtime);
}
LoopbackRequest request(LoopbackServer* server) noexcept {
    auto* runtime = static_cast<Runtime*>(server);
    const auto current = snapshot(runtime);
    const auto parsed = browser_sign_in_parse_request({runtime->storage.data, runtime->wire_bytes}, current.port,
        {runtime->storage.data + kQueryOffset, kBrowserSignInMaximumBodyBytes},
        {reinterpret_cast<u32*>(runtime->storage.data + kIndexOffset), kBrowserSignInStructuralCount});
    if (parsed.state == BrowserSignInParseState::need_input) return LoopbackRequest::more;
    if (!parsed.ok()) return LoopbackRequest::close;
    const auto response = browser_sign_in_write_response(parsed.request, {runtime->storage.data + kResponseOffset, kResponseBytes});
    if (!response.ok()) return LoopbackRequest::close;
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
        if (!candidate) return LoopbackRequest::close;
    }
    // No-token callbacks are acknowledged but cannot change native admission.
    runtime->response_bytes = response.bytes;
    SecureZeroMemory(runtime->storage.data, kTokenOffset);
    SecureZeroMemory(runtime->storage.data + kIndexOffset, kBrowserSignInStructuralCount * sizeof(u32));
    if (candidate) loopback_notify(runtime);
    return LoopbackRequest::respond;
}
constexpr LoopbackProfile kProfile{step, fail, bound, request, wipe, kBrowserSignInFirstPort, kBrowserSignInPortCount,
    kWireBytes, kResponseOffset, 1, false};
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
    receiver->runtime = runtime;
    runtime->status.generation = generation; runtime->status.deadline_ms = deadline_after(GetTickCount64(), timeout_ms);
    runtime->status.phase = BrowserSignInReceiverPhase::starting;
    auto secured = secure_buffer_initialize(&runtime->storage, kStorageBytes);
    if (!secured.ok()) { const u32 native = secured.native_error; destroy(receiver); auto status = invalid(BrowserSignInReceiverError::memory); status.native_error = native; return status; }
    const auto random = secure_random_fill({runtime->storage.data, 16});
    const auto key = random.ok() ? browser_sign_in_write_key({runtime->storage.data, 16}, {runtime->storage.data + kKeyOffset, kBrowserSignInKeyBytes}) : BrowserSignInWriteResult{0, BrowserSignInError::invalid_input};
    SecureZeroMemory(runtime->storage.data, 16);
    if (!key.ok()) { destroy(receiver); return invalid(BrowserSignInReceiverError::random); }
    u32 native = 0;
    const LoopbackStart started = loopback_start(runtime, &kProfile, true, &native);
    if (started != LoopbackStart::none) {
        destroy(receiver); auto status = invalid(started == LoopbackStart::event ? BrowserSignInReceiverError::event : BrowserSignInReceiverError::thread);
        status.native_error = native; return status;
    }
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
    if (!runtime || !loopback_external_output(runtime, receiver, sizeof(*receiver), owner_bytes(), output)) return output_error();
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
    if (!runtime || !expected_candidate_id || !loopback_external_output(runtime, receiver, sizeof(*receiver), owner_bytes(), output)) return output_error();
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
    ReleaseSRWLockExclusive(&runtime->lock); SetEvent(runtime->wake_event); loopback_notify(runtime); return status;
}
BrowserSignInReceiverStatus browser_sign_in_receiver_stop(BrowserSignInReceiver* receiver, u32 timeout_ms) noexcept {
    if (!receiver || timeout_ms > 5000) return invalid(); auto* runtime = runtime_of(receiver); if (!runtime) return {};
    AcquireSRWLockExclusive(&runtime->lock);
    if (live(runtime->status.phase) || runtime->status.phase == BrowserSignInReceiverPhase::stopping) {
        runtime->status.phase = BrowserSignInReceiverPhase::cancelled; runtime->status.error = BrowserSignInReceiverError::none;
    }
    clear_candidate(runtime); clear_key(runtime);
    ReleaseSRWLockExclusive(&runtime->lock);
    const DWORD joined = loopback_join(runtime, timeout_ms);
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
