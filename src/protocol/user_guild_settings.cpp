#include "maicord/protocol/user_guild_settings.hpp"
#include "maicord/protocol/guild_admin.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {
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
bool boolean(ProtocolWriter* w, bool value) noexcept {
    return value ? protocol_writer_append_literal(w, "true") : protocol_writer_append_literal(w, "false");
}
// Fields are written comma-separated in a fixed order inside the caller's braces.
bool partial(ProtocolWriter* w, const UserGuildSettingsPatch& p) noexcept {
    bool first = true;
    const auto key = [&](const char* name) noexcept {
        const bool ok = (first || protocol_writer_append_byte(w, ',')) && protocol_writer_append_byte(w, '"') &&
                        protocol_writer_append_c_string(w, name) && protocol_writer_append_literal(w, "\":");
        first = false;
        return ok;
    };
    if (p.fields & user_guild_settings_muted) {
        if (!key("muted") || !boolean(w, p.muted))
            return false;
        if (p.muted && p.mute_window_seconds) {
            if (!key("mute_config") || !protocol_writer_append_literal(w, "{\"selected_time_window\":"))
                return false;
            if (p.mute_window_seconds < 0 ? !protocol_writer_append_literal(w, "-1")
                                          : !protocol_writer_append_u64(w, static_cast<u64>(p.mute_window_seconds)))
                return false;
            if (!protocol_writer_append_literal(w, ",\"end_time\":"))
                return false;
            if (p.mute_end_ms) {
                char stamp[32];
                const u32 bytes = guild_admin_format_timestamp(p.mute_end_ms / 1000, {stamp, sizeof(stamp)});
                if (!bytes || !protocol_writer_append_json_string(w, {stamp, bytes}))
                    return false;
            } else if (!protocol_writer_append_literal(w, "null"))
                return false;
            if (!protocol_writer_append_byte(w, '}'))
                return false;
        }
    }
    if ((p.fields & user_guild_settings_level) &&
        (!key("message_notifications") || !protocol_writer_append_u64(w, static_cast<u64>(p.level))))
        return false;
    if ((p.fields & user_guild_settings_suppress_everyone) &&
        (!key("suppress_everyone") || !boolean(w, p.suppress_everyone)))
        return false;
    if ((p.fields & user_guild_settings_suppress_roles) && (!key("suppress_roles") || !boolean(w, p.suppress_roles)))
        return false;
    if ((p.fields & user_guild_settings_hide_muted) &&
        (!key("hide_muted_channels") || !boolean(w, p.hide_muted_channels)))
        return false;
    if ((p.fields & user_guild_settings_flags) && (!key("flags") || !protocol_writer_append_u64(w, p.flags)))
        return false;
    return true;
}
bool settings_object(ProtocolWriter* w, const UserGuildSettingsPatch& p) noexcept {
    if (!protocol_writer_append_byte(w, '{'))
        return false;
    if (p.channel_id) {
        if (!protocol_writer_append_literal(w, "\"channel_overrides\":{\"") || !protocol_writer_append_u64(w, p.channel_id) ||
            !protocol_writer_append_literal(w, "\":{") || !partial(w, p) || !protocol_writer_append_literal(w, "}}"))
            return false;
    } else if (!partial(w, p))
        return false;
    return protocol_writer_append_byte(w, '}');
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
    ProtocolWriter t{target, 0}, b{body, 0};
    bool ok = protocol_writer_append_literal(&t, "/api/v10/users/@me/guilds/");
    if (p.guild_id) {
        ok = ok && protocol_writer_append_literal(&t, "settings") &&
             protocol_writer_append_literal(&b, "{\"guilds\":{\"") && protocol_writer_append_u64(&b, p.guild_id) &&
             protocol_writer_append_literal(&b, "\":") && settings_object(&b, p) &&
             protocol_writer_append_literal(&b, "}}");
    } else {
        ok = ok && protocol_writer_append_literal(&t, "@me/settings") && settings_object(&b, p);
    }
    return ok ? UserGuildSettingsWriteResult{t.used, b.used, true} : UserGuildSettingsWriteResult{};
}

bool mentions_query_valid(const MentionsQuery& q) noexcept {
    return q.limit && q.limit <= 100;
}

u32 write_mentions_target(const MentionsQuery& q, Slice<u8> output) noexcept {
    if (!mentions_query_valid(q))
        return 0;
    ProtocolWriter w{output, 0};
    bool ok = protocol_writer_append_literal(&w, "/api/v10/users/@me/mentions?limit=") &&
              protocol_writer_append_u64(&w, q.limit) && protocol_writer_append_literal(&w, "&roles=") &&
              boolean(&w, q.roles) && protocol_writer_append_literal(&w, "&everyone=") && boolean(&w, q.everyone);
    if (q.guild_id)
        ok = ok && protocol_writer_append_literal(&w, "&guild_id=") && protocol_writer_append_u64(&w, q.guild_id);
    if (q.before)
        ok = ok && protocol_writer_append_literal(&w, "&before=") && protocol_writer_append_u64(&w, q.before);
    return ok ? w.used : 0;
}

} // namespace maicord
