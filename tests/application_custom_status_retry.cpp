#include "../src/app/application_custom_status.cpp"

namespace maicord::app_detail {
// The response fixture has no selected account view to repaint.
bool refresh_account_text(bool) noexcept {
    return true;
}
namespace {
u32 retry_completions;
bool WINAPI complete_retry(void*, u64, u64, u64, const RestClientResponse*, Utf8View) noexcept {
    ++retry_completions;
    return true;
}
} // namespace

void test_custom_status_retry(void (*check)(bool, const char*) noexcept) noexcept {
    const auto old_instance = g_app.account.instance;
    const auto old_complete = g_app.account.api.self_status_complete;
    g_app.account.instance = &retry_completions;
    g_app.account.api.self_status_complete = complete_retry;
    retry_completions = 0;
    for (u32 kind = 0; kind < 5; ++kind) {
        request = {};
        request.request_id = 72;
        request.queued = true;
        request.attempts = static_cast<u8>(kind == 4 ? 3 : 1);
        RestClientResponse response{};
        response.request_id = 72;
        response.status_code = 429;
        response.rate_limited = kind != 1;
        response.retry_at_ms = kind == 2 ? 0 : 2345678;
        response.rejection.captcha = kind == 0;
        check(application_custom_status_response(response, {}), "custom status exact completion consumed");
        if (kind == 3)
            check(request.queued && !request.failed && !request.request_id && request.retry_at == 2345678 &&
                      request.attempts == 1,
                  "custom status genuine 429 retains exact deadline and attempt count");
        else
            check(!request.queued && request.failed && !request.request_id && !request.retry_at,
                  "custom status challenge, unknown cooldown or exhausted retry is terminal");
    }
    check(retry_completions == 5, "custom status completes each owned receipt once");
    request = {};
    g_app.account.instance = old_instance;
    g_app.account.api.self_status_complete = old_complete;
}
} // namespace maicord::app_detail
