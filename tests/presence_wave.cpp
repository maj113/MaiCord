// Rich presence decoding and transport, automatic idle, opcode 3 bodies,
// "Share my activity", game detection matching, message requests and DM
// safety warnings (decode, request bodies, controller flows, notice card).
#include "maicord/app/game_detection.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/presence_idle.hpp"
#include "maicord/app/private_channel_actions.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/user_activity.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/client_status.hpp"
#include "maicord/protocol/gateway_payload.hpp"
#include "maicord/protocol/gateway_presence.hpp"
#include "maicord/protocol/private_conversation.hpp"
#include "maicord/protocol/self_status_settings.hpp"
#include "maicord/ui/unread_banner_scene.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/model/chat_state.hpp"

namespace maicord::presence_wave_test {
u32 failures = 0;
void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}
bool same(Utf8View a, Utf8View b) noexcept {
    return utf8_equal(a, b);
}
bool same_bytes(const u8* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(data), bytes}, expected);
}
u32 scratch[16384];

bool decode(Utf8View presence, UserActivitySet* set, bool* has) noexcept {
    const auto scan = json_scan_structurals_avx1(presence, {scratch, 16384});
    return scan.ok() && user_activities_decode({presence, {scratch, scan.count}}, {presence, 0, JsonValueKind::object},
                                               42, set, has);
}
bool appearance(Utf8View presence, UserAppearance* out) noexcept {
    const auto scan = json_scan_structurals_avx1(presence, {scratch, 16384});
    *out = {};
    out->user_id = 42;
    return scan.ok() &&
           activity_decode({presence, {scratch, scan.count}}, {presence, 0, JsonValueKind::object}, out);
}
Utf8View text(const UserActivity& a, UserActivityField f) noexcept {
    return user_activity_text(a, f);
}

