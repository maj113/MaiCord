#include "maicord/app/account_message_ack.hpp"

namespace maicord {
RestClientSubmitResult account_message_ack_submit(RestClient* rest,
    u64 channel_id, u64 message_id, ConstSlice<u8> body, u64 request_id) noexcept {
    if (!rest || !request_id || !body.data || !body.count || !utf8_validate(
        {reinterpret_cast<const char*>(body.data), body.count}).valid)
        return {RestClientSubmitError::invalid_input};
    if (body.count > kRestClientMaximumRequestBodyBytes)
        return {RestClientSubmitError::body_capacity};
    u8 target[kMessageAckMaximumTargetBytes];
    const auto path = write_message_ack_target(channel_id, message_id, {target, sizeof(target)});
    if (!path.ok()) return {RestClientSubmitError::invalid_input};
    return rest_client_submit(rest, {target, path.bytes}, body,
        {kMessageAckRouteId, channel_id, RestMethod::post, RestMajorParameter::channel},
        request_id, true);
}
} // namespace maicord
