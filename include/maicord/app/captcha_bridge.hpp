#pragma once

#include "maicord/app/captcha_challenge.hpp"
#include "maicord/protocol/captcha_bridge.hpp"

namespace maicord {

enum class CaptchaBridgePhase : u8 {
    empty, starting, waiting, token_ready, complete, expired, cancelled, failed, stopping,
};
enum class CaptchaBridgeError : u8 {
    none, invalid_input, memory, random, event, thread, network, rejected, state, stop,
};
struct CaptchaBridgeStatus {
    u64 generation;
    u64 deadline_ms;
    u32 native_error;
    u16 port;
    CaptchaBridgePhase phase;
    CaptchaBridgeError error;
    bool page_served;
    constexpr bool ok() const noexcept { return error == CaptchaBridgeError::none; }
};
struct CaptchaBridge { void* runtime; };
// Views are copied synchronously into the bridge's locked arena.
struct CaptchaBridgeChallenge {
    Utf8View sitekey;
    Utf8View rqdata;
    CaptchaProvider provider;
    bool invisible;
    Utf8View user_flow; // optional bounded UTF-8 enterprise action; empty omits it
};

inline constexpr u32 kCaptchaBridgeDefaultTimeoutMs = 5 * 60 * 1000;

// UI-thread API; only the private socket worker runs concurrently. Start only
// on explicit user action: one loopback listener on 127.0.0.1 with an
// OS-assigned port and a 128-bit single-use state. It does not launch a
// browser. Generation must be nonzero. Timeout: 1 s through 5 minutes.
CaptchaBridgeStatus captcha_bridge_start(CaptchaBridge* bridge, u64 generation, u32 timeout_ms,
                                         const CaptchaBridgeChallenge& challenge) noexcept;
// Starts the bridge from a stored challenge. copy_secret(kind, SecureBuffer*) fills a locked buffer with
// one challenge secret. The sitekey is required; rqdata and user_flow stay empty when absent. A failed
// sitekey copy reports invalid_input with copy_native_error.
template <typename CopySecret>
CaptchaBridgeStatus captcha_bridge_start_copied(CaptchaBridge* bridge, u64 generation, u32 timeout_ms,
                                                CaptchaProvider provider, bool invisible,
                                                CopySecret copy_secret, u32 copy_native_error) noexcept {
    SecureBuffer sitekey{}, rqdata{}, user_flow{};
    const bool copied =
        secure_buffer_initialize(&sitekey, kCaptchaChallengeSitekeyBytes + kCaptchaChallengeUserFlowBytes)
            .ok() &&
        secure_buffer_initialize(&rqdata, kCaptchaChallengeRqdataBytes).ok() &&
        copy_secret(CaptchaChallengeSecretKind::sitekey, &sitekey).ok();
    // user_flow borrows the locked tail; only sitekey owns/releases the arena.
    if (sitekey.data)
        user_flow = {sitekey.data + kCaptchaChallengeSitekeyBytes, kCaptchaChallengeUserFlowBytes, 0,
                     sitekey.locked};
    if (copied && !copy_secret(CaptchaChallengeSecretKind::rqdata, &rqdata).ok())
        secure_buffer_clear(&rqdata);
    if (copied && !copy_secret(CaptchaChallengeSecretKind::user_flow, &user_flow).ok())
        secure_buffer_clear(&user_flow);
    const CaptchaBridgeStatus status =
        copied ? captcha_bridge_start(bridge, generation, timeout_ms,
                                      {{reinterpret_cast<const char*>(sitekey.data), sitekey.bytes},
                                       {reinterpret_cast<const char*>(rqdata.data), rqdata.bytes},
                                       provider,
                                       invisible,
                                       {reinterpret_cast<const char*>(user_flow.data), user_flow.bytes}})
               : CaptchaBridgeStatus{generation,
                                     0,
                                     copy_native_error,
                                     0,
                                     CaptchaBridgePhase::failed,
                                     CaptchaBridgeError::invalid_input,
                                     false};
    secure_buffer_release(&sitekey);
    secure_buffer_release(&rqdata);
    return status;
}
CaptchaBridgeStatus captcha_bridge_status(const CaptchaBridge* bridge) noexcept;
HANDLE captcha_bridge_notification_handle(const CaptchaBridge* bridge) noexcept;
// URL contains the state; do not log it. Output must be locked and disjoint.
SecureBufferResult captcha_bridge_copy_url(const CaptchaBridge* bridge, u64 expected_generation,
                                           SecureBuffer* output) noexcept;
// One shot: token_ready -> complete. Output must be locked and disjoint.
SecureBufferResult captcha_bridge_take_token(CaptchaBridge* bridge, u64 expected_generation,
                                             SecureBuffer* output) noexcept;
// Invalidates the state and erases every secret before joining the worker. A
// failed join keeps the runtime for retry. Join timeout: 0 through 5000 ms.
CaptchaBridgeStatus captcha_bridge_stop(CaptchaBridge* bridge, u32 timeout_ms) noexcept;

} // namespace maicord
