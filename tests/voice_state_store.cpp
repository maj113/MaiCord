#include "maicord/app/gateway_client.hpp"
#include "maicord/model/voice_state_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/voice_channel_rows.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

struct Published {
    GatewayClientEvent events[8];
    u8 payloads[8][4096];
    u32 bytes[8];
    u32 count;
    bool fail;
};
bool capture(void* opaque, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    auto& published = *static_cast<Published*>(opaque);
    if (published.fail || published.count == 8 || payload.count > sizeof(published.payloads[0]))
        return false;
    published.events[published.count] = event;
    copy_bytes(published.payloads[published.count], payload.data, payload.count);
    published.bytes[published.count++] = payload.count;
    return true;
}
JsonValue object(Utf8View text) noexcept {
    return {text, 0, JsonValueKind::object};
}
GatewayClientVoiceStateRecord record_at(const Published& published, u32 event, u32 index) noexcept {
    GatewayClientVoiceStateRecord record{};
    copy_bytes(&record, published.payloads[event] + index * sizeof(record), sizeof(record));
    return record;
}
bool contains(const u8* bytes, u32 count, Utf8View needle) noexcept {
    for (u32 i = 0; i + needle.bytes <= count; ++i)
        if (bytes_equal(bytes + i, needle.data, needle.bytes))
            return true;
    return false;
}

void store_cases(Check check) noexcept {
    static VoiceState records[4];
    VoiceStateStore store{};
    check(voice_state_store_initialize_sparse(&store, {records, 4}) &&
              !voice_state_store_initialize_sparse(&store, {records, 0}),
          "voice store initializes caller-owned rows");
    const auto self_mute = static_cast<u32>(voice_self_mute);
    check(voice_state_store_apply(&store, 1, 10, 100, 0) == VoiceStateResult::changed &&
              voice_state_store_apply(&store, 1, 10, 101, self_mute) == VoiceStateResult::changed &&
              voice_state_store_apply(&store, 1, 11, 102, 0) == VoiceStateResult::changed &&
              voice_state_store_channel_count(store, 10) == 2 &&
              voice_state_store_channel_count(store, 11) == 1,
          "voice store join adds users to their channels");
    const u32 revision = store.revision;
    check(voice_state_store_apply(&store, 1, 10, 100, 0) == VoiceStateResult::unchanged &&
              store.revision == revision,
          "voice store repeated state is unchanged");
    check(voice_state_store_apply(&store, 1, 10, 100, self_mute) == VoiceStateResult::changed &&
              voice_state_store_channel_at(store, 10, 0)->user_id == 100 &&
              voice_state_store_channel_at(store, 10, 0)->flags == self_mute,
          "voice store flag change keeps join order");
    check(voice_state_store_apply(&store, 1, 11, 100, 0) == VoiceStateResult::changed &&
              voice_state_store_channel_count(store, 10) == 1 &&
              voice_state_store_channel_at(store, 11, 1)->user_id == 100 &&
              !voice_state_store_channel_at(store, 11, 2),
          "voice store move re-joins at the end of the new channel");
    check(voice_state_store_apply(&store, 1, 0, 101, 0) == VoiceStateResult::changed &&
              !voice_state_store_find(store, 1, 101) &&
              voice_state_store_apply(&store, 1, 0, 101, 0) == VoiceStateResult::unchanged,
          "voice store leave removes the user once");
    check(voice_state_store_apply(&store, 2, 20, 100, 0) == VoiceStateResult::changed &&
              voice_state_store_find(store, 1, 100) && voice_state_store_find(store, 2, 100),
          "voice store keys states by guild and user");
    store.retained_guild = 1;
    check(voice_state_store_apply(&store, 3, 30, 103, 0) == VoiceStateResult::changed &&
              voice_state_store_apply(&store, 3, 30, 104, 0) == VoiceStateResult::changed &&
              store.evictions == 1 && store.dropped == 1 && !voice_state_store_find(store, 2, 100) &&
              voice_state_store_find(store, 1, 100) && voice_state_store_find(store, 3, 104) &&
              voice_state_store_apply(&store, 1, 10, 0, 0) == VoiceStateResult::invalid_input,
          "voice store at capacity evicts another guild, never the retained one, and rejects zero users");
    check(voice_state_store_clear_guild(&store, 1) == VoiceStateResult::changed &&
              store.count == 2 && !voice_state_store_find(store, 1, 102) &&
              voice_state_store_reset(&store) && store.count == 0,
          "voice store clears one guild and resets");
}

