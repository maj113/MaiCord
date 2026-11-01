#include "perf_account_internal.hpp"
#include "perf_shared.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord::perf {
namespace perf_account_detail {

Region g_region{};

} // namespace perf_account_detail

namespace {

constexpr u64 kRegionBytes = 320ull * 1024 * 1024;

} // namespace

namespace perf_account_detail {

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

} // namespace perf_account_detail

namespace {

constexpr u64 kMessageSpacingMs = 47'000;

u64 message_time(u32 index) noexcept {
    return kLatestMessageMs - static_cast<u64>(kMessagesPerChannel - 1 - index) * kMessageSpacingMs;
}

} // namespace

namespace perf_account_detail {

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

} // namespace perf_account_detail

namespace {

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

} // namespace

namespace perf_account_detail {

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

} // namespace perf_account_detail

namespace {

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

} // namespace

namespace perf_account_detail {

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

} // namespace perf_account_detail

namespace {

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

} // namespace

namespace perf_account_detail {

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

} // namespace perf_account_detail
} // namespace maicord::perf
