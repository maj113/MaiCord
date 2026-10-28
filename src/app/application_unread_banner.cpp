#include "application_private_channel.hpp"
#include "application_content_point.hpp"
#include "application_account_reconnect.hpp"
#include "application_cursor.hpp"
#include "application_attachments.hpp"
#include "application_content_input.hpp"
#include "application_friends.hpp"
#include "application_guild_settings.hpp"
#include "application_profile.hpp"
#include "application_read_state.hpp"
#include "application_scrolling.hpp"
#include "application_settings.hpp"
#include "application_shell_input.hpp"
#include "application_state.hpp"
#include "application_thread_browser.hpp"
#include "maicord/app/unread_banner_projection.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/permission_compute.hpp"
#include "maicord/ui/timestamp_format.hpp"
#include "account_ui_runtime.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
UiUnreadBannerScene g_banner{};
bool g_pressed_inside{};
// The card shows the account's connection notice, which outranks the conversation's.
bool g_connection_notice{};

bool motion_allowed() noexcept {
    return !g_app.preferences.reduced_motion && !g_app.renderer.reduced_motion && !g_app.renderer.live_resize &&
           g_app.window && IsWindowVisible(g_app.window) && !IsIconic(g_app.window);
}
// Covered or absent timelines publish an empty view so the bars retire.
bool timeline_presented() noexcept {
    return g_app.renderer_ready && g_app.account_ui && !g_app.login.visible && g_app.message_scene.active &&
           g_app.message_view.timeline.list && !application_friends_active() && !application_settings_active() &&
           !application_thread_browser_read_blocked() &&
           g_app.account_ui->active_channel_id == g_app.account_ui->chat_state.selected_channel_id;
}
bool input(UnreadBannerInput* out) noexcept {
    if (!timeline_presented())
        return false;
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, g_app.account_ui->active_channel_id);
    if (!channel)
        return false;
    const auto& timeline = g_app.message_view.timeline;
    *out = {&g_app.message_view.store,
            timeline.list,
            channel->id,
            channel->last_read_message_id,
            channel->last_message_id,
            timeline.scroll_units,
            timeline.viewport_units,
            channel->read_state_known,
            (channel->actions & channel_action_read_history) != 0,
            // Initial loads also flag a refetch; only a settled, non-empty window is detached.
            // Forward paging toward the present (incremental) keeps the window detached.
            timeline.newer_refetch_required && g_app.message_view.store.count != 0 &&
                (g_app.message_catchup.incremental ||
                 g_app.message_catchup.state == MessageHistoryCatchupState::complete ||
                 g_app.message_catchup.state == MessageHistoryCatchupState::paused),
            g_app.message_older.channel_id == channel->id &&
                g_app.message_older.state == MessageHistoryOlderState::exhausted};
    return true;
}
// Modal layers own the pointer even where they leave the bars visible.
bool pointer_available() noexcept {
    return ui_unread_banner_visible(g_banner) && !g_app.popup_scene.active && !g_app.reaction_viewer.active &&
           !g_app.image_viewer.scene.active && !(g_app.profile && g_app.profile->scene.active) &&
           !g_app.text_form && g_app.scrollbar_drag == ScrollbarDragKind::none &&
           g_app.pane_resize == PaneResizeKind::none;
}
u32 format_since(u64 since_ms, Slice<char> output) noexcept {
    if (!since_ms)
        return 0;
    const u64 now = clock_unix_milliseconds() / 1000, since = since_ms / 1000;
    char time[48], day[48], today[48];
    const auto t = format_timestamp_local(since, 't', now, {time, sizeof(time)});
    const auto d = format_timestamp_local(since, 'D', now, {day, sizeof(day)});
    const auto n = format_timestamp_local(now, 'D', now, {today, sizeof(today)});
    if (!t.bytes || t.bytes > output.count)
        return 0;
    u32 used = t.bytes;
    copy_bytes(output.data, time, used);
    const bool same_day = d.bytes && n.bytes && d.bytes == n.bytes && bytes_equal(day, today, d.bytes);
    if (!same_day && d.bytes && used + 4 + d.bytes <= output.count) {
        copy_bytes(output.data + used, " on ", 4);
        copy_bytes(output.data + used + 4, day, d.bytes);
        used += 4 + d.bytes;
    }
    return used;
}
bool jump_to_unread() noexcept {
    UnreadBannerInput in{};
    if (!input(&in))
        return false;
    const auto projection = unread_banner_project(in);
    // The official client lands on the first unread; a cursor outside the resident window
    // loads around the last read message instead.
    const u64 target = projection.first_unread_id ? projection.first_unread_id : in.last_read_message_id;
    if (!target)
        return scroll_message_view_to(false);
    return application_navigate_known_message(in.channel_id, target);
}
// Message requests and DM safety warnings for the open conversation.
UiBannerNotice conversation_notice(u64 channel_id) noexcept {
    UiBannerNotice notice{};
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, channel_id);
    const auto* record = channel && channel->type == 1 && state.private_channels
                             ? private_channel_store_find(*state.private_channels, channel_id)
                             : nullptr;
    if (!record)
        return notice;
    notice.busy = application_private_channel_request_busy(channel_id);
    if (private_channel_is_request(*record)) {
        const bool spam = (record->request_flags & private_request_spam) != 0;
        notice.id = channel_id;
        notice.title = spam ? utf8_literal("Possible spam") : utf8_literal("Message request");
        notice.description = spam ? utf8_literal("This may be unwanted. Accept only if you know them.")
                                  : utf8_literal("Accept to reply, or ignore to remove this conversation.");
        notice.primary = utf8_literal("Accept");
        notice.secondary = utf8_literal("Ignore");
        notice.warning = spam;
        return notice;
    }
    if (!record->safety_warning_id)
        return notice;
    notice.id = record->safety_warning_id;
    notice.warning = true;
    notice.secondary = utf8_literal("Dismiss");
    switch (record->safety_warning) {
    case PrivateSafetyWarning::stranger_danger:
        notice.title = utf8_literal("Unwanted message?");
        notice.description = utf8_literal("If you don't want to chat with this person, you can block or mute them.");
        break;
    case PrivateSafetyWarning::likely_account_takeover:
        notice.title = utf8_literal("Watch out for stolen accounts");
        notice.description =
            utf8_literal("Unexpected asks with unusual urgency may be a sign of a stolen account. Stay vigilant!");
        break;
    default:
        notice.title = utf8_literal("It looks like this conversation may not be safe");
        notice.description = utf8_literal("Take action to protect yourself.");
        break;
    }
    return notice;
}
// The guild of a channel whose server is viewed as roles, or zero.
u64 viewing_guild(u64 channel_id) noexcept {
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, channel_id);
    return channel && channel->guild_id && chat_state_store_viewing_as(state, channel->guild_id) ? channel->guild_id
                                                                                                : 0;
}
bool perform(UiUnreadBannerAction action) noexcept {
    const u64 channel = g_banner.view.channel_id;
    if (g_connection_notice && action == UiUnreadBannerAction::notice_primary)
        return application_account_reconnect_now();
    if (g_connection_notice && action == UiUnreadBannerAction::notice_secondary)
        return false;
    application_read_state_activity();
    // View as role: back to Roles, or exit.
    if (const u64 guild = action == UiUnreadBannerAction::notice_primary ||
                                  action == UiUnreadBannerAction::notice_secondary
                              ? viewing_guild(channel)
                              : 0)
        return application_view_as_role_end(guild, action == UiUnreadBannerAction::notice_primary);
    switch (action) {
    case UiUnreadBannerAction::notice_primary:
        return application_private_channel_accept_request(channel);
    case UiUnreadBannerAction::notice_secondary: {
        const auto* record = g_app.account_ui && g_app.account_ui->chat_state.private_channels
                                 ? private_channel_store_find(*g_app.account_ui->chat_state.private_channels, channel)
                                 : nullptr;
        if (record && private_channel_is_request(*record))
            return application_private_channel_ignore_request(channel);
        return application_private_channel_dismiss_safety(channel);
    }
    case UiUnreadBannerAction::jump_to_unread:
        return jump_to_unread();
    case UiUnreadBannerAction::mark_read:
        return application_read_state_mark(channel, true);
    case UiUnreadBannerAction::jump_to_present:
        return application_message_jump_to_present();
    default:
        return false;
    }
}
} // namespace