void projection_cases(Check check) noexcept {
    static u32 scratch[2048];
    static char payload[kGatewayClientDispatchPayloadCapacity];
    static Published published;
    constexpr auto guild = utf8_literal(
        "{\"id\":\"5\",\"name\":\"g\",\"voice_states\":[{\"channel_id\":\"7\",\"user_id\":\"8\",\"session_id\":"
        "\"secret-session\",\"self_mute\":true,\"deaf\":false,\"member\":{\"user\":{\"id\":\"8\",\"username\":"
        "\"alice\",\"global_name\":\"Alice\",\"avatar\":null}}},{\"channel_id\":\"7\",\"user_id\":\"9\","
        "\"session_id\":\"other\",\"self_deaf\":true}]}");
    published = {};
    check(gateway_client_publish_voice_states(object(guild), GatewayVoiceStateSource::guild, 5, {scratch, 2048},
                                              {payload, sizeof(payload)}, 3, 4, capture, &published) &&
              published.count == 2,
          "guild snapshot publishes member users then voice rows");
    const auto& users = published.events[0];
    const auto& rows = published.events[1];
    check(users.dispatch_kind == GatewayClientDispatchKind::user_snapshot && users.state.record_count == 1 &&
              rows.dispatch_kind == GatewayClientDispatchKind::voice_state_snapshot &&
              rows.voice_states.guild_id == 5 && rows.voice_states.replace &&
              rows.voice_states.record_count == 2 && rows.dispatch_payload_bytes == published.bytes[1] &&
              rows.sequence == 3 && rows.has_sequence,
          "guild snapshot event replaces the guild with both rows");
    const auto first = record_at(published, 1, 0), second = record_at(published, 1, 1);
    check(first.guild_id == 5 && first.channel_id == 7 && first.user_id == 8 &&
              first.flags == gateway_voice_self_mute && second.user_id == 9 &&
              second.flags == gateway_voice_self_deaf,
          "guild snapshot rows default the guild and copy flags");
    bool leaked = false;
    for (u32 i = 0; i < published.count; ++i)
        leaked |= contains(published.payloads[i], published.bytes[i], utf8_literal("secret-session"));
    check(!leaked, "voice projection never copies session IDs");

    constexpr auto empty = utf8_literal("{\"id\":\"5\",\"voice_states\":[]}");
    published = {};
    check(gateway_client_publish_voice_states(object(empty), GatewayVoiceStateSource::guild, 5, {scratch, 2048},
                                              {payload, sizeof(payload)}, 3, 4, capture, &published) &&
              published.count == 1 && published.events[0].voice_states.replace &&
              published.events[0].voice_states.record_count == 0,
          "empty guild snapshot still clears the guild");
    constexpr auto absent = utf8_literal("{\"id\":\"5\",\"unavailable\":true}");
    published = {};
    check(gateway_client_publish_voice_states(object(absent), GatewayVoiceStateSource::guild, 5, {scratch, 2048},
                                              {payload, sizeof(payload)}, 3, 4, capture, &published) &&
              published.count == 0,
          "guild without voice_states publishes nothing");

    constexpr auto leave = utf8_literal(
        "{\"guild_id\":\"5\",\"channel_id\":null,\"user_id\":\"8\",\"session_id\":\"s\",\"mute\":true}");
    published = {};
    check(gateway_client_publish_voice_states(object(leave), GatewayVoiceStateSource::update, 0, {scratch, 2048},
                                              {payload, sizeof(payload)}, 6, 4, capture, &published) &&
              published.count == 1 && !published.events[0].voice_states.replace &&
              published.events[0].voice_states.record_count == 1 && record_at(published, 0, 0).guild_id == 5 &&
              record_at(published, 0, 0).channel_id == 0,
          "single update publishes one non-replacing leave row");

    constexpr auto batch = utf8_literal(
        "{\"voice_states\":[{\"guild_id\":\"5\",\"channel_id\":\"7\",\"user_id\":\"8\",\"session_id\":\"s\"},"
        "{\"guild_id\":\"6\",\"channel_id\":\"9\",\"user_id\":\"10\",\"session_id\":\"t\"}]}");
    published = {};
    check(gateway_client_publish_voice_states(object(batch), GatewayVoiceStateSource::batch, 0, {scratch, 2048},
                                              {payload, sizeof(payload)}, 6, 4, capture, &published) &&
              published.count == 1 && published.events[0].voice_states.record_count == 2 &&
              record_at(published, 0, 1).guild_id == 6,
          "batch publishes rows for several guilds");

    constexpr auto malformed = utf8_literal(
        "{\"id\":\"5\",\"voice_states\":[{\"channel_id\":\"7\",\"user_id\":\"8\",\"session_id\":\"s\"},"
        "{\"channel_id\":\"7\",\"session_id\":\"s\"}]}");
    published = {};
    check(gateway_client_publish_voice_states(object(malformed), GatewayVoiceStateSource::guild, 5,
                                              {scratch, 2048}, {payload, sizeof(payload)}, 3, 4, capture,
                                              &published) &&
              published.count == 0,
          "a malformed row skips the whole snapshot before publishing");
    constexpr auto foreign = utf8_literal(
        "{\"id\":\"5\",\"voice_states\":[{\"guild_id\":\"6\",\"channel_id\":\"7\",\"user_id\":\"8\","
        "\"session_id\":\"s\"}]}");
    published = {};
    check(gateway_client_publish_voice_states(object(foreign), GatewayVoiceStateSource::guild, 5,
                                              {scratch, 2048}, {payload, sizeof(payload)}, 3, 4, capture,
                                              &published) &&
              published.count == 0,
          "a row naming another guild is rejected");
    published = {};
    published.fail = true;
    check(!gateway_client_publish_voice_states(object(guild), GatewayVoiceStateSource::guild, 5, {scratch, 2048},
                                               {payload, sizeof(payload)}, 3, 4, capture, &published),
          "publish failure is reported to the caller");
}
// Mirrors the UI pump: a replacing snapshot clears its guild before its rows.
void apply_published(VoiceStateStore* store, const Published& published) noexcept {
    for (u32 e = 0; e < published.count; ++e) {
        const auto& event = published.events[e];
        if (event.dispatch_kind != GatewayClientDispatchKind::voice_state_snapshot)
            continue;
        if (event.voice_states.replace)
            voice_state_store_clear_guild(store, event.voice_states.guild_id);
        for (u32 i = 0; i < event.voice_states.record_count; ++i) {
            const auto record = record_at(published, e, i);
            voice_state_store_apply(store, record.guild_id, record.channel_id, record.user_id,
                                            record.flags);
        }
    }
}
bool channel_users(const VoiceStateStore& store, u64 channel, ConstSlice<u64> users) noexcept {
    ChannelViewRow row{};
    row.id = channel;
    row.type = 2;
    row.voice_count = static_cast<u16>(voice_state_store_channel_count(store, channel));
    if (row.voice_count != users.count)
        return false;
    for (u32 i = 0; i < users.count; ++i) {
        UiVoiceParticipantRow drawn{};
        if (!ui_voice_participant_row(row, {0, 0, 240, 200}, store, i, &drawn) || drawn.state->user_id != users[i])
            return false;
    }
    UiVoiceParticipantRow past{};
    return !ui_voice_participant_row(row, {0, 0, 240, 200}, store, users.count, &past);
}