void activities() noexcept {
    UserActivitySet set{};
    bool has = false;
    check(decode(utf8_literal(R"({"status":"online","activities":[
        {"type":4,"name":"Custom Status","state":"Busy"},
        {"type":0,"name":"Mossrun","application_id":"500000000000000001","details":"Chapter 7",
         "state":"Ascent","timestamps":{"start":1700000000000},"party":{"id":"p","size":[2,4]},
         "assets":{"large_image":"500000000000000002","large_text":"High Ridge","small_image":"mp:external/abc/https/x.png","small_text":"Wanderer"},
         "buttons":["Watch","Join"],"flags":1},
        {"type":2,"name":"Spotify","party":{"id":"spotify:1"},"details":"Song","state":"Artist A; Artist B",
         "timestamps":{"start":1700000000000,"end":1700000180000},
         "assets":{"large_image":"spotify:ab67616d0000b273","large_text":"Album"}},
        {"type":1,"name":"Twitch","details":"Speedrun","url":"https://twitch.tv/x"}]})"),
                 &set, &has) &&
              has && set.count == 3 && set.user_id == 42,
          "activities: custom status is skipped and three rich activities decode in order");
    const auto& game = set.items[0];
    check(game.type == UserActivityType::playing && game.application_id == 500000000000000001ull &&
              same(text(game, UserActivityField::name), utf8_literal("Mossrun")) &&
              same(text(game, UserActivityField::details), utf8_literal("Chapter 7")) &&
              same(text(game, UserActivityField::state), utf8_literal("Ascent")) &&
              same(text(game, UserActivityField::large_text), utf8_literal("High Ridge")) &&
              same(text(game, UserActivityField::small_text), utf8_literal("Wanderer")) &&
              game.start_ms == 1700000000000ull && !game.end_ms && game.party_size == 2 && game.party_max == 4 &&
              game.button_count == 2 && same(text(game, UserActivityField::button_1), utf8_literal("Join")) &&
              game.flags == 1,
          "activities: playing keeps application, text, timestamps, party, assets and button labels");
    char url[384];
    MediaAttachmentFormat format{};
    u32 bytes = user_activity_image_url(game, true, {url, sizeof(url)}, &format);
    check(same({url, bytes}, utf8_literal("https://cdn.discordapp.com/app-assets/500000000000000001/"
                                          "500000000000000002.png?size=160")) &&
              format == MediaAttachmentFormat::png,
          "activities: numeric assets use the application asset CDN as PNG");
    bytes = user_activity_image_url(game, false, {url, sizeof(url)}, &format);
    check(same({url, bytes},
               utf8_literal("https://media.discordapp.net/external/abc/https/x.png?format=webp&width=160&height=160")) &&
              format == MediaAttachmentFormat::webp,
          "activities: mp: assets resolve through the media proxy as WebP");
    const auto& song = set.items[1];
    bytes = user_activity_image_url(song, true, {url, sizeof(url)}, &format);
    check(song.type == UserActivityType::listening && user_activity_is_spotify(song) &&
              same({url, bytes}, utf8_literal("https://i.scdn.co/image/ab67616d0000b273")),
          "activities: Spotify art comes from i.scdn.co");
    const auto& stream = set.items[2];
    check(stream.type == UserActivityType::streaming &&
              same(text(stream, UserActivityField::url), utf8_literal("https://twitch.tv/x")),
          "activities: streaming keeps its URL");
    char line[160];
    const struct {
        UserActivityType type;
        Utf8View name, details, expected;
    } summaries[] = {
        {UserActivityType::playing, utf8_literal("Mossrun"), {}, utf8_literal("Playing Mossrun")},
        {UserActivityType::streaming, utf8_literal("Twitch"), utf8_literal("Speedrun"), utf8_literal("Streaming Speedrun")},
        {UserActivityType::listening, utf8_literal("Spotify"), {}, utf8_literal("Listening to Spotify")},
        {UserActivityType::watching, utf8_literal("YouTube"), {}, utf8_literal("Watching YouTube")},
        {UserActivityType::competing, utf8_literal("Arena"), {}, utf8_literal("Competing in Arena")},
    };
    bool all = true;
    for (const auto& s : summaries)
        all = all && same({line, user_activity_summary(s.type, s.name, s.details, {line, sizeof(line)})}, s.expected);
    check(all && !user_activity_summary(UserActivityType::custom, utf8_literal("x"), {}, {line, sizeof(line)}),
          "activities: one-line summaries read like the official client's for every type");
    check(same({line, user_activity_heading(song, {line, sizeof(line)})}, utf8_literal("Listening to Spotify")) &&
              same({line, user_activity_heading(game, {line, sizeof(line)})}, utf8_literal("Playing")),
          "activities: card headings");

    // Seconds timestamps normalize, malformed rows are ignored, and long
    // names are cut at a code point instead of failing the presence.
    char big[1200];
    u32 at = 0;
    const auto put = [&](Utf8View v) noexcept {
        copy_bytes(big + at, v.data, v.bytes);
        at += v.bytes;
    };
    put(utf8_literal(R"({"activities":[7,{"type":3,"name":")"));
    for (u32 i = 0; i < 100; ++i)
        put(utf8_literal("\xC3\xA9"));
    put(utf8_literal(R"(","timestamps":{"start":1700000000}},{"type":9,"name":"bad"},{"type":5,"name":"x","assets":{"large_image":"mp:../evil"}}]})"));
    check(decode({big, at}, &set, &has) && has && set.count == 2 && set.items[0].type == UserActivityType::watching &&
              text(set.items[0], UserActivityField::name).bytes == 128 &&
              utf8_validate(text(set.items[0], UserActivityField::name)).valid &&
              set.items[0].start_ms == 1700000000000ull &&
              !user_activity_image_url(set.items[1], true, {url, sizeof(url)}, &format),
          "activities: truncation, second timestamps, unknown types and unsafe proxy paths");
    check(decode(utf8_literal(R"({"status":"idle"})"), &set, &has) && !has && !set.count &&
              decode(utf8_literal(R"({"activities":null})"), &set, &has) && has && !set.count,
          "activities: absent activities keep state; null clears");

    // Wire and store.
    decode(utf8_literal(R"({"activities":[{"type":0,"name":"Mossrun","assets":{"large_image":"1"}},{"type":2,"name":"Spotify"}]})"),
           &set, &has);
    u8 wire[4096];
    const u32 written = user_activities_write(set, {wire, sizeof(wire)});
    UserActivitySet restored{};
    check(written && user_activities_read({wire, written}, 42, &restored) && restored.count == 2 &&
              same(text(restored.items[0], UserActivityField::large_image), utf8_literal("1")) &&
              !user_activities_read({wire, written - 1}, 42, &restored) &&
              !user_activities_read({wire + 1, written - 1}, 42, &restored),
          "activities: wire roundtrip validates length and marker");
    UserActivityStore store{};
    const u32 r0 = store.revision;
    check(user_activity_upsert(&store, set) && store.revision == r0 + 1 && user_activity_upsert(&store, set) &&
              store.revision == r0 + 1 && user_activity_find(store, 42, &restored) && restored.count == 2 &&
              user_activity_count(store, 42) == 2 && store.bank_used == written + 4,
          "activities: the store keeps the wire form and advances its revision only on change");
    UserActivitySet other = set;
    other.user_id = 7;
    UserActivitySet cleared{};
    cleared.user_id = 42;
    check(user_activity_upsert(&store, other) && user_activity_upsert(&store, cleared) &&
              !user_activity_find(store, 42, &restored) && !user_activity_count(store, 42) &&
              user_activity_find(store, 7, &restored) && store.count == 1,
          "activities: an empty set removes the user");
    user_activity_release(&store);

    // Churn: records of changing size are replaced at the bank's end and holes
    // are compacted before more pages commit; the user cap evicts the least
    // recently updated user and every other user stays readable.
    UserActivitySet churn = set;
    bool readable = true;
    for (u32 round = 0; round < 3; ++round)
        for (u32 user = 1; user <= 2049; ++user) {
            churn.user_id = user;
            churn.items[0].lengths[static_cast<u32>(UserActivityField::name)] =
                static_cast<u16>(1 + (user + round) % 7);
            readable = readable && user_activity_upsert(&store, churn);
        }
    u32 found = 0;
    for (u32 user = 1; user <= 2049; ++user)
        if (user_activity_find(store, user, &restored)) {
            ++found;
            readable = readable && restored.count == 2 &&
                       text(restored.items[0], UserActivityField::name).bytes == 1 + (user + 2) % 7;
        }
    check(readable && store.count == 2048 && found == 2048 && !user_activity_count(store, 1) &&
              store.bank.capacity <= 2 * (store.bank_used - store.bank_dead) + 4096,
          "activities: churn compacts the bank and evicts the oldest user at the cap");
    user_activity_release(&store);

    UserAppearance a{};
    check(appearance(utf8_literal(R"({"activities":[{"type":0,"name":"Mossrun"}]})"), &a) &&
              same(a.activity.view(), utf8_literal("Playing Mossrun")) &&
              appearance(utf8_literal(R"({"activities":[{"type":0,"name":"Mossrun"},{"type":4,"state":"Away"}]})"), &a) &&
              same(a.activity.view(), utf8_literal("Away")),
          "activities: the member list line reads Playing X and custom status wins");

    // PRESENCE_UPDATE carries the activities trailer after the appearance.
    u64 ids[64]{};
    char packet[16384];
    GatewayEnvelope envelope{};
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    const auto json = utf8_literal(R"({"op":0,"s":3,"t":"PRESENCE_UPDATE","d":{"user":{"id":"42"},"status":"online","activities":[{"type":0,"name":"Mossrun","timestamps":{"start":5}}]}})");
    UserAppearance routed{};
    u32 consumed = 0;
    check(gateway_decode_envelope(json, {scratch, 16384}, 64, &envelope).ok() &&
              gateway_client_route_dispatch(envelope, {scratch, 16384}, 64, {packet, sizeof(packet)}, {ids, 64}, 1,
                                            &event, &payload)
                  .ok() &&
              event.dispatch_kind == GatewayClientDispatchKind::presence_update &&
              user_appearance_read_prefix(payload, 42, 0, &routed, &consumed) && consumed < payload.count &&
              user_activities_read({payload.data + consumed, payload.count - consumed}, 42, &restored) &&
              restored.count == 1 && restored.items[0].start_ms == 5000 &&
              same(routed.activity.view(), utf8_literal("Playing Mossrun")),
          "activities: PRESENCE_UPDATE routes the appearance and the structured activities");
}

