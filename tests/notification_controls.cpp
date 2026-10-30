#include "maicord/protocol/user_guild_settings.hpp"
#include "maicord/protocol/self_status_settings.hpp"
#include "maicord/protocol/gateway_presence.hpp"
#include "maicord/protocol/message_history.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
struct Written {
    char target[kUserGuildSettingsMaximumTargetBytes];
    char body[kUserGuildSettingsMaximumBodyBytes];
    UserGuildSettingsWriteResult result;
};
Written write(const UserGuildSettingsPatch& patch) noexcept {
    Written w{};
    w.result = user_guild_settings_write(patch, {reinterpret_cast<u8*>(w.target), sizeof(w.target)},
                                         {reinterpret_cast<u8*>(w.body), sizeof(w.body)});
    return w;
}
bool equals(const char* data, u32 bytes, Utf8View expected) noexcept {
    return bytes == expected.bytes && bytes_equal(data, expected.data, bytes);
}
bool request(const Written& w, Utf8View target, Utf8View body) noexcept {
    return w.result.ok && equals(w.target, w.result.target_bytes, target) &&
           equals(w.body, w.result.body_bytes, body);
}
// Decodes the base64 settings payload of a settings-proto PATCH body.
u32 patch_proto(const u8* body, u32 bytes, u8* proto, u32 capacity) noexcept {
    constexpr u32 prefix = sizeof("{\"settings\":\"") - 1;
    if (bytes < prefix + 2)
        return 0;
    const auto decoded = base64_decode({reinterpret_cast<const char*>(body) + prefix, bytes - prefix - 2},
                                       {proto, capacity});
    return decoded.ok() ? decoded.bytes : 0;
}

void guild_settings_bodies(Check check) noexcept {
    // 2026-10-06T12:00:00Z plus one hour.
    constexpr u64 now = 1'791'288'000'000ull;
    UserGuildSettingsPatch mute{10, 0, user_guild_settings_mute_end(3600, now), 3600, user_guild_settings_muted};
    mute.muted = true;
    check(request(write(mute), utf8_literal("/api/v10/users/@me/guilds/settings"),
                  utf8_literal("{\"guilds\":{\"10\":{\"muted\":true,\"mute_config\":{\"selected_time_window\":3600,"
                               "\"end_time\":\"2026-10-06T13:00:00+00:00\"}}}}")),
          "guild settings: server mute for one hour goes through the bulk guild route");
    UserGuildSettingsPatch forever{10, 20, 0, -1, user_guild_settings_muted};
    forever.muted = true;
    check(request(write(forever), utf8_literal("/api/v10/users/@me/guilds/settings"),
                  utf8_literal("{\"guilds\":{\"10\":{\"channel_overrides\":{\"20\":{\"muted\":true,\"mute_config\":"
                               "{\"selected_time_window\":-1,\"end_time\":null}}}}}}")),
          "guild settings: channel mute until turned back on carries only that channel");
    UserGuildSettingsPatch dm{0, 30, 0, 0, user_guild_settings_muted | user_guild_settings_level};
    dm.level = MessageNotificationLevel::mentions;
    check(request(write(dm), utf8_literal("/api/v10/users/@me/guilds/@me/settings"),
                  utf8_literal("{\"channel_overrides\":{\"30\":{\"muted\":false,\"message_notifications\":1}}}")),
          "guild settings: direct message override uses the @me route with a plain partial");
    UserGuildSettingsPatch level{10, 0, 0, 0,
                                 user_guild_settings_level | user_guild_settings_suppress_everyone |
                                     user_guild_settings_suppress_roles};
    level.level = MessageNotificationLevel::none;
    level.suppress_everyone = true;
    check(request(write(level), utf8_literal("/api/v10/users/@me/guilds/settings"),
                  utf8_literal("{\"guilds\":{\"10\":{\"message_notifications\":2,\"suppress_everyone\":true,"
                               "\"suppress_roles\":false}}}")),
          "guild settings: level and suppression toggles serialize as numbers and booleans");
    UserGuildSettingsPatch inherit{10, 20, 0, 0, user_guild_settings_level};
    inherit.level = MessageNotificationLevel::inherit;
    check(request(write(inherit), utf8_literal("/api/v10/users/@me/guilds/settings"),
                  utf8_literal("{\"guilds\":{\"10\":{\"channel_overrides\":{\"20\":{\"message_notifications\":3}}}}}")),
          "guild settings: a channel returning to the server default sends 3");
    UserGuildSettingsPatch invalid = inherit;
    invalid.channel_id = 0;
    check(!write(invalid).result.ok, "guild settings: guild level cannot inherit");
    invalid = {0, 0, 0, 0, user_guild_settings_muted};
    check(!write(invalid).result.ok, "guild settings: direct messages need a channel");
    invalid = {10, 20, 0, 0, user_guild_settings_suppress_roles};
    check(!write(invalid).result.ok, "guild settings: suppression is a server field");
    invalid = {10, 0, now, 1234, user_guild_settings_muted, MessageNotificationLevel::all, true};
    check(!write(invalid).result.ok, "guild settings: unknown mute windows are rejected");

    MessageNotificationSetting current{};
    current.level = MessageNotificationLevel::all;
    const auto projected = user_guild_settings_project(current, mute);
    check(projected.muted && projected.mute_end_ms == now + 3'600'000 && (projected.fields & notification_mute_end) &&
              !message_notification_muted(projected, now + 3'600'001) && message_notification_muted(projected, now),
          "guild settings: projected mute expires at its end time");
    UserGuildSettingsPatch unmute{10, 0, 0, 0, user_guild_settings_muted};
    const auto cleared = user_guild_settings_project(projected, unmute);
    check(!cleared.muted && !cleared.mute_end_ms, "guild settings: unmute clears the end time");
}

