#include "loopback_server.hpp"
#include "maicord/app/captcha_bridge.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/random.hpp"

namespace maicord {
namespace {
constexpr u32 kWireBytes = kCaptchaBridgeMaximumHeaderBytes + kCaptchaBridgeMaximumBodyBytes;
constexpr u32 kResponseOffset = kWireBytes;
constexpr u32 kResponseBytes = kCaptchaBridgeMaximumResponseBytes;
constexpr u32 kTokenOffset = kResponseOffset + kResponseBytes;
constexpr u32 kSitekeyOffset = kTokenOffset + kCaptchaBridgeMaximumTokenBytes;
constexpr u32 kRqdataOffset = kSitekeyOffset + kCaptchaBridgeMaximumSitekeyBytes;
constexpr u32 kUserFlowOffset = kRqdataOffset + kCaptchaBridgeMaximumRqdataBytes;
constexpr u32 kStateOffset = kUserFlowOffset + kCaptchaBridgeMaximumUserFlowBytes;
constexpr u32 kNonceOffset = kStateOffset + kCaptchaBridgeStateBytes;
constexpr u32 kStorageBytes = kNonceOffset + kCaptchaBridgeNonceBytes;
static_assert(kCaptchaBridgeMaximumUserFlowBytes == kCaptchaChallengeUserFlowBytes);

struct Runtime : LoopbackServer {
    CaptchaBridgeStatus status;
    u32 token_bytes, sitekey_bytes, rqdata_bytes, user_flow_bytes;
    CaptchaBridgeProvider provider;
    bool invisible, spent;
};
Runtime* runtime_of(const CaptchaBridge* bridge) noexcept {
    return bridge ? static_cast<Runtime*>(bridge->runtime) : nullptr;
}
constexpr u32 kOwnerBytes = loopback_owner_bytes<Runtime>;
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
    runtime->token_bytes = runtime->sitekey_bytes = runtime->rqdata_bytes = runtime->user_flow_bytes = 0;
}
void terminal(Runtime* runtime, CaptchaBridgePhase phase, CaptchaBridgeError error, u32 native = 0) noexcept {
    loopback_terminal<live, erase_secrets>(runtime, phase, error, native);
}
// The worker exclusively owns the wire and response partitions.
void wipe(LoopbackServer* server) noexcept { SecureZeroMemory(server->storage.data, kTokenOffset); }
bool step(LoopbackServer* server, LoopbackState* state) noexcept {
    auto* runtime = static_cast<Runtime*>(server);
    const auto current = loopback_snapshot(runtime);
    const u64 now = GetTickCount64();
    if (!live(current.phase) && current.phase != CaptchaBridgePhase::complete &&
        current.phase != CaptchaBridgePhase::failed) return false;
    const bool admitting = current.phase == CaptchaBridgePhase::starting || current.phase == CaptchaBridgePhase::waiting;
    if (admitting && deadline_due(now, current.deadline_ms)) {
        terminal(runtime, CaptchaBridgePhase::expired, CaptchaBridgeError::none);
        return false;
    }
    *state = {now, current.deadline_ms, admitting, live(current.phase)};
    return true;
}
void fail(LoopbackServer* server, LoopbackFailure failure, u32 native) noexcept {
    terminal(static_cast<Runtime*>(server), CaptchaBridgePhase::failed,
             failure == LoopbackFailure::event ? CaptchaBridgeError::event : CaptchaBridgeError::network, native);
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
                written =
                    captcha_bridge_write_page({view(runtime, kSitekeyOffset, runtime->sitekey_bytes),
                                               view(runtime, kRqdataOffset, runtime->rqdata_bytes),
                                               view(runtime, kStateOffset, kCaptchaBridgeStateBytes),
                                               view(runtime, kNonceOffset, kCaptchaBridgeNonceBytes),
                                               runtime->status.port, runtime->provider, runtime->invisible,
                                               view(runtime, kUserFlowOffset, runtime->user_flow_bytes)},
                                              response);
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
    if (candidate) loopback_notify(runtime);
    if (!written.ok()) return false;
    runtime->response_bytes = written.bytes;
    return true;
}
LoopbackRequest request(LoopbackServer* server) noexcept {
    auto* runtime = static_cast<Runtime*>(server);
    // The state slot is stable while live; stop erases it only under the lock.
    AcquireSRWLockShared(&runtime->lock);
    u8 state[kCaptchaBridgeStateBytes];
    copy_bytes(state, at(runtime, kStateOffset), kCaptchaBridgeStateBytes);
    const u16 port = runtime->status.port;
    ReleaseSRWLockShared(&runtime->lock);
    const auto parsed = captcha_bridge_parse_request({at(runtime, 0), runtime->wire_bytes}, port,
                                                     {reinterpret_cast<const char*>(state), kCaptchaBridgeStateBytes});
    SecureZeroMemory(state, sizeof(state));
    if (parsed.state == CaptchaBridgeParseState::need_input) return LoopbackRequest::more;
    const bool answered = respond(runtime, parsed);
    SecureZeroMemory(at(runtime, 0), kWireBytes);
    return answered ? LoopbackRequest::respond : LoopbackRequest::close;
}
constexpr LoopbackProfile kProfile{step, fail, loopback_bound<Runtime>, request, wipe, 0, 0, kWireBytes, kResponseOffset, 2, true};
CaptchaBridgeStatus invalid(CaptchaBridgeError error = CaptchaBridgeError::invalid_input, u32 native = ERROR_INVALID_PARAMETER) noexcept {
    CaptchaBridgeStatus result{}; result.error = error; result.native_error = native; return result;
}
} // namespace

