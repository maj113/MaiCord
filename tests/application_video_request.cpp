#include "../src/app/application_video_request.hpp"

namespace maicord {
void test_application_video_request(void (*check)(bool, const char*) noexcept) noexcept {
    AccountMediaRequestResult result{};
    result.state = AccountMediaRequestState::retry_wait;
    result.retry_at_ms = ~u64{0};
    check(!app_detail::application_video_source_pending(result),
          "video source: permanent negative cache retires loading without a decoder");
    result.retry_at_ms = 1234;
    check(app_detail::application_video_source_pending(result),
          "video source: finite source retry preserves pending selection");
    constexpr AccountMediaRequestState pending[]{
        AccountMediaRequestState::ready, AccountMediaRequestState::queued,
        AccountMediaRequestState::pending, AccountMediaRequestState::busy,
        AccountMediaRequestState::refresh_required};
    for (const auto state : pending) {
        result.state = state;
        check(app_detail::application_video_source_pending(result),
              "video source: asynchronous fetch and refresh states retain actual pending work");
    }
    constexpr AccountMediaRequestState terminal[]{AccountMediaRequestState::invalid,
                                                  AccountMediaRequestState::unsupported,
                                                  AccountMediaRequestState::failed};
    for (const auto state : terminal) {
        result.state = state;
        check(!app_detail::application_video_source_pending(result),
              "video source: terminal source failures cannot remain a buffering player");
    }
}
} // namespace maicord
