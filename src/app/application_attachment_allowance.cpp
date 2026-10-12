#include "application_attachments.hpp"
#include "application_state.hpp"
#include "maicord/model/upload_allowance.hpp"

namespace maicord::app_detail {
u64 application_attachment_limit(u64 channel_id) noexcept {
    if (!g_app.account_ui)
        return upload_allowance_bytes(0, 0, {});
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, channel_id);
    const auto* guild =
        channel && channel->guild_id ? chat_state_store_find_guild(state, channel->guild_id) : nullptr;
    return upload_allowance_bytes(state.self_premium_type, guild ? guild->upload_features : 0,
                                    state.upload_policy);
}
} // namespace maicord::app_detail
