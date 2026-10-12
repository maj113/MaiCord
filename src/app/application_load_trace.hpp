#pragma once
#include "application_state.hpp"
#include "maicord/core/trace_record.hpp"

namespace maicord::app_detail {
inline void application_trace_load_stage(const char* stage) noexcept {
    const i64 now = clock_now();
    TraceRecord line(stage);
    line.field("startup_us", clock_microseconds(g_app.clock, g_app.process_start, now));
    line.field("channel_epoch", g_app.load_trace.channel_epoch);
    line.field("channel_us", g_app.load_trace.channel_started
                                 ? clock_microseconds(g_app.clock, g_app.load_trace.channel_started, now)
                                 : 0);
    line.write("load_trace");
}
inline const char* rest_owner_name(MessageHistoryRequestOwner owner) noexcept {
    switch (owner) {
    case MessageHistoryRequestOwner::none:
        return "idle";
    case MessageHistoryRequestOwner::private_conversation:
        return "private_conversation";
    case MessageHistoryRequestOwner::attachment_upload:
        return "attachment_upload";
    case MessageHistoryRequestOwner::typing:
        return "typing";
    case MessageHistoryRequestOwner::message_send:
        return "message_send";
    case MessageHistoryRequestOwner::message_delete:
        return "message_delete";
    case MessageHistoryRequestOwner::message_edit:
        return "message_edit";
    case MessageHistoryRequestOwner::message_reaction:
        return "message_reaction";
    case MessageHistoryRequestOwner::reaction_users:
        return "reaction_users";
    case MessageHistoryRequestOwner::user_profile:
        return "profile";
    case MessageHistoryRequestOwner::commands:
        return "commands";
    case MessageHistoryRequestOwner::catchup:
        return "history";
    case MessageHistoryRequestOwner::message_jump:
        return "message_jump";
    case MessageHistoryRequestOwner::attachment_refresh:
        return "attachment_refresh";
    case MessageHistoryRequestOwner::reply_refresh:
        return "reply_refresh";
    case MessageHistoryRequestOwner::media_url_refresh:
        return "media_url_refresh";
    case MessageHistoryRequestOwner::older:
        return "older_history";
    case MessageHistoryRequestOwner::friends:
        return "friends";
    case MessageHistoryRequestOwner::read_state_ack:
        return "read_state_ack";
    case MessageHistoryRequestOwner::scoped_read:
        return "scoped_read";
    case MessageHistoryRequestOwner::message_search:
        return "message_search";
    case MessageHistoryRequestOwner::self_status:
        return "self_status";
    case MessageHistoryRequestOwner::media_catalog:
        return "media_catalog";
    case MessageHistoryRequestOwner::gif_favorites:
        return "gif_favorites";
    case MessageHistoryRequestOwner::emoji_preferences:
        return "emoji_preferences";
    case MessageHistoryRequestOwner::channel_overview:
        return "channel_overview";
    case MessageHistoryRequestOwner::pinned_messages:
        return "pinned_messages";
    case MessageHistoryRequestOwner::thread_browser:
        return "thread_browser";
    case MessageHistoryRequestOwner::thread_creation:
        return "thread_creation";
    case MessageHistoryRequestOwner::forum_post:
        return "forum_post";
    case MessageHistoryRequestOwner::poll:
        return "poll";
    case MessageHistoryRequestOwner::member_moderation:
        return "member_moderation";
    case MessageHistoryRequestOwner::guild_invite:
        return "guild_invite";
    case MessageHistoryRequestOwner::private_channel:
        return "private_channel";
    case MessageHistoryRequestOwner::search_navigation:
        return "search_navigation";
    case MessageHistoryRequestOwner::thread_actions:
        return "thread_actions";
    case MessageHistoryRequestOwner::channel_prefetch:
        return "channel_prefetch";
    case MessageHistoryRequestOwner::guild_notification_settings:
        return "guild_notification_settings";
    case MessageHistoryRequestOwner::custom_status:
        return "custom_status";
    case MessageHistoryRequestOwner::inbox_mentions:
        return "inbox_mentions";
    case MessageHistoryRequestOwner::stream_preview:
        return "stream_preview";
    case MessageHistoryRequestOwner::stage:
        return "stage";
    case MessageHistoryRequestOwner::message_components:
        return "message_components";
    case MessageHistoryRequestOwner::invite_cards:
        return "invite_cards";
    case MessageHistoryRequestOwner::activity_sharing:
        return "activity_sharing";
    case MessageHistoryRequestOwner::soundboard:
        return "soundboard";
    case MessageHistoryRequestOwner::count:
        break;
    }
    return "unknown";
}
} // namespace maicord::app_detail
