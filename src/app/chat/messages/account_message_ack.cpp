#include "maicord/app/chat/messages/account_message_ack.hpp"

namespace maicord {
static_assert(kMessageAckMaximumBodyBytes <= kRestClientMaximumRequestBodyBytes);
RestClientSubmitResult account_message_ack_submit(RestClient* rest,
    u64 channel_id, u64 message_id, ConstSlice<u8> body, u64 request_id) noexcept {
    // read_state.cpp gives a write_message_ack_body body for a resident channel and a request id.
    if (!rest)
        return {RestClientSubmitError::invalid_input};
    u8 target[kMessageAckMaximumTargetBytes];
    const auto path = write_message_ack_target(channel_id, message_id, {target, sizeof(target)});
    if (!path.ok()) return {RestClientSubmitError::invalid_input};
    return rest_client_submit(rest, {target, path.bytes}, body,
        {kMessageAckRouteId, channel_id, RestMethod::post, RestMajorParameter::channel},
        request_id, true);
}
} // namespace maicord
