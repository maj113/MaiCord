#include "maicord/app/account_private_channel.hpp"

#include <Windows.h>

namespace maicord {
RestClientSubmitResult account_private_channel_submit(RestClient* rest,
                                                      const PrivateChannelActionRequest* request,
                                                      u64 request_id) noexcept {
    if (!rest || !request || !request_id) return {RestClientSubmitError::invalid_input};
    u8 target[kPrivateChannelActionTargetBytes]{}, body[kPrivateChannelActionBodyBytes]{};
    const auto written = private_channel_action_write(*request, {target, sizeof(target)}, {body, sizeof(body)});
    if (!written.ok) return {RestClientSubmitError::invalid_input};
    const auto submitted = rest_client_submit(
        rest, {target, written.target_bytes},
        {written.body_bytes ? body : nullptr, written.body_bytes},
        private_channel_action_route(*request),
        request_id,
        written.body_bytes != 0);
    SecureZeroMemory(body, sizeof(body));
    return submitted;
}
RestClientSubmitResult account_private_channel_cancel(RestClient* rest, u64 request_id) noexcept {
    return rest_client_cancel_result(rest, request_id);
}
} // namespace maicord
