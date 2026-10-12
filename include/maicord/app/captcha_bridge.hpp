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
};

inline constexpr u32 kCaptchaBridgeDefaultTimeoutMs = 5 * 60 * 1000;

// UI-thread API; only the private socket worker runs concurrently. Start only
// on explicit user action: one loopback listener on 127.0.0.1 with an
// OS-assigned port and a 128-bit single-use state. It does not launch a
// browser. Generation must be nonzero. Timeout: 1 s through 5 minutes.
CaptchaBridgeStatus captcha_bridge_start(CaptchaBridge* bridge, u64 generation, u32 timeout_ms,
                                         const CaptchaBridgeChallenge& challenge) noexcept;
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
u32 captcha_bridge_runtime_bytes() noexcept;

} // namespace maicord
