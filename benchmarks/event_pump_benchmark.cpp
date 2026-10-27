#include "maicord/app/message_event_pump.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/presence_coalescer.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/member_list.hpp"
#include "maicord/model/user_profile.hpp"
#include "maicord/protocol/gateway_payload.hpp"
#include "benchmark_output.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kMessageItems = 4'096;
inline constexpr u32 kPumpUpdates = 2'000'000;
inline constexpr u32 kMetadataEvents = 10'000'000;
inline constexpr u32 kDrainBatch = 256;
inline constexpr u32 kTextCapacity = 64 * 1024;
inline constexpr u64 kFirstId = 1'270'000'000'000'000'000ull;
inline constexpr u64 kChannelId = 1'260'000'000'000'000'001ull;

MessageRecord g_records[kMessageItems];
u32 g_order[kMessageItems];
u8 g_text[kTextCapacity];
u32 g_heights[kMessageItems];
u64 g_fenwick[kMessageItems];
u8 g_payload_scratch[kMessageEventPumpPayloadCapacity];
u64 g_bulk_ids[kGatewayBulkDeleteMaximumIds];
volatile u64 g_pump_checksum = 0;

struct SyntheticSource {
    GatewayClientEvent event;
    u32 remaining;
    u32 emitted;
};

void write_metric(const char* name, u64 value) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text(" events/s\r\n");
}

void write_value(const char* name, u64 value) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text("\r\n");
}

// One simulated minute of a large guild's presence stream: 1,000 members in a
// dense client-sorted list, 50 PRESENCE_UPDATE/s (status and activity churn),
// a 30-row member viewport and 12 visible message authors. "Text rebuilds"
// count presentation passes the policy would run (each one is a
// refresh_account_text/content-text rebuild in the client).
inline constexpr u32 kPresenceMembers = 1'000;
inline constexpr u32 kPresenceEventsPerMinute = 3'000;
inline constexpr u64 kPresenceEventIntervalMs = 20;
inline constexpr u32 kPresenceVisibleRows = 30;
inline constexpr u32 kPresenceVisibleAuthors = 12;

struct PresenceWorld {
    MessageRecord records[4];
    u32 order[4];
    u8 text[128];
    u32 heights[4];
    u64 fenwick[4];
    MessageStore store;
    UiVariableList list;
    MessageTimeline timeline;
    MessageEventPump pump;
    GuildMemberRecord member_records[kPresenceMembers];
    u32 member_order[kPresenceMembers];
    GuildMemberStore members;
    UserRecord user_records[kPresenceMembers];
    u32 user_order[kPresenceMembers];
    u8 user_names[kPresenceMembers * 64];
    UserStore users;
    GuildRecord guilds[2];
    ChannelRecord channels[2];
    u16 guild_order[2], channel_order[2];
    u8 state_names[1024];
    ChatStateStore state;
    MemberListView projection;
    MemberListRow rows[kPresenceMembers + 8];
    u32 scratch[kPresenceMembers];
    u64 authors[kPresenceVisibleAuthors];
};
PresenceWorld g_world;

bool presence_world_visible(void*, u64, u64 user_id) noexcept {
    for (u32 row = 0; row < kPresenceVisibleRows && row < g_world.projection.count; ++row)
        if (g_world.rows[row].user_id == user_id)
            return true;
    for (u64 author : g_world.authors)
        if (author == user_id)
            return true;
    return false;
}

