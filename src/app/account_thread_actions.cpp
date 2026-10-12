#include "maicord/app/account_thread_actions.hpp"
#include "maicord/protocol/channel_overview.hpp"

namespace maicord {
RestClientSubmitResult account_thread_action_submit(
    RestClient* rest, const ThreadActionRequest* request, u64 request_id) noexcept {
    if (!rest || !request || !request_id || !thread_action_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    u8 target[kThreadActionMaximumTargetBytes]{}, body[kThreadActionMaximumBodyBytes]{};
    const auto path = thread_action_write_target(*request, {target, sizeof(target)});
    const auto written = thread_action_write_body(*request, {body, sizeof(body)});
    if (!path.ok() || !written.ok()) return {RestClientSubmitError::invalid_input};
    RestMethod method{};
    switch (request->operation) {
    case ThreadActionOperation::fetch_self: method = RestMethod::get; break;
    case ThreadActionOperation::join: method = RestMethod::post; break;
    case ThreadActionOperation::leave: method = RestMethod::delete_method; break;
    case ThreadActionOperation::unarchive: method = RestMethod::patch; break;
    }
    const bool unarchive = request->operation == ThreadActionOperation::unarchive;
    return rest_client_submit(rest, {target, path.bytes}, {body, written.bytes},
        {unarchive ? kChannelOverviewRouteId : kThreadMemberRouteId, request->thread_id,
         method, RestMajorParameter::channel}, request_id, unarchive);
}

RestClientSubmitResult account_thread_action_cancel(RestClient* rest, u64 request_id) noexcept {
    return rest_client_cancel_result(rest, request_id);
}
} // namespace maicord
