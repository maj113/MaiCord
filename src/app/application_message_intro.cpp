
#include "application_friends.hpp"
#include "application_state.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/ui/message_scene.hpp"
#include "account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {
const MessageRecord* first_authoritative(const MessageStore& store) noexcept {
    for (u32 index = 0; index < store.count; ++index) {
        const MessageRecord* record = message_store_resolve(store, message_store_ordered_handle(store, index));
        if (record && record->delivery == MessageDeliveryState::authoritative)
            return record;
    }
    return nullptr;
}

// The resident window begins at the channel's first message.
bool window_at_channel_start(const ChannelRecord& channel) noexcept {
    if (g_app.pending_message_jump_message_id)
        return false;
    const MessageTimeline& timeline = g_app.message_view.timeline;
    const MessageHistoryCatchup& catchup = g_app.message_catchup;
    const MessageHistoryOlder& older = g_app.message_older;
    const MessageRecord* first = first_authoritative(g_app.message_view.store);
    const bool settled = catchup.channel_id == channel.id && message_history_browsing_ready(catchup.state);
    if (!first) // An empty channel (local sends only) once its latest page is known.
        return settled && catchup.state == MessageHistoryCatchupState::complete &&
               !timeline.newer_refetch_required;
    if (older.channel_id != channel.id || older.state != MessageHistoryOlderState::exhausted)
        return false;
    // A zero cursor proved an empty channel; everything resident arrived after.
    return older.cursor_before ? first->id <= older.cursor_before : settled;
}

MessageSceneIntroKind intro_kind(const ChannelRecord& channel) noexcept {
    switch (channel.type) {
    case 1:
        return MessageSceneIntroKind::direct_message;
    case 3:
        return MessageSceneIntroKind::group_direct_message;
    case 10:
    case 11:
    case 12:
        return MessageSceneIntroKind::thread;
    default:
        return MessageSceneIntroKind::channel;
    }
}
} // namespace

void application_message_intro_synchronize() noexcept {
    AccountUiRuntime* ui = g_app.account_ui;
    g_app.message_scene.bottom_anchored = true;
    MessageSceneIntro intro{};
    bool visible = false;
    if (ui && ui->active_channel_id && ui->active_channel_id == ui->chat_state.selected_channel_id &&
        !application_friends_active()) {
        const ChatStateStore& state = ui->chat_state;
        const ChannelRecord* channel = chat_state_store_find_channel(state, ui->active_channel_id);
        if (channel) {
            const Utf8View name = chat_state_store_channel_name(state, *channel);
            if ((channel->actions & channel_action_read_history) == 0) {
                visible = g_app.message_view.store.count == 0 &&
                          message_scene_intro_compose(&intro, MessageSceneIntroKind::no_history, channel->id, name,
                                                      {}, 0, false);
            } else if (window_at_channel_start(*channel)) {
                const MessageSceneIntroKind kind = intro_kind(*channel);
                u64 recipient = 0;
                Utf8View username{};
                if (kind == MessageSceneIntroKind::direct_message && state.private_channels) {
                    if (const auto* record = private_channel_store_find(*state.private_channels, channel->id)) {
                        const ConstSlice<u64> recipients =
                            private_channel_store_recipients(*state.private_channels, *record);
                        recipient = recipients.count ? recipients[0] : 0;
                    }
                    if (const UserRecord* user = recipient ? user_store_find(ui->user_store, recipient) : nullptr)
                        username = user_store_username(ui->user_store, *user);
                }
                visible = message_scene_intro_compose(&intro, kind, channel->id, name, username, recipient,
                                                      channel->type == 12);
            }
        }
    }
    if (message_scene_set_intro(&g_app.message_scene, visible ? &intro : nullptr,
                                ui ? &ui->text_measure_cache : nullptr))
        g_app.message_scene.dirty = true;
}

} // namespace maicord::app_detail
