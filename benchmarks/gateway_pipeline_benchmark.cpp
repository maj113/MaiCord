// Offline replay of the account module's Gateway receive path: WebSocket
// framing, zstd-stream decompression, ETF transcoding, envelope scan/decode, READY projection,
// dispatch routing and mailbox publication, drained by a consumer thread
// through the module's own pop. Synthetic, captured-shape payloads only; no
// socket, TLS session, token or profile is used.
//
//   MaiCordGatewayPipelineBenchmark.exe emit <raw-file> large|small
//       writes the deterministic fixture ([u8 phase][u32 bytes][json]...)
//   MaiCordGatewayPipelineBenchmark.exe replay <compressed-file> <runs> large|small
//       replays the zstd-stream chunks ([u8 phase][u32 etf][u32 bytes][z]...):
//       the fixture's payloads as the Gateway's ETF terms, compressed with one
//       shared context and a flush per payload (tools/benchmark_gateway_pipeline.ps1).
#include "../src/app/gateway_client.cpp"
#include "maicord/protocol/writer.hpp"
#include <psapi.h>
#include <shellapi.h>

namespace maicord::gateway_pipeline {
namespace {

// ------------------------------------------------------------------ output
void write_text(const char* text, u32 bytes) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, bytes, &written, nullptr);
}
struct Line {
    char text[1024];
    u32 used = 0;
    explicit Line(const char* label) noexcept { add(label); }
    void add(const char* value) noexcept {
        while (*value && used + 3 < sizeof(text)) text[used++] = *value++;
    }
    Line& field(const char* key, u64 value) noexcept {
        add(" "); add(key); add("=");
        char digits[21];
        u32 count = 0;
        do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
        while (count && used + 3 < sizeof(text)) text[used++] = digits[--count];
        return *this;
    }
    Line& field(const char* key, const char* value) noexcept {
        add(" "); add(key); add("="); add(value);
        return *this;
    }
    void emit() noexcept {
        text[used++] = '\r';
        text[used++] = '\n';
        write_text(text, used);
    }
};

// ----------------------------------------------------------------- fixture
struct Rng {
    u64 state;
    u64 next() noexcept {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 2685821657736338717ull;
    }
    u32 below(u32 bound) noexcept { return static_cast<u32>((next() >> 32) % bound); }
};

// "large": a ~3 MiB READY (70 guilds) and ~1 MiB READY_SUPPLEMENTAL. "small":
// a READY whose compressed payload fits the 128 KiB compressed-message limit.
constexpr u32 kMaximumGuilds = 70, kChannels = 34, kRoles = 22, kEmojis = 28, kStickers = 3,
              kMaximumUsers = 1500, kMaximumFriends = 300, kMaximumPrivate = 220, kBurst = 3200;
constexpr u64 kSelf = 1097750000000000001ull;
u32 guild_count = kMaximumGuilds, user_count = kMaximumUsers, friend_count = kMaximumFriends, direct_count = 200, group_count = 20,
    guild_presence_count = 18;
bool select_preset(const wchar_t* name) noexcept {
    if (name[0] == L'l') return true;
    if (name[0] != L's') return false;
    guild_count = 14; user_count = 360; friend_count = 80; direct_count = 50; group_count = 6; guild_presence_count = 14;
    return true;
}

struct Ids {
    u64 guild[kMaximumGuilds];
    u64 channel[kMaximumGuilds][kChannels];
    u64 role[kMaximumGuilds][kRoles];
    u64 user[kMaximumUsers];
    u64 friend_index[kMaximumFriends];
    u64 direct[kMaximumPrivate];
};
Ids ids;

constexpr const char* kWords[]{
    "the", "stream", "later", "patch", "build", "raid", "tonight", "anyone", "link", "server", "update",
    "lol", "nice", "thanks", "meeting", "docs", "release", "bug", "fixed", "queue", "ranked", "map",
    "clip", "music", "vibes", "coffee", "gg", "wp", "brb", "what", "when", "how", "maybe", "sure",
    "deploy", "rollback", "shader", "texture", "frame", "latency", "packet", "voice", "channel",
    "tomorrow", "weekend", "announcement", "giveaway", "screenshot", "is", "a", "to", "and", "for",
    "with", "this", "that", "it", "on", "in", "of", "you", "we", "they", "should", "could", "really"};
constexpr const char* kSyllables[]{"ka", "ri", "mo", "zen", "lu", "pha", "tor", "vin", "el", "sa", "dra",
                                    "ko", "mi", "nyx", "ash", "qu", "bel", "ro", "ta", "fin"};
constexpr const char* kStatus[]{"online", "idle", "dnd", "online", "online", "offline"};
constexpr char kHex[] = "0123456789abcdef";

struct Json {
    u8* data;
    u32 capacity;
    u32 used;
    bool ok;
    Json& raw(const char* text, u32 bytes) noexcept {
        if (!ok || bytes > capacity - used) { ok = false; return *this; }
        copy_bytes(data + used, text, bytes);
        used += bytes;
        return *this;
    }
    Json& s(const char* text) noexcept {
        u32 bytes = 0;
        while (text[bytes]) ++bytes;
        return raw(text, bytes);
    }
    Json& n(u64 value) noexcept {
        char digits[21];
        u32 count = 0;
        do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
        char ordered[21];
        for (u32 i = 0; i < count; ++i) ordered[i] = digits[count - 1 - i];
        return raw(ordered, count);
    }
    Json& id(u64 value) noexcept { return s("\"").n(value).s("\""); }
    Json& comma(u32 index) noexcept { return index ? s(",") : *this; }
    Json& hex_digits(Rng& rng, u32 count) noexcept {
        char text[64];
        for (u32 i = 0; i < count; ++i) text[i] = kHex[rng.next() >> 60];
        return raw(text, count);
    }
    Json& hex(Rng& rng, u32 count) noexcept { return s("\"").hex_digits(rng, count).s("\""); }
    Json& words(Rng& rng, u32 count) noexcept {
        for (u32 i = 0; i < count; ++i) {
            if (i) s(" ");
            s(kWords[rng.below(sizeof(kWords) / sizeof(kWords[0]))]);
        }
        return *this;
    }
    Json& name(Rng& rng, u32 syllables) noexcept {
        for (u32 i = 0; i < syllables; ++i) s(kSyllables[rng.below(sizeof(kSyllables) / sizeof(kSyllables[0]))]);
        return *this;
    }
    Json& timestamp(Rng& rng) noexcept {
        s("\"20").n(20 + rng.below(6)).s("-0").n(1 + rng.below(9)).s("-1").n(rng.below(10)).s("T1");
        return n(rng.below(10)).s(":3").n(rng.below(10)).s(":2").n(rng.below(10)).s(".").n(100000 + rng.below(899999))
            .s("+00:00\"");
    }
};

u64 snowflake(Rng& rng) noexcept { return 1000000000000000000ull + rng.next() % 300000000000000000ull; }

void make_ids() noexcept {
    Rng rng{0x9E3779B97F4A7C15ull};
    for (u32 g = 0; g < guild_count; ++g) {
        ids.guild[g] = snowflake(rng);
        for (u32 c = 0; c < kChannels; ++c) ids.channel[g][c] = snowflake(rng);
        ids.role[g][0] = ids.guild[g];
        for (u32 r = 1; r < kRoles; ++r) ids.role[g][r] = snowflake(rng);
    }
    for (u32 u = 0; u < user_count; ++u) ids.user[u] = snowflake(rng);
    for (u32 f = 0; f < friend_count; ++f) ids.friend_index[f] = (f * 7) % user_count;
    for (u32 p = 0; p < direct_count + group_count; ++p) ids.direct[p] = snowflake(rng);
}

void user_object(Json& j, Rng& rng, u32 u, bool full = true) noexcept {
    j.s("{\"username\":\"").name(rng, 2 + u % 3).n(u % 97).s("\",\"public_flags\":").n(u % 5 == 0 ? 64 : 0);
    if (!full) {
        j.s(",\"id\":").id(ids.user[u]).s(",\"global_name\":null,\"discriminator\":\"0\",\"avatar\":null}");
        return;
    }
    if (u % 9 == 0)
        j.s(",\"primary_guild\":{\"tag\":\"").name(rng, 2).s("\",\"identity_guild_id\":").id(ids.guild[u % guild_count])
            .s(",\"identity_enabled\":true,\"badge\":").hex(rng, 32).s("}");
    else
        j.s(",\"primary_guild\":null");
    j.s(",\"id\":").id(ids.user[u]).s(",\"global_name\":");
    if (u % 4 == 0) j.s("null");
    else j.s("\"").name(rng, 2).s(u % 7 == 0 ? " \\u2728\"" : " \\u00e9\"");
    j.s(",\"display_name_styles\":null,\"discriminator\":\"0\",\"collectibles\":null,\"clan\":null,\"bot\":")
        .s(u % 53 == 0 ? "true" : "false").s(",\"avatar_decoration_data\":null,\"avatar\":");
    if (u % 6 == 0) j.s("null");
    else j.hex(rng, 32);
    j.s("}");
}