u32 presence_world_initialize() noexcept {
    auto& w = g_world;
    if (!message_store_initialize(&w.store, {w.records, 4}, {w.order, 4}, {w.text, 128}) ||
        !message_store_upsert(&w.store, MessageInput{300, 7, 11, 300, 0, utf8_literal("seed"), 0}).ok() ||
        !ui_variable_list_initialize(&w.list, {w.heights, 4}, {w.fenwick, 4}, 1, 100) ||
        !message_timeline_initialize(&w.timeline, &w.store, &w.list, 100, 50, false) ||
        !guild_member_store_initialize(&w.members, {w.member_records, kPresenceMembers},
                                               {w.member_order, kPresenceMembers}) ||
        !user_store_initialize(&w.users, {w.user_records, kPresenceMembers}, {w.user_order, kPresenceMembers},
                                       {w.user_names, sizeof(w.user_names)}) ||
        !chat_state_store_initialize(&w.state, {w.guilds, 2}, {w.guild_order, 2}, {w.channels, 2},
                                        {w.channel_order, 2}, {w.state_names, sizeof(w.state_names)}) ||
        !member_list_view_initialize(&w.projection, {w.rows, kPresenceMembers + 8},
                                                   {w.scratch, kPresenceMembers}))
        return 1;
    u32 seed = 0x9E3779B9u;
    for (u32 index = 0; index < kPresenceMembers; ++index) {
        char name[12] = "member";
        seed = seed * 1'664'525u + 1'013'904'223u;
        for (u32 digit = 0; digit < 5; ++digit)
            name[6 + digit] = static_cast<char>('a' + ((seed >> (digit * 5)) % 26));
        const u64 id = 1'000 + index;
        if (!user_store_upsert(&w.users, id, {name, 11}).ok())
            return 2;
        if (!guild_member_store_upsert(&w.members, w.state, 7, id, {}, {}, false,
                                               GuildMemberStatus::online, index, 70)
                 .ok())
            return 3;
    }
    for (u32 index = 0; index < kPresenceVisibleAuthors; ++index)
        w.authors[index] = 1'000 + index * 83;
    w.pump = MessageEventPump{};
    w.pump.timeline = &w.timeline;
    w.pump.users = &w.users;
    w.pump.members = &w.members;
    return member_list_view_rebuild(&w.projection, w.members, w.users, w.state, 7, 70).ok() ? 0 : 4;
}

struct PresenceEvent {
    GatewayClientEvent event;
    u8 payload[384];
    u32 bytes;
};

// Deterministic stream: random member, status mostly within the online
// group (5% offline round trips), and an activity string that changes.
void presence_event(u32 index, PresenceEvent* output) noexcept {
    u32 hash = index * 2'654'435'761u + 0x7F4A7C15u;
    hash ^= hash >> 15;
    hash *= 2'246'822'519u;
    hash ^= hash >> 13;
    const u64 user = 1'000 + hash % kPresenceMembers;
    const u32 roll = (hash >> 10) % 100;
    const auto status = roll < 5    ? GatewayClientMemberListStatus::offline
                        : roll < 40 ? GatewayClientMemberListStatus::idle
                        : roll < 60 ? GatewayClientMemberListStatus::dnd
                                    : GatewayClientMemberListStatus::online;
    UserAppearance appearance{};
    appearance.user_id = user;
    appearance.guild_id = 7;
    appearance.fields = appearance_activity;
    char activity[] = "Playing game 000";
    activity[13] = static_cast<char>('0' + (hash >> 20) % 10);
    activity[14] = static_cast<char>('0' + (hash >> 24) % 10);
    activity[15] = static_cast<char>('0' + (hash >> 28) % 10);
    appearance.activity.bytes = 16;
    copy_bytes(appearance.activity.data, activity, 16);
    *output = {};
    output->bytes = user_appearance_write(appearance, {output->payload, sizeof(output->payload)});
    output->event.kind = GatewayClientEventKind::dispatch;
    output->event.dispatch_kind = GatewayClientDispatchKind::presence_update;
    output->event.dispatch_payload_bytes = output->bytes;
    output->event.presence = {7, user, status};
}

struct PresencePolicyResult {
    u64 consume_us;
    u64 projection_us;
    u32 text_rebuilds;
    u32 projection_rebuilds;
};

