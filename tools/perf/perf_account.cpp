// MaiCordPerfAccount.dll: a synthetic account module for MaiCordPerfProbe.
//
// Implements the account module ABI with no network access. A large
// deterministic READY is decoded and projected by the production routes
// (gateway_decode_ready + gateway_ready_project) on one worker; message history,
// member lists, member chunks and generated media frames are served from a
// second; typing/presence/message churn arrives on demand. Every REST
// submission gets a response so no client lane stays busy. All module memory
// comes from one reservation so the probe can report it separately.
#include "perf_shared.hpp"
#include "maicord/app/account_module_api.hpp"
#include "maicord/app/user_account_coordinator.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/protocol/gateway_events.hpp"
#include "maicord/protocol/gateway_payload.hpp"
#include "maicord/protocol/gateway_session.hpp"
#include "../../src/app/gateway_ready_projection.hpp"

namespace maicord::perf {
namespace {

// Memory
struct Region {
    u8* base;
    u64 reserved;
    u64 used;
    u64 committed;
};
Region g_region{};
constexpr u64 kRegionBytes = 320ull * 1024 * 1024;

void* region_take(u64 bytes) noexcept {
    bytes = (bytes + 4095) & ~4095ull;
    if (!g_region.base) {
        g_region.base = static_cast<u8*>(VirtualAlloc(nullptr, kRegionBytes, MEM_RESERVE, PAGE_READWRITE));
        g_region.reserved = g_region.base ? kRegionBytes : 0;
    }
    if (!g_region.base || bytes > g_region.reserved - g_region.used)
        return nullptr;
    u8* at = g_region.base + g_region.used;
    if (!VirtualAlloc(at, bytes, MEM_COMMIT, PAGE_READWRITE))
        return nullptr;
    g_region.used += bytes;
    g_region.committed += bytes;
    return at;
}

// JSON text
struct Json {
    ProtocolWriter writer;
    bool ok = true;
    Json(char* data, u32 capacity) noexcept : writer{{reinterpret_cast<u8*>(data), capacity}, 0} {}
    Json& s(const char* text) noexcept {
        ok = ok && protocol_writer_append_c_string(&writer, text);
        return *this;
    }
    Json& n(u64 value) noexcept {
        ok = ok && protocol_writer_append_u64(&writer, value);
        return *this;
    }
    Json& id(u64 value) noexcept { return s("\"").n(value).s("\""); }
    Json& c(u32 index) noexcept { return index ? s(",") : *this; }
    Json& hex(u64 value, u32 digits) noexcept {
        char text[17]{};
        for (u32 i = 0; i < digits && i < 16; ++i)
            text[digits - 1 - i] = "0123456789abcdef"[(value >> (i * 4)) & 15u];
        return s(text);
    }
    u32 used() const noexcept { return writer.used; }
};

// 2026-10-11T06:00:00Z: the newest synthetic message.
constexpr u64 kLatestMessageMs = 1'792'994'400'000ull;
constexpr u64 kSnowflakeEpochMs = 1'420'070'400'000ull;
constexpr u32 kMessagesPerChannel = 4'000;
constexpr u64 kMessageSpacingMs = 47'000;

u64 message_time(u32 index) noexcept {
    return kLatestMessageMs - static_cast<u64>(kMessagesPerChannel - 1 - index) * kMessageSpacingMs;
}
u64 message_id(u64 channel, u32 index) noexcept {
    return ((message_time(index) - kSnowflakeEpochMs) << 22) | ((channel & 0xFFFu) << 10) |
           (index & 0x3FFu);
}
// Index of the newest message at or before `id` in this channel's timeline.
u32 message_index_at(u64 id) noexcept {
    const u64 at = (id >> 22) + kSnowflakeEpochMs;
    if (at >= kLatestMessageMs)
        return kMessagesPerChannel - 1;
    const u64 back = (kLatestMessageMs - at + kMessageSpacingMs - 1) / kMessageSpacingMs;
    return back >= kMessagesPerChannel ? 0 : kMessagesPerChannel - 1 - static_cast<u32>(back);
}

void append_iso(Json& j, u64 unix_ms) noexcept {
    // Civil-from-days (Howard Hinnant), proleptic Gregorian, UTC.
    const i64 days = static_cast<i64>(unix_ms / 86'400'000ull);
    const u64 day_ms = unix_ms % 86'400'000ull;
    const i64 z = days + 719'468;
    const i64 era = z / 146'097;
    const u64 doe = static_cast<u64>(z - era * 146'097);
    const u64 yoe = (doe - doe / 1'460 + doe / 36'524 - doe / 146'096) / 365;
    const i64 y = static_cast<i64>(yoe) + era * 400;
    const u64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const u64 mp = (5 * doy + 2) / 153;
    const u64 d = doy - (153 * mp + 2) / 5 + 1;
    const u64 m = mp < 10 ? mp + 3 : mp - 9;
    const u64 year = static_cast<u64>(y + (m <= 2 ? 1 : 0));
    char text[40]{};
    auto two = [](char* at, u64 value) noexcept {
        at[0] = static_cast<char>('0' + value / 10 % 10);
        at[1] = static_cast<char>('0' + value % 10);
    };
    text[0] = static_cast<char>('0' + year / 1000 % 10);
    text[1] = static_cast<char>('0' + year / 100 % 10);
    two(text + 2, year);
    text[4] = '-';
    two(text + 5, m);
    text[7] = '-';
    two(text + 8, d);
    text[10] = 'T';
    two(text + 11, day_ms / 3'600'000);
    text[13] = ':';
    two(text + 14, day_ms / 60'000 % 60);
    text[16] = ':';
    two(text + 17, day_ms / 1'000 % 60);
    const char* tail = ".000000+00:00";
    for (u32 i = 0; tail[i]; ++i)
        text[19 + i] = tail[i];
    j.s("\"").s(text).s("\"");
}

// Account
u32 lab_channel_kind(u32 channel) noexcept { // 0 category, 1 text, 2 voice
    const u32 slot = channel % (kPerfLabTextPerCategory + 2);
    return slot == 0 ? 0 : slot == kPerfLabTextPerCategory + 1 ? 2 : 1;
}
u32 guild_channel_count(u32 guild) noexcept {
    return guild == 0 ? perf_lab_channel_count() : 1 + kPerfOtherTextChannels + 2;
}
u32 guild_channel_kind(u32 guild, u32 channel) noexcept {
    if (guild == 0)
        return lab_channel_kind(channel);
    return channel == 0 ? 0 : channel <= kPerfOtherTextChannels ? 1 : 2;
}

void append_user(Json& j, u32 user) noexcept {
    j.s("{\"id\":").id(perf_user_id(user)).s(",\"username\":\"perf_user_").n(user);
    j.s("\",\"global_name\":\"");
    static const char* first[] = {"Ada", "Linus", "Grace", "Ken", "Margaret", "Dennis", "Barbara", "Edsger",
                                  "Frances", "Niklaus", "Radia", "Bjarne", "Hedy", "Tim", "Sophie", "Anders"};
    j.s(first[user % 16]).s(" ").n(user);
    if (user % 9 == 3)
        j.s(" \\u2728");
    j.s("\",\"discriminator\":\"0\",\"avatar\":\"");
    if (user % 7 == 2)
        j.s("a_");
    j.hex(0x5a17'0000'0000ull + user * 0x9e37ull, 16).hex(user * 0x1234567ull, 16);
    j.s("\",\"public_flags\":").n(user % 5 == 0 ? 64 : 0).s(",\"bot\":").s(user % 97 == 13 ? "true" : "false");
    j.s("}");
}

void append_role(Json& j, u32 guild, u32 role) noexcept {
    static const char* names[] = {"@everyone", "Admins", "Moderators", "Helpers", "Artists",
                                  "Musicians", "Gamers", "Readers", "Bots"};
    static const u32 colors[] = {0, 15'548'997, 3'447'003, 5'763'719, 15'105'570, 10'181'046, 1'752'220, 9'807'270, 0};
    j.s("{\"id\":").id(role == 0 ? perf_guild_id(guild) : perf_role_id(guild, role));
    j.s(",\"name\":\"").s(names[role]).s("\",\"permissions\":\"").s(role == 1 ? "8" : "1071698660929");
    j.s("\",\"position\":").n(role).s(",\"color\":").n(colors[role]);
    j.s(",\"hoist\":").s(role >= 1 && role <= 3 ? "true" : "false");
    j.s(",\"managed\":false,\"mentionable\":true,\"flags\":0}");
}

void append_guild(Json& j, u32 guild) noexcept {
    const u64 gid = perf_guild_id(guild);
    j.s("{\"id\":").id(gid).s(",\"properties\":{\"id\":").id(gid).s(",\"name\":\"");
    if (guild == 0)
        j.s("Perf Lab");
    else
        j.s("Guild ").n(guild).s(guild % 3 == 0 ? " \\u2728" : "");
    j.s("\",\"owner_id\":").id(perf_user_id(guild % kPerfUsers));
    j.s(",\"icon\":\"").hex(0xc0ffee00ull + guild, 16).hex(guild * 0x77ull, 16).s("\"");
    j.s(",\"description\":null,\"verification_level\":1,\"default_message_notifications\":1,"
        "\"explicit_content_filter\":2,\"mfa_level\":0,\"nsfw_level\":0,\"afk_timeout\":300,"
        "\"afk_channel_id\":null,\"system_channel_id\":")
        .id(perf_channel_id(guild, 1))
        .s(",\"features\":[\"COMMUNITY\",\"NEWS\",\"ANIMATED_ICON\",\"MAX_FILE_SIZE_50_MB\"]}");
    j.s(",\"member_count\":").n(guild == 0 ? kPerfLabMembers : 200 + guild * 37);
    j.s(",\"lazy\":true,\"large\":true,\"joined_at\":\"2025-01-01T00:00:00+00:00\",\"roles\":[");
    for (u32 role = 0; role < 9; ++role) {
        j.c(role);
        append_role(j, guild, role);
    }
    j.s("],\"emojis\":[");
    const u32 emojis = guild == 0 ? 40 : 12;
    for (u32 e = 0; e < emojis; ++e)
        j.c(e).s("{\"id\":").id(perf_emoji_id(guild, e)).s(",\"name\":\"perf").n(e).s("\",\"roles\":[],"
            "\"require_colons\":true,\"managed\":false,\"animated\":").s(e % 5 == 4 ? "true" : "false")
            .s(",\"available\":true}");
    j.s("],\"stickers\":[],\"stage_instances\":[],\"threads\":[],\"channels\":[");
    const u32 channels = guild_channel_count(guild);
    u64 category = perf_channel_id(guild, 0);
    for (u32 c = 0; c < channels; ++c) {
        const u32 kind = guild_channel_kind(guild, c);
        const u64 cid = perf_channel_id(guild, c);
        j.c(c).s("{\"id\":").id(cid);
        if (kind == 0) {
            category = cid;
            j.s(",\"type\":4,\"name\":\"Category ").n(c).s("\",\"position\":").n(c);
            j.s(",\"permission_overwrites\":[]}");
            continue;
        }
        j.s(",\"type\":").n(kind == 2 ? 2 : 0).s(",\"name\":\"").s(kind == 2 ? "voice-" : "channel-").n(c);
        j.s("\",\"position\":").n(c).s(",\"parent_id\":").id(category);
        if (kind == 1)
            j.s(",\"last_message_id\":").id(message_id(cid, kMessagesPerChannel - 1))
                .s(",\"topic\":\"Synthetic channel ").n(c).s(" topic with a **bold** word\",\"nsfw\":false,"
                "\"rate_limit_per_user\":0");
        else
            j.s(",\"bitrate\":64000,\"user_limit\":0");
        j.s(",\"permission_overwrites\":[");
        if (guild != 0 && c % 7 == 6)
            j.s("{\"id\":").id(perf_role_id(guild, 2)).s(",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}");
        j.s("]}");
    }
    j.s("],\"members\":[{\"user\":{\"id\":").id(kPerfSelfUserId).s(",\"username\":\"perf_self\"},\"roles\":[")
        .id(perf_role_id(guild, 3)).s("],\"pending\":false,\"flags\":0,\"joined_at\":\"2025-01-01T00:00:00+00:00\"}]");
    j.s(",\"voice_states\":[],\"guild_scheduled_events\":[]}");
}

void append_private_channel(Json& j, u32 index) noexcept {
    if (index < kPerfDirectChannels) {
        j.s("{\"id\":").id(perf_private_id(index)).s(",\"type\":1,\"last_message_id\":")
            .id(message_id(perf_private_id(index), kMessagesPerChannel - 1 - index % 50));
        j.s(",\"flags\":0,\"recipient_ids\":[").id(perf_user_id(index % kPerfUsers)).s("]}");
        return;
    }
    j.s("{\"id\":").id(perf_private_id(index)).s(",\"type\":3,\"name\":\"Group ").n(index);
    j.s("\",\"icon\":null,\"owner_id\":").id(perf_user_id(index % kPerfUsers)).s(",\"last_message_id\":")
        .id(message_id(perf_private_id(index), kMessagesPerChannel - 1));
    j.s(",\"recipient_ids\":[");
    for (u32 r = 0; r < 5; ++r)
        j.c(r).id(perf_user_id((index * 3 + r) % kPerfUsers));
    j.s("]}");
}

void append_activity(Json& j, u32 user) noexcept {
    j.s("\"activities\":[");
    if (user % 4 == 3) {
        j.s("]");
        return;
    }
    j.s("{\"type\":4,\"name\":\"Custom Status\",\"state\":\"Benchmarking things ").n(user)
        .s("\",\"emoji\":{\"name\":\"\\ud83d\\udd25\"},\"created_at\":1792990000000}");
    if (user % 3 == 0)
        j.s(",{\"type\":0,\"name\":\"Synthetic Game ").n(user % 40).s("\",\"application_id\":\"")
            .n(400'000'000'000'000'000ull + user % 40).s("\",\"details\":\"Ranked\",\"state\":\"In a party\","
            "\"timestamps\":{\"start\":1792990000000},\"created_at\":1792990000001}");
    j.s("]");
}

u32 build_ready(char* output, u32 capacity) noexcept {
    Json j{output, capacity};
    j.s("{\"v\":9,\"user\":{\"id\":").id(kPerfSelfUserId);
    j.s(",\"username\":\"perf_self\",\"global_name\":\"Perf Self\",\"discriminator\":\"0\",\"avatar\":null,"
        "\"premium_type\":2,\"mfa_enabled\":true,\"flags\":0},\"session_id\":\"perf\","
        "\"resume_gateway_url\":\"wss://gateway.discord.gg\",\"session_type\":\"normal\",\"guilds\":[");
    for (u32 g = 0; g < kPerfGuilds; ++g) {
        j.c(g);
        append_guild(j, g);
    }
    j.s("],\"users\":[");
    for (u32 u = 0; u < kPerfUsers; ++u) {
        j.c(u);
        append_user(j, u);
    }
    j.s("],\"private_channels\":[");
    for (u32 p = 0; p < kPerfDirectChannels + kPerfGroupChannels; ++p) {
        j.c(p);
        append_private_channel(j, p);
    }
    j.s("],\"relationships\":[");
    for (u32 f = 0; f < kPerfFriends; ++f)
        j.c(f).s("{\"id\":").id(perf_user_id(f)).s(",\"type\":1,\"nickname\":null,\"user_id\":")
            .id(perf_user_id(f)).s("}");
    j.s("],\"merged_presences\":{\"friends\":[");
    static const char* status[] = {"online", "idle", "dnd", "offline"};
    for (u32 f = 0; f < kPerfFriends; ++f) {
        j.c(f).s("{\"user_id\":").id(perf_user_id(f)).s(",\"status\":\"").s(status[f % 4]).s("\",");
        j.s("\"client_status\":{\"desktop\":\"online\"},");
        append_activity(j, f);
        j.s("}");
    }
    j.s("],\"guilds\":[]},\"read_state\":{\"entries\":[");
    u32 entry = 0;
    for (u32 g = 0; g < kPerfGuilds; ++g)
        for (u32 c = 0; c < guild_channel_count(g); ++c) {
            if (guild_channel_kind(g, c) != 1)
                continue;
            const u64 cid = perf_channel_id(g, c);
            const u32 unread = (g + c) % 5 == 0 ? 12 : 0;
            j.c(entry++).s("{\"id\":").id(cid).s(",\"last_message_id\":")
                .id(message_id(cid, kMessagesPerChannel - 1 - unread)).s(",\"mention_count\":")
                .n(c % 11 == 1 ? 2 : 0).s("}");
        }
    j.s("],\"partial\":false,\"version\":77},\"user_guild_settings\":{\"entries\":[],\"partial\":false,"
        "\"version\":1},\"notification_settings\":{\"flags\":0},\"user_settings_proto\":\"\"}");
    return j.ok ? j.used() : 0;
}

// Messages
u32 author_of(u64 channel, u32 index) noexcept {
    return static_cast<u32>((index / 3 + (channel & 63)) % 48);
}
void append_attachment_urls(Json& j, u64 channel, u64 attachment, const char* name) noexcept {
    const char* hosts[] = {"https://cdn.discordapp.com/attachments/", "https://media.discordapp.net/attachments/"};
    const char* fields[] = {",\"url\":\"", ",\"proxy_url\":\""};
    for (u32 i = 0; i < 2; ++i) {
        j.s(fields[i]).s(hosts[i]).n(channel).s("/").n(attachment).s("/").s(name);
        j.s("?ex=").hex(0x6720a0b0ull, 8).s("&is=").hex(0x671f4f30ull, 8).s("&hm=")
            .hex(attachment * 0x9e3779b97f4a7c15ull, 16).hex(attachment ^ 0x5bd1e995ull, 16)
            .hex(attachment * 31ull, 16).hex(attachment * 131ull, 16).s("&\"");
    }
}

void append_message(Json& j, u64 guild, u64 channel, u32 index, bool reference_depth) noexcept {
    const u64 id = message_id(channel, index);
    const u32 author = author_of(channel, index);
    // The second Perf Lab channel is text only: nothing in it animates.
    u32 shape = index % 12;
    if (channel == perf_lab_text_channel(1) && (shape == 2 || shape == 3 || shape == 4 || shape == 7 || shape == 10))
        shape = shape == 3 ? 6 : 11;
    j.s("{\"id\":").id(id).s(",\"channel_id\":").id(channel).s(",\"type\":").n(shape == 5 && !reference_depth ? 19 : 0);
    j.s(",\"author\":");
    append_user(j, author);
    j.s(",\"timestamp\":");
    append_iso(j, message_time(index));
    j.s(",\"edited_timestamp\":");
    if (index % 17 == 5)
        append_iso(j, message_time(index) + 30'000);
    else
        j.s("null");
    j.s(",\"tts\":false,\"mention_everyone\":false,\"pinned\":false,\"flags\":0,\"mention_roles\":[],\"content\":\"");
    switch (shape) {
    case 0:
        j.s("Here is a **bold** claim, an *italic* aside, some `inline code`, ~~struck~~ text and a "
            "||spoiler||. See https://example.com/docs/").n(index).s(" for details \\ud83d\\ude00 <:perf3:")
            .n(perf_emoji_id(0, 3)).s(">");
        break;
    case 1:
        j.s("```cpp\\nstruct Frame {\\n    u64 id;\\n    u32 width, height;\\n};\\n"
            "for (u32 i = 0; i < count; ++i)\\n    total += frames[i].width * frames[i].height;\\n```");
        break;
    case 2:
        j.s("Screenshot of build ").n(index);
        break;
    case 3:
        j.s("Release notes for ").n(index);
        break;
    case 4:
        j.s("lol");
        break;
    case 5:
        j.s("Replying to that: agreed, the numbers look *much* better now.");
        break;
    case 6:
        j.s("# Heading\\n## Subheading ").n(index).s("\\n- first item\\n- second item with `code`\\n"
            "  - nested item\\n> quoted line one\\n> quoted line two\\n1. ordered\\n2. list");
        break;
    case 7:
        j.s("<a:perf4:").n(perf_emoji_id(0, 4)).s("> <a:perf9:").n(perf_emoji_id(0, 9))
            .s("> \\ud83c\\udf89\\ud83c\\udf89 party time \\u2764\\ufe0f");
        break;
    case 8:
        j.s("Hey <@").n(perf_user_id((author + 1) % kPerfUsers)).s("> and <@").n(perf_user_id((author + 2) % kPerfUsers))
            .s(">, check <#").n(perf_lab_text_channel(1)).s("> and ping <@&").n(perf_role_id(0, 2)).s(">");
        break;
    case 9:
        j.s("ok");
        break;
    case 10:
        j.s("Animated preview below");
        break;
    default:
        j.s("A longer message that wraps across several lines in the timeline so line breaking, shaping and "
            "layout are exercised on realistic paragraph lengths. It mentions numbers like ").n(index)
            .s(" and punctuation, quotes \\\"like this\\\", and non-Latin text: \\u041f\\u0440\\u0438\\u0432\\u0435"
               "\\u0442, \\u4f60\\u597d, \\u3053\\u3093\\u306b\\u3061\\u306f, \\u0645\\u0631\\u062d\\u0628\\u0627.\\n\\n"
               "Second paragraph after a blank line with a link https://example.org/a/b?c=")
            .n(index).s(".");
        break;
    }
    j.s("\",\"mentions\":[");
    if (shape == 8) {
        append_user(j, (author + 1) % kPerfUsers);
        j.s(",");
        append_user(j, (author + 2) % kPerfUsers);
    }
    j.s("],\"attachments\":[");
    if (shape == 2 || shape == 4 || shape == 10) {
        const u64 attachment = id + 1;
        const char* name = shape == 2 ? "screenshot.png" : shape == 4 ? "reaction.gif" : "loop.webp";
        const char* type = shape == 2 ? "image/png" : shape == 4 ? "image/gif" : "image/webp";
        const u32 width = shape == 2 ? 1280 : 320;
        const u32 height = shape == 2 ? 720 : 240;
        j.s("{\"id\":").id(attachment).s(",\"filename\":\"").s(name).s("\",\"size\":").n(width * height);
        append_attachment_urls(j, channel, attachment, name);
        j.s(",\"width\":").n(width).s(",\"height\":").n(height).s(",\"content_type\":\"").s(type).s("\"");
        if (shape != 2)
            j.s(",\"flags\":0");
        j.s("}");
    }
    j.s("],\"embeds\":[");
    if (shape == 3) {
        const u64 image = id + 2;
        j.s("{\"type\":\"rich\",\"title\":\"Build ").n(index).s(" passed\",\"description\":\"All **")
            .n(1200 + index % 300).s("** tests passed in 14.2 s. Coverage unchanged.\\n`cmake --build .`\","
            "\"url\":\"https://example.com/builds/").n(index).s("\",\"color\":5763719,\"timestamp\":");
        append_iso(j, message_time(index));
        j.s(",\"author\":{\"name\":\"CI Bot\"},\"fields\":[{\"name\":\"Duration\",\"value\":\"14.2 s\",\"inline\":true},"
            "{\"name\":\"Binary\",\"value\":\"5.46 MB\",\"inline\":true},{\"name\":\"Notes\",\"value\":\"No regressions "
            "were found in this run.\",\"inline\":false}],\"thumbnail\":{\"width\":128,\"height\":128");
        append_attachment_urls(j, channel, image, "thumb.png");
        j.s("},\"footer\":{\"text\":\"perf pipeline\"}}");
    }
    j.s("],\"reactions\":[");
    if (shape == 8 || shape == 0) {
        j.s("{\"emoji\":{\"id\":null,\"name\":\"\\ud83d\\udc4d\"},\"count\":").n(3 + index % 5)
            .s(",\"count_details\":{\"burst\":0,\"normal\":").n(3 + index % 5)
            .s("},\"burst_colors\":[],\"me_burst\":false,\"burst_me\":false,\"me\":false,\"burst_count\":0},"
               "{\"emoji\":{\"id\":")
            .id(perf_emoji_id(0, 2)).s(",\"name\":\"perf2\",\"animated\":false},\"count\":2,\"count_details\":"
            "{\"burst\":0,\"normal\":2},\"burst_colors\":[],\"me_burst\":false,\"burst_me\":false,\"me\":true,"
            "\"burst_count\":0}");
    }
    j.s("]");
    if (shape == 7)
        j.s(",\"sticker_items\":[{\"id\":").id(950'000'000'000'000'000ull + index % 8)
            .s(",\"name\":\"wave\",\"format_type\":1}]");
    if (shape == 5 && !reference_depth && index > 0) {
        j.s(",\"message_reference\":{\"type\":0,\"message_id\":").id(message_id(channel, index - 1))
            .s(",\"channel_id\":").id(channel);
        if (guild)
            j.s(",\"guild_id\":").id(guild);
        j.s("},\"referenced_message\":");
        append_message(j, guild, channel, index - 1, true);
    }
    j.s("}");
}

// Event queue
struct QueuedEvent {
    GatewayClientEvent event;
    u64 offset;
    u32 bytes;
};
constexpr u32 kEventCapacity = 65'536;
constexpr u64 kEventPayloadBytes = 96ull * 1024 * 1024;

struct RestEntry {
    RestClientResponse response;
    u64 offset;
};
constexpr u32 kRestCapacity = 64;
constexpr u32 kRestBodyBytes = kAccountHistoryResponseBytes;

enum class WorkKind : u8 { none, history, rest_error, media, media_advance, member_sync, member_chunk, churn_tick };
struct Work {
    WorkKind kind;
    u64 a, b, c, d;
    u32 e, f;
    MediaAssetKey key;
    u64 request_id;
};
constexpr u32 kWorkCapacity = 8'192;

struct MediaSlot {
    AccountMediaFrame frame;
    u64 offset;
};
constexpr u32 kMediaSlots = 48;
constexpr u32 kMediaSlotBytes = 512 * 1024;

struct Scratch {
    char* json;
    u32* structurals;
    char* route;
};
constexpr u32 kScratchJsonBytes = 1024 * 1024;
constexpr u32 kScratchStructurals = 256 * 1024;

struct Module {
    SRWLOCK lock;
    HANDLE gateway_event;   // manual reset: events pending
    HANDLE response_event;  // manual reset: REST or media results pending
    HANDLE work_event;      // auto reset: worker has work
    HANDLE stop_event;      // manual reset
    HANDLE ready_thread;
    HANDLE work_thread;
    HANDLE churn_thread;
    HANDLE churn_stop;
    QueuedEvent* events;
    u8* event_payload;
    u32 event_count, event_cursor;
    u64 event_used;
    RestEntry rest[kRestCapacity];
    u8* rest_bodies;
    u32 rest_head, rest_count;
    Work* work;
    u32 work_head, work_count;
    MediaSlot media[kMediaSlots];
    u8* media_pixels;
    u32 media_head, media_count, media_taken_slot;
    bool media_has_taken;
    // READY worker scratch.
    char* ready_json;
    u32* ready_structurals;
    u32* records;
    char* projection_payload;
    char* header;
    // Service and churn worker scratch (one set per thread).
    Scratch service;
    Scratch churn_scratch;
    u64 member_request_ids[100];
    volatile LONG ready_published;
    u64 sequence;
    PerfAccountControl churn;
    u64 churn_counter;
    PerfAccountCounters counters;
    bool started;
};
Module* g_module = nullptr;

bool enqueue_event_locked(Module& m, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    if (m.event_count == kEventCapacity || payload.count > kEventPayloadBytes - m.event_used)
        return false;
    QueuedEvent& slot = m.events[m.event_count++];
    slot.event = event;
    slot.offset = m.event_used;
    slot.bytes = static_cast<u32>(payload.count);
    copy_bytes(m.event_payload + m.event_used, payload.data, static_cast<u32>(payload.count));
    m.event_used += payload.count;
    ++m.counters.gateway_events;
    m.counters.gateway_payload_bytes += payload.count;
    return true;
}

bool publish(void* context, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    Module& m = *static_cast<Module*>(context);
    AcquireSRWLockExclusive(&m.lock);
    // Producer backpressure, as the real mailbox: wait for the client to drain.
    while (!enqueue_event_locked(m, event, payload)) {
        if (m.event_cursor == 0 && m.event_count == kEventCapacity) {
            ReleaseSRWLockExclusive(&m.lock);
            return false;
        }
        ReleaseSRWLockExclusive(&m.lock);
        SetEvent(m.gateway_event);
        if (WaitForSingleObject(m.stop_event, 2) == WAIT_OBJECT_0)
            return false;
        AcquireSRWLockExclusive(&m.lock);
    }
    ReleaseSRWLockExclusive(&m.lock);
    SetEvent(m.gateway_event);
    return true;
}

bool route_and_publish(Module& m, Scratch& scratch, const char* name, Utf8View data) noexcept {
    GatewayEnvelope envelope{};
    envelope.opcode = 0;
    envelope.has_event_name = true;
    u32 name_bytes = 0;
    while (name[name_bytes])
        ++name_bytes;
    envelope.event_name = {{name, name_bytes}, 0, false};
    envelope.data = {data, 0, JsonValueKind::object};
    envelope.sequence = ++m.sequence;
    envelope.has_sequence = true;
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    u64 bulk[kGatewayBulkDeleteMaximumIds]{};
    const auto routed = gateway_client_route_dispatch(
        envelope, {scratch.structurals, kScratchStructurals}, 64,
        {scratch.route, kGatewayClientDispatchPayloadCapacity}, {bulk, kGatewayBulkDeleteMaximumIds},
        kPerfSelfUserId, &event, &payload);
    if (!routed.ok())
        return false;
    event.timestamp_ms = GetTickCount64();
    return publish(&m, event, payload);
}

// READY worker
DWORD WINAPI ready_thread(void* context) noexcept {
    Module& m = *static_cast<Module*>(context);
    const u32 bytes = build_ready(m.ready_json, 16 * 1024 * 1024);
    m.counters.ready_json_bytes = bytes;
    if (!bytes)
        return 1;
    GatewayReady ready{};
    if (!gateway_decode_ready({{m.ready_json, bytes}, 0, JsonValueKind::object},
                              {m.ready_structurals, kGatewaySessionMaximumStructuralCount}, 64, &ready)
             .ok())
        return 2;
    const u64 before = m.counters.gateway_events;
    const auto projected = gateway_ready_project(
        ready,
        {{m.ready_structurals, kGatewaySessionMaximumStructuralCount},
         {m.records, 512 * 1024},
         {m.projection_payload, kGatewayClientDispatchPayloadCapacity},
         {m.header, 4096}},
        ++m.sequence, GetTickCount64(), publish, nullptr, &m);
    if (!projected.ok())
        return 3;
    GatewayClientEvent complete{};
    complete.kind = GatewayClientEventKind::dispatch;
    complete.dispatch_kind = GatewayClientDispatchKind::supplemental_state_complete;
    complete.timestamp_ms = GetTickCount64();
    complete.sequence = ++m.sequence;
    complete.has_sequence = true;
    publish(&m, complete, {});
    m.counters.ready_events = static_cast<u32>(m.counters.gateway_events - before);
    InterlockedExchange(&m.ready_published, 1);
    return 0;
}

// Service worker
bool push_work(Module& m, const Work& work) noexcept {
    AcquireSRWLockExclusive(&m.lock);
    const bool ok = m.work_count < kWorkCapacity;
    if (ok) {
        m.work[(m.work_head + m.work_count) % kWorkCapacity] = work;
        ++m.work_count;
    }
    ReleaseSRWLockExclusive(&m.lock);
    if (ok)
        SetEvent(m.work_event);
    return ok;
}

bool push_rest(Module& m, u64 request_id, u16 status, ConstSlice<u8> body) noexcept {
    AcquireSRWLockExclusive(&m.lock);
    bool ok = m.rest_count < kRestCapacity && body.count <= kRestBodyBytes;
    if (ok) {
        const u32 slot = (m.rest_head + m.rest_count) % kRestCapacity;
        RestEntry& entry = m.rest[slot];
        entry = {};
        entry.offset = static_cast<u64>(slot) * kRestBodyBytes;
        copy_bytes(m.rest_bodies + entry.offset, body.data, static_cast<u32>(body.count));
        entry.response.request_id = request_id;
        entry.response.status_code = status;
        entry.response.body_bytes = static_cast<u32>(body.count);
        entry.response.wire_body_bytes = static_cast<u32>(body.count);
        entry.response.content_type = RestContentType::json;
        entry.response.content_encoding = RestContentEncoding::identity;
        entry.response.wire_content_encoding = RestContentEncoding::identity;
        entry.response.body_kind = body.count == 0 ? RestBodyKind::empty
                                   : body.data[0] == '[' ? RestBodyKind::json_array
                                                          : RestBodyKind::json_object;
        entry.response.reused_connection = true;
        entry.response.elapsed_us = 1'000;
        ++m.rest_count;
        ++m.counters.rest_responses;
    }
    ReleaseSRWLockExclusive(&m.lock);
    if (ok)
        SetEvent(m.response_event);
    return ok;
}

void serve_history(Module& m, const Work& work) noexcept {
    const u64 channel = work.a;
    const u64 cursor = work.b;
    const u32 limit = minimum<u32>(work.e ? work.e : 50, 100);
    const auto position = static_cast<MessageHistoryPosition>(work.f);
    u32 newest = kMessagesPerChannel - 1;
    u32 oldest = 0;
    if (position == MessageHistoryPosition::before) {
        const u32 at = message_index_at(cursor);
        newest = at == 0 ? 0 : at - (message_id(channel, at) >= cursor ? 1 : 0);
        if (at == 0 && message_id(channel, 0) >= cursor) {
            push_rest(m, work.request_id, 200, {reinterpret_cast<const u8*>("[]"), 2});
            return;
        }
    } else if (position == MessageHistoryPosition::after) {
        const u32 at = message_index_at(cursor);
        oldest = at + 1;
        if (oldest >= kMessagesPerChannel) {
            push_rest(m, work.request_id, 200, {reinterpret_cast<const u8*>("[]"), 2});
            return;
        }
        newest = minimum<u32>(kMessagesPerChannel - 1, oldest + limit - 1);
    } else if (position == MessageHistoryPosition::around) {
        const u32 at = message_index_at(cursor);
        newest = minimum<u32>(kMessagesPerChannel - 1, at + limit / 2);
    }
    const u32 count = minimum<u32>(limit, newest - oldest + 1);
    u64 guild = 0;
    if (channel >= perf_guild_id(0) && channel < perf_guild_id(kPerfGuilds))
        guild = perf_guild_id(static_cast<u32>((channel - perf_guild_id(0)) / 10'000ull));
    Json j{m.service.json, kRestBodyBytes};
    j.s("[");
    u32 written = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 mark = j.used();
        j.c(i);
        append_message(j, guild, channel, newest - i, false);
        if (!j.ok) {
            j.ok = true;
            j.writer.used = mark;
            break;
        }
        ++written;
    }
    j.s("]");
    m.counters.history_messages += written;
    push_rest(m, work.request_id, 200, {reinterpret_cast<const u8*>(m.service.json), j.used()});
}

void serve_rest_error(Module& m, const Work& work) noexcept {
    constexpr char body[] = "{\"message\":\"Unknown Resource (perf probe)\",\"code\":10000}";
    push_rest(m, work.request_id, 404, {reinterpret_cast<const u8*>(body), sizeof(body) - 1});
}

u32 media_extent(MediaAssetKey key, u32 requested_width, u32 requested_height, u32* height) noexcept {
    u32 width = 128;
    *height = 128;
    switch (key.kind) {
    case MediaAssetKind::custom_emoji:
        width = *height = 96;
        break;
    case MediaAssetKind::sticker:
        width = *height = 160;
        break;
    case MediaAssetKind::attachment:
    case MediaAssetKind::embed:
        width = requested_width ? requested_width : 320;
        *height = requested_height ? requested_height : 240;
        break;
    default:
        break;
    }
    while (width * *height > kMediaSlotBytes / 4) {
        width = (width + 1) / 2;
        *height = (*height + 1) / 2;
    }
    return width;
}

bool media_animated(MediaAssetKey key) noexcept {
    return (key.flags & media_asset_flag_animated) != 0;
}

void serve_media(Module& m, const Work& work) noexcept {
    u32 height = 0;
    const u32 width = media_extent(work.key, work.e, work.f, &height);
    const u32 frame_index = work.kind == WorkKind::media_advance ? static_cast<u32>(work.c) : 0;
    const bool animated = media_animated(work.key);
    for (;;) {
        AcquireSRWLockExclusive(&m.lock);
        const bool room = m.media_count < kMediaSlots - 1;
        ReleaseSRWLockExclusive(&m.lock);
        if (room)
            break;
        if (WaitForSingleObject(m.stop_event, 1) == WAIT_OBJECT_0)
            return;
    }
    AcquireSRWLockExclusive(&m.lock);
    const u32 slot = (m.media_head + m.media_count) % kMediaSlots;
    ReleaseSRWLockExclusive(&m.lock);
    MediaSlot& media = m.media[slot];
    media = {};
    media.offset = static_cast<u64>(slot) * kMediaSlotBytes;
    u8* pixels = m.media_pixels + media.offset;
    // Deterministic gradient keyed by the asset; animations move a band.
    const u32 seed = static_cast<u32>(work.key.id * 2654435761u) ^ work.key.variant;
    for (u32 y = 0; y < height; ++y) {
        u8* row = pixels + static_cast<u64>(y) * width * 4;
        for (u32 x = 0; x < width; ++x) {
            const u32 band = animated && ((x + frame_index * 8) / 16) % 4 == 0 ? 80u : 0u;
            row[x * 4 + 0] = static_cast<u8>((seed >> 0) + x * 255 / width + band);
            row[x * 4 + 1] = static_cast<u8>((seed >> 8) + y * 255 / height);
            row[x * 4 + 2] = static_cast<u8>((seed >> 16) + band);
            row[x * 4 + 3] = 255;
        }
    }
    AccountMediaFrame& frame = media.frame;
    frame.key = work.key;
    frame.content_identity = work.key.id ^ (static_cast<u64>(work.key.variant) << 32);
    frame.bytes = {pixels, width * height * 4};
    frame.width = width;
    frame.height = height;
    frame.row_pitch = width * 4;
    frame.request_id = work.request_id;
    frame.source_message_id = work.a;
    frame.source_message_revision = static_cast<u32>(work.b);
    frame.cache_generation = 1;
    frame.source_kind = work.key.kind == MediaAssetKind::attachment || work.key.kind == MediaAssetKind::embed
                            ? AccountMediaSourceKind::message
                            : AccountMediaSourceKind::none;
    if (animated) {
        frame.kind = AccountMediaFrameKind::animated_webp_rgba8;
        frame.frame_index = frame_index % 12;
        frame.frame_count = 12;
        frame.play_count = 0;
        frame.duration_us = 80'000;
        frame.canvas_width = static_cast<u16>(width);
        frame.canvas_height = static_cast<u16>(height);
        frame.background_rgba = 0;
        frame.animated_webp_frame = {};
        frame.animated_webp_frame.payload_bytes = width * height * 4;
        frame.animated_webp_frame.duration_ms = 80;
        frame.animated_webp_frame.width = static_cast<u16>(width);
        frame.animated_webp_frame.height = static_cast<u16>(height);
        frame.animated_webp_frame.blend = AnimatedWebpBlend::source;
        frame.animated_webp_frame.dispose = AnimatedWebpDispose::none;
        frame.animated_webp_frame.has_alpha = false;
        frame.animated_webp_frame.codec = AnimatedWebpCodec::vp8l_lossless;
    } else {
        frame.kind = AccountMediaFrameKind::straight_rgba8;
        frame.frame_count = 1;
    }
    AcquireSRWLockExclusive(&m.lock);
    ++m.media_count;
    ++m.counters.media_frames;
    m.counters.media_pixel_bytes += frame.bytes.count;
    ReleaseSRWLockExclusive(&m.lock);
    SetEvent(m.response_event);
}

// Member list rows for Perf Lab: three hoisted roles, online, offline.
struct ListGroup {
    u64 id;
    u32 count;
    const char* name;
};
void list_groups(ListGroup* groups) noexcept {
    groups[0] = {perf_role_id(0, 1), 20, nullptr};
    groups[1] = {perf_role_id(0, 2), 80, nullptr};
    groups[2] = {perf_role_id(0, 3), 300, nullptr};
    groups[3] = {0, kPerfLabOnline - 400, "online"};
    groups[4] = {0, kPerfLabMembers - kPerfLabOnline, "offline"};
}
void append_group_id(Json& j, const ListGroup& group) noexcept {
    if (group.name)
        j.s("\"").s(group.name).s("\"");
    else
        j.id(group.id);
}

void serve_member_sync(Module& m, const Work& work) noexcept {
    const u64 guild = work.a;
    if (guild != perf_guild_id(0))
        return;
    ListGroup groups[5];
    list_groups(groups);
    const u32 first = work.e;
    const u32 last = minimum<u32>(work.f, first + 99);
    Json j{m.service.json, kScratchJsonBytes};
    j.s("{\"guild_id\":").id(guild).s(",\"id\":\"everyone\",\"member_count\":").n(kPerfLabMembers);
    j.s(",\"online_count\":").n(kPerfLabOnline).s(",\"groups\":[");
    for (u32 g = 0; g < 5; ++g) {
        j.c(g).s("{\"id\":");
        append_group_id(j, groups[g]);
        j.s(",\"count\":").n(groups[g].count).s("}");
    }
    j.s("],\"ops\":[{\"op\":\"SYNC\",\"range\":[").n(first).s(",").n(last).s("],\"items\":[");
    // Walk the virtual list: each group is a header row followed by members.
    u32 position = 0, member_ordinal = 0, written = 0;
    static const char* status[] = {"online", "idle", "dnd"};
    for (u32 g = 0; g < 5 && position <= last; ++g) {
        if (position >= first) {
            j.c(written++).s("{\"group\":{\"id\":");
            append_group_id(j, groups[g]);
            j.s(",\"count\":").n(groups[g].count).s("}}");
        }
        ++position;
        for (u32 i = 0; i < groups[g].count && position <= last; ++i, ++position, ++member_ordinal) {
            if (position < first)
                continue;
            const u32 user = 10'000 + member_ordinal;
            j.c(written++).s("{\"member\":{\"user\":");
            append_user(j, user);
            j.s(",\"roles\":[");
            if (g < 3)
                j.id(groups[g].id);
            j.s("],\"joined_at\":\"2025-02-01T00:00:00+00:00\",\"nick\":null,\"mute\":false,\"deaf\":false,"
                "\"presence\":{\"user\":{\"id\":")
                .id(perf_user_id(user)).s("},\"status\":\"").s(g == 4 ? "offline" : status[member_ordinal % 3])
                .s("\",\"client_status\":{\"desktop\":\"online\"},");
            append_activity(j, user);
            j.s("}}}");
        }
    }
    j.s("]}]}");
    if (j.ok && route_and_publish(m, m.service, "GUILD_MEMBER_LIST_UPDATE", {m.service.json, j.used()}))
        ++m.counters.member_list_syncs;
}

void serve_member_chunk(Module& m, const Work& work) noexcept {
    // work.a guild, work.b nonce, work.e user count; the IDs are in member_request_ids.
    Json j{m.service.json, kScratchJsonBytes};
    j.s("{\"guild_id\":").id(work.a).s(",\"members\":[");
    const u32 count = work.e;
    const u64* users = m.member_request_ids;
    for (u32 i = 0; i < count && i < 100; ++i) {
        const u64 user = users[i] - perf_user_id(0);
        j.c(i).s("{\"user\":");
        append_user(j, static_cast<u32>(user % 100'000));
        j.s(",\"roles\":[").id(perf_role_id(static_cast<u32>((work.a - perf_guild_id(0)) / 10'000ull),
                                              static_cast<u32>(1 + user % 5)))
            .s("],\"joined_at\":\"2025-02-01T00:00:00+00:00\",\"nick\":null,\"mute\":false,\"deaf\":false}");
    }
    j.s("],\"chunk_index\":0,\"chunk_count\":1,\"not_found\":[],\"nonce\":\"").n(work.b).s("\"}");
    if (j.ok)
        route_and_publish(m, m.service, "GUILD_MEMBERS_CHUNK", {m.service.json, j.used()});
}

void churn_message(Module& m, Scratch& scratch, u64 channel, u64 guild, u64 tick) noexcept;

DWORD WINAPI work_thread(void* context) noexcept {
    Module& m = *static_cast<Module*>(context);
    HANDLE handles[2] = {m.stop_event, m.work_event};
    for (;;) {
        Work work{};
        AcquireSRWLockExclusive(&m.lock);
        if (m.work_count) {
            work = m.work[m.work_head];
            m.work_head = (m.work_head + 1) % kWorkCapacity;
            --m.work_count;
        }
        ReleaseSRWLockExclusive(&m.lock);
        if (work.kind == WorkKind::none) {
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0)
                return 0;
            continue;
        }
        if (WaitForSingleObject(m.stop_event, 0) == WAIT_OBJECT_0)
            return 0;
        switch (work.kind) {
        case WorkKind::history:
            serve_history(m, work);
            break;
        case WorkKind::rest_error:
            serve_rest_error(m, work);
            break;
        case WorkKind::media:
        case WorkKind::media_advance:
            serve_media(m, work);
            break;
        case WorkKind::member_sync:
            serve_member_sync(m, work);
            break;
        case WorkKind::member_chunk:
            serve_member_chunk(m, work);
            break;
        case WorkKind::churn_tick:
            churn_message(m, m.service, work.a, work.b, work.c);
            break;
        default:
            break;
        }
    }
}

// Churn worker
void churn_typing(Module& m, Scratch& scratch, u64 tick) noexcept {
    char text[2048];
    Json j{text, sizeof(text)};
    const u32 user = static_cast<u32>(tick % 48);
    j.s("{\"channel_id\":").id(m.churn.channel_id).s(",\"guild_id\":").id(m.churn.guild_id).s(",\"user_id\":")
        .id(perf_user_id(user)).s(",\"timestamp\":1792994400,\"member\":{\"user\":");
    append_user(j, user);
    j.s(",\"roles\":[],\"joined_at\":\"2025-02-01T00:00:00+00:00\",\"nick\":null,\"mute\":false,\"deaf\":false}}");
    if (j.ok)
        route_and_publish(m, scratch, "TYPING_START", {text, j.used()});
}
void churn_presence(Module& m, Scratch& scratch, u64 tick) noexcept {
    char text[2048];
    Json j{text, sizeof(text)};
    const u32 user = 10'000 + static_cast<u32>((tick * 7) % 400);
    static const char* status[] = {"online", "idle", "dnd", "online"};
    j.s("{\"user\":{\"id\":").id(perf_user_id(user)).s("},\"guild_id\":").id(m.churn.guild_id)
        .s(",\"status\":\"").s(status[tick % 4]).s("\",\"client_status\":{\"desktop\":\"online\"},");
    append_activity(j, static_cast<u32>(user + tick));
    j.s("}");
    if (j.ok)
        route_and_publish(m, scratch, "PRESENCE_UPDATE", {text, j.used()});
}
void churn_message(Module& m, Scratch& scratch, u64 channel, u64 guild, u64 tick) noexcept {
    // A new message after the newest history message (fresh ID per tick).
    char* text = scratch.json;
    Json j{text, kScratchJsonBytes};
    const u32 index = static_cast<u32>(tick % 12);
    // Reuse the history generator, then replace the ID with a strictly newer snowflake.
    append_message(j, guild, channel, kMessagesPerChannel - 12 + index, true);
    if (!j.ok)
        return;
    const u64 fresh =
        ((kLatestMessageMs + 1'000 + tick * 500 - kSnowflakeEpochMs) << 22) | (tick & 0x3FFFFF);
    char digits[24];
    u32 count = 0;
    for (u64 v = fresh; v; v /= 10)
        digits[count++] = static_cast<char>('0' + v % 10);
    // The message starts with {"id":"<20 digits>"; history IDs and fresh IDs have the same width.
    for (u32 i = 0; i < count && 7 + i < j.used(); ++i)
        text[7 + i] = digits[count - 1 - i];
    // Append guild_id and member: rewrite the closing brace.
    j.writer.used -= 1;
    j.s(",\"guild_id\":").id(guild).s(",\"member\":{\"roles\":[],\"joined_at\":"
        "\"2025-02-01T00:00:00+00:00\",\"nick\":null,\"mute\":false,\"deaf\":false}}");
    if (j.ok)
        route_and_publish(m, scratch, "MESSAGE_CREATE", {text, j.used()});
}

DWORD WINAPI churn_thread(void* context) noexcept {
    Module& m = *static_cast<Module*>(context);
    constexpr u32 kTickMs = 50;
    u64 typing_budget = 0, presence_budget = 0, message_budget = 0;
    while (WaitForSingleObject(m.churn_stop, kTickMs) == WAIT_TIMEOUT) {
        typing_budget += m.churn.typing_per_second * kTickMs;
        presence_budget += m.churn.presence_per_second * kTickMs;
        message_budget += m.churn.messages_per_second * kTickMs;
        for (; typing_budget >= 1000; typing_budget -= 1000)
            churn_typing(m, m.churn_scratch, m.churn_counter++);
        for (; presence_budget >= 1000; presence_budget -= 1000)
            churn_presence(m, m.churn_scratch, m.churn_counter++);
        for (; message_budget >= 1000; message_budget -= 1000)
            churn_message(m, m.churn_scratch, m.churn.channel_id, m.churn.guild_id, m.churn_counter++);
    }
    return 0;
}

// Module API
AccountModuleStatus status_of(Module& m) noexcept {
    AccountModuleStatus status{};
    status.profile_id = 0x5045'5246'0000'0001ull;
    status.gateway_sources = 1;
    status.gateway_ready = 1;
    status.running = true;
    status.coordinator_phase = static_cast<u8>(UserAccountCoordinatorPhase::running);
    (void)m;
    return status;
}

AccountModuleStartResult WINAPI module_start(const wchar_t*, const wchar_t*, u64, u64) noexcept {
    AccountModuleStartResult result{};
    if (g_module) {
        result.error = AccountModuleError::coordinator_start;
        return result;
    }
    auto* m = static_cast<Module*>(region_take(sizeof(Module)));
    if (!m) {
        result.error = AccountModuleError::memory;
        return result;
    }
    InitializeSRWLock(&m->lock);
    m->events = static_cast<QueuedEvent*>(region_take(sizeof(QueuedEvent) * u64{kEventCapacity}));
    m->event_payload = static_cast<u8*>(region_take(kEventPayloadBytes));
    m->rest_bodies = static_cast<u8*>(region_take(u64{kRestBodyBytes} * kRestCapacity));
    m->work = static_cast<Work*>(region_take(sizeof(Work) * u64{kWorkCapacity}));
    m->media_pixels = static_cast<u8*>(region_take(u64{kMediaSlotBytes} * kMediaSlots));
    m->ready_json = static_cast<char*>(region_take(16ull * 1024 * 1024));
    m->ready_structurals = static_cast<u32*>(region_take(sizeof(u32) * u64{kGatewaySessionMaximumStructuralCount}));
    m->records = static_cast<u32*>(region_take(sizeof(u32) * 512ull * 1024));
    m->projection_payload = static_cast<char*>(region_take(kGatewayClientDispatchPayloadCapacity));
    m->header = static_cast<char*>(region_take(8192));
    Scratch* scratches[2] = {&m->service, &m->churn_scratch};
    for (Scratch* scratch : scratches) {
        scratch->json = static_cast<char*>(region_take(kScratchJsonBytes));
        scratch->structurals = static_cast<u32*>(region_take(sizeof(u32) * u64{kScratchStructurals}));
        scratch->route = static_cast<char*>(region_take(kGatewayClientDispatchPayloadCapacity));
    }
    m->gateway_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m->response_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m->work_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m->churn_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m->events || !m->event_payload || !m->rest_bodies || !m->work || !m->media_pixels || !m->ready_json ||
        !m->ready_structurals || !m->records || !m->projection_payload || !m->header || !m->service.route ||
        !m->churn_scratch.route ||
        !m->gateway_event || !m->response_event || !m->work_event || !m->stop_event || !m->churn_stop) {
        result.error = AccountModuleError::memory;
        return result;
    }
    // The READY worker runs while the client creates its window and device,
    // like the real module's Gateway handshake.
    m->ready_thread = CreateThread(nullptr, 256 * 1024, ready_thread, m, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    m->work_thread = CreateThread(nullptr, 256 * 1024, work_thread, m, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!m->ready_thread || !m->work_thread) {
        result.error = AccountModuleError::coordinator_start;
        return result;
    }
    m->started = true;
    g_module = m;
    result.instance = m;
    result.status = status_of(*m);
    result.session_loaded = true;
    return result;
}

AccountModuleStatus WINAPI module_pump(void* instance, u64, u64) noexcept {
    return status_of(*static_cast<Module*>(instance));
}

u32 WINAPI module_notification_handles(void* instance, HANDLE* output, u32 capacity) noexcept {
    auto* m = static_cast<Module*>(instance);
    u32 count = 0;
    if (m && capacity >= 2) {
        output[count++] = m->gateway_event;
        output[count++] = m->response_event;
    }
    return count;
}

void* WINAPI module_gateway_source(void* instance, u32 index) noexcept {
    return index == 0 ? instance : nullptr;
}

GatewayClientPopResult WINAPI module_gateway_pop(void* source, GatewayClientEvent* event,
                                                 Slice<u8> output) noexcept {
    auto* m = static_cast<Module*>(source);
    if (!m || !event)
        return {0, GatewayClientPopError::invalid_input};
    AcquireSRWLockExclusive(&m->lock);
    if (m->event_cursor == m->event_count) {
        m->event_cursor = m->event_count = 0;
        m->event_used = 0;
        ResetEvent(m->gateway_event);
        ReleaseSRWLockExclusive(&m->lock);
        return {0, GatewayClientPopError::empty};
    }
    const QueuedEvent& queued = m->events[m->event_cursor];
    if (queued.bytes > output.count) {
        ReleaseSRWLockExclusive(&m->lock);
        return {queued.bytes, GatewayClientPopError::output_too_small};
    }
    copy_bytes(output.data, m->event_payload + queued.offset, queued.bytes);
    *event = queued.event;
    ++m->event_cursor;
    const u32 bytes = queued.bytes;
    ReleaseSRWLockExclusive(&m->lock);
    return {bytes, GatewayClientPopError::none};
}

bool WINAPI module_guild_subscribe(void* instance, const GatewayGuildSubscription* subscription) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !subscription)
        return false;
    for (u32 r = 0; r < subscription->range_count; ++r) {
        Work work{};
        work.kind = WorkKind::member_sync;
        work.a = subscription->guild_id;
        work.b = subscription->channel_id;
        work.e = subscription->ranges[r].first;
        work.f = subscription->ranges[r].last;
        push_work(*m, work);
    }
    return true;
}

bool WINAPI module_guild_members_request(void* instance, const GatewayGuildMemberRequest* request) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m)
        return false;
    if (!request || !request->user_count)
        return true;
    // The worker reads the user IDs from member_request_ids; one chunk in flight.
    AcquireSRWLockExclusive(&m->lock);
    copy_bytes(m->member_request_ids, request->user_ids, minimum<u32>(request->user_count, 100) * 8);
    ReleaseSRWLockExclusive(&m->lock);
    Work work{};
    work.kind = WorkKind::member_chunk;
    work.a = request->guild_id;
    work.b = request->nonce;
    work.e = minimum<u32>(request->user_count, 100);
    return push_work(*m, work);
}

GatewayStateRecoveryResult WINAPI module_gateway_refresh(void*) noexcept {
    return {};
}

RestClientSubmitResult submitted() noexcept {
    return {};
}
RestClientSubmitResult rejected() noexcept {
    RestClientSubmitResult result{};
    result.error = RestClientSubmitError::invalid_input;
    return result;
}

RestClientSubmitResult WINAPI module_history_submit(void* instance, u64 channel_id,
                                                    const MessageHistoryQuery* query,
                                                    u64 request_id) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !query || !request_id)
        return rejected();
    Work work{};
    work.kind = WorkKind::history;
    work.a = channel_id;
    work.b = query->cursor;
    work.e = query->limit;
    work.f = static_cast<u32>(query->position);
    work.request_id = request_id;
    return push_work(*m, work) ? submitted() : rejected();
}

RestClientSubmitResult WINAPI module_self_status_submit(void* instance, u64, u64, SelfStatusSettingsOperation,
                                                        GatewayUserStatus, u64 request_id, u64) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !request_id)
        return rejected();
    Work work{};
    work.kind = WorkKind::rest_error;
    work.request_id = request_id;
    return push_work(*m, work) ? submitted() : rejected();
}

RestClientTakeResult WINAPI module_rest_take(void* instance, RestClientResponse* response,
                                             Slice<u8> body) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !response)
        return {0, RestClientTakeError::invalid_input};
    AcquireSRWLockExclusive(&m->lock);
    if (!m->rest_count) {
        if (!m->media_count)
            ResetEvent(m->response_event);
        ReleaseSRWLockExclusive(&m->lock);
        return {0, RestClientTakeError::empty};
    }
    RestEntry& entry = m->rest[m->rest_head];
    if (entry.response.body_bytes > body.count) {
        ReleaseSRWLockExclusive(&m->lock);
        return {entry.response.body_bytes, RestClientTakeError::output_too_small};
    }
    copy_bytes(body.data, m->rest_bodies + entry.offset, entry.response.body_bytes);
    *response = entry.response;
    const u32 bytes = entry.response.body_bytes;
    m->rest_head = (m->rest_head + 1) % kRestCapacity;
    --m->rest_count;
    if (!m->rest_count && !m->media_count)
        ResetEvent(m->response_event);
    ReleaseSRWLockExclusive(&m->lock);
    return {bytes, RestClientTakeError::none};
}

AccountMediaRequestResult queued_media(Module& m, MediaAssetKey key, u64 source_message, u32 revision,
                                       u32 width, u32 height, WorkKind kind, u64 frame_index) noexcept {
    AccountMediaRequestResult result{};
    Work work{};
    work.kind = kind;
    work.key = key;
    work.a = source_message;
    work.b = revision;
    work.c = frame_index;
    work.e = width;
    work.f = height;
    result.state = push_work(m, work) ? AccountMediaRequestState::queued : AccountMediaRequestState::busy;
    return result;
}

AccountMediaRequestResult WINAPI module_media_request(void* instance, MediaAssetKey key, u64, u64) noexcept {
    return queued_media(*static_cast<Module*>(instance), key, 0, 0, 0, 0, WorkKind::media, 0);
}
AccountMediaRequestResult WINAPI module_media_guild_icon_request(void* instance,
                                                                 const AccountMediaGuildIconRequest* request,
                                                                 u64, u64) noexcept {
    if (!request)
        return {};
    return queued_media(*static_cast<Module*>(instance), request->key, 0, 0, 0, 0, WorkKind::media, 0);
}
AccountMediaRequestResult WINAPI module_media_attachment_request(void* instance,
                                                                 const AccountMediaAttachmentRequest* request,
                                                                 u64, u64) noexcept {
    if (!request)
        return {};
    return queued_media(*static_cast<Module*>(instance), request->key, request->source_message_id,
                        request->source_message_revision, request->proxy_width, request->proxy_height,
                        WorkKind::media, 0);
}
AccountMediaRequestResult WINAPI module_media_advance(void* instance, MediaAssetKey key, u16, u32 frame_index,
                                                      u64) noexcept {
    return queued_media(*static_cast<Module*>(instance), key, 0, 0, 0, 0, WorkKind::media_advance, frame_index);
}
bool WINAPI module_media_take(void* instance, AccountMediaFrame* frame) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !frame)
        return false;
    AcquireSRWLockExclusive(&m->lock);
    // The previously taken frame is released by this call.
    if (m->media_has_taken) {
        m->media_head = (m->media_head + 1) % kMediaSlots;
        --m->media_count;
        m->media_has_taken = false;
    }
    if (!m->media_count) {
        if (!m->rest_count)
            ResetEvent(m->response_event);
        ReleaseSRWLockExclusive(&m->lock);
        return false;
    }
    *frame = m->media[m->media_head].frame;
    m->media_has_taken = true;
    ReleaseSRWLockExclusive(&m->lock);
    return true;
}

u32 WINAPI module_runtime_bytes() noexcept {
    return sizeof(Module);
}

AccountModuleStatus WINAPI module_stop(void** instance, u32 timeout_ms, u64, u64) noexcept {
    AccountModuleStatus status{};
    auto* m = instance ? static_cast<Module*>(*instance) : nullptr;
    if (!m)
        return status;
    SetEvent(m->stop_event);
    SetEvent(m->churn_stop);
    HANDLE threads[3] = {m->ready_thread, m->work_thread, m->churn_thread};
    for (HANDLE thread : threads)
        if (thread) {
            WaitForSingleObject(thread, timeout_ms);
            CloseHandle(thread);
        }
    HANDLE events[5] = {m->gateway_event, m->response_event, m->work_event, m->stop_event, m->churn_stop};
    for (HANDLE handle : events)
        if (handle)
            CloseHandle(handle);
    g_module = nullptr;
    *instance = nullptr;
    status.coordinator_phase = static_cast<u8>(UserAccountCoordinatorPhase::stopped);
    return status;
}

// Typed zero results for every entry the scenarios never exercise. REST
// submissions answer with a 404 so the client's single-flight lanes free up;
// a null request pointer (cancellation) answers nothing.
u64 g_stub_calls[160];

template <typename T> u64 pick_u64(T, u64 previous) noexcept { return previous; }
inline u64 pick_u64(u64 value, u64) noexcept { return value; }
template <typename T> bool null_request(T) noexcept { return false; }
template <typename T> bool null_request(T* pointer) noexcept { return pointer == nullptr; }
template <typename T> bool null_request(const T* pointer) noexcept { return pointer == nullptr; }

template <typename A, typename B> inline constexpr bool kSame = false;
template <typename A> inline constexpr bool kSame<A, A> = true;

template <u32 Slot, typename F> struct Stub;
template <u32 Slot, typename R, typename... A> struct Stub<Slot, R (*)(A...) noexcept> {
    static R WINAPI call(A... arguments) noexcept {
        ((void)arguments, ...);
        ++g_stub_calls[Slot];
        if (g_module) {
            ++g_module->counters.stub_calls;
            g_module->counters.last_stub_slot = Slot;
        }
        if constexpr (kSame<R, void>) {
            return;
        } else {
            if constexpr (kSame<R, RestClientSubmitResult>) {
                u64 request_id = 0;
                bool cancel = false;
                bool first = true;
                ((request_id = pick_u64(arguments, request_id),
                  cancel = cancel || (!first && null_request(arguments)), first = false),
                 ...);
                if (g_module && request_id && !cancel) {
                    Work work{};
                    work.kind = WorkKind::rest_error;
                    work.request_id = request_id;
                    push_work(*g_module, work);
                }
            }
            return R{};
        }
    }
};

} // namespace

