#include "test_output.hpp"
#include "maicord/model/notification_sound_admission.hpp"

// Rule table: every combination of focus, selection, mention, guild/channel/
// category/thread levels, mutes, presence, silent flag, author and device
// preferences, against an independent model of the official client's rules
// (NotificationStore MESSAGE_CREATE, NotificationUtils should-notify and the
// read-state mention count). Synthetic ids only.
namespace maicord {
namespace notification_rules_probe {
using L = MessageNotificationLevel;
enum Kind : u8 { text, thread, direct };
enum Mute : u8 { unmuted, guild_mute, channel_mute, category_mute, thread_mute, expired_mute, mute_count };
enum Mention : u8 { no_mention, user_mention, everyone_mention, role_mention, mention_count };
enum Status : u8 { online, dnd, quiet, invisible, status_count };
enum Author : u8 { other, self, blocked, author_count };
enum ThreadFlags : u8 { not_joined, joined_inherit, joined_all, joined_mentions, joined_none, thread_flag_count };
constexpr u64 kSelf = 1, kOther = 2, kGuild = 10, kText = 20, kCategory = 22, kThread = 30, kDirect = 40, kRole = 90;
constexpr u64 kNow = 1000;

struct Settings {
    Kind kind;
    L guild, channel, category;
    Mute mute;
    ThreadFlags flags;
    bool suppress;
};
struct Case {
    Mention mention;
    Status status;
    Author author;
    bool focused, selected, silent, desktop, selected_sounds, sounds, flash;
};
struct Expected {
    MessageReadBadge badge;
    bool selected_sound, popup, flash;
    NotificationMessageSoundRoute sound;
};

// The official client's model, written from its source, not from MaiCord's.
struct Official {
    const Settings& s;
    bool guild_muted() const noexcept { return s.mute == guild_mute; }
    bool own_override_muted() const noexcept { return s.mute == channel_mute; }
    bool category_muted() const noexcept { return s.mute == category_mute; }
    // resolvedMessageNotifications: channel override, parent override, guild.
    L resolved_text() const noexcept {
        return s.channel != L::inherit ? s.channel : s.category != L::inherit ? s.category : s.guild;
    }
    L resolved() const noexcept {
        if (s.kind == direct) return L::all;
        if (s.kind == thread) return s.channel != L::inherit ? s.channel : s.guild; // thread -> parent -> guild
        return resolved_text();
    }
    // isGuildOrCategoryOrChannelMuted for the channel itself: guild, its parent, itself.
    bool scope_muted() const noexcept {
        if (s.kind == direct) return guild_muted() || own_override_muted();
        if (s.kind == thread) return guild_muted() || own_override_muted(); // parent override is the text channel
        return guild_muted() || own_override_muted() || category_muted();
    }
    bool allow_no_messages() const noexcept { return scope_muted() || resolved() == L::none; }
    bool allow_all_messages() const noexcept { return !scope_muted() && resolved() == L::all; }
    static bool mentioned(const Case& c, bool suppress_everyone, bool suppress_roles) noexcept {
        return c.mention == user_mention || (c.mention == everyone_mention && !suppress_everyone) ||
               (c.mention == role_mention && !suppress_roles);
    }
    bool user_gate(const Case& c) const noexcept {
        return c.author == other && c.status != dnd && c.status != quiet && !allow_no_messages();
    }
    // Thread level: member flags, else the parent's resolved level unless the parent is muted.
    L thread_level() const noexcept {
        switch (s.flags) {
        case not_joined: return L::none;
        case joined_all: return L::all;
        case joined_mentions: return L::mentions;
        case joined_none: return L::none;
        default: break;
        }
        if (guild_muted() || own_override_muted() || category_muted()) return L::none;
        return resolved_text();
    }
    bool should_notify(const Case& c, bool include_selected) const noexcept {
        if (c.silent || !user_gate(c) || (!include_selected && c.selected)) return false;
        if (s.kind == thread) {
            if (s.mute == thread_mute) return false;
            const L level = thread_level();
            return level == L::all || (level == L::mentions && mentioned(c, false, false));
        }
        if (allow_all_messages()) return true;
        return s.kind != direct && mentioned(c, s.suppress, s.suppress);
    }
    bool selected_check(const Case& c) const noexcept {
        return c.selected && c.author == other && c.status != dnd && c.status != quiet && !allow_no_messages();
    }
    MessageReadBadge badge(const Case& c) const noexcept {
        if (c.author != other) return MessageReadBadge::none;
        if (s.kind == direct) return !scope_muted() || c.mention == user_mention ? MessageReadBadge::mention : MessageReadBadge::none;
        return mentioned(c, s.suppress, s.suppress) ? MessageReadBadge::mention : MessageReadBadge::none;
    }
    Expected expect(const Case& c) const noexcept {
        Expected e{badge(c), false, false, false, NotificationMessageSoundRoute::none};
        const bool p = should_notify(c, !c.focused);
        const bool t = c.selected_sounds && selected_check(c);
        if (!p && !t) return e;
        if (t) {
            e.selected_sound = c.sounds;
            if (!c.focused) return e;
        }
        if (!p) return e;
        if (!c.desktop) {
            e.sound = c.sounds ? NotificationMessageSoundRoute::direct : NotificationMessageSoundRoute::none;
            return e;
        }
        e.popup = true;
        e.flash = c.flash;
        e.sound = c.sounds ? NotificationMessageSoundRoute::popup : NotificationMessageSoundRoute::none;
        return e;
    }
};

struct Store {
    MessageNotificationGuildSetting guilds[2];
    MessageNotificationOverride overrides[4];
    MessageNotificationThreadSetting threads[2];
    MessageNotificationStore store;
    u64 version;
};
Store g_store{};

MessageNotificationSetting level_row(L level) noexcept {
    MessageNotificationSetting row{};
    row.fields = notification_level;
    row.level = level;
    return row;
}
bool apply(const Settings& s) noexcept {
    auto& b = g_store;
    if (!message_notifications_initialize(&b.store, {b.guilds, 2}, {b.overrides, 4}, {b.threads, 2}))
        return false;
    MessageNotificationGuildSetting guilds[2]{};
    MessageNotificationOverride overrides[3]{};
    u32 override_count = 0;
    const u64 guild = s.kind == direct ? 0 : kGuild;
    guilds[0].guild_id = guild;
    guilds[0].setting = level_row(s.guild);
    guilds[0].setting.fields |= notification_muted | notification_mute_end | notification_suppress_everyone |
                                notification_suppress_roles;
    guilds[0].setting.muted = s.mute == guild_mute || s.mute == expired_mute;
    guilds[0].setting.mute_end_ms = s.mute == expired_mute ? kNow - 1 : 0;
    guilds[0].setting.suppress_everyone = guilds[0].setting.suppress_roles = s.suppress;
    const u64 own = s.kind == direct ? kDirect : kText;
    MessageNotificationSetting channel = level_row(s.channel);
    channel.fields |= notification_muted;
    channel.muted = s.mute == channel_mute;
    overrides[override_count++] = {guild, own, channel};
    if (s.kind != direct) {
        MessageNotificationSetting category = level_row(s.category);
        category.fields |= notification_muted;
        category.muted = s.mute == category_mute;
        overrides[override_count++] = {guild, kCategory, category};
    }
    if (message_notifications_apply_settings(&b.store, {guilds, 1}, {overrides, override_count}, false,
                                             ++b.version) != MessageNotificationApplyError::none)
        return false;
    if (s.kind == thread && s.flags != not_joined) {
        constexpr u32 flags[thread_flag_count] = {0, 1, 2, 4, 8};
        MessageNotificationThreadSetting member{kGuild, kThread, {}};
        member.setting.fields = notification_flags | notification_muted;
        member.setting.flags = flags[s.flags];
        member.setting.muted = s.mute == thread_mute;
        return message_notifications_upsert_thread(&b.store, member) == MessageNotificationApplyError::none;
    }
    return true;
}

MessageNotificationOutputs decide(const Settings& s, const Case& c) noexcept {
    static const u64 role = kRole;
    MessageNotificationPolicyInput in{};
    in.settings = &g_store.store;
    in.self_id = kSelf;
    in.author_id = c.author == self ? kSelf : kOther;
    in.guild_id = s.kind == direct ? 0 : kGuild;
    in.channel_id = s.kind == direct ? kDirect : s.kind == thread ? kThread : kText;
    in.parent_id = s.kind == direct ? 0 : s.kind == thread ? kText : kCategory;
    in.category_id = s.kind == thread ? kCategory : 0;
    in.channel_type = s.kind == direct ? 1 : s.kind == thread ? 11 : 0;
    in.now_ms = kNow;
    in.mention_roles = {&role, c.mention == role_mention ? 1u : 0u};
    in.self_roles = {&role, 1};
    in.global.fields = notification_status | notification_quiet;
    in.global.status = c.status == dnd ? MessageNotificationStatus::dnd
                       : c.status == invisible ? MessageNotificationStatus::invisible
                                               : MessageNotificationStatus::online;
    in.global.quiet = c.status == quiet;
    in.message_flags = c.silent ? 4096u : 0u;
    in.known_gates = in.required_gates = 0xFFFFu;
    in.gates = c.author == blocked ? notification_gate_blocked : 0u;
    in.guild_default = L::all;
    in.ready = in.live = in.identities_known = in.roles_known = in.type_known = in.guild_default_known = true;
    in.direct_mention = c.mention == user_mention;
    in.everyone_mention = c.mention == everyone_mention;
    in.focused = c.focused;
    in.selected = c.selected;
    in.current_voice_channel_known = true;
    const MessageNotificationDevice device{c.desktop, c.sounds, c.selected_sounds, c.flash, false};
    return message_notification_outputs({&in, true, c.selected, true, c.selected_sounds}, device);
}

struct Tally {
    u32 cases, badge, selected_sound, sound, popup, flash, popups, selected_sounds, reported;
};
void report(Tally& t, const Settings& s, const Case& c, const char* output) noexcept {
    if (t.reported++ >= 8) return;
    test_write_text("notification rule mismatch: ");
    test_write_text(output);
    test_write_metric(" settings=", u64{s.kind} | u64{static_cast<u8>(s.guild)} << 4 |
                                         u64{static_cast<u8>(s.channel)} << 8 | u64{static_cast<u8>(s.category)} << 12 |
                                         u64{s.mute} << 16 | u64{s.flags} << 20 | u64{s.suppress} << 24);
    test_write_metric(" case=", u64{c.mention} | u64{c.status} << 4 | u64{c.author} << 8 | u64{c.focused} << 12 |
                                    u64{c.selected} << 13 | u64{c.silent} << 14 | u64{c.desktop} << 15 |
                                    u64{c.selected_sounds} << 16 | u64{c.sounds} << 17 | u64{c.flash} << 18);
}
// The full pass fixes sounds and flashing on and skips invisible; the extended
// pass varies them too.
void run_cases(Tally& t, const Settings& s, bool extended) noexcept {
    const Official official{s};
    const u8 statuses = extended ? status_count : invisible;
    const u32 variants = extended ? 128 : 32;
    for (u8 m = 0; m < mention_count; ++m) {
        if (s.kind == direct && (m == everyone_mention || m == role_mention)) continue;
        for (u8 st = 0; st < statuses; ++st)
            for (u8 au = 0; au < author_count; ++au)
                for (u32 bits = 0; bits < variants; ++bits) {
                    Case c{static_cast<Mention>(m), static_cast<Status>(st), static_cast<Author>(au),
                           (bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0,
                           (bits & 16) != 0, (bits & 32) == 0, (bits & 64) == 0};
                    const auto e = official.expect(c);
                    const auto o = decide(s, c);
                    ++t.cases;
                    if (o.badge != e.badge) { ++t.badge; report(t, s, c, "badge"); }
                    if (o.selected_sound != e.selected_sound) { ++t.selected_sound; report(t, s, c, "selected sound"); }
                    if (o.sound != e.sound) { ++t.sound; report(t, s, c, "sound"); }
                    if (o.popup != e.popup) { ++t.popup; report(t, s, c, "popup"); }
                    if (o.flash != e.flash) { ++t.flash; report(t, s, c, "flash"); }
                    t.popups += o.popup;
                    t.selected_sounds += o.selected_sound;
                }
    }
}
} // namespace notification_rules_probe

void test_notification_rules(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace notification_rules_probe;
    constexpr L levels[4] = {L::inherit, L::all, L::mentions, L::none};
    constexpr L guild_levels[3] = {L::all, L::mentions, L::none};
    Tally t{};
    u32 settings = 0;
    bool applied = true;
    for (u8 kind = text; kind <= direct; ++kind)
        for (const L guild : guild_levels)
            for (const L channel : levels)
                for (const L category : levels)
                    for (u8 mute = 0; mute < mute_count; ++mute)
                        for (u8 flags = 0; flags < thread_flag_count; ++flags)
                            for (u8 suppress = 0; suppress < 2; ++suppress) {
                                // Suppression never applies to threads; their badge uses the text rows.
                                if (kind != thread && (flags || mute == thread_mute)) continue;
                                if (kind == thread && suppress) continue;
                                if (kind == direct && (guild != L::all || channel != L::inherit ||
                                                       category != L::inherit || mute == category_mute || suppress))
                                    continue;
                                const Settings s{static_cast<Kind>(kind), guild, channel, category,
                                                 static_cast<Mute>(mute), static_cast<ThreadFlags>(flags), suppress != 0};
                                applied = apply(s) && applied;
                                ++settings;
                                run_cases(t, s, false);
                            }
    const Settings extended[3] = {{text, L::all, L::inherit, L::inherit, unmuted, not_joined, false},
                                  {thread, L::mentions, L::inherit, L::inherit, unmuted, joined_inherit, false},
                                  {direct, L::all, L::inherit, L::inherit, unmuted, not_joined, false}};
    for (const auto& s : extended) {
        applied = apply(s) && applied;
        run_cases(t, s, true);
    }
    check(applied, "notification rules: every settings row is admitted");
    test_write_metric("notification rules: settings=", settings);
    test_write_metric("notification rules: cases=", t.cases);
    check(settings == 480 + 1440 + 4 && t.cases == (480 + 1440) * 1152 + 4 * 576 + 2 * 6144 + 3072,
          "notification rules: the table covers every settings row and case");
    check(t.popups && t.selected_sounds, "notification rules: the table reaches popups and selected sounds");
    check(!t.badge, "notification rules: badge matches the official client");
    check(!t.selected_sound, "notification rules: selected-channel sound matches the official client");
    check(!t.sound, "notification rules: message sound matches the official client");
    check(!t.popup, "notification rules: popup matches the official client");
    check(!t.flash, "notification rules: taskbar flash matches the official client");
}
} // namespace maicord