// legacy=true replays the previous policy: every changed presence repaints
// and the next presentation rescans and heap-sorts the member projection.
// Zero on success, otherwise the failing stage.
u32 run_presence_policy(bool legacy, const Clock& clock, PresencePolicyResult* result) noexcept {
    auto& w = g_world;
    if (const u32 failed = presence_world_initialize())
        return failed;
    message_event_pump_set_presence_visibility(&w.pump, legacy ? nullptr : presence_world_visible, nullptr);
    *result = {};
    const u32 rebuilds_before = w.projection.rebuilds;
    PresenceCoalescer coalescer{};
    PresenceEvent next{};
    u32 rendered_members = w.members.revision, rendered_users = w.users.revision;
    for (u32 index = 0; index < kPresenceEventsPerMinute; ++index) {
        const u64 now_ms = static_cast<u64>(index) * kPresenceEventIntervalMs;
        presence_event(index, &next);
        i64 begin = clock_now();
        const auto consumed = message_event_pump_consume(&w.pump, next.event, {next.payload, next.bytes});
        result->consume_us += clock_microseconds(clock, begin, clock_now());
        if (!consumed.ok())
            return 5;
        begin = clock_now();
        bool rebuild_text = false;
        if (legacy) {
            // The old pump bumped store revisions for every status change.
            if (consumed.repaint && w.members.presence_revision != w.projection.source_presence_revision)
                saturating_increment(w.members.revision);
            rebuild_text = consumed.repaint;
        } else {
            if (w.pump.presence_visible_dirty ||
                w.projection.source_presence_revision != w.members.presence_revision)
                presence_coalescer_note(&coalescer, now_ms);
            if (presence_coalescer_take(&coalescer, now_ms)) {
                if (w.pump.presence_visible_dirty) {
                    w.pump.presence_visible_dirty = false;
                    saturating_increment(w.users.revision);
                    rebuild_text = true;
                }
                rebuild_text = member_list_view_refresh_presence(&w.projection, w.members, 0,
                                                                               kPresenceVisibleRows)
                                   .visible ||
                               rebuild_text;
            }
        }
        if (rebuild_text || rendered_members != w.members.revision || rendered_users != w.users.revision) {
            if (!member_list_view_rebuild(&w.projection, w.members, w.users, w.state, 7, 70).ok())
                return 6;
            rendered_members = w.members.revision;
            rendered_users = w.users.revision;
            ++result->text_rebuilds;
        }
        result->projection_us += clock_microseconds(clock, begin, clock_now());
    }
    result->projection_rebuilds = w.projection.rebuilds - rebuilds_before;
    user_appearance_release(&w.users.appearance);
    return 0;
}

// One simulated minute of a large guild's lazy member list (op 14): 1,000
// members in an authoritative list, 50 GUILD_MEMBER_LIST_UPDATE/s, each one
// UPDATE op for a random member at its index (status churn inside its group,
// 10% activity changes, 1% nickname changes) plus the group/count header that
// every packet repeats. A 30-row member viewport is visible.
inline constexpr u32 kStormEventsPerMinute = 3'000;
inline constexpr u64 kStormEventIntervalMs = 20;
inline constexpr u64 kStormGuild = 7, kStormList = 70;

struct StormText {
    char bytes[1024];
    u32 used;
    void put(const char* text) noexcept {
        while (*text && used < sizeof(bytes))
            bytes[used++] = *text++;
    }
    void number(u64 value) noexcept {
        char digits[24];
        u32 count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (count && used < sizeof(bytes))
            bytes[used++] = digits[--count];
    }
};

struct StormRouting {
    u32 envelope[2048];
    u32 route[4096];
    char payload[kMessageEventPumpPayloadCapacity];
};
StormRouting g_storm_routing;

u32 storm_member_name(u32 index, char name[12]) noexcept {
    u32 seed = 0x9E3779B9u;
    for (u32 i = 0; i <= index; ++i)
        seed = seed * 1'664'525u + 1'013'904'223u;
    copy_bytes(name, "member", 6);
    for (u32 digit = 0; digit < 5; ++digit)
        name[6 + digit] = static_cast<char>('a' + ((seed >> (digit * 5)) % 26));
    name[11] = 0;
    return 11;
}

