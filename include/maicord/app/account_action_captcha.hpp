#pragma once
#include "maicord/app/captcha_bridge.hpp"
#include "maicord/app/rest_client.hpp"

namespace maicord {
// A CAPTCHA demanded by a signed-in user action (send, add friend, accept
// invite). The challenge, solution and replay values stay in the module's
// locked memory; the host only starts the browser bridge and asks to arm the
// one retry.
enum class AccountActionCaptchaOperation : u8 {
    start,    // decode the 400 body and start the loopback bridge
    status,   // poll; token_ready once a solution is held
    copy_url, // bridge URL (holds the single-use state) into a locked buffer
    arm,      // attach X-Captcha-* headers to the submit with request_id, once
    cancel,   // stop the bridge, erase every secret and disarm
};
struct AccountActionCaptchaRequest {
    Utf8View body;
    SecureBuffer* url;
    u64 generation;
    u64 request_id;
    u32 timeout_ms;
    u16 status_code;
    AccountActionCaptchaOperation operation;
    bool details; // arm the DETAILS lane instead of PRIMARY
};

struct AccountActionCaptcha {
    CaptchaChallenge challenge;
    CaptchaBridge bridge;
    CaptchaBridgeStatus status;
    SecureBuffer token;
    bool challenge_ready;
};

// Returns the bridge status; error invalid_input/state reports a refused
// operation. arm succeeds only with a held solution and clears it.
CaptchaBridgeStatus account_action_captcha(AccountActionCaptcha*, RestClient* primary, RestClient* details,
                                           const AccountActionCaptchaRequest&) noexcept;
void account_action_captcha_release(AccountActionCaptcha*, RestClient* primary,
                                    RestClient* details) noexcept;
} // namespace maicord
