#include "maicord/app/gateway_channel_info_requests.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/voice_channel_info_store.hpp"
#include "maicord/protocol/voice_channel_info.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/voice_channel_rows.hpp"

// Voice channel statuses and start times: the Request Channel Info command
// and its pacing, the CHANNEL_INFO / update decoders and routing, the store
// and the channel list flags. Fixtures are synthetic.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

u32 scratch[4096];
char storage[kGatewayClientDispatchPayloadCapacity];
char text[kVoiceChannelStatusMaximumBytes];

bool bytes_are(const u8* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(data), bytes}, expected);
}

void request_cases(Check check) noexcept {
    u8 output[128];
    const auto written =
        gateway_write_channel_info_request(1'234'567'890'123'456'789ull, {output, sizeof(output)});
    check(
        written.ok() &&
            bytes_are(
                output, written.bytes,
                utf8_literal(
                    R"({"op":43,"d":{"guild_id":"1234567890123456789","fields":["status","voice_start_time"]}})")),
        "channel info: opcode 43 asks for the guild's status and voice_start_time");
    check(gateway_write_channel_info_request(0, {output, sizeof(output)}).error ==
                  GatewayCommandError::invalid_input &&
              gateway_write_channel_info_request(5, {output, 68}).error ==
                  GatewayCommandError::output_too_small,
          "channel info: a zero guild or short output writes nothing");
}