void activities(Json& j, Rng& rng, u32 f) noexcept {
    j.s("\"activities\":[");
    if (f % 4 == 3) { j.s("]"); return; }
    j.s("{\"type\":4,\"state\":\"").words(rng, 3 + f % 5).s("\",\"name\":\"Custom Status\",\"id\":\"custom\",")
        .s("\"emoji\":{\"name\":\"\\ud83d\\udd25\"},\"created_at\":").n(1759800000000ull + rng.below(1000000)).s("}");
    if (f % 3 == 0)
        j.s(",{\"type\":0,\"timestamps\":{\"start\":").n(1759800000000ull + rng.below(1000000))
            .s("},\"state\":\"In a party\",\"session_id\":").hex(rng, 32).s(",\"name\":\"").name(rng, 3)
            .s(" Online\",\"id\":").hex(rng, 16).s(",\"details\":\"").words(rng, 4)
            .s("\",\"created_at\":").n(1759800000000ull + rng.below(1000000)).s(",\"assets\":{\"large_image\":\"")
            .n(snowflake(rng)).s("\",\"large_text\":\"").words(rng, 2).s("\"},\"application_id\":").id(snowflake(rng))
            .s(",\"party\":{\"size\":[2,5],\"id\":").hex(rng, 24).s("}}");
    if (f % 5 == 0)
        j.s(",{\"type\":2,\"timestamps\":{\"start\":1759800000000,\"end\":1759800200000},\"sync_id\":").hex(rng, 22)
            .s(",\"state\":\"").name(rng, 3).s("; ").name(rng, 2).s("\",\"session_id\":").hex(rng, 32)
            .s(",\"party\":{\"id\":\"spotify:").n(ids.user[f % user_count]).s("\"},\"name\":\"Spotify\",\"id\":\"spotify:1\",")
            .s("\"flags\":48,\"details\":\"").words(rng, 3).s("\",\"created_at\":1759800000000,\"assets\":{\"large_text\":\"")
            .words(rng, 2).s("\",\"large_image\":\"spotify:ab67616d0000b273").hex_digits(rng, 24).s("\"}}");
    j.s("]");
}

void presence(Json& j, Rng& rng, u32 u, bool with_guild, u32 g) noexcept {
    j.s("{\"user\":{\"id\":").id(ids.user[u]).s("},\"status\":\"").s(kStatus[u % 5]).s("\"");
    if (with_guild) j.s(",\"guild_id\":").id(ids.guild[g]);
    j.s(",\"client_status\":{").s(u % 3 == 0 ? "\"web\":\"online\"" : "\"desktop\":\"online\"").s("},\"broadcast\":null,");
    activities(j, rng, u);
    j.s("}");
}

void member_object(Json& j, Rng& rng, u32 g, u32 u, bool with_user, bool with_presence) noexcept {
    j.s("{");
    if (with_user) { j.s("\"user\":"); user_object(j, rng, u); j.s(","); }
    else j.s("\"user_id\":").id(ids.user[u]).s(",");
    j.s("\"roles\":[");
    const u32 roles = u % 4;
    for (u32 r = 0; r < roles; ++r) j.comma(r).id(ids.role[g][1 + (u + r * 5) % (kRoles - 1)]);
    j.s("],\"premium_since\":null,\"pending\":false,\"nick\":");
    if (u % 5 == 0) j.s("\"").name(rng, 2).s("\"");
    else j.s("null");
    j.s(",\"mute\":false,\"joined_at\":").timestamp(rng).s(",\"flags\":0,\"deaf\":false,"
        "\"communication_disabled_until\":null,\"banner\":null,\"avatar\":null");
    if (with_presence) { j.s(",\"presence\":"); presence(j, rng, u, false, g); }
    j.s("}");
}

void guild(Json& j, Rng& rng, u32 g) noexcept {
    j.s("{\"version\":").n(1759800000000ull + g).s(",\"threads\":[");
    for (u32 t = 0; t < 2; ++t)
        j.comma(t).s("{\"type\":11,\"total_message_sent\":").n(rng.below(500)).s(",\"thread_metadata\":{\"locked\":false,"
            "\"create_timestamp\":").timestamp(rng).s(",\"auto_archive_duration\":4320,\"archived\":false,"
            "\"archive_timestamp\":").timestamp(rng).s("},\"rate_limit_per_user\":0,\"parent_id\":").id(ids.channel[g][1])
            .s(",\"owner_id\":").id(ids.user[(g + t) % user_count]).s(",\"name\":\"").words(rng, 3).s("\",\"message_count\":")
            .n(rng.below(500)).s(",\"member_count\":").n(rng.below(40)).s(",\"last_message_id\":").id(snowflake(rng))
            .s(",\"id\":").id(snowflake(rng)).s(",\"guild_id\":").id(ids.guild[g]).s(",\"flags\":0}");
    j.s("],\"stickers\":[");
    for (u32 s = 0; s < kStickers; ++s)
        j.comma(s).s("{\"type\":2,\"tags\":\"").words(rng, 1).s("\",\"name\":\"").name(rng, 2).s("\",\"id\":")
            .id(snowflake(rng)).s(",\"guild_id\":").id(ids.guild[g])
            .s(",\"format_type\":1,\"description\":null,\"available\":true,\"asset\":\"\"}");
    j.s("],\"stage_instances\":[],\"roles\":[");
    for (u32 r = 0; r < kRoles; ++r) {
        const u32 color = r ? static_cast<u32>(rng.next() & 0xFFFFFF) : 0;
        j.comma(r).s("{\"version\":").n(1700000000000ull + rng.below(100000000)).s(",\"unicode_emoji\":null,\"tags\":")
            .s(r == kRoles - 1 ? "{\"bot_id\":\"" : "{").s(r == kRoles - 1 ? "1100000000000000001\"}" : "}")
            .s(",\"position\":").n(r).s(",\"permissions\":\"").n(r ? 1071698660929ull >> (r % 7) : 1071698660929ull)
            .s("\",\"name\":\"").s(r ? "" : "@everyone");
        if (r) j.name(rng, 2 + r % 2);
        j.s("\",\"mentionable\":").s(r % 3 ? "false" : "true").s(",\"managed\":").s(r == kRoles - 1 ? "true" : "false")
            .s(",\"id\":").id(ids.role[g][r]).s(",\"icon\":null,\"hoist\":").s(r % 4 == 1 ? "true" : "false")
            .s(",\"flags\":0,\"colors\":{\"tertiary_color\":null,\"secondary_color\":null,\"primary_color\":").n(color)
            .s("},\"color\":").n(color).s("}");
    }
    j.s("],\"properties\":{\"system_channel_id\":").id(ids.channel[g][1]).s(",\"name\":\"").name(rng, 3)
        .s(g % 3 ? " Community\"" : " \\u2728 Hangout\"").s(",\"owner_id\":").id(ids.user[g % user_count]).s(",\"id\":")
        .id(ids.guild[g]).s(",\"icon\":").hex(rng, 32)
        .s(",\"features\":[\"COMMUNITY\",\"NEWS\",\"GUILD_ONBOARDING\",\"ANIMATED_ICON\",\"INVITE_SPLASH\","
           "\"MEMBER_VERIFICATION_GATE_ENABLED\",\"GUILD_ONBOARDING_HAS_PROMPTS\",\"SOUNDBOARD\",\"AUTO_MODERATION\"]"
           ",\"description\":")
        .s(g % 2 ? "null" : "\"A place to hang out and talk about things\"")
        .s(",\"default_message_notifications\":1,\"verification_level\":1,\"explicit_content_filter\":2,"
           "\"mfa_level\":0,\"nsfw_level\":0,\"afk_timeout\":300,\"afk_channel_id\":null,\"premium_tier\":")
        .n(g % 4).s(",\"preferred_locale\":\"en-US\",\"banner\":null,\"splash\":null,\"vanity_url_code\":null,"
                    "\"rules_channel_id\":").id(ids.channel[g][2]).s(",\"public_updates_channel_id\":").id(ids.channel[g][3])
        .s(",\"max_members\":500000,\"max_video_channel_users\":25,\"premium_progress_bar_enabled\":false,"
           "\"safety_alerts_channel_id\":null,\"hub_type\":null,\"incidents_data\":null,"
           "\"latest_onboarding_question_id\":null,\"home_header\":null,\"discovery_splash\":null,\"clan\":null,"
           "\"inventory_settings\":null,\"application_id\":null,\"max_stage_video_channel_users\":50,\"nsfw\":false,"
           "\"system_channel_flags\":0},\"premium_subscription_count\":").n(rng.below(40))
        .s(",\"member_count\":").n(500 + rng.below(90000)).s(",\"lazy\":true,\"large\":true,\"joined_at\":").timestamp(rng)
        .s(",\"id\":").id(ids.guild[g]).s(",\"guild_scheduled_events\":[");
    if (g % 3 == 0)
        for (u32 e = 0; e < 2; ++e)
            j.comma(e).s("{\"id\":").id(snowflake(rng)).s(",\"guild_id\":").id(ids.guild[g])
                .s(",\"channel_id\":null,\"creator_id\":").id(ids.user[e]).s(",\"name\":\"").words(rng, 3)
                .s("\",\"description\":\"").words(rng, 14).s("\",\"scheduled_start_time\":").timestamp(rng)
                .s(",\"scheduled_end_time\":").timestamp(rng).s(",\"privacy_level\":2,\"status\":1,\"entity_type\":3,"
                    "\"entity_id\":null,\"entity_metadata\":{\"location\":\"").words(rng, 2)
                .s("\"},\"user_count\":").n(rng.below(200)).s(",\"image\":null}");
    j.s("],\"emojis\":[");
    for (u32 e = 0; e < kEmojis; ++e)
        j.comma(e).s("{\"version\":0,\"roles\":[],\"require_colons\":true,\"name\":\"").name(rng, 2).n(e)
            .s("\",\"managed\":false,\"id\":").id(snowflake(rng)).s(",\"available\":true,\"animated\":")
            .s(e % 5 ? "false" : "true").s("}");
    j.s("],\"data_mode\":\"full\",\"channels\":[");
    for (u32 c = 0; c < kChannels; ++c) {
        const bool category = c % 9 == 0;
        const bool voice = !category && c % 9 == 8;
        const u32 parent = c - c % 9;
        j.comma(c).s("{\"version\":").n(1700000000000ull + rng.below(100000000)).s(",\"type\":")
            .n(category ? 4 : voice ? 2 : (c % 9 == 1 ? 5 : 0));
        if (!category && !voice) {
            j.s(",\"topic\":");
            if (c % 3) j.s("\"").words(rng, 6 + c % 10).s("\"");
            else j.s("null");
            j.s(",\"rate_limit_per_user\":0,\"nsfw\":false,\"last_message_id\":").id(snowflake(rng));
        }
        if (voice) j.s(",\"user_limit\":0,\"rtc_region\":null,\"bitrate\":64000,\"rate_limit_per_user\":0");
        j.s(",\"position\":").n(c).s(",\"permission_overwrites\":[");
        const u32 overwrites = category ? 3 : 1 + c % 3;
        for (u32 o = 0; o < overwrites; ++o)
            j.comma(o).s("{\"type\":").n(o == 2 ? 1 : 0).s(",\"id\":")
                .id(o == 2 ? ids.user[(g + c) % user_count] : ids.role[g][(o * 7 + c) % kRoles])
                .s(",\"deny\":\"").n(o ? 0 : 1024).s("\",\"allow\":\"").n(o ? 3072 : 0).s("\"}");
        j.s("]");
        if (!category) j.s(",\"parent_id\":").id(ids.channel[g][parent]);
        j.s(",\"name\":\"").name(rng, 2).s(category ? "\"" : "-chat\"").s(",\"id\":").id(ids.channel[g][c])
            .s(",\"flags\":0}");
    }
    j.s("],\"application_command_counts\":{},\"activity_instances\":[],\"voice_states\":[],\"members\":[");
    j.s("{\"user\":{\"id\":").id(kSelf).s(",\"username\":\"self\"},\"roles\":[").id(ids.role[g][1])
        .s("],\"premium_since\":null,\"pending\":false,\"nick\":null,\"mute\":false,\"joined_at\":").timestamp(rng)
        .s(",\"flags\":0,\"deaf\":false,\"communication_disabled_until\":null,\"avatar\":null}]}");
}