// Per-platform presence: client_status decoding, packing and routing.
u8 platforms(Utf8View json, bool* ok) noexcept {
    u8 packed = 0xFF;
    *ok = client_status_decode({json, 0, json.bytes && json.data[0] == '{' ? JsonValueKind::object
                                                                            : JsonValueKind::null_value},
                               &packed);
    return packed;
}
void client_status() noexcept {
    bool ok = false;
    const u8 all = platforms(
        utf8_literal(R"({"desktop":"idle","mobile":"online","web":"dnd","embedded":"online","vr":"online"})"), &ok);
    using P = ClientPlatform;
    using S = ClientPlatformStatus;
    check(ok && client_status_get(all, P::desktop) == S::idle && client_status_get(all, P::mobile) == S::online &&
              client_status_get(all, P::web) == S::dnd && client_status_get(all, P::embedded) == S::online &&
              all == 0x76,
          "client status: four platforms pack two bits each; unknown platforms are ignored");
    const u8 odd = platforms(utf8_literal(R"({"mobile":"busy","desktop":1,"web":"offline"})"), &ok);
    const u8 none = platforms(utf8_literal("null"), &ok);
    bool duplicate_ok = true;
    platforms(utf8_literal(R"({"mobile":"online","mobile":"idle"})"), &duplicate_ok);
    check(!odd && ok && !none && !duplicate_ok &&
              client_status_with(client_status_with(0, P::web, S::dnd), P::web, S::offline) == 0,
          "client status: unknown values read as offline, null is empty and a repeated platform is refused");
    check(client_status_mobile_online(client_status_with(0, P::mobile, S::online)) &&
              !client_status_mobile_online(client_status_with(client_status_with(0, P::mobile, S::online), P::desktop,
                                                              S::online)) &&
              !client_status_mobile_online(client_status_with(0, P::mobile, S::idle)),
          "client status: mobile-only online follows the official client's rule");

    u64 ids[64]{};
    char packet[16384];
    GatewayEnvelope envelope{};
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    const auto update = utf8_literal(
        R"({"op":0,"s":4,"t":"PRESENCE_UPDATE","d":{"user":{"id":"42"},"guild_id":"7","status":"online",)"
        R"("client_status":{"mobile":"online","web":"idle"},"activities":[]}})");
    check(gateway_decode_envelope(update, {scratch, 16384}, 64, &envelope).ok() &&
              gateway_client_route_dispatch(envelope, {scratch, 16384}, 64, {packet, sizeof(packet)}, {ids, 64}, 1,
                                            &event, &payload)
                  .ok() &&
              event.dispatch_kind == GatewayClientDispatchKind::presence_update && event.presence.user_id == 42 &&
              event.presence.client_status == client_status_with(client_status_with(0, P::mobile, S::online), P::web,
                                                                 S::idle),
          "client status: PRESENCE_UPDATE carries the packed platforms");
    const auto malformed = utf8_literal(
        R"({"op":0,"s":5,"t":"PRESENCE_UPDATE","d":{"user":{"id":"42"},"status":"idle","client_status":[1]}})");
    event = {};
    check(gateway_decode_envelope(malformed, {scratch, 16384}, 64, &envelope).ok() &&
              gateway_client_route_dispatch(envelope, {scratch, 16384}, 64, {packet, sizeof(packet)}, {ids, 64}, 1,
                                            &event, &payload)
                  .ok() &&
              event.presence.status == GatewayClientMemberListStatus::idle && !event.presence.client_status,
          "client status: an unexpected client_status shape keeps the presence with no platform");
    const auto list = utf8_literal(
        R"({"op":0,"s":6,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"SYNC",)"
        R"("range":[0,1],"items":[{"member":{"user":{"id":"20","username":"Phone"},"roles":[],)"
        R"("presence":{"status":"online","client_status":{"mobile":"online"}}}},)"
        R"({"member":{"user":{"id":"21","username":"Desk"},"roles":[],"presence":{"status":"online",)"
        R"("client_status":{"desktop":"online","mobile":"online"}}}}]}]}})");
    event = {};
    bool handled = false;
    bool routed = gateway_decode_envelope(list, {scratch, 16384}, 64, &envelope).ok() &&
                  gateway_client_route_member_list_dispatch(envelope, {scratch, 16384}, 64,
                                                            {packet, sizeof(packet)}, &event, &payload, &handled)
                      .ok() &&
                  handled;
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_payload_bytes = static_cast<u32>(payload.count);
    u32 cursor = 0, index = 0, members = 0;
    u8 seen[2]{};
    GatewayClientMemberListMutation mutation{};
    ConstSlice<u8> roles{};
    Utf8View nickname{}, display{}, avatar{};
    while (routed && gateway_client_event_member_list_record(event, payload, &cursor, &index, &mutation, &roles,
                                                             &nickname, &display, &avatar))
        if (mutation.kind == GatewayClientMemberListItemKind::member &&
            mutation.operation != GatewayClientMemberListOperation::invalidate && members < 2)
            seen[members++] = mutation.client_status;
    check(routed && members == 2 && client_status_mobile_online(seen[0]) && !client_status_mobile_online(seen[1]) &&
              client_status_get(seen[1], P::desktop) == S::online,
          "client status: member list presences carry their platforms");
}

// The store keeps one activity set per user while presences arrive per scope
// (friends without a guild, each shared server with one). A server that hides
// the activity must not erase the friend presence's (the missing profile card).
void scopes() noexcept {
    UserActivitySet song{}, none{};
    bool has = false;
    decode(utf8_literal(R"({"activities":[{"type":2,"name":"Spotify","details":"Song","state":"Band",)"
                        R"("party":{"id":"spotify:9"}}]})"),
           &song, &has);
    none.user_id = 42;
    UserActivityStore store{};
    check(song.count == 1 && user_activity_apply(&store, song, 0, false) &&
              user_activity_apply(&store, none, 7, false) && user_activity_count(store, 42) == 1,
          "scopes: an empty server presence keeps the friend presence's activities");
    check(user_activity_apply(&store, none, 0, false) && !user_activity_count(store, 42),
          "scopes: the friend presence clears its own activities");
    const u32 before = store.revision;
    check(user_activity_apply(&store, song, 7, false) && user_activity_apply(&store, none, 8, false) &&
              user_activity_count(store, 42) == 1 && store.revision == before + 1 &&
              user_activity_apply(&store, none, 7, false) && !user_activity_count(store, 42),
          "scopes: a server's set is cleared by that server, not another");
    check(user_activity_apply(&store, song, 0, false) && user_activity_apply(&store, none, 7, true) &&
              !user_activity_count(store, 42),
          "scopes: going offline anywhere clears the activities");
    UserActivityType type{};
    check(user_activity_apply(&store, song, 0, false) && user_activity_first_type(store, 42, &type) &&
              type == UserActivityType::listening && !user_activity_first_type(store, 43, &type),
          "scopes: the first activity's type reads without decoding");
    user_activity_release(&store);

    // At the cap, server-scoped records go before older friend records.
    UserActivitySet churn = song;
    bool filled = true;
    churn.user_id = 1;
    filled = user_activity_apply(&store, churn, 0, false);
    for (u64 user = 2; filled && user <= 2049; ++user) {
        churn.user_id = user;
        filled = user_activity_apply(&store, churn, 5, false);
    }
    check(filled && store.count == 2048 && user_activity_count(store, 1) == 1 && !user_activity_count(store, 2) &&
              user_activity_count(store, 2049) == 1,
          "scopes: eviction keeps friends and drops the oldest server record");
    user_activity_release(&store);
}