// Pacing: once per guild per session, through the outbound limiter.
void pacing_cases(Check check) noexcept {
    static GatewayChannelInfoRequests owner;
    owner = {};
    check(gateway_channel_info_admit(&owner, 5) && !gateway_channel_info_admit(&owner, 5) &&
              !gateway_channel_info_admit(&owner, 0),
          "pacing: a guild queues once; zero is refused");
    check(gateway_channel_info_next(owner, 0) == 5, "pacing: the queued guild is sent");
    gateway_channel_info_sent(&owner, 5);
    check(!gateway_channel_info_next(owner, 0) && !gateway_channel_info_admit(&owner, 5),
          "pacing: a guild is asked once per session");
    gateway_channel_info_forget(&owner, 5);
    check(gateway_channel_info_admit(&owner, 5), "pacing: GUILD_CREATE allows asking the guild again");
    gateway_channel_info_sent(&owner, 5);
    gateway_channel_info_reset_session(&owner);
    check(gateway_channel_info_admit(&owner, 5) && gateway_channel_info_next(owner, 0) == 5,
          "pacing: READY or RESUMED allows every guild again");
    gateway_channel_info_sent(&owner, 5);

    GatewayRateLimited limited{5, 2'500, 0, 8};
    gateway_channel_info_rate_limited(&owner, limited, 1'000);
    check(!gateway_channel_info_next(owner, 1'000) && gateway_channel_info_requested(owner, 5),
          "pacing: RATE_LIMITED for another opcode changes nothing");
    limited.opcode = kGatewayRequestChannelInfoOpcode;
    gateway_channel_info_rate_limited(&owner, limited, 1'000);
    check(!gateway_channel_info_next(owner, 3'499) &&
              gateway_channel_info_deadline(owner, 3'499) == 3'500 &&
              gateway_channel_info_next(owner, 3'500) == 5 && !gateway_channel_info_deadline(owner, 3'500),
          "pacing: RATE_LIMITED for opcode 43 resends the guild after retry_after");

    owner = {};
    u32 admitted = 0;
    for (u64 guild = 1; guild <= kGatewayChannelInfoPending + 4; ++guild)
        admitted += gateway_channel_info_admit(&owner, guild) ? 1u : 0u;
    check(admitted == kGatewayChannelInfoPending, "pacing: the queue is bounded");
    // A burst of selections: every queued guild is one command, each taking
    // one send of the connection's 120-per-60 s budget.
    u32 sends = 0;
    for (u64 now = 0; gateway_channel_info_next(owner, now); ++now) {
        gateway_channel_info_sent(&owner, gateway_channel_info_next(owner, now));
        ++sends;
    }
    u32 repeats = 0;
    for (u64 guild = 1; guild <= kGatewayChannelInfoPending; ++guild)
        repeats += gateway_channel_info_admit(&owner, guild) ? 1u : 0u;
    check(sends == kGatewayChannelInfoPending && !repeats &&
              owner.requested_count == kGatewayChannelInfoPending,
          "pacing: revisiting guilds in one session sends nothing more");
}

struct Seen {
    VoiceChannelInfoEntry entries[4];
    char status[4][64];
    u32 count;
};
bool collect(void* opaque, const VoiceChannelInfoEntry& entry) noexcept {
    auto& seen = *static_cast<Seen*>(opaque);
    if (seen.count == 4 || entry.status.bytes > sizeof(seen.status[0]))
        return false;
    copy_bytes(seen.status[seen.count], entry.status.data, entry.status.bytes);
    seen.entries[seen.count] = entry;
    seen.entries[seen.count].status = {seen.status[seen.count], entry.status.bytes};
    ++seen.count;
    return true;
}
VoiceChannelInfoResult decode(VoiceChannelInfoEvent event, Utf8View data, Seen* seen) noexcept {
    *seen = {};
    return voice_channel_info_decode(event, data, {scratch, 4096}, {text, sizeof(text)}, collect, seen);
}

void decode_cases(Check check) noexcept {
    static Seen seen;
    auto result = decode(
        VoiceChannelInfoEvent::channel_info,
        utf8_literal(R"({"channels":[{"id":"21","status":"Caf\u00e9 \"night\"","voice_start_time":1790000000},)"
                     R"({"id":"22","voice_start_time":null,"status":null},{"id":"23"}],"guild_id":"5"})"),
        &seen);
    check(result.ok() && result.guild_id == 5 && result.count == 3 && seen.count == 3,
          "decode: CHANNEL_INFO visits every channel with the document's guild");
    const auto& first = seen.entries[0];
    check(first.guild_id == 5 && first.channel_id == 21 && first.has_start &&
              first.start_s == 1'790'000'000 && first.has_status &&
              utf8_equal(first.status, utf8_literal("Caf\xC3\xA9 \"night\"")),
          "decode: status is unescaped and the start time is Unix seconds");
    check(seen.entries[1].has_start && !seen.entries[1].start_s && seen.entries[1].has_status &&
              !seen.entries[1].status.bytes && !seen.entries[2].has_start && !seen.entries[2].has_status,
          "decode: null clears a field and an absent one is left out");
    result = decode(VoiceChannelInfoEvent::start_time_update,
                    utf8_literal(R"({"id":"21","guild_id":"5","voice_start_time":"1790000100"})"), &seen);
    check(result.ok() && seen.count == 1 && seen.entries[0].has_start && !seen.entries[0].has_status &&
              seen.entries[0].start_s == 1'790'000'100,
          "decode: VOICE_CHANNEL_START_TIME_UPDATE carries the start");
    result = decode(VoiceChannelInfoEvent::status_update,
                    utf8_literal(R"({"id":"21","guild_id":"5","status":""})"), &seen);
    check(result.ok() && seen.count == 1 && seen.entries[0].has_status && !seen.entries[0].status.bytes &&
              !seen.entries[0].has_start,
          "decode: VOICE_CHANNEL_STATUS_UPDATE with an empty status clears it");
    check(
        decode(VoiceChannelInfoEvent::status_update, utf8_literal(R"({"id":"21","guild_id":"5"})"), &seen)
                    .error == VoiceChannelInfoError::field &&
            decode(VoiceChannelInfoEvent::channel_info,
                   utf8_literal(R"({"guild_id":"5","channels":[{"status":"x"}]})"), &seen)
                    .error == VoiceChannelInfoError::field &&
            decode(VoiceChannelInfoEvent::channel_info, utf8_literal(R"({"channels":[]})"), &seen).error ==
                VoiceChannelInfoError::field &&
            decode(VoiceChannelInfoEvent::start_time_update,
                   utf8_literal(R"({"id":"21","guild_id":"5","voice_start_time":true})"), &seen)
                    .error == VoiceChannelInfoError::field &&
            decode(VoiceChannelInfoEvent::status_update, utf8_literal(R"({"id":"21",)"), &seen).error !=
                VoiceChannelInfoError::none,
        "decode: missing IDs or fields, wrong types and broken JSON are refused");
    char tiny[4];
    check(voice_channel_info_decode(VoiceChannelInfoEvent::status_update,
                                    utf8_literal(R"({"id":"21","guild_id":"5","status":"longer"})"),
                                    {scratch, 4096}, {tiny, sizeof(tiny)}, nullptr, nullptr)
                  .error == VoiceChannelInfoError::capacity,
          "decode: a status beyond the text buffer reports capacity");

    char display[64];
    const auto shown = [&](Utf8View status, Utf8View expected) noexcept {
        const u32 bytes = voice_channel_status_display(status, {display, sizeof(display)});
        return utf8_equal({display, bytes}, expected);
    };
    check(shown(utf8_literal("Gaming <:wave:123> and <a:spin:45>!"),
                utf8_literal("Gaming :wave: and :spin:!")) &&
              shown(utf8_literal("a < b <:x:> <:y:z>"), utf8_literal("a < b <:x:> <:y:z>")),
          "display: custom emoji markup shows its name; other text stays");
}

void store_cases(Check check) noexcept {
    static VoiceChannelInfoStore store;
    voice_channel_info_store_reset(&store);
    const u32 base = store.revision;
    check(voice_channel_info_store_set_start(&store, 5, 21, 100) &&
              voice_channel_info_store_set_status(&store, 5, 21, utf8_literal("Movie night")) &&
              voice_channel_info_store_set_status(&store, 5, 22, utf8_literal("Study")) &&
              store.count == 2 && store.revision == base + 3,
          "store: one record per channel with a value");
    const auto* info = voice_channel_info_store_find(store, 21);
    check(info && info->start_s == 100 &&
              utf8_equal(voice_channel_info_status(store, *info), utf8_literal("Movie night")),
          "store: start and status read back");
    check(voice_channel_info_store_set_status(&store, 5, 21, utf8_literal("Movie night")) &&
              store.revision == base + 3,
          "store: an unchanged value is not a change");
    check(voice_channel_info_store_set_status(&store, 5, 21, {}) &&
              voice_channel_info_store_find(store, 21)->start_s == 100 && store.text_used == 5 &&
              utf8_equal(voice_channel_info_status(store, *voice_channel_info_store_find(store, 22)),
                         utf8_literal("Study")),
          "store: clearing a status packs the text and keeps the start");
    check(voice_channel_info_store_set_start(&store, 5, 21, 0) &&
              !voice_channel_info_store_find(store, 21) && store.count == 1,
          "store: a channel with no value left has no record");
    check(voice_channel_info_store_set_start(&store, 6, 31, 200) &&
              voice_channel_info_store_clear_guild(&store, 5) && store.count == 1 &&
              voice_channel_info_store_find(store, 31) && !store.text_used,
          "store: a guild's values clear together");
    check(voice_channel_info_store_remove(&store, 31) && !store.count &&
              !voice_channel_info_store_remove(&store, 31),
          "store: a deleted channel loses its values");

    voice_channel_info_store_reset(&store);
    bool filled = true;
    for (u64 i = 0; i < kVoiceChannelInfoCapacity; ++i)
        filled = filled && voice_channel_info_store_set_start(&store, 1 + i % 2, 1'000 + i, 1);
    check(filled && voice_channel_info_store_set_start(&store, 1, 9'999, 1) &&
              store.count == kVoiceChannelInfoCapacity && store.dropped == 1 &&
              voice_channel_info_store_find(store, 9'999) && !voice_channel_info_store_find(store, 1'001),
          "store: when full another guild's record makes room");
}

GatewayEnvelope envelope(Utf8View name, Utf8View data) noexcept {
    GatewayEnvelope value{};
    value.has_event_name = true;
    value.event_name = {name, 0, false};
    value.data = {data, 0, JsonValueKind::object};
    return value;
}

void routing_cases(Check check) noexcept {
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    const auto route = [&](Utf8View name, Utf8View data) noexcept {
        event = {};
        payload = {};
        return gateway_client_route_dispatch(envelope(name, data), {scratch, 4096}, 64,
                                             {storage, sizeof(storage)}, {}, &event, &payload)
            .ok();
    };
    constexpr auto info = utf8_literal(R"({"guild_id":"5","channels":[{"id":"21","status":"x"}]})");
    check(route(utf8_literal("CHANNEL_INFO"), info) &&
              event.dispatch_kind == GatewayClientDispatchKind::channel_info &&
              bytes_are(payload.data, payload.count, info),
          "routing: CHANNEL_INFO reaches the UI verbatim");
    check(route(utf8_literal("VOICE_CHANNEL_START_TIME_UPDATE"),
                utf8_literal(R"({"id":"21","guild_id":"5","voice_start_time":null})")) &&
              event.dispatch_kind == GatewayClientDispatchKind::voice_channel_start_time_update &&
              payload.count,
          "routing: start time updates reach the UI");
    check(route(utf8_literal("VOICE_CHANNEL_STATUS_UPDATE"),
                utf8_literal(R"({"id":"21","guild_id":"5","status":"y"})")) &&
              event.dispatch_kind == GatewayClientDispatchKind::voice_channel_status_update &&
              payload.count,
          "routing: status updates reach the UI");
}

void navigation_cases(Check check) noexcept {
    static VoiceChannelInfoStore store;
    voice_channel_info_store_reset(&store);
    voice_channel_info_store_set_status(&store, 5, 21, utf8_literal("Busy"));
    voice_channel_info_store_set_status(&store, 5, 23, utf8_literal("Stage"));
    UiVoiceChannelExtras extras{};
    extras.info = &store;
    extras.status_channel_id = 22;
    extras.invite_channel_id = 22;
    static ChannelViewRow rows[4];
    rows[0] = {};
    rows[0].id = 21;
    rows[0].type = 2;
    rows[1] = rows[0];
    rows[1].id = 22;
    rows[2] = rows[0];
    rows[2].id = 23;
    rows[2].type = 13;
    rows[3] = rows[0];
    rows[3].id = 24;
    NavigationView view{};
    view.channel_rows = rows;
    view.channel_capacity = view.channel_count = 4;
    view.voice_extras = &extras;
    extras.revision = 3;
    check(navigation_view_refresh_voice(&view) && rows[0].voice_subtitle && !rows[0].voice_invite &&
              rows[1].voice_subtitle && rows[1].voice_invite && !rows[2].voice_subtitle &&
              !rows[3].voice_subtitle && view.source_extras_revision == 3,
          "navigation: statuses show on voice channels, set-status and invite on the connected one");
    extras.invite_channel_id = 0;
    check(navigation_view_refresh_voice(&view) && !rows[1].voice_invite &&
              !navigation_view_refresh_voice(&view),
          "navigation: a dismissed invite row drops out once");
}
} // namespace

void test_voice_channel_info(Check check) noexcept {
    request_cases(check);
    pacing_cases(check);
    decode_cases(check);
    store_cases(check);
    routing_cases(check);
    navigation_cases(check);
}
} // namespace maicord