void private_channel(Json& j, Rng& rng, u32 p) noexcept {
    if (p < direct_count) {
        j.s("{\"type\":1,\"recipient_ids\":[").id(ids.user[(p * 3) % user_count]).s("],\"last_message_id\":")
            .id(snowflake(rng)).s(",\"is_spam\":false,\"id\":").id(ids.direct[p]).s(",\"flags\":0");
        if (p % 9 == 0) j.s(",\"is_message_request\":true,\"is_message_request_timestamp\":").timestamp(rng);
        j.s("}");
        return;
    }
    j.s("{\"type\":3,\"recipient_ids\":[");
    for (u32 r = 0; r < 5; ++r) j.comma(r).id(ids.user[(p * 5 + r) % user_count]);
    j.s("],\"owner_id\":").id(ids.user[p % user_count]).s(",\"name\":\"").words(rng, 2).s("\",\"last_message_id\":")
        .id(snowflake(rng)).s(",\"id\":").id(ids.direct[p]).s(",\"icon\":null,\"flags\":0}");
}

void ready(Json& j, Rng& rng) noexcept {
    j.s("{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"v\":9,\"users\":[");
    for (u32 u = 0; u < user_count; ++u) { j.comma(u); user_object(j, rng, u); }
    j.s("],\"user_settings_proto\":\"\",\"user_guild_settings\":{\"version\":5,\"partial\":false,\"entries\":[");
    for (u32 g = 0; g < guild_count; ++g) {
        j.comma(g).s("{\"version\":").n(rng.below(100)).s(",\"suppress_roles\":false,\"suppress_everyone\":false,"
            "\"notify_highlights\":0,\"muted\":").s(g % 7 ? "false" : "true")
            .s(",\"mute_scheduled_events\":false,\"mute_config\":null,\"mobile_push\":true,\"message_notifications\":1,"
               "\"hide_muted_channels\":false,\"guild_id\":").id(ids.guild[g]).s(",\"flags\":0,\"channel_overrides\":[");
        for (u32 c = 0; c < 3; ++c)
            j.comma(c).s("{\"muted\":").s(c == 2 ? "true" : "false").s(",\"mute_config\":null,\"message_notifications\":3,"
                "\"collapsed\":false,\"channel_id\":").id(ids.channel[g][1 + c * 4]).s("}");
        j.s("]}");
    }
    j.s("]},\"user\":{\"verified\":true,\"username\":\"self\",\"purchased_flags\":0,\"premium_type\":2,\"premium\":true,"
        "\"phone\":null,\"nsfw_allowed\":true,\"mobile\":false,\"mfa_enabled\":true,\"id\":").id(kSelf)
        .s(",\"global_name\":\"Self\",\"flags\":0,\"email\":\"self@example.invalid\",\"discriminator\":\"0\","
           "\"desktop\":true,\"bio\":\"\",\"banner_color\":null,\"banner\":null,\"avatar_decoration_data\":null,"
           "\"avatar\":").hex(rng, 32).s(",\"accent_color\":null},\"sessions\":[{\"status\":\"online\",\"session_id\":")
        .hex(rng, 32).s(",\"client_info\":{\"version\":0,\"os\":\"windows\",\"client\":\"desktop\"},\"activities\":[]}],"
                        "\"session_type\":\"normal\",\"session_id\":").hex(rng, 32)
        .s(",\"resume_gateway_url\":\"wss://gateway-us-east1-b.discord.gg\",\"relationships\":[");
    for (u32 f = 0; f < friend_count; ++f)
        j.comma(f).s("{\"user_id\":").id(ids.user[ids.friend_index[f]]).s(",\"type\":1,\"since\":").timestamp(rng)
            .s(",\"nickname\":null,\"is_spam_request\":false,\"id\":").id(ids.user[ids.friend_index[f]]).s("}");
    j.s("],\"read_state\":{\"version\":1234,\"partial\":false,\"entries\":[");
    u32 entry = 0;
    for (u32 g = 0; g < guild_count; ++g)
        for (u32 c = 0; c < kChannels; ++c)
            if (c % 9 != 0 && c % 9 != 8)
                j.comma(entry++).s("{\"mention_count\":").n(c == 1 ? rng.below(3) : 0)
                    .s(",\"last_pin_timestamp\":\"1970-01-01T00:00:00+00:00\",\"last_message_id\":").id(snowflake(rng))
                    .s(",\"id\":").id(ids.channel[g][c]).s(",\"flags\":0,\"last_viewed\":").n(3000 + rng.below(100)).s("}");
    for (u32 p = 0; p < direct_count + group_count; ++p)
        j.comma(entry++).s("{\"mention_count\":0,\"last_pin_timestamp\":\"1970-01-01T00:00:00+00:00\","
                           "\"last_message_id\":").id(snowflake(rng)).s(",\"id\":").id(ids.direct[p]).s(",\"flags\":0}");
    j.s("]},\"private_channels\":[");
    for (u32 p = 0; p < direct_count + group_count; ++p) { j.comma(p); private_channel(j, rng, p); }
    j.s("],\"notification_settings\":{\"flags\":16},\"merged_members\":[");
    for (u32 g = 0; g < guild_count; ++g)
        j.comma(g).s("[{\"user_id\":").id(kSelf).s(",\"roles\":[").id(ids.role[g][1]).s("],\"premium_since\":null,"
            "\"pending\":false,\"nick\":null,\"mute\":false,\"joined_at\":").timestamp(rng)
            .s(",\"flags\":0,\"deaf\":false,\"communication_disabled_until\":null,\"banner\":null,\"avatar\":null}]");
    j.s("],\"guilds\":[");
    for (u32 g = 0; g < guild_count; ++g) { j.comma(g); guild(j, rng, g); }
    j.s("],\"guild_join_requests\":[],\"guild_experiments\":[");
    for (u32 e = 0; e < 160; ++e) {
        j.comma(e).s("[").n(rng.next() & 0xFFFFFFFF).s(",null,").n(e % 3).s(",[[[[").n(e % 4).s(",[{\"s\":0,\"e\":")
            .n(1000 + rng.below(9000)).s("}]]],[[1604612045,[[").n(rng.below(10000)).s(",").n(rng.below(10000))
            .s("]]]]]],[],[[").n(rng.next() & 0xFFFF).s(",[[").n(rng.below(100)).s(",").n(rng.below(10000)).s("]]]],0,0]");
    }
    j.s("],\"geo_ordered_rtc_regions\":[\"frankfurt\",\"milan\",\"rotterdam\",\"madrid\",\"london\"],"
        "\"friend_suggestion_count\":0,\"experiments\":[");
    for (u32 e = 0; e < 320; ++e)
        j.comma(e).s("[").n(rng.next() & 0xFFFFFFFF).s(",0,").n(rng.below(3)).s(",-1,0,").n(rng.below(5)).s(",0,0]");
    j.s("],\"country_code\":\"SI\",\"consents\":{\"personalization\":{\"consented\":true}},\"connected_accounts\":[],"
        "\"auth_session_id_hash\":").hex(rng, 44).s(",\"api_code_version\":1,\"analytics_token\":").hex(rng, 60)
        .s("}}");
}