// Short status lines for user rows, with status_display_type.
void status_text() noexcept {
    UserActivitySet set{};
    bool has = false;
    char out[160];
    const auto line = [&](u32 i) noexcept {
        return Utf8View{out, user_activity_status_text(set.items[i], {out, sizeof(out)})};
    };
    check(decode(utf8_literal(R"({"activities":[
        {"type":2,"name":"Spotify","details":"Track","state":"First; Second","party":{"id":"spotify:3"}},
        {"type":0,"name":"Turbo Ball","details":"Duel 2 - 2","state":"In match"},
        {"type":2,"name":"Radio Ten","details":"Show"}]})"),
                 &set, &has) &&
              same(line(0), utf8_literal("First, Second")) && same(line(1), utf8_literal("Turbo Ball")) &&
              same(line(2), utf8_literal("Listening to Radio Ten")),
          "status text: Spotify artists, a game's name, and Listening to for other music");
    check(decode(utf8_literal(R"({"activities":[
        {"type":1,"name":"Stream Site","details":"Building a deck"},
        {"type":3,"name":"Cinema Night"},
        {"type":5,"name":"League Cup"}]})"),
                 &set, &has) &&
              same(line(0), utf8_literal("Streaming Building a deck")) &&
              same(line(1), utf8_literal("Watching Cinema Night")) && same(line(2), utf8_literal("Competing in League Cup")),
          "status text: streams name their details; watching and competing keep their verbs");
    check(decode(utf8_literal(R"({"activities":[
        {"type":0,"name":"Turbo Ball","details":"Duel","state":"Ranked","status_display_type":1},
        {"type":0,"name":"Turbo Ball","details":"Duel","state":"Ranked","status_display_type":2},
        {"type":0,"name":"Turbo Ball","details":"","state":"Ranked","status_display_type":2}]})"),
                 &set, &has) &&
              same(line(0), utf8_literal("Ranked")) && same(line(1), utf8_literal("Duel")) &&
              same(line(2), utf8_literal("Turbo Ball")),
          "status text: status_display_type picks state or details when they are set");
    u8 wire[kUserActivityRecordMaximumBytes];
    UserActivitySet restored{};
    const u32 written = user_activities_write(set, {wire, sizeof(wire)});
    check(written && user_activities_read({wire, written}, 42, &restored) &&
              (restored.items[1].detail & user_activity_display_mask) == (3u << user_activity_display_shift),
          "status text: the display type survives the wire form");
}

// READY_SUPPLEMENTAL's server presences reach the store as guild presences.
struct Captured {
    u32 count;
    GatewayClientEvent events[4];
    u8 payloads[4][2048];
    u32 bytes[4];
};
bool capture(void* context, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    auto& c = *static_cast<Captured*>(context);
    if (c.count == 4 || payload.count > sizeof(c.payloads[0]))
        return false;
    c.events[c.count] = event;
    copy_bytes(c.payloads[c.count], payload.data, payload.count);
    c.bytes[c.count++] = payload.count;
    return true;
}
void guild_presences() noexcept {
    const auto data = utf8_literal(
        R"({"guilds":[{"id":"7"},{"id":"8"}],"merged_presences":{"friends":[],"guilds":[)"
        R"([{"user_id":"42","status":"online","activities":[{"type":0,"name":"Mossrun","timestamps":{"start":5}}]},)"
        R"({"user_id":"43","status":"online","activities":[]}],)"
        R"([{"bad":true},{"user_id":"44","status":"idle","activities":[{"type":4,"name":"Custom Status","state":"Away"}]}]]}})");
    static u32 objects[4096];
    static Captured captured{};
    const auto routed = gateway_client_publish_guild_presences({data, 0, JsonValueKind::object}, {scratch, 16384},
                                                               {objects, 4096}, 9, 1, capture, &captured);
    UserAppearance appearance{};
    UserActivitySet restored{};
    u32 consumed = 0;
    check(routed.ok() && captured.count == 2 && captured.events[0].presence.user_id == 42 &&
              captured.events[0].presence.guild_id == 7 &&
              user_appearance_read_prefix({captured.payloads[0], captured.bytes[0]}, 42, 7, &appearance, &consumed) &&
              consumed < captured.bytes[0] &&
              user_activities_read({captured.payloads[0] + consumed, captured.bytes[0] - consumed}, 42, &restored) &&
              restored.count == 1,
          "guild presences: a server presence with activities carries its guild and the activities trailer");
    check(captured.events[1].presence.user_id == 44 && captured.events[1].presence.guild_id == 8 &&
              user_appearance_read({captured.payloads[1], captured.bytes[1]}, 44, 8, &appearance) &&
              same(appearance.activity.view(), utf8_literal("Away")) &&
              (appearance.fields & appearance_custom_status),
          "guild presences: custom status only rows go without a trailer; empty and malformed rows are skipped");

    // Member list presences carry the trailer behind the member appearance.
    static char packet[64 * 1024];
    GatewayEnvelope envelope{};
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    bool handled = false;
    const auto list = utf8_literal(
        R"({"op":0,"s":6,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"7","id":"70","ops":[{"op":"SYNC",)"
        R"("range":[0,1],"items":[{"member":{"user":{"id":"20","username":"Listener"},"roles":[],)"
        R"("presence":{"status":"online","activities":[{"type":2,"name":"Spotify","details":"Track",)"
        R"("state":"Band","party":{"id":"spotify:20"}}]}}},)"
        R"({"member":{"user":{"id":"21","username":"Idle"},"roles":[],"presence":{"status":"online","activities":[]}}}]}]}})");
    bool ok = gateway_decode_envelope(list, {scratch, 16384}, 64, &envelope).ok() &&
              gateway_client_route_member_list_dispatch(envelope, {scratch, 16384}, 64, {packet, sizeof(packet)},
                                                        &event, &payload, &handled)
                  .ok() &&
              handled;
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_payload_bytes = static_cast<u32>(payload.count);
    u32 cursor = 0, index = 0, members = 0, trailers = 0, known = 0;
    GatewayClientMemberListMutation mutation{};
    ConstSlice<u8> roles{}, global_extra{}, guild_extra{};
    Utf8View nickname{}, display{}, avatar{}, username{};
    while (ok && gateway_client_event_member_list_record(event, payload, &cursor, &index, &mutation, &roles, &nickname,
                                                         &display, &avatar, &username, &global_extra, &guild_extra)) {
        // A SYNC also invalidates its range first.
        if (mutation.kind != GatewayClientMemberListItemKind::member ||
            mutation.operation == GatewayClientMemberListOperation::invalidate)
            continue;
        ++members;
        known += (mutation.hydration_flags & gateway_member_activities_known) != 0;
        if (user_appearance_read_prefix(guild_extra, mutation.user_id, 7, &appearance, &consumed) &&
            consumed < guild_extra.count &&
            user_activities_read({guild_extra.data + consumed, guild_extra.count - consumed}, mutation.user_id,
                                 &restored) &&
            restored.count == 1 && mutation.user_id == 20)
            ++trailers;
    }
    check(ok, "guild presences: a member list SYNC with activities routes");
    check(members == 2 && known == 2, "guild presences: member list rows mark their activities known");
    check(trailers == 1, "guild presences: the active member carries the activities trailer");
}