void application_unread_banner_synchronize(u64 now_ms) noexcept {
    g_app.renderer.unread_banner_scene = &g_banner;
    UnreadBannerInput in{};
    UnreadBannerProjection projection{};
    const bool presented = input(&in);
    if (presented)
        projection = unread_banner_project(in);
    char view_title[kUiBannerNoticeTitleBytes], connection_text[kUiBannerNoticeTextBytes];
    UiBannerNotice notice =
        presented ? application_account_connection_notice({connection_text, sizeof(connection_text)})
                  : UiBannerNotice{};
    g_connection_notice = notice.id != 0;
    if (presented && !notice.id)
        notice = conversation_notice(in.channel_id);
    if (presented && !notice.id)
        notice = application_view_as_role_notice(in.channel_id, {view_title, sizeof(view_title)});
    if (notice.id && !projection.view.channel_id)
        projection.view.channel_id = in.channel_id;
    const bool notice_changed = ui_unread_banner_set_notice(&g_banner, &notice);
    char label[kUiUnreadBannerLabelBytes], since[96];
    const u32 since_bytes = format_since(projection.view.since_unix_ms, {since, sizeof(since)});
    const u32 label_bytes = projection.view.banner
                                ? ui_unread_banner_format_label(projection.view.count, projection.view.count_estimated,
                                                                {since, since_bytes}, {label, sizeof(label)})
                                : 0;
    const UiWidgetReference owner{&g_app.message_scene.widgets, g_app.message_scene.root};
    const bool changed = ui_unread_banner_update(&g_banner, projection.view, {label, label_bytes},
                                                 timeline_presented() ? g_app.message_scene.viewport : UiRect{},
                                                 owner, now_ms, motion_allowed());
    if (changed || notice_changed)
        application_repaint();
    // The hover toolbar belongs to the timeline layer: these bars draw above
    // it, so it clips (or hides) itself where they cover it.
    UiRect occluders[2]{};
    ui_unread_banner_occluders(g_banner, occluders);
    bool bar_changed = false;
    if (message_action_bar_set_occluders(&g_app.message_action_bar, occluders, 2, &bar_changed) && bar_changed)
        application_request_frame();
}