bool fill_api(AccountModuleApi* api) noexcept {
    *api = {};
    api->version = kAccountModuleApiVersion;
    api->bytes = sizeof(AccountModuleApi);
    api->start = module_start;
    api->pump = module_pump;
    api->notification_handles = module_notification_handles;
    api->gateway_source = module_gateway_source;
    api->gateway_pop = module_gateway_pop;
    api->guild_subscribe = module_guild_subscribe;
    api->gateway_refresh = module_gateway_refresh;
    api->history_submit = module_history_submit;
    api->rest_take = module_rest_take;
    api->media_request = module_media_request;
    api->media_guild_icon_request = module_media_guild_icon_request;
    api->media_attachment_request = module_media_attachment_request;
    api->media_advance = module_media_advance;
    api->media_take = module_media_take;
    api->runtime_bytes = module_runtime_bytes;
    api->stop = module_stop;
    api->guild_members_request = module_guild_members_request;
#define MC_STUB(slot, field) api->field = Stub<slot, decltype(api->field)>::call
    MC_STUB(1, private_conversation);
    MC_STUB(2, attachment_upload);
    MC_STUB(3, upload);
    MC_STUB(4, message_submit);
    MC_STUB(5, message_delete);
    MC_STUB(6, message_edit);
    MC_STUB(7, message_reaction);
    MC_STUB(8, reaction_users);
    MC_STUB(9, message_typing);
    MC_STUB(10, media_url_refresh);
    MC_STUB(11, login_start);
    MC_STUB(12, login_submit_password);
    MC_STUB(13, login_submit_mfa);
    MC_STUB(14, login_send_sms);
    MC_STUB(15, login_start_remote_auth);
    MC_STUB(16, login_copy_remote_auth_qr);
    MC_STUB(17, login_copy_remote_auth_user);
    MC_STUB(18, login_cancel_remote_auth);
    MC_STUB(19, login_pump);
    MC_STUB(20, login_copy_captcha_secret);
    MC_STUB(21, login_submit_captcha);
    MC_STUB(22, login_copy_webauthn_challenge);
    MC_STUB(23, login_notification_handle);
    MC_STUB(24, login_stop);
    MC_STUB(25, profile_get);
    MC_STUB(26, application_command);
    MC_STUB(27, set_rest_trace);
    MC_STUB(28, login_submit_verification);
    MC_STUB(29, relationships);
    MC_STUB(30, message_ack);
    MC_STUB(31, login_submit_recovery);
    MC_STUB(32, login_start_reset);
    MC_STUB(33, login_submit_reset);
    MC_STUB(34, login_submit_phone_verify);
    MC_STUB(35, login_submit_phone_resend);
    MC_STUB(36, login_take_phone_reset_token);
    MC_STUB(37, video_select);
    MC_STUB(38, video_control);
    MC_STUB(39, video_status);
    MC_STUB(40, video_take);
    MC_STUB(41, video_acknowledge);
    MC_STUB(42, message_search);
    MC_STUB(43, login_start_browser);
    MC_STUB(44, login_copy_browser_url);
    MC_STUB(45, self_status_snapshot);
    api->self_status_submit = module_self_status_submit;
    MC_STUB(47, self_status_complete);
    MC_STUB(48, media_catalog);
    MC_STUB(49, catalog_preview_request);
    MC_STUB(50, gif_favorites);
    MC_STUB(51, channel_overview);
    MC_STUB(52, pinned_messages);
    MC_STUB(53, message_forward);
    MC_STUB(54, download);
    MC_STUB(55, thread_action);
    MC_STUB(56, thread_browser);
    MC_STUB(57, thread_creation);
    MC_STUB(58, forum_post);
    MC_STUB(59, media_local_preview);
    MC_STUB(60, logout);
    MC_STUB(61, bulk_message_ack);
    MC_STUB(62, poll);
    MC_STUB(63, guild_admin);
    MC_STUB(64, voice_request);
    MC_STUB(65, voice_snapshot);
    MC_STUB(66, login_start_captcha_bridge);
    MC_STUB(67, login_captcha_bridge_status);
    MC_STUB(68, login_copy_captcha_bridge_url);
    MC_STUB(69, login_cancel_captcha_bridge);
    MC_STUB(70, voice_user_audio);
    MC_STUB(71, voice_audio_configure);
    MC_STUB(72, voice_audio_status);
    MC_STUB(73, guild_invite);
    MC_STUB(74, emoji_preferences);
    MC_STUB(75, account_settings);
    MC_STUB(76, action_captcha);
    MC_STUB(77, poll_create);
    MC_STUB(78, private_channel);
    MC_STUB(79, custom_status_snapshot);
    MC_STUB(80, custom_status_submit);
    MC_STUB(81, guild_settings);
    MC_STUB(82, mentions);
    MC_STUB(83, guild_assets);
    MC_STUB(84, voice_video_snapshot);
    MC_STUB(85, voice_video_wanted);
    MC_STUB(86, voice_video_device);
    MC_STUB(87, voice_video_frame);
    MC_STUB(88, stream_watch);
    MC_STUB(89, stream_snapshot);
    MC_STUB(90, stream_volume);
    MC_STUB(91, stream_preview);
    MC_STUB(92, stage);
    MC_STUB(93, message_interaction);
    MC_STUB(94, user_social);
    MC_STUB(95, guild_community);
    MC_STUB(96, local_presence);
    MC_STUB(97, activity_sharing);
    MC_STUB(98, soundboard);
    MC_STUB(99, soundboard_audio);
    MC_STUB(100, voice_message);
    MC_STUB(101, profile_edit);
    MC_STUB(102, profile_image);
    MC_STUB(103, guild_moderation);
    MC_STUB(104, guild_members_query_cancel);
#undef MC_STUB
    api->decoders = {
        gateway_decode_message_create,      gateway_decode_message_create, gateway_decode_message_create,
        gateway_decode_message_create,      gateway_decode_message_snapshot_core,
        gateway_extract_message_snapshot,   gateway_decode_message_poll,
        gateway_client_route_channel_create, gateway_client_route_private_channel,
    };
    api->cdn_urls = {
        parse_signed_attachment_url,   parse_media_url,
        catalog_provider_target_valid, sticker_json_target_valid,
        soundboard_target_valid,       parse_catalog_preview_url,
        catalog_favorite_format,       media_url_log_fields,
        write_media_proxy_target,      write_custom_emoji_cdn_target,
        cdn_guild_icon_variant,        write_guild_icon_cdn_target,
        write_user_avatar_cdn_target,  write_channel_icon_cdn_target,
        write_sticker_cdn_target,      cdn_host_name,
    };
    return true;
}