void supplemental(Json& j, Rng& rng, u64 sequence) noexcept {
    j.s("{\"t\":\"READY_SUPPLEMENTAL\",\"s\":").n(sequence).s(",\"op\":0,\"d\":{\"merged_presences\":{\"guilds\":[");
    for (u32 g = 0; g < guild_count; ++g) {
        j.comma(g).s("[");
        for (u32 p = 0; p < guild_presence_count; ++p) {
            const u32 u = (g * 31 + p * 13) % user_count;
            j.comma(p).s("{\"user_id\":").id(ids.user[u]).s(",\"status\":\"").s(kStatus[u % 5])
                .s("\",\"client_status\":{\"desktop\":\"online\"},\"broadcast\":null,");
            activities(j, rng, u);
            j.s("}");
        }
        j.s("]");
    }
    j.s("],\"friends\":[");
    for (u32 f = 0; f < friend_count; ++f) {
        const u32 u = static_cast<u32>(ids.friend_index[f]);
        j.comma(f).s("{\"user_id\":").id(ids.user[u]).s(",\"status\":\"").s(kStatus[f % 6])
            .s("\",\"last_modified\":").n(1759800000000ull + rng.below(1000000))
            .s(",\"client_status\":{\"desktop\":\"online\"},\"broadcast\":null,");
        activities(j, rng, f);
        j.s("}");
    }
    j.s("]},\"merged_members\":[");
    for (u32 g = 0; g < guild_count; ++g) {
        j.comma(g).s("[");
        for (u32 p = 0; p < guild_presence_count; ++p) { j.comma(p); member_object(j, rng, g, (g * 31 + p * 13) % user_count, false, false); }
        j.s("]");
    }
    j.s("],\"lazy_private_channels\":[],\"guilds\":[");
    for (u32 g = 0; g < guild_count; ++g) {
        j.comma(g).s("{\"voice_states\":[");
        const u32 voice = g % 4 == 1 ? 3 : 0;
        for (u32 v = 0; v < voice; ++v)
            j.comma(v).s("{\"user_id\":").id(ids.user[(g + v) % user_count]).s(",\"suppress\":false,\"session_id\":")
                .hex(rng, 32).s(",\"self_video\":false,\"self_mute\":true,\"self_deaf\":false,"
                                "\"request_to_speak_timestamp\":null,\"mute\":false,\"deaf\":false,\"channel_id\":")
                .id(ids.channel[g][8]).s("}");
        j.s("],\"id\":").id(ids.guild[g]).s(",\"embedded_activities\":[],\"activity_instances\":[]}");
    }
    j.s("],\"game_invites\":[],\"disclose\":[\"pomelo\"]}}");
}

void message_body(Json& j, Rng& rng, u32 g, u32 c, u32 u, u32 i, bool create) noexcept {
    j.s("{\"type\":0,\"tts\":false,\"timestamp\":").timestamp(rng).s(",\"pinned\":false,");
    if (create) j.s("\"nonce\":\"").n(snowflake(rng)).s("\",");
    j.s("\"mentions\":[");
    const u32 mentions = i % 6 == 0 ? 1 + i % 3 : 0;
    for (u32 m = 0; m < mentions; ++m) { j.comma(m); user_object(j, rng, (u + m * 17) % user_count); }
    j.s("],\"mention_roles\":[],\"mention_everyone\":false,\"member\":{\"roles\":[").id(ids.role[g][1 + u % (kRoles - 1)])
        .s("],\"premium_since\":null,\"pending\":false,\"nick\":null,\"mute\":false,\"joined_at\":").timestamp(rng)
        .s(",\"flags\":0,\"deaf\":false,\"communication_disabled_until\":null,\"banner\":null,\"avatar\":null},\"id\":")
        .id(snowflake(rng)).s(",\"flags\":0,\"embeds\":[");
    if (i % 5 == 0)
        j.s("{\"type\":\"article\",\"url\":\"https://example.invalid/").words(rng, 1).s("/").n(rng.below(100000))
            .s("\",\"title\":\"").words(rng, 7).s("\",\"description\":\"").words(rng, 28)
            .s("\",\"thumbnail\":{\"width\":1200,\"url\":\"https://example.invalid/i/").hex_digits(rng, 16).s("\"")
            .s(",\"proxy_url\":\"https://images-ext-1.discordapp.net/external/").hex_digits(rng, 40).s("\"")
            .s(",\"height\":630,\"flags\":0,\"content_type\":\"image/jpeg\",\"placeholder\":").hex(rng, 28)
            .s(",\"placeholder_version\":1},\"provider\":{\"name\":\"Example\"},\"content_scan_version\":2}");
    j.s("],\"edited_timestamp\":null,\"content\":\"");
    j.words(rng, 4 + rng.below(30));
    if (i % 7 == 0) j.s(" <@").n(ids.user[(u + 3) % user_count]).s(">");
    if (i % 11 == 0) j.s(" \\ud83d\\ude02\\ud83d\\udc4d");
    j.s("\",\"components\":[],\"channel_type\":0,\"channel_id\":").id(ids.channel[g][c]).s(",\"author\":");
    user_object(j, rng, u);
    j.s(",\"attachments\":[");
    if (i % 9 == 0)
        j.s("{\"width\":1920,\"url\":\"https://cdn.discordapp.com/attachments/").n(ids.channel[g][c]).s("/")
            .n(snowflake(rng)).s("/image.png?ex=").hex_digits(rng, 8).s("\"").s(",\"size\":").n(100000 + rng.below(900000))
            .s(",\"proxy_url\":\"https://media.discordapp.net/attachments/").n(ids.channel[g][c]).s("/")
            .n(snowflake(rng)).s("/image.png\",\"placeholder_version\":1,\"placeholder\":").hex(rng, 28)
            .s(",\"id\":").id(snowflake(rng)).s(",\"height\":1080,\"filename\":\"image.png\",\"content_type\":\"image/png\","
                                                 "\"content_scan_version\":2}");
    j.s("],\"guild_id\":").id(ids.guild[g]).s("}");
}

