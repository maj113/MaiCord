#include "maicord/app/account_poll_create.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
RestClientSubmitResult account_poll_create_submit(RestClient* rest, u64 channel_id, u64 nonce,
                                                  const PollCreate* poll, u64 request_id) noexcept {
    if (!rest || !channel_id || !nonce || !poll || !request_id || !poll_create_valid(*poll))
        return {RestClientSubmitError::invalid_input};
    u8 target[96];
    // Fully escaped question, ten answers and their emoji stay below this.
    u8 body[16 * 1024];
    const auto path = write_message_create_target(channel_id, {target, sizeof(target)});
    if (!path.ok()) return {RestClientSubmitError::target_capacity};
    const auto written = write_message_poll_body(*poll, nonce, {body, sizeof(body)});
    if (!written.ok()) return {RestClientSubmitError::body_capacity};
    const auto submitted = rest_client_submit(
        rest, {{target, path.bytes},
               {body, written.bytes},
               {kMessageCreateRouteId, channel_id, RestMethod::post, RestMajorParameter::channel},
               request_id,
               kRestClientDefaultTimeoutMs,
               true,
               false});
    secure_clear_bytes(body, written.bytes);
    return submitted;
}
RestClientSubmitResult account_poll_create_cancel(RestClient* rest, u64 request_id) noexcept {
    return rest_client_cancel_result(rest, request_id);
}
} // namespace maicord
