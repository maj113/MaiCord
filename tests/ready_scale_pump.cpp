// A READY shaped like a large real account (hundreds of guilds, DMs, friends
// with rich presence, scheduled events, voice states, message requests and
// safety warnings) through the production projection, a mailbox-shaped queue
// and the application's message event pump with app-sized stores. Every
// projected event must apply; a defective event is skipped and reported
// without stopping the drain.
#include "../src/app/gateway_ready_projection.hpp"
#include "maicord/app/message_event_pump.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/model/user_activity.hpp"
#include "maicord/protocol/writer.hpp"
#include "fixtures/oversized_guild.hpp"
#include "test_output.hpp"

namespace maicord::ready_scale_pump_test {
u32 failures = 0;
void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}

constexpr u32 kGuilds = 200, kChannelsPerGuild = 5, kDirect = 260, kGroups = 40, kFriends = 420,
              kUsers = 700;
constexpr u64 kSelf = 1, kGuildBase = 100'000, kUserBase = 5'000'000, kPrivateBase = 9'000'000;
constexpr u64 guild_id(u32 g) noexcept { return kGuildBase + g * 100; }
constexpr u64 channel_id(u32 g, u32 c) noexcept { return guild_id(g) + 1 + c; }
constexpr u64 user_id(u32 u) noexcept { return kUserBase + u; }
constexpr u64 private_id(u32 p) noexcept { return kPrivateBase + p; }

// ---------------------------------------------------------------- fixture JSON
char ready_source[6 * 1024 * 1024];
struct Json {
    ProtocolWriter writer{{reinterpret_cast<u8*>(ready_source), sizeof(ready_source)}, 0};
    bool ok = true;
    Json& s(const char* text) noexcept {
        ok = ok && protocol_writer_append_c_string(&writer, text);
        return *this;
    }
    Json& n(u64 value) noexcept {
        ok = ok && protocol_writer_append_u64(&writer, value);
        return *this;
    }
    Json& id(u64 value) noexcept { return s("\"").n(value).s("\""); }
    Json& comma(u32 index) noexcept { return index ? s(",") : *this; }
};

