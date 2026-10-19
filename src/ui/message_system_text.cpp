#include "maicord/ui/message_system_text.hpp"

#include "maicord/core/decimal.hpp"

namespace maicord {
namespace {

// Placeholders: {a} author, {t} target, {c} content, {g} server, {d} call
// duration, {q} poll question, {w} winning answer, {v} vote summary,
// [..] the message action span, <..> the pinned-messages action span.
struct Writer {
    Slice<char> output;
    u32 used;
    MessageSystemText* result;
    bool overflow;

    void raw(Utf8View text) noexcept {
        if (text.bytes > output.count - used) {
            overflow = true;
            return;
        }
        for (u32 i = 0; i < text.bytes; ++i)
            output.data[used + i] = text.data[i];
        used += text.bytes;
    }
    void span(Utf8View text, MessageSystemSpanKind kind) noexcept {
        const u32 begin = used;
        raw(text);
        if (!overflow && text.bytes && result->span_count < kMessageSystemSpanMaximum)
            result->spans[result->span_count++] = {begin, text.bytes, kind};
    }
};

Utf8View view(const char* text) noexcept {
    u32 bytes = 0;
    while (text[bytes])
        ++bytes;
    return {text, bytes};
}

// Original welcome lines, picked by message snowflake time.
constexpr const char* kJoinLines[kMessageSystemJoinLineCount] = {
    "{a} just arrived. Say hi!",
    "Everyone, welcome {a}!",
    "{a} hopped into the server.",
    "{a} wandered in. Make yourself at home.",
    "{a} is here. Now the party can start.",
    "Look who's here: {a}!",
    "{a} just landed.",
    "Make some room, {a} has joined.",
    "{a} found their way in. Welcome!",
    "Good to see you, {a}.",
    "{a} pulled up a chair.",
    "{a} joined the crew.",
    "Glad you made it, {a}!",
};

struct Pattern {
    const char* text;
    UiIcon icon;
    MessageSystemTone tone;
};

Pattern pattern(const MessageSystemTextInput& input, u32* tier) noexcept {
    using T = MessageSystemTone;
    switch (input.type) {
    case message_type_recipient_add:
        return {"{a} added {t} to the group.", UiIcon::arrow_right, T::positive};
    case message_type_recipient_remove:
        return input.same_user || !input.target.bytes
                   ? Pattern{"{a} left the group.", UiIcon::arrow_left, T::negative}
                   : Pattern{"{a} removed {t} from the group.", UiIcon::arrow_left, T::negative};
    case message_type_call:
        if (input.call_present && !input.call_participated && input.call_ended)
            return {"You missed a call from {a} that lasted {d}.", UiIcon::phone_off, T::negative};
        if (input.call_present && input.call_ended)
            return {"{a} started a call that lasted {d}.", UiIcon::phone, T::positive};
        return {"{a} started a call.", UiIcon::phone, T::positive};
    case message_type_channel_name_change:
        return input.forum_post ? Pattern{"{a} changed the post title: {c}", UiIcon::edit, T::neutral}
                                : Pattern{"{a} changed the channel name: {c}", UiIcon::edit, T::neutral};
    case message_type_channel_icon_change:
        return {"{a} changed the group icon.", UiIcon::edit, T::neutral};
    case message_type_channel_pinned_message:
        return {"{a} pinned [a message] to this channel. See all <pinned messages>.", UiIcon::pin, T::neutral};
    case message_type_user_join:
        return {kJoinLines[message_system_join_line_index(input.message_id)], UiIcon::arrow_right, T::positive};
    case message_type_guild_boost: {
        u32 count = 0;
        for (u32 i = 0; i < input.content.bytes && i < 4; ++i) {
            const char c = input.content.data[i];
            if (c < '0' || c > '9')
                break;
            count = count * 10u + static_cast<u32>(c - '0');
        }
        *tier = count;
        return count > 1 ? Pattern{"{a} just boosted the server {n} times!", UiIcon::boost, T::boost}
                         : Pattern{"{a} just boosted the server!", UiIcon::boost, T::boost};
    }
    case message_type_guild_boost_tier_1:
    case message_type_guild_boost_tier_2:
    case message_type_guild_boost_tier_3:
        *tier = static_cast<u32>(input.type - message_type_guild_boost_tier_1 + 1u);
        return input.guild.bytes
                   ? Pattern{"{a} just boosted the server! {g} reached Level {l}!", UiIcon::boost, T::boost}
                   : Pattern{"{a} just boosted the server! The server reached Level {l}!", UiIcon::boost, T::boost};
    case message_type_channel_follow_add:
        return {"{a} added {c} to this channel. Its most important updates will show up here.", UiIcon::webhook,
                T::neutral};
    case message_type_guild_stream:
        return {"{a} is live now.", UiIcon::screen_share, T::accent};
    case message_type_discovery_disqualified:
        return {"This server was removed from Server Discovery because it no longer meets the requirements.",
                UiIcon::circle_x, T::negative};
    case message_type_discovery_requalified:
        return {"This server meets the Server Discovery requirements again and was relisted.",
                UiIcon::circle_check, T::positive};
    case message_type_discovery_grace_initial:
        return {"This server missed the Server Discovery activity requirements for a week. Keep it active to "
                "stay listed.",
                UiIcon::warning, T::negative};
    case message_type_discovery_grace_final:
        return {"This server missed the Server Discovery activity requirements for three weeks in a row and "
                "will be removed soon.",
                UiIcon::warning, T::negative};
    case message_type_thread_created:
        return {"{a} started a thread: [{c}]", UiIcon::thread, T::neutral};
    case message_type_guild_invite_reminder:
        return {"Wondering who to invite? Start with anyone who can help build the server.", UiIcon::user_plus,
                T::neutral};
    case message_type_auto_moderation_action:
        return {"AutoMod flagged a message from {a}.", UiIcon::shield_alert, T::negative};
    case message_type_role_subscription_purchase:
        return {"{a} subscribed to a server tier.", UiIcon::crown, T::boost};
    case message_type_stage_start:
        return {"{a} started {c}", UiIcon::podcast, T::positive};
    case message_type_stage_end:
        return {"{a} ended {c}", UiIcon::podcast, T::neutral};
    case message_type_stage_speaker:
        return {"{a} is now a speaker.", UiIcon::mic, T::positive};
    case message_type_stage_raise_hand:
        return {"{a} asked to speak.", UiIcon::hand, T::neutral};
    case message_type_stage_topic:
        return {"{a} changed the Stage topic: {c}", UiIcon::edit, T::neutral};
    case message_type_guild_application_premium:
        return {"{a} upgraded an app to premium for this server.", UiIcon::bot, T::boost};
    case message_type_private_integration_added:
        return {"{a} added an app to this group.", UiIcon::bot, T::positive};
    case message_type_private_integration_removed:
        return {"{a} removed an app from this group.", UiIcon::bot, T::negative};
    case message_type_incident_alert_enabled:
        return {"{a} turned on security actions for {g}.", UiIcon::shield_alert, T::negative};
    case message_type_incident_alert_disabled:
        return {"{a} turned off security actions for {g}.", UiIcon::shield_check, T::neutral};
    case message_type_incident_report_raid:
        return {"{a} reported a raid in {g}.", UiIcon::shield_alert, T::negative};
    case message_type_incident_report_false_alarm:
        return {"{a} reported a false alarm in {g}.", UiIcon::shield_check, T::neutral};
    case message_type_purchase_notification:
        return {"{a} made a purchase in this server.", UiIcon::tag, T::boost};
    case message_type_poll_result:
        // The winner shows in the result card below the line.
        return {"{a}'s poll [{q}] has closed.", UiIcon::poll, T::accent};
    case message_type_join_request_accepted:
        return {"{a}'s request to join was accepted.", UiIcon::user_check, T::positive};
    case message_type_join_request_rejected:
        return {"{a}'s request to join was declined.", UiIcon::user_x, T::negative};
    case message_type_join_request_withdrawn:
        return {"{a} withdrew their request to join.", UiIcon::user_minus, T::neutral};
    case message_type_hd_streaming_upgraded:
        return {"{a} upgraded this stream to high quality.", UiIcon::screen_share, T::boost};
    case message_type_chat_wallpaper_set:
        return {"{a} set a chat wallpaper.", UiIcon::image, T::neutral};
    case message_type_chat_wallpaper_removed:
        return {"{a} removed the chat wallpaper.", UiIcon::image_off, T::neutral};
    case message_type_report_deleted_message:
        return {"{a} deleted a reported message.", UiIcon::trash, T::neutral};
    case message_type_report_timeout_user:
        return {"{a} timed out a reported member.", UiIcon::timer, T::neutral};
    case message_type_report_kick_user:
        return {"{a} kicked a reported member.", UiIcon::user_x, T::neutral};
    case message_type_report_ban_user:
        return {"{a} banned a reported member.", UiIcon::ban, T::negative};
    case message_type_report_closed:
        return {"{a} closed a report.", UiIcon::circle_check, T::neutral};
    case message_type_emoji_added:
        return {"{a} added a new emoji.", UiIcon::emoji, T::positive};
    case message_type_voice_session:
        return {"{a} started a voice session.", UiIcon::phone, T::positive};
    case message_type_friend_request_accepted:
        return {"{a} accepted your friend request.", UiIcon::user_check, T::positive};
    default:
        return {input.content.bytes ? "{c}" : "System message", UiIcon::info, T::neutral};
    }
}

} // namespace

u32 message_system_join_line_index(u64 message_id) noexcept {
    // Snowflake: milliseconds since 2015 in the high 42 bits.
    return static_cast<u32>((message_id >> 22u) % kMessageSystemJoinLineCount);
}

u32 message_system_duration(u64 milliseconds, Slice<char> output) noexcept {
    const u64 minutes = milliseconds / 60'000u;
    const u64 hours = minutes / 60u;
    const u64 days = hours / 24u;
    char text[32];
    u32 bytes = 0;
    const auto append = [&](const char* value) noexcept {
        for (u32 i = 0; value[i] && bytes < sizeof(text); ++i)
            text[bytes++] = value[i];
    };
    const auto count = [&](u64 value, const char* unit) noexcept {
        bytes += decimal_write(text + bytes, value > 999'999u ? 999'999u : value);
        append(unit);
    };
    if (minutes < 1)
        append("a few seconds");
    else if (minutes == 1)
        append("a minute");
    else if (hours < 1)
        count(minutes, " minutes");
    else if (hours == 1)
        append("an hour");
    else if (days < 1)
        count(hours, " hours");
    else if (days == 1)
        append("a day");
    else
        count(days, " days");
    if (bytes > output.count)
        return 0;
    for (u32 i = 0; i < bytes; ++i)
        output.data[i] = text[i];
    return bytes;
}

bool message_system_text_build(const MessageSystemTextInput& input, Slice<char> output,
                               MessageSystemText* result) noexcept {
    if (!result || !output.data || output.count == 0)
        return false;
    *result = MessageSystemText{};
    u32 tier = 0;
    const Pattern selected = pattern(input, &tier);
    result->icon = selected.icon;
    result->tone = selected.tone;
    Writer writer{output, 0, result, false};
    const Utf8View author = input.author.bytes ? input.author : utf8_literal("Someone");
    const Utf8View target = input.target.bytes ? input.target : utf8_literal("someone");
    const Utf8View guild = input.guild.bytes ? input.guild : utf8_literal("this server");
    const Utf8View text = view(selected.text);
    // Action span state: [..] jumps to the referenced message or thread,
    // <..> opens the pinned messages.
    u32 span_begin = 0;
    MessageSystemSpanKind span_kind{};
    bool in_span = false;
    for (u32 i = 0; i < text.bytes && !writer.overflow; ++i) {
        const char c = text.data[i];
        if (c == '{' && i + 2 < text.bytes && text.data[i + 2] == '}') {
            const char key = text.data[i + 1];
            i += 2;
            char number[16];
            switch (key) {
            case 'a':
                writer.span(author, MessageSystemSpanKind::author);
                break;
            case 't':
                writer.span(target, MessageSystemSpanKind::target);
                break;
            case 'c':
                if (in_span)
                    writer.raw(input.content);
                else
                    writer.span(input.content, MessageSystemSpanKind::emphasis);
                break;
            case 'g':
                writer.span(guild, MessageSystemSpanKind::emphasis);
                break;
            case 'q':
                writer.raw(input.poll_question.bytes ? input.poll_question : utf8_literal("Poll"));
                break;
            case 'd': {
                const u64 duration = input.call_ended_ms > input.timestamp_ms && input.timestamp_ms
                                         ? input.call_ended_ms - input.timestamp_ms
                                         : 0u;
                char storage[32];
                const u32 bytes = message_system_duration(duration, {storage, sizeof(storage)});
                writer.raw({storage, bytes});
                break;
            }
            case 'n':
            case 'l': {
                const u32 bytes = decimal_write(number, tier);
                writer.span({number, bytes}, MessageSystemSpanKind::emphasis);
                break;
            }
            default:
                break;
            }
            continue;
        }
        if ((c == '[' || c == '<') && !in_span) {
            in_span = true;
            span_begin = writer.used;
            span_kind = c == '<' ? MessageSystemSpanKind::pins
                        : input.type == message_type_thread_created ? MessageSystemSpanKind::thread
                                                                     : MessageSystemSpanKind::message;
            continue;
        }
        if ((c == ']' || c == '>') && in_span) {
            in_span = false;
            if (writer.used > span_begin && result->span_count < kMessageSystemSpanMaximum)
                result->spans[result->span_count++] = {span_begin, writer.used - span_begin, span_kind};
            continue;
        }
        writer.raw({&text.data[i], 1});
    }
    if (writer.overflow)
        return false;
    result->text = {output.data, writer.used};
    return true;
}

} // namespace maicord