void idle() noexcept {
    PresenceIdle s{};
    PresenceIdleInput in{kPresenceIdleTimeoutMs - 1, 1'000'000, kPresenceIdleTimeoutMs, false, false};
    check(!presence_idle_update(&s, in) && !s.idle && presence_idle_next_check_ms(s, in) == 1'000,
          "idle: active input stays online and checks again near the timeout");
    in.input_idle_ms = kPresenceIdleTimeoutMs;
    check(presence_idle_update(&s, in) && s.idle && s.since_unix_ms == 1'000'000 - kPresenceIdleTimeoutMs &&
              presence_idle_next_check_ms(s, in) == kPresenceIdlePollMs,
          "idle: ten minutes without input goes idle since input stopped");
    in.input_idle_ms = 10;
    check(presence_idle_update(&s, in) && !s.idle && !s.since_unix_ms, "idle: input ends idle immediately");
    in.input_idle_ms = kPresenceIdleTimeoutMs * 3;
    in.in_call = true;
    check(!presence_idle_update(&s, in) && !s.idle, "idle: never idle during a call");
    in.in_call = false;
    in.input_idle_ms = 0;
    in.locked = true;
    check(presence_idle_update(&s, in) && s.idle, "idle: a locked session is idle at once");

    const GatewayLocalPresence local{1234, 0, 0, {}, 0, true, false, {}};
    const auto online = gateway_presence_effective({0, GatewayUserStatus::online, false, false}, &local);
    const auto dnd = gateway_presence_effective({0, GatewayUserStatus::dnd, false, false}, &local);
    const auto invisible = gateway_presence_effective({0, GatewayUserStatus::invisible, false, false}, &local);
    const auto manual = gateway_presence_effective({0, GatewayUserStatus::idle, false, false}, &local);
    const auto active = gateway_presence_effective({0, GatewayUserStatus::online, false, false}, nullptr);
    check(online.status == GatewayUserStatus::idle && online.has_since && online.since_ms == 1234 && online.afk &&
              dnd.status == GatewayUserStatus::dnd && dnd.has_since && dnd.since_ms == 1234 && dnd.afk &&
              invisible.status == GatewayUserStatus::invisible && invisible.since_ms == 1234 && invisible.afk &&
              manual.status == GatewayUserStatus::idle && manual.has_since && manual.afk &&
              active.status == GatewayUserStatus::online && !active.afk,
          "idle: automatic idle only replaces online; DND, invisible and manual idle stay chosen with since");
}