u32 burst_message(Json& j, Rng& rng, u32 i, u64 sequence) noexcept {
    const u32 g = rng.below(guild_count), u = rng.below(user_count);
    const u32 c = 1 + rng.below(7);
    const u32 pick = rng.below(100);
    if (pick < 45) {
        j.s("{\"t\":\"PRESENCE_UPDATE\",\"s\":").n(sequence).s(",\"op\":0,\"d\":");
        presence(j, rng, u, true, g);
        j.s("}");
        return 0;
    }
    if (pick < 65) {
        j.s("{\"t\":\"TYPING_START\",\"s\":").n(sequence).s(",\"op\":0,\"d\":{\"user_id\":").id(ids.user[u])
            .s(",\"timestamp\":").n(1760000000 + i).s(",\"member\":");
        member_object(j, rng, g, u, true, false);
        j.s(",\"channel_id\":").id(ids.channel[g][c]).s(",\"guild_id\":").id(ids.guild[g]).s("}}");
        return 1;
    }
    if (pick < 85) {
        j.s("{\"t\":\"MESSAGE_CREATE\",\"s\":").n(sequence).s(",\"op\":0,\"d\":");
        message_body(j, rng, g, c, u, i, true);
        j.s("}");
        return 2;
    }
    if (pick < 90) {
        j.s("{\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"s\":").n(sequence).s(",\"op\":0,\"d\":{\"ops\":[");
        if (i % 5 == 0) {
            j.s("{\"range\":[0,99],\"op\":\"SYNC\",\"items\":[{\"group\":{\"id\":").id(ids.role[g][1])
                .s(",\"count\":4}}");
            for (u32 m = 0; m < 98; ++m) {
                if (m == 4) j.s(",{\"group\":{\"id\":\"online\",\"count\":").n(1000 + rng.below(5000)).s("}}");
                j.s(",{\"member\":");
                member_object(j, rng, g, (u + m * 11) % user_count, true, true);
                j.s("}");
            }
            j.s("]}");
        } else {
            j.s("{\"op\":\"").s(i % 3 ? "UPDATE" : "INSERT").s("\",\"item\":{\"member\":");
            member_object(j, rng, g, u, true, true);
            j.s("},\"index\":").n(1 + rng.below(98)).s("}");
            if (i % 2) j.s(",{\"op\":\"DELETE\",\"index\":").n(1 + rng.below(98)).s("}");
        }
        j.s("],\"online_count\":").n(1000 + rng.below(5000)).s(",\"member_count\":").n(5000 + rng.below(50000))
            .s(",\"id\":\"everyone\",\"guild_id\":").id(ids.guild[g]).s(",\"groups\":[{\"id\":").id(ids.role[g][1])
            .s(",\"count\":4},{\"id\":\"online\",\"count\":").n(1000 + rng.below(5000))
            .s("},{\"id\":\"offline\",\"count\":").n(4000 + rng.below(40000)).s("}]}}");
        return 3;
    }
    if (pick < 97) {
        j.s("{\"t\":\"MESSAGE_REACTION_ADD\",\"s\":").n(sequence).s(",\"op\":0,\"d\":{\"user_id\":").id(ids.user[u])
            .s(",\"type\":0,\"message_id\":").id(snowflake(rng)).s(",\"message_author_id\":").id(ids.user[(u + 1) % user_count])
            .s(",\"member\":");
        member_object(j, rng, g, u, true, false);
        j.s(",\"emoji\":{\"name\":\"\\ud83d\\udc4d\",\"id\":null},\"channel_id\":").id(ids.channel[g][c])
            .s(",\"burst\":false,\"guild_id\":").id(ids.guild[g]).s("}}");
        return 4;
    }
    j.s("{\"t\":\"MESSAGE_UPDATE\",\"s\":").n(sequence).s(",\"op\":0,\"d\":");
    message_body(j, rng, g, c, u, 0, false);
    j.s("}");
    return 5;
}

// Fixture: every payload as one complete envelope, in arrival order.
constexpr u32 kFixtureBytes = 24u * 1024u * 1024u;
constexpr u32 kMaximumMessages = kBurst + 8;
struct Message {
    u32 offset, bytes;
    u8 phase, kind;
};
struct Fixture {
    u8* data;
    u32 used;
    Message messages[kMaximumMessages];
    u32 count;
    u32 kinds[8];
};
Fixture fixture;

bool append(Json& j, u8 phase, u8 kind) noexcept {
    if (!j.ok || fixture.count == kMaximumMessages) return false;
    fixture.messages[fixture.count++] = {fixture.used, j.used, phase, kind};
    fixture.used += j.used;
    return true;
}
Json writer_at_end() noexcept { return {fixture.data + fixture.used, kFixtureBytes - fixture.used, 0, true}; }

bool build_fixture() noexcept {
    fixture.data = static_cast<u8*>(VirtualAlloc(nullptr, kFixtureBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!fixture.data) return false;
    make_ids();
    Rng rng{0xD1B54A32D192ED03ull};
    Json first = writer_at_end();
    ready(first, rng);
    if (!append(first, 0, 7)) return false;
    Json second = writer_at_end();
    supplemental(second, rng, 2);
    if (!append(second, 1, 8)) return false;
    for (u32 i = 0; i < kBurst; ++i) {
        Json j = writer_at_end();
        const u32 kind = burst_message(j, rng, i, 3 + i);
        if (!append(j, 2, static_cast<u8>(kind))) return false;
        ++fixture.kinds[kind];
    }
    return true;
}

// ------------------------------------------------------------------- files
bool write_file(const wchar_t* path, const u8* bytes, u32 count) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes, count, &written, nullptr) && written == count;
    CloseHandle(file);
    return ok;
}
u8* read_file(const wchar_t* path, u32* bytes) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size{};
    u8* data = nullptr;
    DWORD read = 0;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 256ll * 1024 * 1024) {
        data = static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(size.QuadPart), MEM_RESERVE | MEM_COMMIT,
                                             PAGE_READWRITE));
        if (data && (!ReadFile(file, data, static_cast<DWORD>(size.QuadPart), &read, nullptr) ||
                     read != static_cast<DWORD>(size.QuadPart))) {
            VirtualFree(data, 0, MEM_RELEASE);
            data = nullptr;
        }
    }
    CloseHandle(file);
    *bytes = read;
    return data;
}
u32 load_u32(const u8* bytes) noexcept {
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) | (static_cast<u32>(bytes[2]) << 16) |
           (static_cast<u32>(bytes[3]) << 24);
}
void store_u32(u8* bytes, u32 value) noexcept {
    for (u32 i = 0; i < 4; ++i) bytes[i] = static_cast<u8>(value >> (i * 8));
}

int emit(const wchar_t* path) noexcept {
    if (!build_fixture()) return 2;
    const u32 bytes = fixture.used + fixture.count * 5;
    u8* output = static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!output) return 3;
    u32 cursor = 0;
    for (u32 i = 0; i < fixture.count; ++i) {
        const auto& message = fixture.messages[i];
        output[cursor++] = message.phase;
        store_u32(output + cursor, message.bytes);
        cursor += 4;
        copy_bytes(output + cursor, fixture.data + message.offset, message.bytes);
        cursor += message.bytes;
    }
    return write_file(path, output, cursor) ? 0 : 4;
}

// ------------------------------------------------------------------ replay
struct Chunk {
    const u8* data;
    u32 bytes, raw_bytes;
    u8 phase;
};
Chunk chunks[kMaximumMessages];
u32 chunk_count;

// Wire stream: each compressed payload as one unmasked binary server frame,
// delivered to the session in TLS-record pieces.
constexpr u32 kWireMessageBytes = ~0u;
constexpr u32 kRecordBytes = 16 * 1024;
constexpr u32 kReceiveBytes = 64 * 1024;
u8* wire;
u32 wire_offsets[kMaximumMessages + 1];

bool build_wire() noexcept {
    u32 total = 0;
    for (u32 i = 0; i < chunk_count; ++i) total += chunks[i].bytes + (chunks[i].bytes / kWireMessageBytes + 1) * 10;
    wire = static_cast<u8*>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!wire) return false;
    u32 cursor = 0;
    for (u32 i = 0; i < chunk_count; ++i) {
        wire_offsets[i] = cursor;
        u32 offset = 0;
        while (offset < chunks[i].bytes) {
            const u32 part = minimum(chunks[i].bytes - offset, kWireMessageBytes);
            wire[cursor++] = 0x82;
            if (part < 126) wire[cursor++] = static_cast<u8>(part);
            else if (part < 65536) {
                wire[cursor++] = 126;
                wire[cursor++] = static_cast<u8>(part >> 8);
                wire[cursor++] = static_cast<u8>(part);
            } else {
                wire[cursor++] = 127;
                for (u32 shift = 56; shift < 64; shift -= 8)
                    wire[cursor++] = static_cast<u8>(static_cast<u64>(part) >> shift);
            }
            copy_bytes(wire + cursor, chunks[i].data + offset, part);
            cursor += part;
            offset += part;
        }
    }
    wire_offsets[chunk_count] = cursor;
    return true;
}