bool control(const PerfAccountControl* control) noexcept {
    Module* m = g_module;
    if (!m || !control)
        return false;
    switch (control->command) {
    case PerfAccountCommand::start_churn:
        if (m->churn_thread)
            return false;
        m->churn = *control;
        ResetEvent(m->churn_stop);
        m->churn_thread = CreateThread(nullptr, 256 * 1024, churn_thread, m, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        return m->churn_thread != nullptr;
    case PerfAccountCommand::stop_churn:
        if (!m->churn_thread)
            return true;
        SetEvent(m->churn_stop);
        WaitForSingleObject(m->churn_thread, INFINITE);
        CloseHandle(m->churn_thread);
        m->churn_thread = nullptr;
        return true;
    case PerfAccountCommand::message_create: {
        Work work{};
        work.kind = WorkKind::churn_tick;
        work.a = control->channel_id;
        work.b = control->guild_id;
        work.c = 100'000 + m->churn_counter++;
        return push_work(*m, work);
    }
    }
    return false;
}

bool counters(PerfAccountCounters* output) noexcept {
    if (!output)
        return false;
    *output = g_module ? g_module->counters : PerfAccountCounters{};
    output->module_base = reinterpret_cast<u64>(g_region.base);
    output->module_reserved_bytes = g_region.reserved;
    output->module_committed_bytes = g_region.committed;
    return true;
}

} // namespace maicord::perf

extern "C" bool WINAPI maicord_account_get_api(maicord::u32 requested_version,
                                               maicord::AccountModuleApi* api) noexcept {
    return api && requested_version == maicord::kAccountModuleApiVersion && maicord::perf::fill_api(api);
}
extern "C" bool WINAPI maicord_perf_account_control(const maicord::perf::PerfAccountControl* control) noexcept {
    return maicord::perf::control(control);
}
extern "C" bool WINAPI maicord_perf_account_counters(maicord::perf::PerfAccountCounters* output) noexcept {
    return maicord::perf::counters(output);
}
extern "C" BOOL WINAPI maicord_perf_account_entry(HINSTANCE instance, DWORD reason, void*) noexcept {
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