void opcode3() noexcept {
    u8 out[1024];
    const GatewayCustomStatusActivity custom{utf8_literal("Away"), {}, 0, 0};
    const GatewayGameActivity game{utf8_literal("Mossrun"), 500000000000000001ull, 1700000000000ull};
    auto w = gateway_write_presence_update({1234, GatewayUserStatus::idle, true, true}, &custom, &game, {out, 1024});
    check(w.ok() && same_bytes(out, w.bytes,
                               utf8_literal(R"({"op":3,"d":{"status":"idle","since":1234,"activities":[{"name":"Custom Status","type":4,"state":"Away"},{"name":"Mossrun","type":0,"application_id":"500000000000000001","timestamps":{"start":1700000000000}}],"afk":true}})")),
          "op 3: automatic idle with custom status and a shared game");
    const GatewayGameActivity bare{utf8_literal("Terraria"), 0, 0};
    w = gateway_write_presence_update({0, GatewayUserStatus::online, false, false}, nullptr, &bare, {out, 1024});
    check(w.ok() && same_bytes(out, w.bytes,
                               utf8_literal(R"({"op":3,"d":{"status":"online","since":0,"activities":[{"name":"Terraria","type":0}],"afk":false}})")),
          "op 3: a game without application or start");
    w = gateway_write_presence_update({0, GatewayUserStatus::dnd, false, false}, nullptr, nullptr, {out, 1024});
    check(w.ok() && same_bytes(out, w.bytes,
                               utf8_literal(R"({"op":3,"d":{"status":"dnd","since":0,"activities":[],"afk":false}})")),
          "op 3: status only body is unchanged");
    const GatewayGameActivity rich{utf8_literal("Lo-fi \"beats\""), 0, 1700000000000ull, 1700000300000ull,
                                   utf8_literal("Track 3"), utf8_literal("Album"), 2};
    w = gateway_write_presence_update({0, GatewayUserStatus::online, false, false}, &custom, &rich, {out, 1024});
    check(w.ok() && same_bytes(out, w.bytes,
                               utf8_literal(R"({"op":3,"d":{"status":"online","since":0,"activities":[{"name":"Custom Status","type":4,"state":"Away"},{"name":"Lo-fi \"beats\"","type":2,"details":"Track 3","state":"Album","timestamps":{"start":1700000000000,"end":1700000300000}}],"afk":false}})")),
          "op 3: a plugin's listening activity with details, state and both timestamps");
    const GatewayGameActivity ending{utf8_literal("Match"), 7, 0, 99, {}, {}, 5};
    const GatewayGameActivity streaming{utf8_literal("Stream"), 0, 0, 0, {}, {}, 1};
    w = gateway_write_presence_update({0, GatewayUserStatus::online, false, false}, nullptr, &ending, {out, 1024});
    check(w.ok() && same_bytes(out, w.bytes,
                               utf8_literal(R"({"op":3,"d":{"status":"online","since":0,"activities":[{"name":"Match","type":5,"application_id":"7","timestamps":{"end":99}}],"afk":false}})")) &&
              !gateway_write_presence_update({0, GatewayUserStatus::online, false, false}, nullptr, &streaming,
                                             {out, 1024}).ok(),
          "op 3: competing with only an end timestamp; streaming and custom types are refused");
    const GatewayGameActivity empty{{}, 0, 0};
    check(!gateway_write_presence_update({0, GatewayUserStatus::online, false, false}, nullptr, &empty, {out, 1024})
               .ok() &&
              !gateway_write_presence_update({0, GatewayUserStatus::online, false, false}, nullptr, &bare, {out, 40})
                   .ok(),
          "op 3: empty game names and small buffers are rejected");
    GatewayLocalPresence a{}, b{};
    a.game = b.game = true;
    copy_bytes(a.game_name, "Mossrun", 7);
    copy_bytes(b.game_name, "Mossrun", 7);
    a.game_name_bytes = b.game_name_bytes = 7;
    b.game_started_ms = 9;
    GatewayLocalPresence invalid{};
    invalid.idle = true;
    check(gateway_local_presence_valid(a) && !gateway_local_presence_valid(invalid) &&
              !gateway_local_presence_equal(a, b) && (b.game_started_ms = 0, gateway_local_presence_equal(a, b)),
          "op 3: local presence validation and change detection");
    copy_bytes(b.game_state, "Lobby", 5);
    b.game_state_bytes = 5;
    const bool state_differs = !gateway_local_presence_equal(a, b);
    b.game_type = 1;
    const bool streaming_refused = !gateway_local_presence_valid(b);
    b.game_type = 3;
    b.game_started_ms = 10;
    b.game_ends_ms = 5;
    check(state_differs && streaming_refused && !gateway_local_presence_valid(b) &&
              (b.game_ends_ms = 20, gateway_local_presence_valid(b)),
          "op 3: rich activity fields take part in validation and change detection");

    // StatusSettings.show_current_game: absent is shared; patches keep siblings.
    const u8 group[] = {0x0a, 0x08, 0x0a, 0x06, 'o', 'n', 'l', 'i', 'n', 'e'};
    bool share = false;
    u8 scratch_bytes[64], body[256];
    check(self_status_settings_read_show_game({group, sizeof(group)}, &share) && share,
          "share: an absent show_current_game reads as shared");
    const auto patch = self_status_settings_write_show_game_patch({group, sizeof(group)}, false,
                                                                   {scratch_bytes, sizeof(scratch_bytes)},
                                                                   {body, sizeof(body)});
    u8 decoded[64];
    Base64Result b64{};
    if (patch.ok() && patch.bytes > 15)
        b64 = base64_decode({reinterpret_cast<const char*>(body) + 13, patch.bytes - 15}, {decoded, sizeof(decoded)});
    const u8 expected[] = {0x5a, 0x0c, 0x0a, 0x08, 0x0a, 0x06, 'o', 'n', 'l', 'i', 'n', 'e', 0x1a, 0x00};
    bool shared_after = true;
    check(patch.ok() && bytes_equal(body, "{\"settings\":\"", 13) && b64.ok() && b64.bytes == sizeof(expected) &&
              bytes_equal(decoded, expected, sizeof(expected)) &&
              self_status_settings_read_show_game({decoded + 2, b64.bytes - 2}, &shared_after) && !shared_after,
          "share: turning sharing off writes an empty BoolValue and keeps the status");
    const u8 off[] = {0x1a, 0x00, 0x0a, 0x08, 0x0a, 0x06, 'o', 'n', 'l', 'i', 'n', 'e'};
    const auto on = self_status_settings_write_show_game_patch({off, sizeof(off)}, true, {scratch_bytes, 64},
                                                                {body, sizeof(body)});
    b64 = on.ok() ? base64_decode({reinterpret_cast<const char*>(body) + 13, on.bytes - 15}, {decoded, 64}) : Base64Result{};
    check(on.ok() && b64.ok() && self_status_settings_read_show_game({decoded + 2, b64.bytes - 2}, &share) && share,
          "share: turning sharing on replaces the old value");
}

void detection() noexcept {
    const Utf8View running[] = {utf8_literal("explorer.exe"), utf8_literal("terraria.exe"),
                                utf8_literal("league of legends.exe")};
    RegisteredGames games{};
    auto match = game_detection_match(games, game_detection_builtin(), {running, 3});
    check(match.found && !match.custom && same(match.name, utf8_literal("League of Legends")),
          "detection: the first built-in game in table order matches a running process");
    check(registered_games_add(&games, utf8_literal("Terraria Modded"), utf8_literal("Terraria.EXE")) &&
              !registered_games_add(&games, utf8_literal("Again"), utf8_literal("terraria.exe")) &&
              !registered_games_add(&games, utf8_literal("Path"), utf8_literal("C:\\x.exe")),
          "detection: added games normalize, reject duplicates and paths");
    match = game_detection_match(games, game_detection_builtin(), {running, 3});
    check(match.found && match.custom && same(match.name, utf8_literal("Terraria Modded")),
          "detection: added games win over the built-in table");
    const Utf8View idle_list[] = {utf8_literal("explorer.exe"), utf8_literal("svchost.exe")};
    check(!game_detection_match(games, game_detection_builtin(), {idle_list, 2}).found,
          "detection: no game without a matching process");
    char name[64];
    check(same({name, game_detection_display_name(utf8_literal("stardew valley.exe"), {name, 64})},
               utf8_literal("Stardew valley")),
          "detection: display names drop .exe and capitalize");
    u8 record[1024];
    RegisteredGames back{};
    const u32 bytes = registered_games_encode(games, {record, sizeof(record)});
    check(bytes && registered_games_decode({record, bytes}, &back) && back.count == 1 &&
              !registered_games_decode({record, bytes - 1}, &back) && registered_games_remove(&games, 0) &&
              !games.count,
          "detection: added games persist with a validated record");
}

