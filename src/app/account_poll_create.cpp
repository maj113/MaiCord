#include "maicord/app/account_poll_create.hpp"

#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
RestClientSubmitResult account_poll_create_submit(RestClient* rest, u64 channel_id, u64 nonce,
                                                  const PollCreate* poll, u64 request_id) noexcept {
    if (!rest || !channel_id || !nonce || !poll || !request_id || !poll_create_valid(*poll))
        return {RestClientSubmitError::invalid_input};
    u8 target[96];
    const auto path = write_message_create_target(channel_id, {target, sizeof(target)});
    if (!path.ok()) return {RestClientSubmitError::target_capacity};
    // Fully escaped question, ten answers and their emoji stay below this; staged off the stack.
    constexpr u32 kBodyBytes = 16 * 1024;
    auto* body = static_cast<u8*>(VirtualAlloc(nullptr, kBodyBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
                   true,
                   false});
        secure_clear_bytes(body, written.bytes);
    }
    VirtualFree(body, 0, MEM_RELEASE);
    return result;
}
} // namespace maicord