void activities(Json& j, u32 f) noexcept {
    j.s("\"activities\":[");
    if (f % 7 == 6) {
        j.s("]");
        return;
    }
    // Custom status first, as the server sends it.
    j.s("{\"type\":4,\"name\":\"Custom Status\",\"state\":\"Working on things \\u2728 ")
        .n(f)
        .s("\",\"emoji\":{\"name\":\"\\ud83d\\udd25\"},\"created_at\":1759800000000}");
    // A game with every optional part, and strings longer than any budget.
    j.s(",{\"type\":0,\"name\":\"An Extremely Long Game Title That Keeps Going Past Every Reasonable Limit For "
        "Display Purposes Number ")
        .n(f)
        .s(" \\u00e9\\u00e8\\u00ea\\u4e16\\u754c\\u4e16\\u754c\\u4e16\\u754c\",\"application_id\":\"")
        .n(400'000 + f % 50)
        .s("\",\"details\":\"Ranked match on a map with a name that is also quite long indeed, very long, "
           "longer than one hundred and twenty eight bytes for sure \\ud83c\\udfae\\ud83c\\udfae\","
           "\"state\":\"In a party\",\"timestamps\":{\"start\":")
        .n(1'759'800'000'000ull + f)
        .s("},\"party\":{\"id\":\"p\",\"size\":[2,5]},\"assets\":{\"large_image\":\"mp:external/abcdefghijklmnop/"
           "https/media.example.invalid/very/long/path/to/an/image/asset/that/keeps/going/and/going/and/going/"
           "and/going/and/going/and/going/and/going/and/going/and/going/and/going/and/going/and/going/and/"
           "going/image.png\",\"large_text\":\"Season 12\",\"small_image\":\"")
        .n(500'000 + f)
        .s("\",\"small_text\":\"Level 99\"},\"buttons\":[\"Watch\",\"Join the community server now\"],"
           "\"flags\":3,\"created_at\":1759800000001}");
    if (f % 3 == 0)
        j.s(",{\"type\":2,\"name\":\"Spotify\",\"id\":\"spotify:1\",\"details\":\"A Song Title\",\"state\":\"Artist "
            "One; Artist Two\",\"timestamps\":{\"start\":1759800000000,\"end\":1759800200000},\"assets\":"
            "{\"large_image\":\"spotify:ab67616d0000b273\",\"large_text\":\"An Album\"},\"sync_id\":\"x\","
            "\"party\":{\"id\":\"spotify:5000000\"},\"flags\":48}");
    if (f % 5 == 0)
        j.s(",{\"type\":1,\"name\":\"Twitch\",\"url\":\"https://www.twitch.tv/example\",\"details\":\"Live\"}"
            ",{\"type\":3,\"name\":\"A Show\"},{\"type\":5,\"name\":\"A Tournament\"}");
    // Malformed rows are ignored, never fatal.
    if (f % 11 == 0)
        j.s(",{\"type\":0},{\"type\":9,\"name\":\"Future\"},7");
    j.s("]");
}

void guild(Json& j, u32 g) noexcept {
    const bool nested = g % 2 == 1;
    j.s("{\"id\":").id(guild_id(g));
    if (nested)
        j.s(",\"properties\":{\"id\":").id(guild_id(g));
    else
        j.s(",\"id_echo\":0");
    j.s(",\"name\":\"Guild ").n(g).s(" \\u2728\",\"owner_id\":").id(user_id(g % kUsers));
    j.s(",\"icon\":null,\"description\":null,\"verification_level\":1,\"default_message_notifications\":1,"
        "\"explicit_content_filter\":2,\"mfa_level\":0,\"nsfw_level\":0,\"afk_timeout\":300,\"afk_channel_id\":null,"
        "\"system_channel_id\":")
        .id(channel_id(g, 1))
        .s(",\"features\":[\"COMMUNITY\",\"NEWS\",\"GUILD_ONBOARDING\",\"MEMBER_VERIFICATION_GATE_ENABLED\","
           "\"GUILD_ONBOARDING_HAS_PROMPTS\",\"ANIMATED_ICON\",\"MAX_FILE_SIZE_50_MB\"]");
    if (nested)
        j.s("}");
    j.s(",\"member_count\":12345,\"lazy\":true,\"large\":true,\"joined_at\":\"2025-01-01T00:00:00+00:00\"");
    j.s(",\"roles\":[{\"id\":").id(guild_id(g)).s(",\"name\":\"@everyone\",\"permissions\":\"1071698660929\","
                                                  "\"position\":0,\"color\":0,\"hoist\":false,\"managed\":false,"
                                                  "\"mentionable\":false,\"flags\":0}");
    for (u32 r = 1; r < 3; ++r)
        j.s(",{\"id\":").id(guild_id(g) + 50 + r).s(",\"name\":\"Role ").n(r).s("\",\"permissions\":\"8\","
                                                                               "\"position\":")
            .n(r)
            .s(",\"color\":16711680,\"hoist\":true,\"managed\":false,\"mentionable\":true,\"flags\":0}");
    j.s("],\"emojis\":[{\"id\":").id(guild_id(g) + 70).s(",\"name\":\"blob\",\"roles\":[],\"require_colons\":true,"
                                                         "\"managed\":false,\"animated\":false,\"available\":true}]");
    j.s(",\"stickers\":[],\"stage_instances\":[],\"threads\":[]");
    j.s(",\"channels\":[{\"id\":").id(channel_id(g, 0)).s(",\"type\":4,\"name\":\"Text\",\"position\":0,"
                                                          "\"permission_overwrites\":[]}");
    for (u32 c = 1; c < kChannelsPerGuild; ++c) {
        const bool voice = c == kChannelsPerGuild - 1;
        j.s(",{\"id\":").id(channel_id(g, c)).s(",\"type\":").n(voice ? 2 : 0).s(",\"name\":\"channel-").n(c);
        j.s("\",\"position\":").n(c).s(",\"parent_id\":").id(channel_id(g, 0));
        j.s(",\"last_message_id\":").id(channel_id(g, c) * 1000).s(",\"topic\":null,\"nsfw\":false,"
                                                                     "\"rate_limit_per_user\":0,"
                                                                     "\"permission_overwrites\":[{\"id\":")
            .id(guild_id(g))
            .s(",\"type\":0,\"allow\":\"0\",\"deny\":\"2048\"}]}");
    }
    j.s("]");
    // The current member: pending screening, onboarding started.
    j.s(",\"members\":[{\"user\":{\"id\":\"1\",\"username\":\"self\"},\"roles\":[],\"pending\":")
        .s(g % 4 == 0 ? "true" : "false")
        .s(",\"flags\":10,\"joined_at\":\"2025-01-01T00:00:00+00:00\"}]");
    if (g % 4 == 1) {
        j.s(",\"voice_states\":[");
        for (u32 v = 0; v < 3; ++v)
            j.comma(v)
                .s("{\"user_id\":")
                .id(user_id(v + g))
                .s(",\"channel_id\":")
                .id(channel_id(g, kChannelsPerGuild - 1))
                .s(",\"session_id\":\"s\",\"deaf\":false,\"mute\":false,\"self_deaf\":false,\"self_mute\":true,"
                   "\"self_video\":false,\"suppress\":false,\"request_to_speak_timestamp\":null}");
        j.s("]");
    } else
        j.s(",\"voice_states\":[]");
    j.s(",\"guild_scheduled_events\":[");
    if (g % 2 == 0)
        for (u32 e = 0; e < 4; ++e)
            j.comma(e)
                .s("{\"id\":")
                .id(guild_id(g) + 80 + e)
                .s(",\"guild_id\":")
                .id(guild_id(g))
                .s(",\"channel_id\":null,\"creator_id\":")
                .id(user_id(e))
                .s(",\"name\":\"Community night ")
                .n(e)
                .s("\",\"description\":\"A long description of the event that goes on for a while to make the "
                   "array a realistic size \\u2014 bring friends.\",\"scheduled_start_time\":\"2026-10-08T18:00:00"
                   "+00:00\",\"scheduled_end_time\":\"2026-10-08T20:00:00+00:00\",\"privacy_level\":2,\"status\":1,"
                   "\"entity_type\":3,\"entity_id\":null,\"entity_metadata\":{\"location\":\"Somewhere\"},"
                   "\"user_count\":42,\"image\":null}");
    j.s("]}");
}

void private_channel(Json& j, u32 p) noexcept {
    if (p < kDirect) {
        j.s("{\"id\":").id(private_id(p)).s(",\"type\":1,\"last_message_id\":").id(private_id(p) * 10);
        j.s(",\"flags\":0,\"recipient_ids\":[").id(user_id(p % kUsers)).s("]");
        if (p % 5 == 0)
            j.s(",\"is_message_request\":true,\"is_message_request_timestamp\":\"2026-10-01T00:00:00+00:00\","
                "\"is_spam\":")
                .s(p % 10 == 0 ? "true" : "false");
        if (p % 6 == 0)
            j.s(",\"safety_warnings\":[{\"id\":").id(700'000 + p).s(",\"type\":")
                .n(1 + p % 4)
                .s(",\"expiry\":\"2026-11-01T00:00:00+00:00\",\"dismiss_timestamp\":null}]");
        else if (p % 6 == 1)
            j.s(",\"safety_warnings\":[]");
        j.s("}");
        return;
    }
    j.s("{\"id\":").id(private_id(p)).s(",\"type\":3,\"name\":\"Group ").n(p).s("\",\"icon\":null,\"owner_id\":");
    j.id(user_id(p % kUsers)).s(",\"last_message_id\":").id(private_id(p) * 10).s(",\"recipient_ids\":[");
    for (u32 r = 0; r < 4; ++r)
        j.comma(r).id(user_id((p + r) % kUsers));
    j.s("],\"nicks\":[{\"id\":").id(user_id(p % kUsers)).s(",\"nick\":\"Nick \\u00e9\"}]}");
}

Utf8View build_ready() noexcept {
    Json j{};
    j.s("{\"v\":9,\"user\":{\"id\":\"1\",\"username\":\"self\",\"global_name\":\"Self\",\"discriminator\":\"0\","
        "\"avatar\":null,\"premium_type\":2,\"mfa_enabled\":true,\"flags\":0},\"session_id\":\"scale\","
        "\"resume_gateway_url\":\"wss://gateway.discord.gg\",\"session_type\":\"normal\",\"guilds\":[");
    for (u32 g = 0; g < kGuilds; ++g) {
        j.comma(g);
        guild(j, g);
    }
    j.s("],\"users\":[");
    for (u32 u = 0; u < kUsers; ++u)
        j.comma(u)
            .s("{\"id\":")
            .id(user_id(u))
            .s(",\"username\":\"user")
            .n(u)
            .s("\",\"global_name\":\"User \\u00c5\\u00e4 ")
            .n(u)
            .s("\",\"discriminator\":\"0\",\"avatar\":null,\"public_flags\":64,\"bot\":false}");
    j.s("],\"private_channels\":[");
    for (u32 p = 0; p < kDirect + kGroups; ++p) {
        j.comma(p);
        private_channel(j, p);
    }
    j.s("],\"relationships\":[");
    for (u32 f = 0; f < kFriends; ++f)
        j.comma(f).s("{\"id\":").id(user_id(f)).s(",\"type\":1,\"nickname\":null,\"user_id\":").id(user_id(f)).s("}");
    j.s("],\"merged_presences\":{\"friends\":[");
    static const char* status[] = {"online", "idle", "dnd", "offline"};
    for (u32 f = 0; f < kFriends; ++f) {
        j.comma(f).s("{\"user_id\":").id(user_id(f)).s(",\"status\":\"").s(status[f % 4]).s("\",");
        j.s("\"client_status\":{\"desktop\":\"online\"},");
        activities(j, f);
        j.s("}");
    }
    j.s("],\"guilds\":[]},\"read_state\":{\"entries\":[");
    u32 entry = 0;
    for (u32 g = 0; g < kGuilds; ++g)
        for (u32 c = 1; c < kChannelsPerGuild; ++c)
            j.comma(entry++)
                .s("{\"id\":")
                .id(channel_id(g, c))
                .s(",\"last_message_id\":")
                .id(channel_id(g, c) * 1000 - 1)
                .s(",\"mention_count\":")
                .n(c == 1 ? 2 : 0)
                .s("}");
    j.s("],\"partial\":false,\"version\":77},\"user_guild_settings\":{\"entries\":[],\"partial\":false,\"version\":1},"
        "\"notification_settings\":{\"flags\":0},\"user_settings_proto\":\"\"}");
    return j.ok ? Utf8View{ready_source, j.writer.used} : Utf8View{};
}

// ------------------------------------------------------------- mailbox queue
struct Queued {
    GatewayClientEvent event;
    u32 offset, bytes;
};
constexpr u32 kQueueCapacity = 8192;
Queued queue[kQueueCapacity];
u8 queue_payload[16 * 1024 * 1024];
struct Queue {
    u32 count, cursor, used;
    u32 kinds[256];
    bool overflow;
};
Queue mailbox{};
bool enqueue(void* opaque, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    auto& q = *static_cast<Queue*>(opaque);
    if (q.count == kQueueCapacity || payload.count > sizeof(queue_payload) - q.used ||
        payload.count > kGatewayClientGuildRecordCapacity) {
        q.overflow = true;
        return false;
    }
    copy_bytes(queue_payload + q.used, payload.data, payload.count);
    queue[q.count++] = {event, q.used, static_cast<u32>(payload.count)};
    q.used += static_cast<u32>(payload.count);
    if (event.kind == GatewayClientEventKind::dispatch)
        ++q.kinds[static_cast<u8>(event.dispatch_kind)];
    return true;
}
GatewayClientPopResult pop(void* opaque, GatewayClientEvent* event, Slice<u8> output) noexcept {
    auto& q = *static_cast<Queue*>(opaque);
    if (q.cursor == q.count)
        return {0, GatewayClientPopError::empty};
    const auto& item = queue[q.cursor];
    if (item.bytes > output.count)
        return {item.bytes, GatewayClientPopError::output_too_small};
    copy_bytes(output.data, queue_payload + item.offset, item.bytes);
    *event = item.event;
    ++q.cursor;
    return {item.bytes, GatewayClientPopError::none};
}

// ------------------------------------------------------- app-shaped stores
constexpr u32 kGuildCapacity = 256, kChannelCapacity = 4096, kRoleCapacity = 8192,
              kOverwriteCapacity = 32768, kEmojiCapacity = 4096, kEmojiRoleCapacity = 16384,
              kUserCapacity = 4096, kMemberCapacity = 512;
struct Stores {
    GuildRecord guilds[kGuildCapacity];
    u16 guild_order[kGuildCapacity];
    ChannelRecord channels[kChannelCapacity];
    u16 channel_order[kChannelCapacity];
    u8 names[256 * 1024];
    RoleRecord roles[kRoleCapacity];
    PermissionOverwriteRecord overwrites[kOverwriteCapacity];
    CustomEmojiRecord emojis[kEmojiCapacity];
    u64 emoji_roles[kEmojiRoleCapacity];
    ReadStateEntry reads[kChannelCapacity];
    UserRecord users[kUserCapacity];
    u32 user_order[kUserCapacity];
    u8 user_names[128 * 1024];
    GuildMemberRecord members[kMemberCapacity];
    u32 member_order[kMemberCapacity];
    u64 thread_a[kThreadMemberMaximumIds], thread_b[kThreadMemberMaximumIds];
    TypingRecord typing[kTypingRecordCapacity];
    VoiceState voice[kVoiceStateCapacity];
    MessageRecord messages[8];
    u32 message_order[8];
    u8 message_text[1024];
    u32 heights[8];
    u64 fenwick[8];
    u8 pump_scratch[kGatewayClientDispatchPayloadCapacity];
    u64 bulk_ids[kGatewayBulkDeleteMaximumIds];
    u64 guild_ids[kGuildCapacity];

    ChatStateStore state;
    ReadStateStore read_state;
    UserStore user_store;
    PrivateChannelStore private_channels;
    GuildMemberStore member_store;
    ThreadMemberStore thread_members;
    TypingStore typing_store;
    VoiceStateStore voice_states;
    MessageStore message_store;
    UiVariableList list;
    MessageTimeline timeline;
    MessageEventPump pump;
};
Stores stores;

bool initialize(Stores& s) noexcept {
    return chat_state_store_initialize(&s.state, {s.guilds, kGuildCapacity}, {s.guild_order, kGuildCapacity},
                                       {s.channels, kChannelCapacity}, {s.channel_order, kChannelCapacity},
                                       {s.names, sizeof(s.names)}) &&
           chat_state_store_bind_permissions(&s.state, {s.roles, kRoleCapacity},
                                             {s.overwrites, kOverwriteCapacity}) &&
           chat_state_store_bind_emojis_sparse(&s.state, {s.emojis, kEmojiCapacity}, {s.emoji_roles, kEmojiRoleCapacity}) &&
           read_state_initialize(&s.read_state, {s.reads, kChannelCapacity}) &&
           read_state_bind(&s.state, &s.read_state) &&
           user_store_initialize(&s.user_store, {s.users, kUserCapacity}, {s.user_order, kUserCapacity},
                                 {s.user_names, sizeof(s.user_names)}) &&
           private_channel_store_initialize_owned(&s.private_channels, kChannelCapacity, kChannelCapacity * 16) &&
           guild_member_store_initialize(&s.member_store, {s.members, kMemberCapacity},
                                         {s.member_order, kMemberCapacity}) &&
           thread_member_store_initialize(&s.thread_members, {s.thread_a, kThreadMemberMaximumIds},
                                          {s.thread_b, kThreadMemberMaximumIds}) &&
           typing_store_initialize(&s.typing_store, {s.typing, kTypingRecordCapacity}) &&
           voice_state_store_initialize_sparse(&s.voice_states, {s.voice, kVoiceStateCapacity}) &&
           message_store_initialize(&s.message_store, {s.messages, 8}, {s.message_order, 8},
                                    {s.message_text, sizeof(s.message_text)}) &&
           ui_variable_list_initialize(&s.list, {s.heights, 8}, {s.fenwick, 8}, 0, 100) &&
           message_timeline_initialize(&s.timeline, &s.message_store, &s.list, 100, 100, true) &&
           message_event_pump_initialize(&s.pump, &s.timeline, {s.pump_scratch, sizeof(s.pump_scratch)},
                                         {s.bulk_ids, kGatewayBulkDeleteMaximumIds},
                                         MessageContentAvailability::unknown) &&
           (s.state.private_channels = &s.private_channels, true) &&
           message_event_pump_set_state_store(&s.pump, &s.state, {s.guild_ids, kGuildCapacity}) &&
           message_event_pump_set_user_store(&s.pump, &s.user_store) &&
           message_event_pump_set_member_store(&s.pump, &s.member_store) &&
           message_event_pump_set_thread_member_store(&s.pump, &s.thread_members) &&
           message_event_pump_set_typing_store(&s.pump, &s.typing_store) &&
           message_event_pump_set_voice_state_store(&s.pump, &s.voice_states);
}

struct DrainTotals {
    u32 drains, popped, apply_failures, rejected;
    MessageEventPumpFailure first;
    bool ok = true, ready = false, state_incomplete = false;
};
DrainTotals drain_all(Queue* q) noexcept {
    DrainTotals totals{};
    while (q->cursor < q->count && totals.drains < 1000) {
        const auto drained = message_event_pump_drain(&stores.pump, q, pop, kGatewayClientEventCapacity);
        ++totals.drains;
        totals.popped += drained.popped;
        totals.rejected += drained.rejected;
        if (drained.apply_failures && !totals.apply_failures)
            totals.first = drained.failure;
        totals.apply_failures += drained.apply_failures;
        totals.ok = totals.ok && drained.ok();
        totals.ready = totals.ready || drained.ready_applied;
        totals.state_incomplete = totals.state_incomplete || drained.state_incomplete;
        if (!drained.ok())
            break;
    }
    return totals;
}

u32 arrays[kGatewaySessionMaximumStructuralCount];
u32 records[256 * 1024];
char projection_payload[kGatewayClientDispatchPayloadCapacity];
char header[4096];

void scale() noexcept {
    const Utf8View source = build_ready();
    check(source.bytes > 1024 * 1024, "fixture: READY is larger than 1 MiB");
    GatewayReady ready{};
    check(gateway_decode_ready({source, 0, JsonValueKind::object}, {arrays, kGatewaySessionMaximumStructuralCount}, 64,
                               &ready)
              .ok(),
          "fixture: READY decodes");
    const auto projected = gateway_ready_project(
        ready, {{arrays, kGatewaySessionMaximumStructuralCount}, {records, 256 * 1024},
                {projection_payload, sizeof(projection_payload)}, {header, sizeof(header)}},
        40, 1'759'900'000'000ull, enqueue, nullptr, &mailbox);
    check(projected.ok() && !mailbox.overflow, "projection: a large account READY projects completely");
    using K = GatewayClientDispatchKind;
    check(mailbox.kinds[static_cast<u8>(K::guild_create)] == kGuilds &&
              mailbox.kinds[static_cast<u8>(K::channel_create)] == kDirect + kGroups &&
              mailbox.kinds[static_cast<u8>(K::presence_update)] == kFriends &&
              mailbox.kinds[static_cast<u8>(K::guild_scheduled_event_snapshot)] == kGuilds &&
              mailbox.kinds[static_cast<u8>(K::voice_state_snapshot)] >= kGuilds / 4,
          "projection: guilds, DMs, presences, scheduled events and voice states are all published");
    // Every dispatch descriptor describes exactly its own payload.
    u32 mismatched = 0;
    for (u32 i = 0; i < mailbox.count; ++i)
        if (queue[i].event.kind == GatewayClientEventKind::dispatch &&
            queue[i].event.dispatch_payload_bytes != queue[i].bytes)
            ++mismatched;
    check(!mismatched, "projection: dispatch_payload_bytes matches every published payload");

    check(initialize(stores), "stores: app-sized stores initialize");
    const auto totals = drain_all(&mailbox);
    check(totals.ok && totals.ready && mailbox.cursor == mailbox.count && totals.popped == mailbox.count,
          "pump: the whole READY stream drains");
    check(!totals.apply_failures && !totals.state_incomplete && !stores.pump.private_state_incomplete,
          "pump: no READY event is rejected");
    if (totals.apply_failures) {
        // Name the first rejected event for diagnosis.
        char line[160];
        ProtocolWriter w{{reinterpret_cast<u8*>(line), sizeof(line) - 1}, 0};
        protocol_writer_append_c_string(&w, "  first rejected: kind=");
        protocol_writer_append_u64(&w, static_cast<u64>(totals.first.kind));
        protocol_writer_append_c_string(&w, " dispatch=");
        protocol_writer_append_u64(&w, static_cast<u64>(totals.first.dispatch_kind));
        protocol_writer_append_c_string(&w, " reason=");
        protocol_writer_append_c_string(&w, message_event_pump_reject_reason_name(totals.first.reason));
        protocol_writer_append_c_string(&w, " detail=");
        protocol_writer_append_u64(&w, totals.first.detail);
        line[w.used] = 0;
        check(false, line);
    }
    check(stores.state.guild_count == kGuilds &&
              stores.state.channel_count == kGuilds * kChannelsPerGuild + kDirect + kGroups,
          "state: every guild, channel and DM is resident");
    // Rich presence and custom status reached the user store.
    static UserActivitySet rich_storage;
    const auto* rich =
        user_activity_find(stores.user_store.activities, user_id(0), &rich_storage) ? &rich_storage : nullptr;
    const auto* custom = user_appearance_find_activity(stores.user_store.appearance, user_id(1));
    check(rich && rich->count == kUserActivityMaximum && rich->items[0].type == UserActivityType::playing &&
              user_activity_text(rich->items[0], UserActivityField::name).bytes <=
                  kUserActivityFieldBudget[0] &&
              custom && custom->activity.bytes,
          "presence: friends' rich presence and custom status apply, bounded");
    check(!user_activity_count(stores.user_store.activities, user_id(6)),
          "presence: an empty activities list clears rich presence");
    const auto* request = private_channel_store_find(stores.private_channels, private_id(10));
    const auto* warned = private_channel_store_find(stores.private_channels, private_id(6));
    check(request && (request->request_flags & private_request_pending) &&
              (request->request_flags & private_request_spam) && warned && warned->safety_warning_id == 700'006,
          "private: message request and safety warning trailers apply");
    const auto nickname = private_channel_store_nickname(stores.private_channels, private_id(kDirect),
                                                         user_id(kDirect % kUsers));
    check(utf8_equal(nickname, utf8_literal("Nick é")),
          "private: READY wire nickname arrays reach the channel-local store");
    check(stores.voice_states.count == (kGuilds / 4) * 3, "voice: READY voice states apply");
}

bool same_text(const char* a, const char* b) noexcept {
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

GatewayClientEvent dispatch(GatewayClientDispatchKind kind, u32 bytes) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = kind;
    event.dispatch_payload_bytes = bytes;
    event.has_sequence = true;
    return event;
}

void resilience() noexcept {
    // A friend presence whose descriptor omits its payload (the defect this
    // suite guards), a truncated read-state snapshot, then valid events.
    mailbox = {};
    u8 appearance[512];
    UserAppearance custom{};
    custom.user_id = user_id(2);
    custom.fields = appearance_activity;
    const char text[] = "back soon";
    copy_bytes(custom.activity.data, text, sizeof(text) - 1);
    custom.activity.bytes = sizeof(text) - 1;
    const u32 appearance_bytes = user_appearance_write(custom, {appearance, sizeof(appearance)});
    auto broken = dispatch(GatewayClientDispatchKind::presence_update, 0);
    broken.presence.user_id = user_id(2);
    broken.presence.status = GatewayClientMemberListStatus::online;
    broken.sequence = 900;
    auto truncated = dispatch(GatewayClientDispatchKind::read_state_snapshot, 3);
    truncated.read_state.record_count = 1;
    const u8 junk[3]{1, 2, 3};
    auto fixed = broken;
    fixed.dispatch_payload_bytes = appearance_bytes;
    fixed.sequence = 902;
    GatewayClientEvent offline = dispatch(GatewayClientDispatchKind::presence_update, 0);
    offline.presence.user_id = user_id(3);
    offline.presence.status = GatewayClientMemberListStatus::offline;
    check(appearance_bytes && enqueue(&mailbox, broken, {appearance, appearance_bytes}) &&
              enqueue(&mailbox, truncated, {junk, 3}) && enqueue(&mailbox, fixed, {appearance, appearance_bytes}) &&
              enqueue(&mailbox, offline, {}),
          "resilience: fixture queues");
    const u32 rejected_before = stores.pump.rejected;
    const auto drained = message_event_pump_drain(&stores.pump, &mailbox, pop, kGatewayClientEventCapacity);
    check(drained.ok() && drained.popped == 4 && drained.apply_failures == 2 && mailbox.cursor == 4,
          "resilience: rejected events are skipped and the drain continues");
    check(drained.failure.kind == GatewayClientEventKind::dispatch &&
              drained.failure.dispatch_kind == GatewayClientDispatchKind::presence_update &&
              drained.failure.reason == MessageEventPumpRejectReason::descriptor_payload_mismatch &&
              drained.failure.sequence == 900 && drained.failure.payload_bytes == appearance_bytes,
          "resilience: the first skipped event is described by kind, reason and size");
    check(drained.state_incomplete && stores.pump.rejected == rejected_before + 2,
          "resilience: a rejected read-state snapshot marks account state incomplete");
    const auto* applied = user_appearance_find_activity(stores.user_store.appearance, user_id(2));
    check(applied && utf8_equal(applied->activity.view(), utf8_literal("back soon")),
          "resilience: events after a skipped one still apply");
    check(same_text(message_event_pump_reject_reason_name(MessageEventPumpRejectReason::guild_state), "guild_state") &&
              same_text(message_event_pump_reject_reason_name(static_cast<MessageEventPumpRejectReason>(250)),
                        "unknown"),
          "diagnostics: reasons have stable names");
}

// One oversized guild record: the worker routes it into reserved storage that
// it commits on demand, and the pump pops it into its own reservation for
// that record only. Both return to zero committed bytes afterwards.
ReservedBytes worker_large{};
u8 oversized_source[1024 * 1024];
bool acquire_worker_large(void*, u32 bytes, Slice<char>* storage) noexcept {
    if (!worker_large.base && !reserved_bytes_initialize(&worker_large, kGatewayClientGuildRecordCapacity))
        return false;
    if (!reserved_bytes_commit(&worker_large, bytes))
        return false;
    *storage = {reinterpret_cast<char*>(worker_large.base), worker_large.committed};
    return true;
}
void oversized_guild() noexcept {
    namespace fixture = oversized_guild_fixture;
    constexpr u64 guild = 77'000'000;
    const Utf8View source = fixture::write({oversized_source, sizeof(oversized_source)}, guild, kSelf);
    GatewayEnvelope envelope{};
    envelope.data = {source, 0, JsonValueKind::object};
    envelope.event_name = {utf8_literal("GUILD_CREATE"), 0, false};
    envelope.sequence = 950;
    envelope.opcode = 0;
    envelope.has_sequence = envelope.has_event_name = true;
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    const auto routed = gateway_client_route_dispatch_large(
        envelope, {records, 256 * 1024}, 64, {projection_payload, sizeof(projection_payload)}, {}, kSelf,
        {acquire_worker_large, nullptr}, &event, &payload);
    check(source.bytes && routed.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_create &&
              payload.count > kGatewayClientDispatchPayloadCapacity &&
              payload.data == worker_large.base && worker_large.committed == 256 * 1024 &&
              event.state.record_count == fixture::kChannels &&
              event.state.overwrite_count == fixture::kChannels * fixture::kOverwrites,
          "oversized guild: routes into 256 KiB of the worker's 1 MiB reservation");
    event.timestamp_ms = 1;
    mailbox = {};
    check(enqueue(&mailbox, event, payload), "oversized guild: record queues");
    reserved_bytes_decommit(&worker_large);
    const auto drained = message_event_pump_drain(&stores.pump, &mailbox, pop, kGatewayClientEventCapacity);
    const auto* resident = chat_state_store_find_guild(stores.state, guild);
    const auto* last = chat_state_store_find_channel(stores.state, fixture::channel_id(guild, fixture::kChannels - 1));
    check(drained.ok() && drained.popped == 1 && !drained.apply_failures && resident && last &&
              last->overwrite_count == fixture::kOverwrites,
          "oversized guild: the pump applies every channel and overwrite of the oversized record");
    MEMORY_BASIC_INFORMATION region{};
    check(stores.pump.large_payload.base && !stores.pump.large_payload.committed &&
              stores.pump.large_payload.peak == 256 * 1024 && !worker_large.committed &&
              VirtualQuery(stores.pump.large_payload.base, &region, sizeof(region)) &&
              region.State == MEM_RESERVE,
          "oversized guild: both reservations are decommitted once the record is applied");
    reserved_bytes_release(&worker_large);
}

void release() noexcept {
    message_event_pump_reset(&stores.pump);
    private_channel_store_release(&stores.private_channels);
    user_appearance_release(&stores.user_store.appearance);
    user_activity_release(&stores.user_store.activities);
}
} // namespace maicord::ready_scale_pump_test

extern "C" __declspec(noreturn) void WINAPI maicord_ready_scale_pump_entry() noexcept {
    using namespace maicord::ready_scale_pump_test;
    scale();
    resilience();
    oversized_guild();
    release();
    ExitProcess(failures ? 1 : 0);
}