void mute_queries(Check check) noexcept {
    MessageNotificationGuildSetting guilds[2]{};
    MessageNotificationOverride overrides[3]{};
    MessageNotificationThreadSetting threads[1]{};
    MessageNotificationStore store{};
    check(message_notifications_initialize(&store, {guilds, 2}, {overrides, 3}, {threads, 1}),
          "mute queries: store initializes");
    MessageNotificationGuildSetting entry{10, {5000, 0, notification_muted | notification_mute_end,
                                               MessageNotificationLevel::all, true}};
    MessageNotificationOverride channel{10, 20, {9000, 0, notification_muted | notification_mute_end,
                                                 MessageNotificationLevel::inherit, true}};
    check(message_notifications_apply_settings(&store, {&entry, 1}, {&channel, 1}, true, 0) ==
              MessageNotificationApplyError::none,
          "mute queries: timed guild and channel mutes apply");
    check(message_notifications_guild_muted(store, 10, 4000) && !message_notifications_guild_muted(store, 10, 5001) &&
              message_notifications_override_muted(store, 10, 20, 8000) &&
              !message_notifications_override_muted(store, 10, 21, 8000),
          "mute queries: guild and override mutes end at their expiry");
    check(message_notifications_next_mute_expiry(store, 1000) == 5000 &&
              message_notifications_next_mute_expiry(store, 6000) == 9000 &&
              !message_notifications_next_mute_expiry(store, 9001),
          "mute queries: next expiry timer walks forward through scopes");
}