u32 storm_world_initialize(bool legacy) noexcept {
    auto& w = g_world;
    if (const u32 failed = presence_world_initialize())
        return failed;
    // Same identities as the gateway items, so unchanged users never bump.
    for (u32 index = 0; index < kPresenceMembers; ++index) {
        char name[12];
        storm_member_name(index, name);
        if (!user_store_upsert(&w.users, 1'000 + index, {name, 11}, {name, 11}).ok() ||
            !guild_member_store_upsert(&w.members, w.state, kStormGuild, 1'000 + index, {}, {}, false,
                                       GuildMemberStatus::online, index + 1, kStormList)
                 .ok())
            return 7;
    }
    if (!guild_member_store_observe_list(&w.members, kStormGuild, kStormList, kPresenceMembers,
                                         kPresenceMembers, true, true, true, 1, kPresenceMembers + 1) ||
        !guild_member_store_set_group(&w.members, kStormGuild, kStormList, 0, kMemberListOnlineGroup,
                                      kPresenceMembers))
        return 8;
    guild_member_store_finish_groups(&w.members, kStormGuild, kStormList);
    if (!guild_member_store_set_visible_range(&w.members, kStormGuild, kStormList, 0, kPresenceVisibleRows))
        return 9;
    w.pump.state = &w.state;
    message_event_pump_set_presence_visibility(&w.pump, legacy ? nullptr : presence_world_visible, nullptr);
    return member_list_view_rebuild(&w.projection, w.members, w.users, w.state, kStormGuild, kStormList).ok() &&
                   w.projection.authoritative
               ? 0
               : 10;
}

bool storm_event(u32 index, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    u32 hash = index * 2'654'435'761u + 0x2545F491u;
    hash ^= hash >> 15;
    hash *= 2'246'822'519u;
    hash ^= hash >> 13;
    const u32 member = hash % kPresenceMembers;
    const u32 roll = (hash >> 10) % 100;
    const char* status = roll < 35 ? "idle" : roll < 55 ? "dnd" : "online";
    char name[12];
    storm_member_name(member, name);
    StormText text{};
    text.put(R"({"op":0,"s":)");
    text.number(index + 1);
    text.put(R"(,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","member_count":1000,)"
             R"("online_count":1000,"groups":[{"id":"online","count":1000}],"ops":[{"op":"UPDATE","index":)");
    text.number(member + 1);
    text.put(R"(,"item":{"member":{"user":{"id":")");
    text.number(1'000 + member);
    text.put(R"(","username":")");
    text.put(name);
    text.put(R"("},"roles":[])");
    if ((hash >> 20) % 100 == 0) {
        text.put(R"(,"nick":"Nick )");
        text.number(index);
        text.put("\"");
    }
    text.put(R"(,"presence":{"status":")");
    text.put(status);
    text.put("\"");
    if ((hash >> 24) % 10 == 0) {
        text.put(R"(,"activities":[{"type":0,"name":"Game )");
        text.number(hash % 97);
        text.put(R"("}])");
    }
    text.put("}}}}]}}");
    auto& r = g_storm_routing;
    GatewayEnvelope envelope{};
    bool handled = false;
    if (!gateway_decode_envelope({text.bytes, text.used}, {r.envelope, 2048}, 32, &envelope).ok() ||
        !gateway_client_route_member_list_dispatch(envelope, {r.route, 4096}, 32,
                                                   {r.payload, kMessageEventPumpPayloadCapacity}, event, payload,
                                                   &handled)
             .ok() ||
        !handled)
        return false;
    event->kind = GatewayClientEventKind::dispatch;
    event->sequence = envelope.sequence;
    event->has_sequence = envelope.has_sequence;
    event->dispatch_payload_bytes = payload->count;
    return event->dispatch_kind == GatewayClientDispatchKind::guild_member_list_update;
}

