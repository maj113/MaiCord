#pragma once
#include "maicord/app/rest_client.hpp"

namespace maicord::app_detail {
// One pending CAPTCHA for a signed-in user action. The rejected body waits in
// locked memory until the user chooses to verify; the module then holds the
// challenge, runs the loopback bridge and arms exactly one retry.
enum class ApplicationActionCaptchaKind : u8 {
    none,
    message_send,
    friend_request,
    guild_invite,
    guild_join, // the server tag popout's Join (subject: the server ID)
};
enum class ApplicationActionCaptchaPhase : u8 {
    none,
    captured, // waiting for the user to choose Verify in browser
    waiting,  // browser challenge open
    solved,   // the retry can be armed
    failed,
};
// subject identifies the retried request (message nonce, invite/user key).
bool application_action_captcha_capture(ApplicationActionCaptchaKind, u64 subject,
                                        const RestClientResponse&, Utf8View body) noexcept;
ApplicationActionCaptchaPhase application_action_captcha_phase(ApplicationActionCaptchaKind, u64 subject) noexcept;
// Subject of the pending challenge of this kind, or 0.
u64 application_action_captcha_subject(ApplicationActionCaptchaKind) noexcept;
// Starts the bridge and opens the browser; true when the challenge is open.
bool application_action_captcha_verify(ApplicationActionCaptchaKind, u64 subject) noexcept;
// Arms the solved retry on the request about to be submitted; one shot.
bool application_action_captcha_arm(ApplicationActionCaptchaKind, u64 subject, u64 request_id,
                                    bool details) noexcept;
void application_action_captcha_cancel() noexcept;
// Polls the browser bridge; returns true when the phase changed.
bool application_action_captcha_poll() noexcept;
u64 application_action_captcha_next_wake() noexcept;
Utf8View application_action_captcha_status(ApplicationActionCaptchaPhase) noexcept;
// Account stop: the module already erased its copy.
void application_action_captcha_release() noexcept;
} // namespace maicord::app_detail