// Consumer: the UI side of the mailbox, through the module's pop.
struct Consumer {
    GatewayClient* client;
    HANDLE thread;
    volatile LONG stop;
    volatile LONG64 events;
    volatile LONG64 payload_bytes;
    volatile LONG64 wakes;
    volatile LONG64 last_event_counter;
    volatile LONG errors;
    GatewayClientEvent first_error;
    u32 kinds[128];
    u8 scratch[kGatewayClientDispatchPayloadCapacity];
};
Consumer consumer;

DWORD WINAPI consumer_main(void*) noexcept {
    for (;;) {
        WaitForSingleObject(consumer.client->notification_event, INFINITE);
        InterlockedIncrement64(&consumer.wakes);
        for (;;) {
            GatewayClientEvent event{};
            const auto popped = gateway_client_pop_event_payload(consumer.client, &event,
                                                                 {consumer.scratch, sizeof(consumer.scratch)});
            if (popped.error == GatewayClientPopError::empty) break;
            if (!popped.ok()) { InterlockedIncrement(&consumer.errors); break; }
            if (event.kind == GatewayClientEventKind::dispatch) {
                ++consumer.kinds[static_cast<u8>(event.dispatch_kind) & 127];
                if (event.dispatch_kind == GatewayClientDispatchKind::message_catchup_required ||
                    event.failure_stage != GatewayClientFailureStage::none) {
                    if (!consumer.errors) consumer.first_error = event;
                    InterlockedIncrement(&consumer.errors);
                }
            } else if (event.kind == GatewayClientEventKind::reconnecting ||
                       event.kind == GatewayClientEventKind::failed) {
                if (!consumer.errors) consumer.first_error = event;
                InterlockedIncrement(&consumer.errors);
            }
            InterlockedExchangeAdd64(&consumer.payload_bytes, popped.payload_bytes);
            WriteRelease64(&consumer.last_event_counter, clock_now());
            InterlockedIncrement64(&consumer.events);
        }
        if (ReadAcquire(&consumer.stop)) return 0;
    }
}

// Opt-in sampler: suspends the producer every ~250 us, unwinds it with
// .pdata and counts function-start RVAs of this image per phase (self and
// inclusive). Time in other modules (system calls) is charged to the nearest
// caller in this image. The runner resolves RVAs through the link map.
struct ProfileEntry {
    u32 key, self, inclusive;
};
constexpr u32 kProfileEntries = 8192;
struct PhaseProfile {
    ProfileEntry entries[kProfileEntries];
    u32 samples, outside;
};
PhaseProfile profiles[4];
volatile LONG profile_phase = 3; // 3: not replaying
volatile LONG sampler_stop;
HANDLE sampled_thread;
u64 image_base, image_end;

void profile_count(PhaseProfile& profile, u32 key, u32 self, u32 inclusive) noexcept {
    u32 slot = (key * 2654435761u) & (kProfileEntries - 1);
    for (u32 probe = 0; probe < kProfileEntries; ++probe, slot = (slot + 1) & (kProfileEntries - 1)) {
        ProfileEntry& entry = profile.entries[slot];
        if (entry.key == key || !entry.key) {
            entry.key = key;
            entry.self += self;
            entry.inclusive += inclusive;
            return;
        }
    }
}
u32 primary_begin(u64 base, const RUNTIME_FUNCTION* function) noexcept {
    for (u32 depth = 0; depth < 8 && function; ++depth) {
        if (function->UnwindData & 1) return function->BeginAddress;
        const u8* info = reinterpret_cast<const u8*>(base + function->UnwindData);
        if (!((info[0] >> 3) & 0x4)) return function->BeginAddress;
        const u32 codes = info[2];
        function = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + ((codes + 1) & ~1u) * 2);
    }
    return function ? function->BeginAddress : 0;
}
DWORD WINAPI sampler_main(void*) noexcept {
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    const i64 interval = frequency.QuadPart / 4000;
    QueryPerformanceCounter(&now);
    i64 next = now.QuadPart + interval;
    while (!ReadAcquire(&sampler_stop)) {
        do { YieldProcessor(); QueryPerformanceCounter(&now); } while (now.QuadPart < next);
        next = now.QuadPart + interval;
        const LONG phase = ReadAcquire(&profile_phase);
        if (phase > 2) continue;
        if (SuspendThread(sampled_thread) == static_cast<DWORD>(-1)) continue;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        u32 keys[32];
        u32 frames = 0;
        bool outside_first = false;
        if (GetThreadContext(sampled_thread, &context)) {
            for (u32 depth = 0; depth < 48 && frames < 32 && context.Rip; ++depth) {
                const u64 rip = context.Rip;
                DWORD64 base = 0;
                const RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(rip, &base, nullptr);
                if (rip >= image_base && rip < image_end && function)
                    keys[frames++] = primary_begin(base, function);
                else if (!frames)
                    outside_first = true;
                if (!function) {
                    context.Rip = *reinterpret_cast<const u64*>(context.Rsp);
                    context.Rsp += 8;
                    continue;
                }
                void* handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, rip, const_cast<RUNTIME_FUNCTION*>(function), &context,
                                 &handler_data, &establisher, nullptr);
            }
        }
        ResumeThread(sampled_thread);
        PhaseProfile& profile = profiles[phase];
        ++profile.samples;
        if (outside_first) ++profile.outside;
        for (u32 i = 0; i < frames; ++i) {
            bool repeated = false;
            for (u32 j = 0; j < i && !repeated; ++j) repeated = keys[j] == keys[i];
            profile_count(profile, keys[i], i == 0 ? 1u : 0u, repeated ? 0u : 1u);
        }
    }
    return 0;
}
void emit_profile() noexcept {
    for (u32 phase = 0; phase < 3; ++phase) {
        const PhaseProfile& profile = profiles[phase];
        Line("profile").field("phase", phase).field("samples", profile.samples).field("outside", profile.outside).emit();
        for (u32 i = 0; i < kProfileEntries; ++i) {
            const auto& entry = profile.entries[i];
            if (entry.key && entry.inclusive * 200 >= profile.samples)
                Line("hot").field("phase", phase).field("rva", entry.key).field("self", entry.self)
                    .field("inclusive", entry.inclusive).emit();
        }
    }
}

GatewayClient client;
GatewayClientRuntime* runtime;

bool start_runtime() noexcept {
    client = GatewayClient{};
    client.runtime = VirtualAlloc(nullptr, kGatewayUserClientRuntimeBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!client.runtime) return false;
    runtime = static_cast<GatewayClientRuntime*>(client.runtime);
    runtime->allocation_bytes = kGatewayUserClientRuntimeBytes;
    runtime->identity = GatewayClientIdentity::normal_user;
    runtime->owner = &client;
    runtime->user = reinterpret_cast<GatewayUserRuntime*>(static_cast<u8*>(client.runtime) + kGatewayUserRuntimeOffset);
    if (!stable_storage_initialize(&runtime->structural_scratch, kStructuralCount, kInitialStructuralCount,
                                   sizeof(u32), alignof(u32)) ||
        !reserved_bytes_initialize(&runtime->large_record, kGatewayClientGuildRecordCapacity) ||
        !stable_storage_initialize(&runtime->decoded_message, kGatewaySessionMaximumDecodedMessageBytes,
                                   kInitialDecodedMessageBytes, sizeof(u8), alignof(u8)) ||
        !stable_storage_initialize(&runtime->ready_record_index, kStructuralCount, kInitialStructuralCount,
                                   sizeof(u32), alignof(u32)) ||
        !reserved_bytes_initialize(&runtime->ready_staging, kReadyStagingBytes) ||
        !reserved_bytes_initialize(&runtime->compressed_spill, kGatewaySessionMaximumDecodedMessageBytes) ||
        !stable_storage_initialize(&runtime->zstd_storage, kZstdStorageBytes, kZstdInitialStorageBytes, sizeof(u8),
                                   alignof(u8)))
        return false;
    client.state_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client.notification_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    runtime->mailbox_space = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    runtime->mailbox_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!client.state_event || !client.notification_event || !runtime->mailbox_space || !runtime->mailbox_cancel)
        return false;
    runtime->authenticated = true;
    const u8 key[kWebSocketClientKeyBytes]{};
    if (!initialize_session(runtime, key)) return false;
    runtime->session.opened = true;
    // HELLO and IDENTIFY have happened; the next payload is READY.
    runtime->session.lifecycle.phase = GatewayLifecyclePhase::awaiting_session;
    return true;
}

