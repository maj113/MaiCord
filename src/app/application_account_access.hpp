#pragma once
#include "account_ui_runtime.hpp"
#include "application_state.hpp"

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

} // namespace maicord::app_detail
