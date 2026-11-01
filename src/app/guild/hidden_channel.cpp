#include "hidden_channel.hpp"
#include "../account/account_presentation.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../chat/composer/composer.hpp"
#include "../chat/messages/interaction.hpp"
#include "../shell/state.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/guild/hidden_channel_menu.hpp"

namespace maicord::app_detail {
namespace {
constexpr float kPopupWidth = 264.0f;

// The popup borrows its row text; it stays here until the next open.
char g_text[kHiddenChannelMenuTextBytes];

const ChannelRecord* hidden_channel(u64 channel_id) noexcept {
    if (!g_app.account_ui || g_app.login.visible)
        return nullptr;
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, channel_id);
    return channel && chat_state_store_channel_hidden(state, *channel) ? channel : nullptr;
}
} // namespace

bool application_channel_hidden(u64 channel_id) noexcept {
    return hidden_channel(channel_id) != nullptr;
}

bool application_hidden_channel_open(u64 channel_id, UiRect anchor) noexcept {
    const ChannelRecord* channel = hidden_channel(channel_id);
    if (!channel)
        return false;
    auto& ui = *g_app.account_ui;
    TextBank bank{g_text, sizeof(g_text), 0};
    PopupItem items[kHiddenChannelMenuRows]{};
    const u32 count = hidden_channel_menu_build(ui.chat_state, ui.user_store, *channel,
                                                clock_unix_milliseconds() / 1000, &bank,
                                                {items, kHiddenChannelMenuRows});
    if (!count)
        return false;
    const auto opened = popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, channel_id, {items, count},
                                         anchor, application_viewport_dips(), kPopupWidth);
    if (!opened.ok() || !g_app.popup_scene.active)
        return dismiss_popup();
    set_popup_owner(PopupOwnerKind::hidden_channel);
    g_app.context_focus = &g_app.popup_scene.widgets;
    return refresh_popup_text();
}
} // namespace maicord::app_detail
