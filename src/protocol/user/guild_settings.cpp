#include "maicord/protocol/user/guild_settings.hpp"
#include "maicord/protocol/guild/admin.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "../encoding/json_helpers.hpp"

namespace maicord {
namespace {
using protocol_detail::JsonBody;
constexpr u16 all_fields = user_guild_settings_muted | user_guild_settings_level |
                           user_guild_settings_suppress_everyone | user_guild_settings_suppress_roles |
                           user_guild_settings_hide_muted | user_guild_settings_flags;
bool window_valid(i32 window) noexcept {
    if (!window || window == -1)
        return true;
    for (const i32 known : kUserGuildMuteWindows)
        if (known == window)
            return true;
    return false;
}
// Fields are written comma-separated in a fixed order inside the caller's braces.
void partial(JsonBody& b, const UserGuildSettingsPatch& p) noexcept {
    if (p.fields & user_guild_settings_muted) {
        b.member("muted", json_bool(p.muted));
        if (p.muted && p.mute_window_seconds) {
            b.begin_object("mute_config");
            if (p.mute_window_seconds < 0)
                b.member("selected_time_window", "-1");
            else
                b.member("selected_time_window", static_cast<u64>(p.mute_window_seconds));
            b.key("end_time");
            if (p.mute_end_ms) {
                char stamp[32];
                const u32 bytes = guild_admin_format_timestamp(p.mute_end_ms / 1000, {stamp, sizeof(stamp)});
                b.ok = b.ok && bytes;
                b.put(json_text({stamp, bytes}));
            } else
                b.put("null");
            b.end_object();
        }
    }
    b.member_if(p.fields & user_guild_settings_level, "message_notifications", static_cast<u64>(p.level));
    b.member_if(p.fields & user_guild_settings_suppress_everyone, "suppress_everyone", json_bool(p.suppress_everyone));
    b.member_if(p.fields & user_guild_settings_suppress_roles, "suppress_roles", json_bool(p.suppress_roles));
    b.member_if(p.fields & user_guild_settings_hide_muted, "hide_muted_channels", json_bool(p.hide_muted_channels));
    b.member_if(p.fields & user_guild_settings_flags, "flags", p.flags);
}
void settings_object(JsonBody& b, const UserGuildSettingsPatch& p) noexcept {
    b.begin_object();
    if (p.channel_id) {
        b.begin_object("channel_overrides");
        b.next();
        b.put(snowflake_text(p.channel_id), ':');
        b.begin_object();
        partial(b, p);
        b.end_object();
        b.end_object();
    } else
        partial(b, p);
    b.end_object();
}
} // namespace

bool user_guild_settings_patch_valid(const UserGuildSettingsPatch& p) noexcept {
    if (!p.fields || (p.fields & ~all_fields) || static_cast<u8>(p.level) > static_cast<u8>(MessageNotificationLevel::inherit) ||
        !window_valid(p.mute_window_seconds))
        return false;
    // Direct messages have only per-conversation overrides.
    if (!p.guild_id && !p.channel_id)
        return false;
    if (p.channel_id && (p.fields & ~kUserGuildSettingsChannelFields))
        return false;
    // Inherit exists only for channel overrides.
    if ((p.fields & user_guild_settings_level) && !p.channel_id && p.level == MessageNotificationLevel::inherit)
        return false;
    if ((p.fields & user_guild_settings_muted) && (!p.muted || p.mute_window_seconds <= 0) && p.mute_end_ms)
        return false;
    return !(p.mute_window_seconds > 0 && !p.mute_end_ms);
}

MessageNotificationSetting user_guild_settings_project(MessageNotificationSetting s,
                                                       const UserGuildSettingsPatch& p) noexcept {
    if (p.fields & user_guild_settings_muted) {
        s.muted = p.muted;
        s.fields |= notification_muted;
        if (!p.muted || p.mute_window_seconds) {
            s.mute_end_ms = p.muted ? p.mute_end_ms : 0;
            s.fields |= notification_mute_end;
        }
    }
    if (p.fields & user_guild_settings_level) {
        s.level = p.level;
        s.fields |= notification_level;
    }
    if (p.fields & user_guild_settings_suppress_everyone) {
        s.suppress_everyone = p.suppress_everyone;
        s.fields |= notification_suppress_everyone;
    }
    if (p.fields & user_guild_settings_suppress_roles) {
        s.suppress_roles = p.suppress_roles;
        s.fields |= notification_suppress_roles;
    }
    if (p.fields & user_guild_settings_hide_muted) {
        s.hide_muted_channels = p.hide_muted_channels;
        s.fields |= notification_hide_muted;
    }
    if (p.fields & user_guild_settings_flags) {
        s.flags = p.flags;
        s.fields |= notification_flags;
    }
    return s;
}

UserGuildSettingsWriteResult user_guild_settings_write(const UserGuildSettingsPatch& p, Slice<u8> target,
                                                       Slice<u8> body) noexcept {
    if (!user_guild_settings_patch_valid(p))
        return {};
    ProtocolWriter t{target, 0};
    JsonBody b{body};
    bool ok = protocol_write(&t, "/api/v10/users/@me/guilds/");
    if (p.guild_id) {
        ok = ok && protocol_write(&t, "settings");
        b.begin_object();
        b.begin_object("guilds");
        b.next();
        b.put(snowflake_text(p.guild_id), ':');
        settings_object(b, p);
        b.end_object();
        b.end_object();
    } else {
        ok = ok && protocol_write(&t, "@me/settings");
        settings_object(b, p);
    }
    return ok && b.ok ? UserGuildSettingsWriteResult{t.used, b.writer.used, true} : UserGuildSettingsWriteResult{};
}

bool mentions_query_valid(const MentionsQuery& q) noexcept {
    return q.limit && q.limit <= 100;
}

u32 write_mentions_target(const MentionsQuery& q, Slice<u8> output) noexcept {
    if (!mentions_query_valid(q))
        return 0;
    ProtocolWriter w{output, 0};
    bool ok = protocol_write(&w, "/api/v10/users/@me/mentions?limit=", q.limit, "&roles=", json_bool(q.roles),
                             "&everyone=", json_bool(q.everyone));
    if (q.guild_id)
        ok = ok && protocol_write(&w, "&guild_id=", q.guild_id);
    if (q.before)
        ok = ok && protocol_write(&w, "&before=", q.before);
    return ok ? w.used : 0;
}

} // namespace maicord