void custom_status_codec(Check check) noexcept {
    // Status group: status "dnd", show_current_game wrapper, an unknown field 9 and an old custom status.
    const u8 group[] = {0x0a, 5,    0x0a, 3,    'd', 'n', 'd', 0x1a, 2, 8, 1, 0x48, 7,
                        0x12, 4,    0x0a, 2,    'h', 'i'};
    SelfCustomStatus read{};
    check(self_status_settings_read_custom({group, sizeof(group)}, &read) && read.present && read.text_bytes == 2 &&
              read.text[0] == 'h' && self_custom_status_active(read, 1),
          "custom status: field 2 reads from the status group");
    SelfCustomStatus next{};
    next.present = true;
    copy_bytes(next.text, "Out to lunch", 12);
    next.text_bytes = 12;
    copy_bytes(next.emoji_name, "\xF0\x9F\x8D\x94", 4);
    next.emoji_name_bytes = 4;
    next.expires_at_ms = 0x0102030405060708ull;
    next.created_at_ms = 0x1112131415161718ull;
    u8 scratch[sizeof(group) + 704]{}, body[2048]{}, proto[1024]{};
    const auto written = self_status_settings_write_custom_patch({group, sizeof(group)}, &next, {scratch, sizeof(scratch)},
                                                                 {body, sizeof(body)});
    const u32 bytes = written.ok() ? patch_proto(body, written.bytes, proto, sizeof(proto)) : 0;
    // Outer group 11 holds the kept fields, then the new custom status.
    const u8 expected_prefix[] = {0x5a, 0, 0x0a, 5, 0x0a, 3, 'd', 'n', 'd', 0x1a, 2, 8, 1, 0x48, 7, 0x12};
    bool prefix = bytes > sizeof(expected_prefix);
    for (u32 i = 0; prefix && i < sizeof(expected_prefix); ++i)
        prefix = i == 1 || proto[i] == expected_prefix[i];
    SelfCustomStatus round{};
    check(prefix && proto[1] == bytes - 2 &&
              self_status_settings_read_custom({proto + 2, bytes - 2}, &round) &&
              self_custom_status_equal(round, next),
          "custom status: patch keeps status, game and unknown fields and replaces the custom status");
    const auto cleared = self_status_settings_write_custom_patch({group, sizeof(group)}, nullptr,
                                                                 {scratch, sizeof(scratch)}, {body, sizeof(body)});
    const u32 clear_bytes = cleared.ok() ? patch_proto(body, cleared.bytes, proto, sizeof(proto)) : 0;
    check(clear_bytes == 2 + 13 && proto[0] == 0x5a && self_status_settings_read_custom({proto + 2, 13}, &round) &&
              !round.present,
          "custom status: clearing drops only field 2");
    SelfCustomStatus expiring = next;
    expiring.expires_at_ms = 5000;
    check(self_custom_status_active(expiring, 4999) && !self_custom_status_active(expiring, 5000),
          "custom status: expiry hides the status");
    SelfCustomStatus empty{};
    empty.present = true;
    check(!self_custom_status_active(empty, 0), "custom status: an empty status is not shown");
    SelfCustomStatus built{};
    check(self_custom_status_build(utf8_literal(" <a:party:123> "), utf8_literal("  Lunch "),
                                   SelfCustomStatusClear::hour_1, 1000, 0, &built) &&
              built.emoji_id == 123 && built.emoji_name_bytes == 5 && built.text_bytes == 5 &&
              built.expires_at_ms == 3'601'000 && built.created_at_ms == 1000,
          "custom status: dialog builds a trimmed custom emoji status clearing after one hour");
    check(self_custom_status_build(utf8_literal("\xF0\x9F\x98\x80"), utf8_literal("x"), SelfCustomStatusClear::today,
                                   10'000'000, 86'000'000, &built) &&
              built.expires_at_ms == 10'400'000 && built.emoji_name_bytes == 4 && !built.emoji_id,
          "custom status: Today clears at the next local midnight");
    check(self_custom_status_build({}, utf8_literal("x"), SelfCustomStatusClear::never, 1, 0, &built) &&
              !built.expires_at_ms && self_custom_status_build({}, utf8_literal("  "), SelfCustomStatusClear::hours_4,
                                                                1, 0, &built) &&
              !built.present,
          "custom status: Don't clear has no expiry and an empty dialog clears");
    char long_text[kSelfCustomStatusTextCodepoints + 1];
    for (auto& c : long_text)
        c = 'a';
    check(!self_custom_status_build(utf8_literal("<:x:abc>"), {}, SelfCustomStatusClear::never, 1, 0, &built) &&
              !self_custom_status_build({}, {long_text, sizeof(long_text)}, SelfCustomStatusClear::never, 1, 0, &built) &&
              self_custom_status_build({}, {long_text, sizeof(long_text) - 1}, SelfCustomStatusClear::never, 1, 0, &built),
          "custom status: malformed custom emoji and text past 128 characters are rejected");
}

void presence_activity(Check check) noexcept {
    u8 output[512]{};
    const GatewayCustomStatusActivity custom{utf8_literal("Busy \"now\""), utf8_literal("smile"), 42, 9000};
    const auto written = gateway_write_presence_update({0, GatewayUserStatus::dnd, false, false}, &custom, nullptr,
                                                       {output, sizeof(output)});
    constexpr auto expected = utf8_literal(
        "{\"op\":3,\"d\":{\"status\":\"dnd\",\"since\":0,\"activities\":[{\"name\":\"Custom Status\",\"type\":4,"
        "\"state\":\"Busy \\\"now\\\"\",\"timestamps\":{\"end\":9000},\"emoji\":{\"id\":\"42\",\"name\":\"smile\","
        "\"animated\":false}}],\"afk\":false}}");
    check(written.ok() && equals(reinterpret_cast<const char*>(output), written.bytes, expected),
          "presence: custom status publishes as a type 4 activity");
    const auto plain = gateway_write_presence_update({0, GatewayUserStatus::online, false, false}, nullptr, nullptr,
                                                     {output, sizeof(output)});
    check(plain.ok() && equals(reinterpret_cast<const char*>(output), plain.bytes,
                               utf8_literal("{\"op\":3,\"d\":{\"status\":\"online\",\"since\":0,\"activities\":[],"
                                            "\"afk\":false}}")),
          "presence: without a custom status the activity set stays empty");
    check(!gateway_write_presence_update({0, GatewayUserStatus::dnd, false, false}, &custom, nullptr, {output, 120})
               .ok(),
          "presence: truncated output is rejected");
}

