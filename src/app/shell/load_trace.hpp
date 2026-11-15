#pragma once
#include "state.hpp"
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
// The log name of each owner, in the order of the enum.
inline const char* rest_owner_name(MessageHistoryRequestOwner owner) noexcept {
    static constexpr char kNames[] =
        "idle\0private_conversation\0attachment_upload\0typing\0message_send\0message_delete\0message_edit\0"
        "message_reaction\0reaction_users\0profile\0commands\0history\0message_jump\0attachment_refresh\0"
        "media_url_refresh\0older_history\0friends\0read_state_ack\0message_search\0self_status\0media_catalog\0"
        "gif_favorites\0channel_overview\0reply_refresh\0pinned_messages\0search_navigation\0thread_actions\0"
        "thread_browser\0thread_creation\0forum_post\0scoped_read\0poll\0member_moderation\0guild_invite\0"
        "emoji_preferences\0channel_prefetch\0unknown\0unknown\0private_channel\0guild_notification_settings\0"
        "custom_status\0inbox_mentions\0stream_preview\0stage\0message_components\0invite_cards\0activity_sharing\0"
        "soundboard\0reaction_tooltip\0dm_profile\0startup_history\0expression_card\0interaction_data\0game_lookup\0"
        "test_mode";
    constexpr u32 kCount = static_cast<u32>(MessageHistoryRequestOwner::count);
    static_assert(c_string_list_entry(kNames, kCount - 1).data + sizeof("test_mode") == kNames + sizeof(kNames));
    return static_cast<u32>(owner) < kCount ? c_string_list_entry(kNames, static_cast<u32>(owner)).data : "unknown";
}
} // namespace maicord::app_detail