// Every Gateway shape that carries other members' voice states reaches the
// store, and the channel-row projection draws those members, not only self.
void roster_cases(Check check) noexcept {
    static u32 scratch[4096];
    static u32 guild_scratch[2048];
    static char payload[kGatewayClientDispatchPayloadCapacity];
    static Published published;
    static VoiceState records[16];
    VoiceStateStore store{};
    check(voice_state_store_initialize_sparse(&store, {records, 16}), "roster store initializes");
    // Stale rows: guild 11 is replaced by an empty list, guild 6 is not listed.
    voice_state_store_apply(&store, 11, 30, 31, 0);
    voice_state_store_apply(&store, 6, 40, 41, 0);

    // READY_SUPPLEMENTAL: guilds[] hold voice_states; session_id may be absent
    // and optional flags may be null. Members come from merged_members.
    constexpr auto supplemental = utf8_literal(
        R"({"merged_presences":{"guilds":[[],[],[]],"friends":[]},"merged_members":[[{"user_id":"8","roles":[]}],[],[]],)"
        R"("guilds":[{"id":"5","voice_states":[{"user_id":"8","channel_id":"7","session_id":"secret-a","self_mute":true,)"
        R"("self_video":false,"suppress":false,"request_to_speak_timestamp":null},{"user_id":"9","channel_id":"7",)"
        R"("deaf":false,"mute":false,"self_deaf":false,"self_mute":false,"self_stream":null}],"embedded_activities":[],)"
        R"("activity_instances":[]},{"id":"6","activity_instances":[]},{"id":"11","voice_states":[]}],)"
        R"("lazy_private_channels":[]})");
    published = {};
    check(gateway_client_publish_supplemental_voice_states(object(supplemental), {scratch, 4096},
                                                           {guild_scratch, 2048}, {payload, sizeof(payload)}, 12,
                                                           4, capture, &published) &&
              published.count == 2 && published.events[0].voice_states.guild_id == 5 &&
              published.events[0].voice_states.replace && published.events[0].voice_states.record_count == 2 &&
              published.events[1].voice_states.guild_id == 11 && published.events[1].voice_states.replace &&
              published.events[1].voice_states.record_count == 0,
          "READY_SUPPLEMENTAL publishes each listed guild's voice states, sessions optional");
    check(record_at(published, 0, 0).flags == gateway_voice_self_mute && record_at(published, 0, 1).user_id == 9 &&
              record_at(published, 0, 1).flags == 0,
          "READY_SUPPLEMENTAL rows copy flags and read null flags as absent");
    bool leaked = false;
    for (u32 i = 0; i < published.count; ++i)
        leaked |= contains(published.payloads[i], published.bytes[i], utf8_literal("secret-a"));
    check(!leaked, "supplemental voice projection never copies session IDs");
    apply_published(&store, published);
    const u64 initial[]{8, 9};
    check(channel_users(store, 7, {initial, 2}) && !voice_state_store_find(store, 11, 31) &&
              voice_state_store_find(store, 6, 41),
          "READY_SUPPLEMENTAL rows project every member; unlisted guilds keep their rows");

    // PASSIVE_UPDATE_V2 replaces VOICE_STATE_UPDATE for passively synced guilds.
    constexpr auto passive = utf8_literal(
        R"({"guild_id":"5","updated_voice_states":[{"user_id":"10","channel_id":"7","session_id":"secret-p",)"
        R"("self_mute":true,"deaf":false,"mute":false,"self_deaf":false,"self_video":false,"suppress":false},)"
        R"({"user_id":"8","channel_id":"12","deaf":false,"mute":false,"self_deaf":true,"self_mute":false}],)"
        R"("removed_voice_states":["9"],"updated_members":[{"user":{"id":"10","username":"bob","global_name":"Bob",)"
        R"("avatar":null},"roles":[]}],"updated_channels":[{"id":"7","last_message_id":"1"}]})");
    published = {};
    check(gateway_client_publish_passive_voice_states(object(passive), {scratch, 4096}, {payload, sizeof(payload)},
                                                      13, 4, capture, &published) &&
              published.count == 2 &&
              published.events[0].dispatch_kind == GatewayClientDispatchKind::user_snapshot &&
              published.events[0].state.record_count == 1 &&
              published.events[1].dispatch_kind == GatewayClientDispatchKind::voice_state_snapshot &&
              !published.events[1].voice_states.replace && published.events[1].voice_states.record_count == 3,
          "PASSIVE_UPDATE_V2 publishes member users, then join, move and leave rows");
    check(record_at(published, 1, 0).guild_id == 5 && record_at(published, 1, 0).user_id == 10 &&
              record_at(published, 1, 1).channel_id == 12 && record_at(published, 1, 2).user_id == 9 &&
              record_at(published, 1, 2).channel_id == 0,
          "PASSIVE_UPDATE_V2 rows take the document guild; removals clear the channel");
    leaked = false;
    for (u32 i = 0; i < published.count; ++i)
        leaked |= contains(published.payloads[i], published.bytes[i], utf8_literal("secret-p"));
    check(!leaked, "passive voice projection never copies session IDs");
    apply_published(&store, published);
    const u64 after_passive[]{10};
    const u64 moved[]{8};
    check(channel_users(store, 7, {after_passive, 1}) && channel_users(store, 12, {moved, 1}),
          "PASSIVE_UPDATE_V2 join, move and leave reach the channel rows");
    constexpr auto bad_passive = utf8_literal(
        R"({"guild_id":"5","updated_voice_states":[{"user_id":"10","channel_id":"7"}],"removed_voice_states":[false]})");
    published = {};
    check(gateway_client_publish_passive_voice_states(object(bad_passive), {scratch, 4096},
                                                      {payload, sizeof(payload)}, 13, 4, capture, &published) &&
              published.count == 0,
          "malformed PASSIVE_UPDATE_V2 publishes nothing");
    constexpr auto no_voice = utf8_literal(R"({"guild_id":"5","updated_channels":[]})");
    published = {};
    check(gateway_client_publish_passive_voice_states(object(no_voice), {scratch, 4096}, {payload, sizeof(payload)},
                                                      13, 4, capture, &published) &&
              published.count == 0,
          "PASSIVE_UPDATE_V2 without voice fields publishes nothing");

    // VOICE_STATE_UPDATE for another user: join (with member), move, leave.
    constexpr auto join = utf8_literal(
        R"({"guild_id":"5","channel_id":"7","user_id":"20","session_id":"x","self_deaf":true,)"
        R"("member":{"user":{"id":"20","username":"carol","avatar":"0123456789abcdef0123456789abcdef"},"roles":[]}})");
    constexpr auto move = utf8_literal(R"({"guild_id":"5","channel_id":"12","user_id":"20","session_id":"x"})");
    constexpr auto leave = utf8_literal(R"({"guild_id":"5","channel_id":null,"user_id":"20","session_id":"x"})");
    published = {};
    check(gateway_client_publish_voice_states(object(join), GatewayVoiceStateSource::update, 0, {scratch, 4096},
                                              {payload, sizeof(payload)}, 14, 4, capture, &published) &&
              published.count == 2 &&
              published.events[0].dispatch_kind == GatewayClientDispatchKind::user_snapshot,
          "another user's join publishes their member user before the row");
    apply_published(&store, published);
    const u64 joined[]{10, 20};
    const auto* carol = voice_state_store_find(store, 5, 20);
    check(channel_users(store, 7, {joined, 2}) && carol && (carol->flags & voice_self_deaf),
          "another user's join is drawn in the channel");
    published = {};
    check(gateway_client_publish_voice_states(object(move), GatewayVoiceStateSource::update, 0, {scratch, 4096},
                                              {payload, sizeof(payload)}, 15, 4, capture, &published) &&
              published.count == 1,
          "another user's move publishes");
    apply_published(&store, published);
    const u64 moved_both[]{8, 20};
    check(channel_users(store, 7, {after_passive, 1}) && channel_users(store, 12, {moved_both, 2}),
          "another user's move re-joins the target channel");
    published = {};
    check(gateway_client_publish_voice_states(object(leave), GatewayVoiceStateSource::update, 0, {scratch, 4096},
                                              {payload, sizeof(payload)}, 16, 4, capture, &published) &&
              published.count == 1,
          "another user's leave publishes");
    apply_published(&store, published);
    check(channel_users(store, 12, {moved, 1}) && !voice_state_store_find(store, 5, 20),
          "another user's leave removes their row");

    // GUILD_CREATE (lazy guild) states without session_id still replace the guild.
    constexpr auto guild_create = utf8_literal(
        R"({"id":"5","voice_states":[{"user_id":"21","channel_id":"7","mute":true},{"user_id":"22","channel_id":"7"}]})");
    published = {};
    check(gateway_client_publish_voice_states(object(guild_create), GatewayVoiceStateSource::guild, 5,
                                              {scratch, 4096}, {payload, sizeof(payload)}, 17, 4, capture,
                                              &published) &&
              published.count == 1,
          "GUILD_CREATE voice states without sessions publish");
    apply_published(&store, published);
    const u64 created[]{21, 22};
    check(channel_users(store, 7, {created, 2}) && voice_state_store_channel_count(store, 12) == 0,
          "GUILD_CREATE replaces the guild's rows with every member");
}
} // namespace

void test_voice_state_store(Check check) noexcept {
    store_cases(check);
    projection_cases(check);
    roster_cases(check);
}
} // namespace maicord
