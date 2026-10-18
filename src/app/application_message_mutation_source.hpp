#pragma once
#include "application_state.hpp"

namespace maicord::app_detail {
// The message and account state an edit or delete was accepted against.
struct MessageMutationSource {
    MessageHandle handle{};
    u64 scope{}, profile{}, self{};
    void* instance{};
    u32 revision{};
    u16 channel_generation{};
};
inline MessageMutationSource message_mutation_source_capture(MessageHandle handle,
                                                             const MessageRecord& record) noexcept {
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, record.channel_id);
    return {handle,
            g_app.plugin_account_scope_serial,
            g_app.account.status.profile_id,
            g_app.account_ui->chat_state.self_user_id,
            g_app.account.instance,
            record.revision,
            channel ? channel->generation : u16{0}};
}
// The captured message while its account, channel and message are unchanged; otherwise null.
inline const MessageRecord* message_mutation_source_record(const MessageMutationSource& source,
                                                           u64 channel_id, u64 message_id) noexcept {
    if (!g_app.account_ui || !source.instance || source.instance != g_app.account.instance ||
        source.scope != g_app.plugin_account_scope_serial || source.profile != g_app.account.status.profile_id ||
        source.self != g_app.account_ui->chat_state.self_user_id)
        return nullptr;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    const auto* record = message_store_resolve(g_app.message_view.store, source.handle);
    return channel && channel->generation == source.channel_generation && record && record->id == message_id &&
                   record->channel_id == channel->id && record->revision == source.revision
               ? record
               : nullptr;
}
} // namespace maicord::app_detail
