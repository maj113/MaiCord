#include "maicord/app/chat/polls/account_poll_create.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"


namespace maicord {
RestClientSubmitResult account_poll_create_submit(RestClient* rest, u64 channel_id, u64 nonce,
                                                  const PollCreate* poll, u64 request_id) noexcept {
    // account_module_poll_create gives its request lane, a poll and the poll form's channel, nonce and request id.
    if (!poll_create_valid(*poll))
        return {RestClientSubmitError::invalid_input};
    u8 target[96];
    const auto path = write_message_create_target(channel_id, {target, sizeof(target)});
    if (!path.ok()) return {RestClientSubmitError::target_capacity};
    // Fully escaped question, ten answers and their emoji stay below this; staged off the stack.
    constexpr u32 kBodyBytes = 16 * 1024;
    auto* body = commit_pages<u8>(kBodyBytes);
    if (!body) return {RestClientSubmitError::memory};
    RestClientSubmitResult result{RestClientSubmitError::body_capacity};
    const auto written = write_message_poll_body(*poll, nonce, {body, kBodyBytes});
    if (written.ok()) {
        result = rest_client_submit(
            rest, {{target, path.bytes},
                   {body, written.bytes},
                   {kMessageCreateRouteId, channel_id, RestMethod::post, RestMajorParameter::channel},
                   request_id,
                   kRestClientDefaultTimeoutMs,
                   true});
        secure_clear_bytes(body, written.bytes);
    }
    pages_release(body);
    return result;
}
} // namespace maicord
