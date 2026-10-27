#include "maicord/app/account_message_poll.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
RestClientSubmitResult account_message_poll_submit(RestClient* rest,
    const PollRequest* request,u64 request_id) noexcept {
    if (!rest || !request || !request_id) return {RestClientSubmitError::invalid_input};
    u8 target[kPollMaximumTargetBytes]{},body[kPollMaximumRequestBodyBytes]{};
    const auto written=write_poll_request(*request,{target,sizeof(target)},{body,sizeof(body)});
    if (!written.ok()) return {written.error==PollWriteError::output_capacity
        ?RestClientSubmitError::target_capacity:RestClientSubmitError::invalid_input};
    const auto submitted=rest_client_submit(rest,
        {{target,written.target_bytes},{body,written.body_bytes},
         {written.route_id,request->channel_id,written.method,RestMajorParameter::channel},
         request_id,kRestClientDefaultTimeoutMs,written.body_bytes!=0,false});
    secure_clear_bytes(body,sizeof(body));
    return submitted;
}
} // namespace maicord