// legacy=true replays the previous policy: every packet bumped the structural
// member revision (header, invalidate and re-upsert) and repainted.
u32 run_member_storm(bool legacy, const Clock& clock, PresencePolicyResult* result) noexcept {
    auto& w = g_world;
    if (const u32 failed = storm_world_initialize(legacy))
        return failed;
    *result = {};
    const u32 rebuilds_before = w.projection.rebuilds;
    PresenceCoalescer coalescer{};
    u32 rendered_members = w.members.revision, rendered_users = w.users.revision;
    for (u32 index = 0; index < kStormEventsPerMinute; ++index) {
        const u64 now_ms = static_cast<u64>(index) * kStormEventIntervalMs;
        GatewayClientEvent event{};
        ConstSlice<u8> payload{};
        if (!storm_event(index, &event, &payload))
            return 11;
        i64 begin = clock_now();
        const auto consumed = message_event_pump_consume(&w.pump, event, payload);
        result->consume_us += clock_microseconds(clock, begin, clock_now());
        if (!consumed.ok())
            return 12;
        begin = clock_now();
        bool rebuild_text = false;
        if (legacy) {
            saturating_increment(w.members.revision);
            rebuild_text = true;
        } else {
            if (w.pump.presence_visible_dirty ||
                w.projection.source_presence_revision != w.members.presence_revision)
                presence_coalescer_note(&coalescer, now_ms);
            if (presence_coalescer_take(&coalescer, now_ms)) {
                if (w.pump.presence_visible_dirty) {
                    w.pump.presence_visible_dirty = false;
                    saturating_increment(w.users.revision);
                    rebuild_text = true;
                }
                rebuild_text = member_list_view_refresh_presence(&w.projection, w.members, 0,
                                                                 kPresenceVisibleRows + 1)
                                   .visible ||
                               rebuild_text;
            }
        }
        if (rebuild_text || rendered_members != w.members.revision || rendered_users != w.users.revision) {
            if (!member_list_view_rebuild(&w.projection, w.members, w.users, w.state, kStormGuild, kStormList)
                     .ok())
                return 13;
            rendered_members = w.members.revision;
            rendered_users = w.users.revision;
            ++result->text_rebuilds;
        }
        result->projection_us += clock_microseconds(clock, begin, clock_now());
    }
    result->projection_rebuilds = w.projection.rebuilds - rebuilds_before;
    user_appearance_release(&w.users.appearance);
    return 0;
}

GatewayClientPopResult synthetic_pop(void* source_pointer,
                                     GatewayClientEvent* event,
                                     Slice<u8>) noexcept {
    if (!source_pointer || !event) {
        return GatewayClientPopResult{0, GatewayClientPopError::invalid_input};
    }
    auto* source = static_cast<SyntheticSource*>(source_pointer);
    if (source->remaining == 0) {
        return GatewayClientPopResult{0, GatewayClientPopError::empty};
    }
    *event = source->event;
    event->sequence = ++source->emitted;
    event->has_sequence = true;
    event->message.flags = source->emitted & 1;
    --source->remaining;
    return GatewayClientPopResult{0, GatewayClientPopError::none};
}

