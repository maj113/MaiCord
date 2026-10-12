#pragma once
// Inert action-CAPTCHA owner for fixtures that link callers but not the
// browser bridge. Include in exactly one translation unit per executable.
#include "../src/app/application_action_captcha.hpp"

namespace maicord::app_detail {
bool application_action_captcha_capture(ApplicationActionCaptchaKind, u64, const RestClientResponse&,
                                        Utf8View) noexcept {
    return false;
}
ApplicationActionCaptchaPhase application_action_captcha_phase(ApplicationActionCaptchaKind, u64) noexcept {
    return ApplicationActionCaptchaPhase::none;
}
u64 application_action_captcha_subject(ApplicationActionCaptchaKind) noexcept {
    return 0;
}
bool application_action_captcha_verify(ApplicationActionCaptchaKind, u64) noexcept {
    return false;
}
bool application_action_captcha_arm(ApplicationActionCaptchaKind, u64, u64, bool) noexcept {
    return false;
}
void application_action_captcha_cancel() noexcept {
}
bool application_action_captcha_poll() noexcept {
    return false;
}
u64 application_action_captcha_next_wake() noexcept {
    return 0;
}
Utf8View application_action_captcha_status(ApplicationActionCaptchaPhase) noexcept {
    return {};
}
void application_action_captcha_release() noexcept {
}
} // namespace maicord::app_detail
