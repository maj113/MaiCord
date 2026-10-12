#include "application_action_captcha.hpp"
#include "application_state.hpp"
#include "maicord/core/time.hpp"

#include <Windows.h>
#include <shellapi.h>

namespace maicord::app_detail {
namespace {
constexpr u64 kPollMs = 500;
constexpr u32 kMaximumBodyBytes = 32 * 1024;
struct ActionCaptcha {
    SecureBuffer body;
    u64 scope, subject, generation, poll_at;
    u16 status_code;
    ApplicationActionCaptchaKind kind;
    ApplicationActionCaptchaPhase phase;
};
ActionCaptcha g_captcha{};

bool available() noexcept {
    return g_app.account.instance && g_app.account.api.action_captcha && g_app.account_ui;
}
bool current(ApplicationActionCaptchaKind kind, u64 subject) noexcept {
    return g_captcha.kind == kind && g_captcha.subject == subject && kind != ApplicationActionCaptchaKind::none &&
           g_captcha.scope == g_app.plugin_account_scope_serial;
}
CaptchaBridgeStatus call(AccountActionCaptchaOperation operation, u64 request_id = 0, bool details = false,
                         SecureBuffer* url = nullptr) noexcept {
    if (!available())
        return {0, 0, 0, 0, CaptchaBridgePhase::empty, CaptchaBridgeError::state, false};
    AccountActionCaptchaRequest request{};
    request.operation = operation;
    request.generation = g_captcha.generation;
    request.request_id = request_id;
    request.details = details;
    request.url = url;
    if (operation == AccountActionCaptchaOperation::start) {
        request.body = {reinterpret_cast<const char*>(g_captcha.body.data), g_captcha.body.bytes};
        request.status_code = g_captcha.status_code;
        request.timeout_ms = kCaptchaBridgeDefaultTimeoutMs;
    }
    return g_app.account.api.action_captcha(g_app.account.instance, &request);
}
void reset() noexcept {
    secure_buffer_release(&g_captcha.body);
    const u64 generation = g_captcha.generation;
    g_captcha = {};
    g_captcha.generation = generation;
}
bool open_url(const SecureBuffer& url) noexcept {
    // The URL carries the bridge's single-use state; it is never logged.
    wchar_t wide[512];
    if (!url.bytes || url.bytes >= 512)
        return false;
    for (u32 i = 0; i < url.bytes; ++i) {
        if (url.data[i] < 0x21 || url.data[i] > 0x7E)
            return false;
        wide[i] = static_cast<wchar_t>(url.data[i]);
    }
    wide[url.bytes] = 0;
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(g_app.window, L"open", wide, nullptr, nullptr,
                                                                SW_SHOWNORMAL));
    SecureZeroMemory(wide, sizeof(wide));
    return result > 32;
}
} // namespace

bool application_action_captcha_capture(ApplicationActionCaptchaKind kind, u64 subject,
                                        const RestClientResponse& response, Utf8View body) noexcept {
    if (kind == ApplicationActionCaptchaKind::none || !subject || !response.transport_ok() ||
        response.status_code != 400 || !response.rejection.captcha || !body.data || !body.bytes ||
        body.bytes > kMaximumBodyBytes || !available())
        return false;
    if (g_captcha.phase == ApplicationActionCaptchaPhase::waiting ||
        g_captcha.phase == ApplicationActionCaptchaPhase::solved)
        call(AccountActionCaptchaOperation::cancel);
    reset();
    if (!secure_buffer_initialize(&g_captcha.body, body.bytes).ok() ||
        !secure_buffer_assign(&g_captcha.body, {reinterpret_cast<const u8*>(body.data), body.bytes}).ok()) {
        reset();
        return false;
    }
    g_captcha.scope = g_app.plugin_account_scope_serial;
    g_captcha.subject = subject;
    g_captcha.kind = kind;
    g_captcha.status_code = response.status_code;
    g_captcha.phase = ApplicationActionCaptchaPhase::captured;
    return true;
}

ApplicationActionCaptchaPhase application_action_captcha_phase(ApplicationActionCaptchaKind kind,
                                                               u64 subject) noexcept {
    if (!current(kind, subject))
        return ApplicationActionCaptchaPhase::none;
    application_action_captcha_poll();
    return g_captcha.phase;
}

