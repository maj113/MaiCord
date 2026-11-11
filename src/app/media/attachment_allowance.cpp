#include "../shell/state.hpp"
#include "maicord/model/chat/upload_allowance.hpp"
#include "../account/account_ui_runtime.hpp"

namespace maicord::app_detail {
u64 application_attachment_limit(u64 channel_id) noexcept {
    if (!g_app.account_ui)
        return upload_allowance_bytes(0, 0, 0, {});
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, channel_id);
    const u64 guild_id = channel ? channel->guild_id : 0;
    const auto* guild = guild_id ? chat_state_store_find_guild(state, guild_id) : nullptr;
    const auto* details = guild_id ? guild_detail_store_find(g_app.account_ui->guild_details, guild_id) : nullptr;
    return upload_allowance_bytes(state.self_premium_type, details ? details->premium_tier : 0,
                                  guild ? guild->upload_features : 0, state.upload_policy);
}
} // namespace maicord::app_detail