u64 application_unread_banner_next_wake(u64 now_ms) noexcept {
    return ui_unread_banner_next_wake(g_banner, now_ms, motion_allowed());
}

bool application_unread_banner_window_message(UINT message, WPARAM, LPARAM lparam) noexcept {
    if (message != WM_MOUSEMOVE && message != WM_LBUTTONDOWN && message != WM_LBUTTONUP &&
        message != WM_SETCURSOR && message != WM_MOUSELEAVE)
        return false;
    if (message == WM_MOUSELEAVE || !pointer_available()) {
        if (ui_unread_banner_set_hover(&g_banner, UiUnreadBannerAction::none))
            application_request_frame();
        if (message == WM_LBUTTONUP && g_pressed_inside) {
            g_pressed_inside = false;
            return true;
        }
        return false;
    }
    POINT cursor{};
    float x = 0, y = 0;
    if (message == WM_SETCURSOR) {
        if (!GetCursorPos(&cursor) || !application_screen_to_content(&cursor))
            return false;
        const float scale = 96.f / static_cast<float>(maximum<u32>(g_app.renderer.dpi, 1));
        x = static_cast<float>(cursor.x) * scale;
        y = static_cast<float>(cursor.y) * scale;
    } else {
        x = client_dip_x(lparam);
        y = client_dip_y(lparam);
    }
    const auto action = ui_unread_banner_hit_test(g_banner, x, y);
    switch (message) {
    case WM_SETCURSOR:
        if (action == UiUnreadBannerAction::none)
            return false;
        application_set_cursor(ui_action_cursor(true));
        return true;
    case WM_MOUSEMOVE:
        if (ui_unread_banner_set_hover(&g_banner, action))
            application_request_frame();
        return action != UiUnreadBannerAction::none;
    case WM_LBUTTONDOWN:
        if (action == UiUnreadBannerAction::none)
            return false;
        g_pressed_inside = true;
        perform(action);
        application_request_frame();
        return true;
    default:
        if (!g_pressed_inside)
            return false;
        g_pressed_inside = false;
        return true;
    }
}

bool application_unread_banner_escape() noexcept {
    UnreadBannerInput in{};
    if (!input(&in) || application_attachment_input().label.bytes)
        return false;
    const auto plan = unread_banner_escape_plan(in);
    if (!plan.any())
        return false;
    application_read_state_activity();
    if (plan.mark_read)
        application_read_state_mark(in.channel_id, true);
    if (plan.jump_to_present)
        application_message_jump_to_present();
    application_request_frame();
    return true;
}

void application_unread_banner_release() noexcept {
    if (g_app.renderer.unread_banner_scene == &g_banner)
        g_app.renderer.unread_banner_scene = nullptr;
    ui_unread_banner_clear(&g_banner);
    g_pressed_inside = g_connection_notice = false;
}

} // namespace maicord::app_detail