int run() noexcept {
    MessageStore store{};
    if (!message_store_initialize(
            &store, Slice<MessageRecord>{g_records, kMessageItems},
            Slice<u32>{g_order, kMessageItems},
            Slice<u8>{g_text, kTextCapacity})) {
        return 1;
    }
    for (u32 index = 0; index < kMessageItems; ++index) {
        if (!message_store_upsert(
                &store,
                MessageInput{kFirstId + static_cast<u64>(index) * 2, kChannelId,
                             1'250'000'000'000'000'001ull + (index & 63), 0, 0,
                             utf8_literal(""), index & 7})
                 .ok()) {
            return 2;
        }
    }
    UiVariableList list{};
    if (!ui_variable_list_initialize(
            &list, Slice<u32>{g_heights, kMessageItems},
            Slice<u64>{g_fenwick, kMessageItems}, kMessageItems, 100)) {
        return 3;
    }
    MessageTimeline timeline{};
    if (!message_timeline_initialize(&timeline, &store, &list, 100, 720, false)) {
        return 4;
    }
    MessageEventPump pump{};
    if (!message_event_pump_initialize(
            &pump, &timeline,
            Slice<u8>{g_payload_scratch, kMessageEventPumpPayloadCapacity},
            Slice<u64>{g_bulk_ids, kGatewayBulkDeleteMaximumIds},
            MessageContentAvailability::available)) {
        return 5;
    }

    SyntheticSource source{};
    source.event.kind = GatewayClientEventKind::dispatch;
    source.event.dispatch_kind = GatewayClientDispatchKind::message_update;
    source.event.message.id =
        kFirstId + static_cast<u64>(kMessageItems / 2) * 2;
    source.event.message.channel_id = kChannelId;
    source.event.message.fields = gateway_message_field_id |
                                  gateway_message_field_channel_id |
                                  gateway_message_field_flags;
    source.remaining = kPumpUpdates;

    const Clock clock = clock_initialize();
    u64 checksum = 0;
    i64 begin = clock_now();
    while (source.remaining != 0) {
        const MessageEventPumpDrainResult drained = message_event_pump_drain(
            &pump, &source, synthetic_pop, kDrainBatch);
        if (!drained.ok() || drained.popped == 0 ||
            drained.applied != drained.popped || !drained.repaint ||
            drained.invalidation.first != kMessageItems / 2) {
            return 6;
        }
        checksum += drained.popped + drained.invalidation.first +
                    drained.invalidation.count;
    }
    const u64 update_us = clock_microseconds(clock, begin, clock_now());

    GatewayClientEvent unknown{};
    unknown.kind = GatewayClientEventKind::dispatch;
    unknown.dispatch_kind = GatewayClientDispatchKind::unknown;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMetadataEvents; ++iteration) {
        unknown.sequence = iteration + 1;
        unknown.has_sequence = true;
        const MessageEventPumpConsumeResult consumed = message_event_pump_consume(
            &pump, unknown, ConstSlice<u8>{nullptr, 0});
        if (!consumed.ok() ||
            consumed.kind != MessageEventPumpConsumeKind::ignored_dispatch) {
            return 7;
        }
        checksum += pump.last_sequence;
    }
    const u64 metadata_us = clock_microseconds(clock, begin, clock_now());

    g_pump_checksum = checksum;
    if (update_us == 0 || metadata_us == 0 || g_pump_checksum == 0 ||
        pump.drained != kPumpUpdates || pump.applied != kPumpUpdates ||
        pump.ignored != kMetadataEvents) {
        return 8;
    }
    write_metric("message_event_pump_update",
                 static_cast<u64>(kPumpUpdates) * 1'000'000ull / update_us);
    write_metric("message_event_pump_unknown",
                 static_cast<u64>(kMetadataEvents) * 1'000'000ull /
                     metadata_us);

    PresencePolicyResult legacy{}, current{};
    if (const u32 failed = run_presence_policy(true, clock, &legacy))
        return 10 + failed;
    if (const u32 failed = run_presence_policy(false, clock, &current))
        return 20 + failed;
    write_value("presence_events_per_minute", kPresenceEventsPerMinute);
    write_value("presence_legacy_text_rebuilds_per_minute", legacy.text_rebuilds);
    write_value("presence_text_rebuilds_per_minute", current.text_rebuilds);
    write_value("presence_legacy_projection_rebuilds_per_minute", legacy.projection_rebuilds);
    write_value("presence_projection_rebuilds_per_minute", current.projection_rebuilds);
    write_value("presence_legacy_model_us_per_minute", legacy.consume_us + legacy.projection_us);
    write_value("presence_model_us_per_minute", current.consume_us + current.projection_us);

    PresencePolicyResult storm_legacy{}, storm{};
    if (const u32 failed = run_member_storm(true, clock, &storm_legacy))
        return 30 + failed;
    if (const u32 failed = run_member_storm(false, clock, &storm))
        return 50 + failed;
    write_value("member_list_update_events_per_minute", kStormEventsPerMinute);
    write_value("member_list_legacy_text_rebuilds_per_minute", storm_legacy.text_rebuilds);
    write_value("member_list_text_rebuilds_per_minute", storm.text_rebuilds);
    write_value("member_list_legacy_projection_rebuilds_per_minute", storm_legacy.projection_rebuilds);
    write_value("member_list_projection_rebuilds_per_minute", storm.projection_rebuilds);
    write_value("member_list_legacy_model_us_per_minute", storm_legacy.consume_us + storm_legacy.projection_us);
    write_value("member_list_model_us_per_minute", storm.consume_us + storm.projection_us);
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI
maicord_event_pump_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