void mentions(Check check) noexcept {
    u8 target[kMentionsMaximumTargetBytes]{};
    const u32 first = write_mentions_target({0, 0, kMentionsPageLimit, true, true}, {target, sizeof(target)});
    check(equals(reinterpret_cast<const char*>(target), first,
                 utf8_literal("/api/v10/users/@me/mentions?limit=25&roles=true&everyone=true")),
          "mentions: first page queries every server with roles and @everyone");
    const u32 next = write_mentions_target({300, 10, kMentionsPageLimit, false, true}, {target, sizeof(target)});
    check(equals(reinterpret_cast<const char*>(target), next,
                 utf8_literal("/api/v10/users/@me/mentions?limit=25&roles=false&everyone=true&guild_id=10&before=300")),
          "mentions: later pages filter the server and continue before the oldest mention");
    check(!write_mentions_target({0, 0, 0, true, true}, {target, sizeof(target)}), "mentions: zero limit rejected");
    static u32 page[2048], message[2048];
    static char text[4096];
    static GatewayMessageEvent events[4];
    constexpr auto body = utf8_literal(
        R"([{"id":"500","channel_id":"7","author":{"id":"41","username":"ana"},"content":"<@1> hi","timestamp":"2026-10-06T12:00:00Z"},)"
        R"({"id":"400","channel_id":"8","author":{"id":"42","username":"bo"},"content":"@everyone","timestamp":"2026-10-06T11:00:00Z","mention_everyone":true}])");
    const auto decoded = decode_message_mentions_page(body, {page, 2048}, 32, {text, sizeof(text)},
                                                      {events, 4}, {});
    check(decoded.ok() && decoded.page.count == 2 && events[0].channel_id == 7 && events[1].channel_id == 8 &&
              events[1].id == 400 && !mentions_has_more(decoded.page.count, kMentionsPageLimit),
          "mentions: a page spans channels and a short page ends paging");
    const auto empty = decode_message_mentions_page(utf8_literal("[]"), {page, 2048}, 32,
                                                    {text, sizeof(text)}, {events, 4}, {});
    check(empty.ok() && !empty.page.count, "mentions: an empty page decodes");
    check(!decode_message_mentions_page(utf8_literal("{\"message\":\"x\"}"), {page, 2048}, 32,
                                        {text, sizeof(text)}, {events, 4}, {})
               .ok(),
          "mentions: an error object is not a page");
    check(mentions_has_more(25, 25), "mentions: a full page may continue");
    constexpr auto avatar_body = utf8_literal(
        R"([{"id":"501","channel_id":"7","author":{"id":"43","username":"cy","avatar":"a_0123456789abcdef0123456789abcdef"},"content":"<@1>","timestamp":"2026-10-06T12:00:00Z"},)"
        R"({"id":"502","channel_id":"7","author":{"id":"44","username":"di","avatar":null},"content":"<@1>","timestamp":"2026-10-06T11:00:00Z"}])");
    const auto avatars = decode_message_mentions_page(avatar_body, {page, 2048}, 32,
                                                      {text, sizeof(text)}, {events, 4}, {});
    check(avatars.ok() && avatars.page.count == 2 && events[0].author_id == 43 &&
              utf8_equal(events[0].author_avatar_hash, utf8_literal("a_0123456789abcdef0123456789abcdef")) &&
              events[1].author_id == 44 && !events[1].author_avatar_hash.bytes,
          "mentions: author ids and avatar hashes decode for inbox card avatars");
}
} // namespace

void test_notification_controls(Check check) noexcept {
    guild_settings_bodies(check);
    mute_queries(check);
    custom_status_codec(check);
    presence_activity(check);
    mentions(check);
}
} // namespace maicord
