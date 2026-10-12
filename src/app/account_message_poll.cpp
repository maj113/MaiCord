#include "maicord/app/account_message_poll.hpp"

#include <Windows.h>

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
    SecureZeroMemory(body,sizeof(body));
    return submitted;
}
RestClientSubmitResult account_message_poll_cancel(RestClient* rest,u64 request_id) noexcept {
    if (!rest || !request_id) return {RestClientSubmitError::invalid_input};
    return {rest_client_cancel(rest,request_id)?RestClientSubmitError::none:
                                                       RestClientSubmitError::busy};
}
} // namespace maicord
