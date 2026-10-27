#include "maicord/app/account_thread_browser.hpp"

namespace maicord {
RestClientSubmitResult account_thread_browser_submit(
    RestClient* rest, const ThreadBrowserRequest* request, u64 request_id) noexcept {
    if (!rest || !request || !request_id || !thread_browser_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    u8 target[kThreadBrowserMaximumTargetBytes];
    const auto written = thread_browser_write_target(*request, {target, sizeof(target)});
    if (!written.ok())
        return {written.error == ThreadBrowserError::capacity
                    ? RestClientSubmitError::target_capacity
                    : RestClientSubmitError::invalid_input};
    return rest_client_submit(
        rest, {target, written.bytes}, {},
        {kThreadBrowserRouteId, request->parent_id, RestMethod::get,
         RestMajorParameter::channel},
        request_id, false);
}
} // namespace maicord