CaptchaBridgeStatus captcha_bridge_start(CaptchaBridge* bridge, u64 generation, u32 timeout_ms,
                                         const CaptchaBridgeChallenge& challenge) noexcept {
    const CaptchaBridgeProvider provider = protocol_provider(challenge.provider);
    if (!bridge || bridge->runtime || !generation || timeout_ms < 1000 ||
        timeout_ms > kCaptchaBridgeDefaultTimeoutMs || provider == CaptchaBridgeProvider::unknown ||
        !captcha_bridge_sitekey_valid(challenge.sitekey) ||
        !captcha_bridge_rqdata_valid(challenge.rqdata) ||
        !captcha_bridge_user_flow_valid(challenge.user_flow))
        return invalid();
    auto* runtime = static_cast<Runtime*>(VirtualAlloc(nullptr, kOwnerBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime) return invalid(CaptchaBridgeError::memory, thread_last_error());
    bridge->runtime = runtime;
    runtime->status.generation = generation;
    runtime->status.deadline_ms = deadline_after(GetTickCount64(), timeout_ms);
    runtime->status.phase = CaptchaBridgePhase::starting;
    runtime->provider = provider;
    runtime->invisible = challenge.invisible;
    const auto secured = secure_buffer_initialize(&runtime->storage, kStorageBytes);
    if (!secured.ok()) { const u32 native = secured.native_error; loopback_release<Runtime>(bridge); return invalid(CaptchaBridgeError::memory, native); }
    copy_bytes(at(runtime, kSitekeyOffset), challenge.sitekey.data, challenge.sitekey.bytes);
    runtime->sitekey_bytes = challenge.sitekey.bytes;
    if (challenge.rqdata.bytes) copy_bytes(at(runtime, kRqdataOffset), challenge.rqdata.data, challenge.rqdata.bytes);
    runtime->rqdata_bytes = challenge.rqdata.bytes;
    if (challenge.user_flow.bytes)
        copy_bytes(at(runtime, kUserFlowOffset), challenge.user_flow.data, challenge.user_flow.bytes);
    runtime->user_flow_bytes = challenge.user_flow.bytes;
    u8 random[16];
    const bool state = secure_random_fill({random, sizeof(random)}).ok() &&
        captcha_bridge_write_hex({random, sizeof(random)}, {at(runtime, kStateOffset), kCaptchaBridgeStateBytes}).ok();
    SecureZeroMemory(random, sizeof(random));
    if (!state) { loopback_release<Runtime>(bridge); return invalid(CaptchaBridgeError::random); }
    u32 native = 0;
    const LoopbackStart started = loopback_start(runtime, &kProfile, false, &native);
    if (started != LoopbackStart::none) {
        loopback_release<Runtime>(bridge);
        return invalid(started == LoopbackStart::event ? CaptchaBridgeError::event : CaptchaBridgeError::thread, native);
    }
    return loopback_snapshot(runtime);
}
CaptchaBridgeStatus captcha_bridge_status(const CaptchaBridge* bridge) noexcept {
    auto* runtime = runtime_of(bridge); return runtime ? loopback_snapshot(runtime) : CaptchaBridgeStatus{};
}
HANDLE captcha_bridge_notification_handle(const CaptchaBridge* bridge) noexcept {
    auto* runtime = runtime_of(bridge); return runtime ? runtime->notification : nullptr;
}
SecureBufferResult captcha_bridge_copy_url(const CaptchaBridge* bridge, u64 expected_generation, SecureBuffer* output) noexcept {
    auto* runtime = runtime_of(bridge);
    if (!runtime || !loopback_external_output(runtime, bridge, sizeof(*bridge), kOwnerBytes, output)) return loopback_output_error();
    AcquireSRWLockShared(&runtime->lock);
    CaptchaBridgeWriteResult written{0, CaptchaBridgeProtocolError::invalid_input};
    if (expected_generation && runtime->status.generation == expected_generation &&
        runtime->status.phase == CaptchaBridgePhase::waiting && !runtime->spent &&
        !deadline_due(GetTickCount64(), runtime->status.deadline_ms))
        written = captcha_bridge_write_url(runtime->status.port, view(runtime, kStateOffset, kCaptchaBridgeStateBytes),
                                           {output->data, output->capacity});
    if (written.ok()) output->bytes = written.bytes;
    ReleaseSRWLockShared(&runtime->lock);
    return loopback_output_result(written.ok(), written.error == CaptchaBridgeProtocolError::capacity, written.bytes);
}
SecureBufferResult captcha_bridge_take_token(CaptchaBridge* bridge, u64 expected_generation, SecureBuffer* output) noexcept {
    auto* runtime = runtime_of(bridge);
    if (!runtime || !loopback_external_output(runtime, bridge, sizeof(*bridge), kOwnerBytes, output)) return loopback_output_error();
    AcquireSRWLockExclusive(&runtime->lock);
    SecureBufferResult result = loopback_output_error();
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
    bool joined;
    const auto status = loopback_stop_worker(runtime, timeout_ms, &joined);
    if (joined) loopback_release<Runtime>(bridge);
    return status;
}
} // namespace maicord
