#include "../src/app/application_notification_controls.cpp"

namespace maicord::app_detail {
const MessageNotificationStore* application_notifications_store(const Application*) noexcept {
    return nullptr;
}
namespace {
u32 retry_rollbacks;
} // namespace
bool application_notifications_project(Application*, u64, u64, const MessageNotificationSetting&) noexcept {
    ++retry_rollbacks;
    return true;
}

void test_notification_retry(void (*check)(bool, const char*) noexcept) noexcept {
    retry_rollbacks = 0;
    for (u32 kind = 0; kind < 5; ++kind) {
        owner = {};
        owner.request_id = 73;
        owner.count = 1;
        owner.queue[0].submitted = true;
        owner.queue[0].attempts = kind == 4 ? 3 : 1;
        RestClientResponse response{};
        response.request_id = 73;
        response.status_code = 429;
        response.rate_limited = kind != 1;
        response.retry_at_ms = kind == 2 ? 0 : 3456789;
        response.rejection.captcha = kind == 0;
        check(application_notification_controls_response(response, {}),
              "notification exact completion consumed");
        if (kind == 3)
            check(owner.count == 1 && !owner.request_id && owner.retry_at == 3456789 &&
                      !owner.queue[0].submitted && owner.queue[0].attempts == 1,
                  "notification genuine 429 retains exact deadline and attempt count");
        else
            check(!owner.count && !owner.request_id && !owner.retry_at,
                  "notification challenge, unknown cooldown or exhausted retry leaves no queued write");
    }
    check(retry_rollbacks == 4, "notification terminal failures restore the prior setting exactly once");
    owner = {};
}
} // namespace maicord::app_detail