void stop_runtime() noexcept {
    CloseHandle(runtime->mailbox_space);
    CloseHandle(runtime->mailbox_cancel);
    CloseHandle(client.notification_event);
    CloseHandle(client.state_event);
    release_runtime_allocation(&client);
    client = GatewayClient{};
    runtime = nullptr;
}

u64 thread_cycles(HANDLE thread) noexcept {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(thread, &cycles);
    return cycles;
}
u64 page_faults() noexcept {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    return counters.PageFaultCount;
}

struct PhaseTotals {
    u64 producer_minimum;
    u64 producer_cycles, consumer_cycles, wall_us, latency_us, events, payload_bytes, wakes, faults, raw_bytes,
        compressed_bytes, messages;
};
constexpr u32 kPhases = 3;
constexpr const char* kPhaseNames[kPhases]{"ready", "supplemental", "burst"};

bool replay_once(PhaseTotals (&totals)[kPhases], u64* committed_peak) noexcept {
    if (!start_runtime()) return false;
    consumer.client = &client;
    consumer.stop = 0;
    consumer.events = consumer.payload_bytes = consumer.wakes = 0;
    consumer.errors = 0;
    consumer.thread = CreateThread(nullptr, 256 * 1024, consumer_main, nullptr, 0, nullptr);
    if (!consumer.thread) return false;
    const Clock clock = clock_initialize();
    const HANDLE self = GetCurrentThread();
    bool ok = true;
    u32 message = 0;
    for (u32 phase = 0; phase < kPhases && ok; ++phase) {
        const u64 events_before = static_cast<u64>(ReadAcquire64(&consumer.events));
        const u64 bytes_before = static_cast<u64>(ReadAcquire64(&consumer.payload_bytes));
        const u64 wakes_before = static_cast<u64>(ReadAcquire64(&consumer.wakes));
        const u64 consumer_before = thread_cycles(consumer.thread);
        const u64 faults_before = page_faults();
        const u64 producer_before = thread_cycles(self);
        const i64 begin = clock_now();
        WriteRelease(&profile_phase, static_cast<LONG>(phase));
        u64 raw = 0, compressed = 0, messages = 0;
        const u32 first = message;
        while (message < chunk_count && chunks[message].phase == phase) {
            raw += chunks[message].raw_bytes;
            compressed += chunks[message].bytes;
            ++messages;
            ++message;
        }
        // The phase's wire bytes arrive as kReceiveBytes receive completions of
        // kRecordBytes TLS records; the worker handles each record, then
        // signals the consumer once per completion, as the network worker does.
        const u32 end = wire_offsets[message];
        for (u32 offset = wire_offsets[first]; offset < end && ok;) {
            const u32 receive_end = minimum(end, offset + kReceiveBytes);
            for (; offset < receive_end && ok;) {
                const u32 piece = minimum(receive_end - offset, kRecordBytes);
                ok = gateway_session_push(&runtime->session, {wire + offset, piece});
                offset += piece;
                const auto drive = process_session(&client, runtime, GetTickCount64());
                if (drive != PlaintextDrive::receive) {
                    Line("error").field("phase", kPhaseNames[phase]).field("offset", offset)
                        .field("drive", static_cast<u64>(drive))
                        .field("stage", static_cast<u64>(runtime->failure_stage))
                        .field("session", static_cast<u64>(runtime->session_error))
                        .field("websocket", static_cast<u64>(runtime->websocket_error))
                        .field("compression", static_cast<u64>(runtime->compression_error))
                        .field("event", static_cast<u64>(runtime->event_decode_error))
                        .field("read", static_cast<u64>(runtime->json_read_error)).emit();
                    ok = false;
                }
            }
            notify_consumer(&client, runtime);
        }
        WriteRelease(&profile_phase, 3);
        const u64 producer_after = thread_cycles(self);
        const i64 produced = clock_now();
        // Wait (outside the producer's cycle count) until the consumer drained the phase.
        while (spsc_mailbox_size(runtime->events) != 0) SwitchToThread();
        for (u32 spin = 0; spin < 64; ++spin) SwitchToThread();
        const i64 last = ReadAcquire64(&consumer.last_event_counter);
        auto& t = totals[phase];
        t.producer_cycles += producer_after - producer_before;
        if (!t.producer_minimum || producer_after - producer_before < t.producer_minimum)
            t.producer_minimum = producer_after - producer_before;
        t.consumer_cycles += thread_cycles(consumer.thread) - consumer_before;
        t.wall_us += clock_microseconds(clock, begin, produced);
        t.latency_us += clock_microseconds(clock, begin, last > produced ? last : produced);
        t.events += static_cast<u64>(ReadAcquire64(&consumer.events)) - events_before;
        t.payload_bytes += static_cast<u64>(ReadAcquire64(&consumer.payload_bytes)) - bytes_before;
        t.wakes += static_cast<u64>(ReadAcquire64(&consumer.wakes)) - wakes_before;
        t.faults += page_faults() - faults_before;
        t.raw_bytes += raw;
        t.compressed_bytes += compressed;
        t.messages += messages;
    }
    const u64 committed = runtime->decoded_message.arena.committed + runtime->structural_scratch.arena.committed +
                          runtime->ready_record_index.arena.committed + runtime->zstd_storage.arena.committed;
    if (committed > *committed_peak) *committed_peak = committed;
    WriteRelease(&consumer.stop, 1);
    SetEvent(client.notification_event);
    WaitForSingleObject(consumer.thread, INFINITE);
    CloseHandle(consumer.thread);
    if (consumer.errors) {
        const auto& e = consumer.first_error;
        Line("consumer_errors").field("count", static_cast<u64>(consumer.errors))
            .field("sequence", e.sequence).field("kind", static_cast<u64>(e.kind))
            .field("dispatch", static_cast<u64>(e.dispatch_kind)).field("stage", static_cast<u64>(e.failure_stage))
            .field("event", static_cast<u64>(e.event_decode_error)).field("scan", static_cast<u64>(e.json_scan_error))
            .field("read", static_cast<u64>(e.json_read_error)).field("offset", e.route_error_offset)
            .field("detail", e.route_detail)
            .field("fixture_kind", e.sequence && e.sequence <= fixture.count ? fixture.messages[e.sequence - 1].kind : 99u).emit();
        ok = false;
    }
    stop_runtime();
    return ok;
}

// Stage microbenchmarks on the same payloads (single thread, no mailbox).
ZstdStreamDecoder decompressor;
u8* stream_storage;
constexpr u32 kStreamStorageBytes = 64u * 1024u * 1024u;
u8* decoded;
u32 decoded_offsets[kMaximumMessages + 1];
u32* index_storage;
constexpr u32 kIndexCount = kGatewaySessionMaximumStructuralCount;

struct DecodeTotals {
    u64 decompress[3], transcode[3];
    u64 blocks, etf_bytes;
};
bool stage_decode(DecodeTotals* totals) noexcept {
    const HANDLE self = GetCurrentThread();
    if (!zstd_stream_initialize(&decompressor, {stream_storage, kStreamStorageBytes}, kStreamStorageBytes, nullptr,
                                nullptr))
        return false;
    u32 cursor = 0;
    for (u32 i = 0; i < chunk_count; ++i) {
        ConstSlice<u8> etf{};
        const u64 before = thread_cycles(self);
        const auto result = zstd_stream_decompress(&decompressor, {chunks[i].data, chunks[i].bytes},
                                                   kGatewaySessionMaximumDecodedMessageBytes, &etf);
        const u64 middle = thread_cycles(self);
        Slice<u8> output{decoded + cursor, kFixtureBytes - cursor};
        const auto json = etf_to_json(etf, &output, output.count, nullptr, nullptr);
        totals->decompress[chunks[i].phase] += middle - before;
        totals->transcode[chunks[i].phase] += thread_cycles(self) - middle;
        if (!result.ok() || etf.count != chunks[i].raw_bytes || !json.ok()) return false;
        totals->blocks += result.blocks;
        totals->etf_bytes += etf.count;
        decoded_offsets[i] = cursor;
        cursor += json.bytes;
    }
    decoded_offsets[chunk_count] = cursor;
    zstd_stream_clear(&decompressor);
    return true;
}

