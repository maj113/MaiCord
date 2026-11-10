#pragma once
#include "account_ui_runtime.hpp"
#include "../shell/state.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord::app_detail {

// Channel record of the active account, or null.
inline const ChannelRecord* application_channel_record(u64 id) noexcept {
    return g_app.account_ui ? chat_state_store_find_channel(g_app.account_ui->chat_state, id) : nullptr;
}

// Next account REST request id; zero means "no request" and is skipped.
inline u64 application_next_request_id() noexcept {
    u64& id = g_app.account_ui->message_history_request_id;
    if (!++id)
        ++id;
    return id;
}

// Cancels `request_id` on a request lane (the module entry called without a request) when an account
// module is attached and the id is set; its response is still drained.
template <typename Request>
inline void application_cancel_request(RestClientSubmitResult (*lane)(void*, const Request*, u64) noexcept,
                                       u64 request_id) noexcept {
    const AccountHost& host = g_app.account;
    if (request_id && host.instance)
        lane(host.instance, nullptr, request_id);
}

// A refused submit that only waited for the lane (the previous request is still in flight): arms
// `retry_at` for `delay_ms` from `now` and returns true; any other result returns false untouched.
inline bool submit_busy(const RestClientSubmitResult& result, u64& retry_at, u64 now, u64 delay_ms) noexcept {
    if (result.error != RestClientSubmitError::busy)
        return false;
    retry_at = deadline_after(now, delay_ms);
    return true;
}

} // namespace maicord::app_detail