struct Fixture {
    ChatStateStore state{};
    GuildRecord guilds[2]{};
    ChannelRecord channels[16]{};
    u16 guild_order[2]{}, channel_order[16]{};
    u8 names[4096]{};
    PrivateChannelStore metadata{};
    UserStore users{};
    UserRecord user_records[32]{};
    u32 user_order[32]{};
    u8 user_names[4096]{};
    char packet[8192]{};
    u64 ids[256]{};
    bool ready() noexcept {
        if (!chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 16}, {channel_order, 16},
                                         {names, 4096}) ||
            !private_channel_store_initialize_owned(&metadata, 16, 256) ||
            !user_store_initialize(&users, {user_records, 32}, {user_order, 32}, {user_names, 4096}))
            return false;
        state.private_channels = &metadata;
        state.self_user_id = 1;
        return true;
    }
    bool gateway(Utf8View json) noexcept {
        GatewayEnvelope envelope{};
        GatewayClientEvent event{};
        ConstSlice<u8> payload{};
        return gateway_decode_envelope(json, {scratch, 16384}, 64, &envelope).ok() &&
               gateway_client_route_dispatch(envelope, {scratch, 16384}, 64, {packet, 8192}, {ids, 256}, 1, &event,
                                             &payload)
                   .ok() &&
               chat_state_apply_gateway_event(&state, event, payload, {ids, 2}, &users).ok();
    }
};

