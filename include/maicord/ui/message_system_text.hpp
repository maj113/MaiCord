#pragma once

#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/ui/icon.hpp"

namespace maicord {

// Message type values with dedicated system presentation.
enum MessageSystemType : u16 {
    message_type_default = 0,
    message_type_recipient_add = 1,
    message_type_recipient_remove = 2,
    message_type_call = 3,
    message_type_channel_name_change = 4,
    message_type_channel_icon_change = 5,
    message_type_channel_pinned_message = 6,
    message_type_user_join = 7,
    message_type_guild_boost = 8,
    message_type_guild_boost_tier_1 = 9,
    message_type_guild_boost_tier_2 = 10,
    message_type_guild_boost_tier_3 = 11,
    message_type_channel_follow_add = 12,
    message_type_guild_stream = 13,
    message_type_discovery_disqualified = 14,
    message_type_discovery_requalified = 15,
    message_type_discovery_grace_initial = 16,
    message_type_discovery_grace_final = 17,
    message_type_thread_created = 18,
    message_type_reply = 19,
    message_type_chat_input_command = 20,
    message_type_thread_starter = 21,
    message_type_guild_invite_reminder = 22,
    message_type_context_menu_command = 23,
    message_type_auto_moderation_action = 24,
    message_type_role_subscription_purchase = 25,
    message_type_interaction_premium_upsell = 26,
    message_type_stage_start = 27,
    message_type_stage_end = 28,
    message_type_stage_speaker = 29,
    message_type_stage_raise_hand = 30,
    message_type_stage_topic = 31,
    message_type_guild_application_premium = 32,
    message_type_private_integration_added = 33,
    message_type_private_integration_removed = 34,
    message_type_premium_referral = 35,
    message_type_incident_alert_enabled = 36,
    message_type_incident_alert_disabled = 37,
    message_type_incident_report_raid = 38,
    message_type_incident_report_false_alarm = 39,
    message_type_deadchat_revive_prompt = 40,
    message_type_custom_gift = 41,
    message_type_gaming_stats_prompt = 42,
    message_type_purchase_notification = 44,
    message_type_voice_hangout_invite = 45,
    message_type_poll_result = 46,
    message_type_changelog = 47,
    message_type_nitro_notification = 48,
    message_type_channel_linked_to_lobby = 49,
    message_type_gifting_prompt = 50,
    message_type_in_game_message_nux = 51,
    message_type_join_request_accepted = 52,
    message_type_join_request_rejected = 53,
    message_type_join_request_withdrawn = 54,
    message_type_hd_streaming_upgraded = 55,
    message_type_chat_wallpaper_set = 56,
    message_type_chat_wallpaper_removed = 57,
    message_type_report_deleted_message = 58,
    message_type_report_timeout_user = 59,
    message_type_report_kick_user = 60,
    message_type_report_ban_user = 61,
    message_type_report_closed = 62,
    message_type_emoji_added = 63,
    message_type_premium_group_invite = 64,
    message_type_voice_session = 65,
    message_type_guild_boost_upsell = 66,
    message_type_friend_request_accepted = 67,
    message_type_media_mention = 68,
    message_type_guild_space = 69,
};

// message_type_is_system (gateway_message_events.hpp) selects these rows.
enum class MessageSystemSpanKind : u8 {
    author,   // opens the author's profile
    target,   // opens the mentioned user's profile
    message,  // jumps to the referenced message
    pins,     // opens the channel's pinned messages
    thread,   // opens the created thread
    emphasis, // bold text without an action
};

enum class MessageSystemTone : u8 { neutral, positive, negative, boost, accent };

struct MessageSystemSpan {
    u32 offset;
    u32 bytes;
    MessageSystemSpanKind kind;
};

inline constexpr u32 kMessageSystemSpanMaximum = 4;
inline constexpr u32 kMessageSystemTextMaximumBytes = 1'024;
inline constexpr u32 kMessageSystemJoinLineCount = 13;

struct MessageSystemTextInput {
    u64 message_id;           // snowflake; selects the welcome line
    u64 call_ended_ms;
    u64 timestamp_ms;         // message time, for call durations
    Utf8View author;          // display name of the author
    Utf8View target;          // recipient add/remove subject (mentions[0])
    Utf8View content;         // channel name, thread name, Stage topic, boost count
    Utf8View guild;           // server name when known
    Utf8View poll_question;   // poll result embed fields
    Utf8View poll_answer;
    u32 poll_votes;
    u32 poll_total_votes;
    u16 type;
    bool same_user;           // target equals author (a member left)
    bool call_present;
    bool call_ended;
    bool call_participated;
    bool forum_post;          // channel name changes rename a post
};

struct MessageSystemText {
    Utf8View text;
    MessageSystemSpan spans[kMessageSystemSpanMaximum];
    u32 span_count;
    UiIcon icon;
    MessageSystemTone tone;
};

// Builds the system line into output (at most kMessageSystemTextMaximumBytes
// are needed). Strings are copied; the result borrows output only.
bool message_system_text_build(const MessageSystemTextInput& input, Slice<char> output,
                               MessageSystemText* result) noexcept;
// "a few seconds", "a minute", "5 minutes", "an hour", "3 hours", "2 days".
u32 message_system_duration(u64 milliseconds, Slice<char> output) noexcept;
u32 message_system_join_line_index(u64 message_id) noexcept;

} // namespace maicord
