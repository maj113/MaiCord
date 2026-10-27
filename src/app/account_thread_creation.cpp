#include "maicord/app/account_thread_creation.hpp"

namespace maicord {
RestClientSubmitResult account_thread_creation_submit(
    RestClient* rest, const ThreadCreationRequest* request, u64 request_id) noexcept {
    if (!rest || !request || !request_id || !thread_creation_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    u8 target[kThreadCreationMaximumTargetBytes]{}, body[kThreadCreationMaximumBodyBytes]{};
    const auto path = thread_creation_write_target(*request, {target, sizeof(target)});
    if (!path.ok()) return {RestClientSubmitError::target_capacity};
    const auto written = thread_creation_write_body(*request, {body, sizeof(body)});
    if (!written.ok()) return {RestClientSubmitError::body_capacity};
    const bool creating = request->operation == ThreadCreationOperation::from_message;
    return rest_client_submit(rest,
        {target, path.bytes}, creating ? ConstSlice<u8>{body, written.bytes} : ConstSlice<u8>{},
        {kThreadCreationRouteId, creating ? request->parent_id : request->source_message_id,
         creating ? RestMethod::post : RestMethod::get, RestMajorParameter::channel},
        request_id, creating);
}
} // namespace maicord