void requests() noexcept {
    // Request bodies, targets and routes.
    u8 target[kPrivateChannelActionTargetBytes], body[kPrivateChannelActionBodyBytes];
    PrivateChannelActionRequest r{};
    r.kind = PrivateChannelActionKind::accept_request;
    r.channel_id = 50;
    auto w = private_channel_action_write(r, {target, sizeof(target)}, {body, sizeof(body)});
    check(w.ok && same_bytes(target, w.target_bytes, utf8_literal("/api/v9/channels/50/recipients/@me")) &&
              same_bytes(body, w.body_bytes, utf8_literal(R"({"consent_status":2})")) &&
              private_channel_action_route(r).method == RestMethod::put,
          "requests: accept is PUT recipients/@me with consent_status 2");
    r.kind = PrivateChannelActionKind::ignore_request;
    w = private_channel_action_write(r, {target, sizeof(target)}, {body, sizeof(body)});
    check(w.ok && !w.body_bytes && same_bytes(target, w.target_bytes, utf8_literal("/api/v9/channels/50/recipients/@me")) &&
              private_channel_action_route(r).method == RestMethod::delete_method,
          "requests: ignore is DELETE recipients/@me");
    r = {};
    r.kind = PrivateChannelActionKind::ignore_requests;
    r.recipients[0] = 50;
    r.recipients[1] = 51;
    r.recipient_count = 2;
    w = private_channel_action_write(r, {target, sizeof(target)}, {body, sizeof(body)});
    check(w.ok && same_bytes(target, w.target_bytes, utf8_literal("/api/v9/channels/recipients/@me/batch-reject")) &&
              same_bytes(body, w.body_bytes, utf8_literal(R"({"channel_ids":["50","51"]})")),
          "requests: ignore all uses the batch reject endpoint");
    r = {};
    r.kind = PrivateChannelActionKind::acknowledge_safety;
    r.channel_id = 50;
    r.recipients[0] = 900;
    r.recipient_count = 1;
    w = private_channel_action_write(r, {target, sizeof(target)}, {body, sizeof(body)});
    check(w.ok && same_bytes(target, w.target_bytes, utf8_literal("/api/v9/channels/50/safety-warnings/ack")) &&
              same_bytes(body, w.body_bytes, utf8_literal(R"({"warning_ids":["900"]})")) &&
              private_channel_action_route(r).method == RestMethod::post,
          "safety: dismiss posts the warning ids to safety-warnings/ack");
    r.recipient_count = 0;
    check(!private_channel_action_valid(r), "safety: an acknowledgement needs a warning id");

    static Fixture f;
    check(f.ready() &&
              f.gateway(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","s":1,"d":{"id":"50","type":1,"is_message_request":true,"is_message_request_timestamp":"2026-10-07T10:00:00Z","last_message_id":"5000","recipients":[{"id":"2","username":"B"}]}})")) &&
              f.gateway(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","s":2,"d":{"id":"51","type":1,"is_message_request":true,"is_spam":true,"recipients":[{"id":"3","username":"C"}]}})")) &&
              f.gateway(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","s":3,"d":{"id":"52","type":1,"safety_warnings":[{"id":"800","type":2,"dismiss_timestamp":"2026-10-07T10:00:00Z"},{"id":"801","type":4,"expiry":"2026-11-01T00:00:00Z","dismiss_timestamp":null}],"recipients":[{"id":"4","username":"D"}]}})")) &&
              f.gateway(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","s":4,"d":{"id":"53","type":1,"recipients":[{"id":"5","username":"E"}]}})")),
          "requests: request, spam and warned DMs arrive through the gateway");
    const auto* request = private_channel_store_find(f.metadata, 50);
    const auto* spam = private_channel_store_find(f.metadata, 51);
    const auto* warned = private_channel_store_find(f.metadata, 52);
    const auto* plain = private_channel_store_find(f.metadata, 53);
    check(request && request->request_flags == private_request_pending && spam &&
              spam->request_flags == (private_request_pending | private_request_spam) && warned &&
              warned->safety_warning_id == 801 && warned->safety_warning == PrivateSafetyWarning::likely_account_takeover &&
              plain && !plain->request_flags && !plain->safety_warning_id &&
              private_channel_store_request_count(f.metadata, false) == 1 &&
              private_channel_store_request_count(f.metadata, true) == 1,
          "requests: flags, spam and the first undismissed safety warning are stored");

    PrivateChannelActionController c{};
    RestClientResponse response{};
    response.status_code = 204;
    const auto respond = [&](u64 id) noexcept {
        response.request_id = id;
        return private_channel_action_due(c, 0) && private_channel_action_submitted(&c, id) &&
               private_channel_action_accept(&c, &f.state, &f.users, response, {}, {scratch, 16384},
                                             {f.packet, 8192}, {f.ids, 256});
    };
    check(!private_channel_begin_accept_request(&c, f.state, 53) && private_channel_begin_accept_request(&c, f.state, 50) &&
              c.request.kind == PrivateChannelActionKind::accept_request && respond(1) &&
              c.phase == PrivateChannelActionPhase::idle && !private_channel_store_find(f.metadata, 50)->request_flags,
          "requests: accepting clears the request locally after success");
    check(private_channel_begin_ignore_request(&c, f.state, 51) && respond(2) &&
              !chat_state_store_find_channel(f.state, 51) && !private_channel_store_request_count(f.metadata, true),
          "requests: ignoring removes the conversation");
    check(!private_channel_begin_dismiss_safety(&c, f.state, 53) && private_channel_begin_dismiss_safety(&c, f.state, 52) &&
              c.request.recipients[0] == 801 && respond(3) && !private_channel_store_find(f.metadata, 52)->safety_warning_id,
          "safety: dismissing acknowledges the shown warning and hides it");
    check(f.gateway(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","s":5,"d":{"id":"53","type":1,"is_message_request":false,"safety_warnings":[{"id":"802","type":1}],"recipients":[{"id":"5","username":"E"}]}})")) &&
              private_channel_store_find(f.metadata, 53)->safety_warning == PrivateSafetyWarning::stranger_danger &&
              f.gateway(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","s":6,"d":{"id":"53","type":1,"safety_warnings":[],"recipients":[{"id":"5","username":"E"}]}})")) &&
              !private_channel_store_find(f.metadata, 53)->safety_warning_id,
          "safety: channel updates add and clear warnings");

    // The notice card above the timeline.
    UiUnreadBannerScene scene{};
    UiWidgetManager widgets{};
    UiWidget widget_storage[4]{};
    UiLayoutNode node_storage[4]{};
    check(ui_widget_manager_initialize(&widgets, {widget_storage, 4}, {node_storage, 4}), "notice: widgets");
    auto style = ui_layout_style();
    style.width = style.height = ui_layout_fill();
    const auto root = ui_widget_manager_add(&widgets, ui_widget_invalid_handle(),
                                            {1, UiWidgetKind::container, ui_widget_enabled,
                                             {utf8_literal("Root"), style, {}, 0, 1}});
    const UiBannerNotice notice{50, utf8_literal("Message request"), utf8_literal("Accept to reply."),
                                utf8_literal("Accept"), utf8_literal("Ignore"), false, false};
    const UiRect viewport{0, 0, 800, 600};
    UiUnreadBannerView view{};
    view.channel_id = 50;
    check(root.ok() && ui_unread_banner_set_notice(&scene, &notice) &&
              ui_unread_banner_update(&scene, view, {}, viewport, {&widgets, root.handle}, 0, false) &&
              ui_unread_banner_visible(scene) &&
              ui_unread_banner_hit_test(scene, (scene.notice_primary_rect.left + scene.notice_primary_rect.right) / 2,
                                        (scene.notice_primary_rect.top + scene.notice_primary_rect.bottom) / 2) ==
                  UiUnreadBannerAction::notice_primary &&
              ui_unread_banner_hit_test(scene, (scene.notice_secondary_rect.left + scene.notice_secondary_rect.right) / 2,
                                        (scene.notice_secondary_rect.top + scene.notice_secondary_rect.bottom) / 2) ==
                  UiUnreadBannerAction::notice_secondary &&
              !ui_unread_banner_set_notice(&scene, &notice),
          "notice: the card shows with Accept and Ignore targets and ignores identical updates");
    UiBannerNotice busy = notice;
    busy.busy = true;
    check(ui_unread_banner_set_notice(&scene, &busy) &&
              ui_unread_banner_hit_test(scene, (scene.notice_primary_rect.left + scene.notice_primary_rect.right) / 2,
                                        (scene.notice_primary_rect.top + scene.notice_primary_rect.bottom) / 2) ==
                  UiUnreadBannerAction::none,
          "notice: actions wait while a request is in flight");
    // Long warnings wrap instead of truncating; narrow cards stack the
    // actions under the text and never squeeze the title column.
    const UiBannerNotice warning{51, utf8_literal("Be careful with this conversation"),
                                 utf8_literal("This person is new to Discord and isn't in any of your servers. Never "
                                              "share your password or download files from strangers you just met."),
                                 utf8_literal("Got it"), utf8_literal("Block"), true, false};
    const auto inside = [](UiRect inner, UiRect outer) noexcept {
        return inner.left >= outer.left && inner.right <= outer.right && inner.top >= outer.top &&
               inner.bottom <= outer.bottom;
    };
    const bool long_notice = ui_unread_banner_set_notice(&scene, &warning);
    ui_unread_banner_update(&scene, view, {}, viewport, {&widgets, root.handle}, 2, false);
    check(long_notice && ui_rect_height(scene.notice) > scene.style.notice_height &&
              ui_rect_height(scene.notice_text_rect) >= 36 && inside(scene.notice_text_rect, scene.notice) &&
              scene.notice_text_rect.right <= scene.notice_primary_rect.left &&
              scene.notice_primary_rect.right <= scene.notice_secondary_rect.left,
          "notice: a long warning wraps beside its actions on a wide card");
    const UiRect narrow{0, 0, 360, 600};
    ui_unread_banner_update(&scene, view, {}, narrow, {&widgets, root.handle}, 3, false);
    check(scene.notice_primary_rect.top >= scene.notice_text_rect.bottom &&
              ui_rect_width(scene.notice_title_rect) > 200 && inside(scene.notice_primary_rect, scene.notice) &&
              inside(scene.notice_secondary_rect, scene.notice) &&
              scene.notice_primary_rect.right <= scene.notice_secondary_rect.left &&
              ui_unread_banner_hit_test(scene, (scene.notice_secondary_rect.left + scene.notice_secondary_rect.right) / 2,
                                        (scene.notice_secondary_rect.top + scene.notice_secondary_rect.bottom) / 2) ==
                  UiUnreadBannerAction::notice_secondary,
          "notice: a narrow card moves its actions below the full-width text");
    check(ui_unread_banner_set_notice(&scene, nullptr) &&
              ui_unread_banner_update(&scene, view, {}, viewport, {&widgets, root.handle}, 1, false) &&
              !ui_unread_banner_visible(scene),
          "notice: a cleared notice hides the card");
}
} // namespace maicord::presence_wave_test

extern "C" __declspec(noreturn) void WINAPI maicord_presence_wave_entry() noexcept {
    using namespace maicord::presence_wave_test;
    activities();
    client_status();
    scopes();
    status_text();
    guild_presences();
    idle();
    opcode3();
    detection();
    requests();
    ExitProcess(failures ? 1 : 0);
}