u64 application_action_captcha_subject(ApplicationActionCaptchaKind kind) noexcept {
    return current(kind, g_captcha.subject) ? g_captcha.subject : 0;
}

bool application_action_captcha_verify(ApplicationActionCaptchaKind kind, u64 subject) noexcept {
    if (!current(kind, subject) || (g_captcha.phase != ApplicationActionCaptchaPhase::captured &&
                                    g_captcha.phase != ApplicationActionCaptchaPhase::failed) ||
        !g_captcha.body.bytes)
        return false;
    if (!++g_captcha.generation)
        ++g_captcha.generation;
    const CaptchaBridgeStatus started = call(AccountActionCaptchaOperation::start);
    // The module decoded its own locked copy; drop ours immediately.
    secure_buffer_release(&g_captcha.body);
    SecureBuffer url{};
    const bool opened = started.ok() && secure_buffer_initialize(&url, 512).ok() &&
                        call(AccountActionCaptchaOperation::copy_url, 0, false, &url).ok() && open_url(url);
    secure_buffer_release(&url);
    if (!opened) {
        call(AccountActionCaptchaOperation::cancel);
        g_captcha.phase = ApplicationActionCaptchaPhase::failed;
        return false;
    }
    g_captcha.phase = ApplicationActionCaptchaPhase::waiting;
    g_captcha.poll_at = clock_monotonic_milliseconds() + kPollMs;
    return true;
}

bool application_action_captcha_poll() noexcept {
    if (g_captcha.phase != ApplicationActionCaptchaPhase::waiting)
        return false;
    if (g_captcha.scope != g_app.plugin_account_scope_serial || !available()) {
        reset();
        return true;
    }
    const u64 now = clock_monotonic_milliseconds();
    if (now < g_captcha.poll_at)
        return false;
    g_captcha.poll_at = now + kPollMs;
    const CaptchaBridgeStatus status = call(AccountActionCaptchaOperation::status);
    if (status.phase == CaptchaBridgePhase::token_ready) {
        g_captcha.phase = ApplicationActionCaptchaPhase::solved;
        return true;
    }
    if (status.phase == CaptchaBridgePhase::expired || status.phase == CaptchaBridgePhase::failed ||
        status.phase == CaptchaBridgePhase::cancelled || status.phase == CaptchaBridgePhase::empty ||
        !status.ok()) {
        call(AccountActionCaptchaOperation::cancel);
        g_captcha.phase = ApplicationActionCaptchaPhase::failed;
        return true;
    }
    return false;
}

bool application_action_captcha_arm(ApplicationActionCaptchaKind kind, u64 subject, u64 request_id,
                                    bool details) noexcept {
    if (!current(kind, subject) || g_captcha.phase != ApplicationActionCaptchaPhase::solved || !request_id)
        return false;
    const bool armed = call(AccountActionCaptchaOperation::arm, request_id, details).ok();
    reset();
    return armed;
}

void application_action_captcha_cancel() noexcept {
    if (g_captcha.phase == ApplicationActionCaptchaPhase::waiting ||
        g_captcha.phase == ApplicationActionCaptchaPhase::solved)
        call(AccountActionCaptchaOperation::cancel);
    reset();
}

u64 application_action_captcha_next_wake() noexcept {
    return g_captcha.phase == ApplicationActionCaptchaPhase::waiting ? maximum<u64>(1, g_captcha.poll_at) : 0;
}

Utf8View application_action_captcha_status(ApplicationActionCaptchaPhase phase) noexcept {
    switch (phase) {
    case ApplicationActionCaptchaPhase::captured:
        return utf8_literal("Discord needs a CAPTCHA first. Verify in your browser to continue.");
    case ApplicationActionCaptchaPhase::waiting:
        return utf8_literal("Complete the CAPTCHA in your browser\xE2\x80\xA6");
    case ApplicationActionCaptchaPhase::solved:
        return utf8_literal("Verified. Retrying\xE2\x80\xA6");
    case ApplicationActionCaptchaPhase::failed:
        return utf8_literal("The CAPTCHA wasn't completed. Try again.");
    case ApplicationActionCaptchaPhase::none:
        break;
    }
    return {};
}

void application_action_captcha_release() noexcept {
    reset();
}
} // namespace maicord::app_detail