bool stage_scan(u64* cycles, u64* structurals) noexcept {
    const HANDLE self = GetCurrentThread();
    const u64 before = thread_cycles(self);
    u64 count = 0;
    for (u32 i = 0; i < chunk_count; ++i) {
        const Utf8View text{reinterpret_cast<const char*>(decoded + decoded_offsets[i]),
                            decoded_offsets[i + 1] - decoded_offsets[i]};
        const auto scan = json_scan_structurals_avx1(text, {index_storage, kIndexCount});
        if (!scan.ok()) {
            Line("scan_error").field("message", i).field("error", static_cast<u64>(scan.error))
                .field("offset", scan.error_offset).field("count", scan.count).emit();
            return false;
        }
        count += scan.count;
    }
    *cycles += thread_cycles(self) - before;
    *structurals = count;
    return true;
}

bool stage_envelope(u64* cycles) noexcept {
    const HANDLE self = GetCurrentThread();
    const u64 before = thread_cycles(self);
    for (u32 i = 0; i < chunk_count; ++i) {
        const Utf8View text{reinterpret_cast<const char*>(decoded + decoded_offsets[i]),
                            decoded_offsets[i + 1] - decoded_offsets[i]};
        GatewayEnvelope envelope{};
        if (!gateway_decode_envelope(text, {index_storage, kIndexCount}, 64, &envelope).ok()) return false;
    }
    *cycles += thread_cycles(self) - before;
    return true;
}

int replay(const wchar_t* path, u32 runs, bool profile) noexcept {
    if (!build_fixture()) return 2;
    u32 file_bytes = 0;
    const u8* file = read_file(path, &file_bytes);
    if (!file) return 3;
    for (u32 cursor = 0; cursor < file_bytes;) {
        if (chunk_count == kMaximumMessages || file_bytes - cursor < 9) return 4;
        Chunk& chunk = chunks[chunk_count];
        chunk.phase = file[cursor];
        chunk.raw_bytes = load_u32(file + cursor + 1);
        chunk.bytes = load_u32(file + cursor + 5);
        cursor += 9;
        if (chunk.bytes > file_bytes - cursor) return 4;
        chunk.data = file + cursor;
        cursor += chunk.bytes;
        if (chunk.phase != fixture.messages[chunk_count].phase)
            return 5;
        ++chunk_count;
    }
    if (chunk_count != fixture.count || !build_wire()) return 6;

    u64 largest_compressed = 0;
    for (u32 i = 0; i < chunk_count; ++i) largest_compressed = maximum<u64>(largest_compressed, chunks[i].bytes);
    Line("fixture").field("messages", chunk_count).field("ready_bytes", fixture.messages[0].bytes)
        .field("ready_etf", chunks[0].raw_bytes).field("ready_compressed", chunks[0].bytes).field("supplemental_bytes", fixture.messages[1].bytes)
        .field("supplemental_compressed", chunks[1].bytes).field("raw_total", fixture.used)
        .field("largest_compressed", largest_compressed).field("presence", fixture.kinds[0])
        .field("typing", fixture.kinds[1]).field("message_create", fixture.kinds[2])
        .field("member_list", fixture.kinds[3]).field("reaction", fixture.kinds[4])
        .field("message_update", fixture.kinds[5]).emit();

    Line("runtime").field("committed_bytes", kGatewayUserClientRuntimeBytes)
        .field("mailbox", sizeof(GatewayClientRuntime::events)).field("voice_connection", sizeof(VoiceConnection))
        .field("voice_receive", sizeof(VoiceReceive)).field("voice_send", sizeof(VoiceSend))
        .field("stream", sizeof(GatewayStreamSession)).field("event", sizeof(GatewayClientEvent))
        .field("user", sizeof(GatewayUserRuntime)).emit();
    decoded = static_cast<u8*>(VirtualAlloc(nullptr, kFixtureBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    stream_storage =
        static_cast<u8*>(VirtualAlloc(nullptr, kStreamStorageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    index_storage = static_cast<u32*>(VirtualAlloc(nullptr, kIndexCount * sizeof(u32), MEM_RESERVE | MEM_COMMIT,
                                                   PAGE_READWRITE));
    if (!decoded || !index_storage || !stream_storage) return 7;
    DecodeTotals decoded_totals{};
    u64 scan_cycles = 0, envelope_cycles = 0, structurals = 0, scan_minimum = ~0ull, envelope_minimum = ~0ull;
    for (u32 run = 0; run < runs; ++run) {
        if (!stage_decode(&decoded_totals)) return 8;
        u64 scan = 0, envelope = 0;
        if (!stage_scan(&scan, &structurals)) return 9;
        if (!stage_envelope(&envelope)) return 10;
        scan_cycles += scan;
        envelope_cycles += envelope;
        scan_minimum = minimum(scan_minimum, scan);
        envelope_minimum = minimum(envelope_minimum, envelope);
    }
    const auto& d = decoded_totals;
    Line("stage").field("name", "zstd")
        .field("kcycles_per_run", (d.decompress[0] + d.decompress[1] + d.decompress[2]) / runs / 1000)
        .field("ready_kcycles", d.decompress[0] / runs / 1000).field("supplemental_kcycles", d.decompress[1] / runs / 1000)
        .field("burst_kcycles", d.decompress[2] / runs / 1000).field("etf_bytes", d.etf_bytes / runs)
        .field("blocks", d.blocks / runs).emit();
    Line("stage").field("name", "etf")
        .field("kcycles_per_run", (d.transcode[0] + d.transcode[1] + d.transcode[2]) / runs / 1000)
        .field("ready_kcycles", d.transcode[0] / runs / 1000).field("supplemental_kcycles", d.transcode[1] / runs / 1000)
        .field("burst_kcycles", d.transcode[2] / runs / 1000).field("json_bytes", decoded_offsets[chunk_count]).emit();
    Line("stage").field("name", "scan").field("kcycles_per_run", scan_cycles / runs / 1000)
        .field("min_kcycles", scan_minimum / 1000)
        .field("structurals", structurals).emit();
    Line("stage").field("name", "envelope").field("kcycles_per_run", envelope_cycles / runs / 1000)
        .field("min_kcycles", envelope_minimum / 1000).emit();

    PhaseTotals totals[kPhases]{};
    u64 committed_peak = 0;
    HANDLE sampler = nullptr;
    if (profile) {
        const auto* base = reinterpret_cast<const u8*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        image_base = reinterpret_cast<u64>(base);
        image_end = image_base + nt->OptionalHeader.SizeOfImage;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sampled_thread,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
            return 13;
        sampler = CreateThread(nullptr, 64 * 1024, sampler_main, nullptr, 0, nullptr);
        if (!sampler) return 13;
    }
    for (u32 run = 0; run < runs; ++run)
        if (!replay_once(totals, &committed_peak)) return 12;
    if (sampler) {
        WriteRelease(&sampler_stop, 1);
        WaitForSingleObject(sampler, INFINITE);
        CloseHandle(sampler);
    }
    u64 producer = 0, consumer_total = 0;
    for (u32 phase = 0; phase < kPhases; ++phase) {
        const auto& t = totals[phase];
        producer += t.producer_cycles;
        consumer_total += t.consumer_cycles;
        Line("phase").field("name", kPhaseNames[phase]).field("messages", t.messages / runs)
            .field("raw_bytes", t.raw_bytes / runs).field("compressed_bytes", t.compressed_bytes / runs)
            .field("producer_kcycles", t.producer_cycles / runs / 1000)
            .field("producer_min_kcycles", t.producer_minimum / 1000)
            .field("consumer_kcycles", t.consumer_cycles / runs / 1000).field("produce_us", t.wall_us / runs)
            .field("drained_us", t.latency_us / runs).field("events", t.events / runs)
            .field("payload_bytes", t.payload_bytes / runs).field("consumer_wakes", t.wakes / runs)
            .field("page_faults", t.faults / runs).emit();
    }
    Line("total").field("producer_kcycles", producer / runs / 1000)
        .field("consumer_kcycles", consumer_total / runs / 1000).field("committed_peak", committed_peak).emit();
    if (profile) emit_profile();
    return 0;
}

// Arguments: emit <path> <preset> | replay <path> <runs> <preset> [profile].
int run() noexcept {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments || count < 3) return 1;
    const wchar_t* mode = arguments[1];
    if (mode[0] == L'e') return count >= 4 && select_preset(arguments[3]) ? emit(arguments[2]) : 1;
    if (count < 5 || !select_preset(arguments[4])) return 1;
    u32 runs = 5;
    {
        runs = 0;
        for (const wchar_t* digit = arguments[3]; *digit >= L'0' && *digit <= L'9'; ++digit)
            runs = runs * 10 + static_cast<u32>(*digit - L'0');
        if (!runs || runs > 100) return 1;
    }
    return replay(arguments[2], runs, count >= 6 && arguments[5][0] == L'p');
}

} // namespace
} // namespace maicord::gateway_pipeline

extern "C" void maicord_gateway_pipeline_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::gateway_pipeline::run()));
}
