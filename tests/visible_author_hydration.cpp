#include "maicord/app/visible_author_hydration.hpp"
#include "maicord/app/gateway_member_request_owner.hpp"
#include "maicord/app/message_event_pump.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
struct Fixture {
    GuildMemberStore members;
    GuildMemberRecord member_records[8];
    u32 member_order[8];
    UserStore users;
    UserRecord user_records[8];
    u32 user_order[8];
    u8 names[2048];
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channel;
    RoleRecord roles[4];
    PermissionOverwriteRecord overwrites[1];
    u16 guild_order[1], channel_order[1];
    u8 state_names[1024];
    MessageTimeline timeline;
    MessageStore messages;
    MessageRecord message_records[3];
    u32 message_order[3];
    u8 message_text[128];
    UiVariableList list;
    u32 heights[3];
    u64 fenwick[3];
    MessageRichStore rich;
    MessageRichRecord rich_records[3];
    u16 rich_order[3];
    u8 rich_text[128];
    MessageScene scene;
    MessageSceneRowWidget rows[3];
    UiWidget widgets[3 * kMessageSceneWidgetsPerRow + 1];
    UiLayoutNode nodes[3 * kMessageSceneWidgetsPerRow + 1];
    MessageEventPump pump;
    char payload[16 * 1024];
    u32 structurals[4096];
};
Fixture fixture{};
bool initialize() noexcept {
    auto& f = fixture;
    f = {};
    if (!guild_member_store_initialize(&f.members, {f.member_records, 8}, {f.member_order, 8}) ||
        !user_store_initialize(&f.users, {f.user_records, 8}, {f.user_order, 8},
                                       {f.names, sizeof(f.names)}) ||
        !chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1}, {&f.channel, 1},
                                        {f.channel_order, 1}, {f.state_names, sizeof(f.state_names)}) ||
        !chat_state_store_bind_permissions(&f.state, {f.roles, 4}, {f.overwrites, 1}) ||
        !message_store_initialize(&f.messages, {f.message_records, 3}, {f.message_order, 3},
                                  {f.message_text, sizeof(f.message_text)}) ||
        !message_rich_store_initialize(&f.rich, {f.rich_records, 3}, {f.rich_order, 3},
                                       {f.rich_text, sizeof(f.rich_text)}))
        return false;
    f.guild.id = 7;
    f.guild.occupied = true;
    f.state.guild_count = 1;
    f.guild_order[0] = 0;
    f.channel.id = 80;
    f.channel.guild_id = 7;
    f.channel.occupied = true;
    f.state.channel_count = 1;
    f.channel_order[0] = 0;
    f.state.selected_channel_id = 80;
    f.state.selected_guild_id = 7;
    f.state.self_user_id = 1;
    if (!chat_state_store_upsert_role(&f.state, {17, 7, 0, {}, 5, 0x9922CC, true}) ||
        !message_store_upsert(&f.messages, {100, 80, 20, 1000, 0, utf8_literal("Original first row"), 0})
             .ok() ||
        !message_store_upsert(&f.messages,
                              {101, 80, 201, 400000, 0, utf8_literal("Original second row"), 0})
             .ok() ||
        !message_store_upsert(&f.messages, {102, 80, 30, 800000, 0, utf8_literal("Original last row"), 0})
             .ok())
        return false;
    GatewayMessageEvent reply{};
    reply.id = 100;
    reply.fields = gateway_message_field_message_reference | gateway_message_field_referenced_message;
    reply.reference = {90,
                       80,
                       7,
                       21,
                       utf8_literal("Original reply author"),
                       {},
                       utf8_literal("Original reply"),
                       GatewayMessageReferenceState::available,
                       0};
    if (!message_rich_store_apply(&f.rich, 100, f.message_records[0].revision, reply).ok() ||
        !ui_variable_list_initialize(&f.list, {f.heights, 3}, {f.fenwick, 3}, 3, ui_extent_units(100)) ||
        !message_timeline_initialize(&f.timeline, &f.messages, &f.list, ui_extent_units(100),
                                     ui_extent_units(1000), false) ||
        !message_timeline_bind_rich_store(&f.timeline, &f.rich) ||
        !message_scene_initialize(&f.scene, &f.timeline, {f.rows, 3},
                                  {f.widgets, 3 * kMessageSceneWidgetsPerRow + 1},
                                  {f.nodes, 3 * kMessageSceneWidgetsPerRow + 1}) ||
        !message_scene_update(&f.scene, {0, 0, 640, 1000}).ok())
        return false;
    f.pump.timeline = &f.timeline;
    f.pump.members = &f.members;
    f.pump.users = &f.users;
    f.pump.state = &f.state;
    return true;
}
bool route(Utf8View source, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    auto& f = fixture;
    GatewayEnvelope envelope{};
    if (!gateway_decode_envelope(source, {f.structurals, 4096}, 32, &envelope).ok())
        return false;
    return gateway_client_route_dispatch(envelope, {f.structurals, 4096}, 32,
                                         {f.payload, sizeof(f.payload)}, {}, 1, event, payload)
        .ok();
}
bool contains(ConstSlice<u64> ids, u64 id) noexcept {
    for (u32 i = 0; i < ids.count; ++i)
        if (ids[i] == id)
            return true;
    return false;
}

void test_member_rate_limits(void (*check)(bool, const char*) noexcept) noexcept {
    u32 structural[128]{};
    GatewayRateLimited decoded{91, 92, 93, 94};
    const auto decode = [&](Utf8View text) noexcept {
        return gateway_decode_rate_limited({text, 0, JsonValueKind::object}, {structural, 128}, 16,
                                           &decoded);
    };
    check(decode(utf8_literal(R"({"opcode":8,"retry_after":0.0001,"meta":{"guild_id":"7","nonce":"9"}})"))
                  .ok() &&
              decoded.guild_id == 7 && decoded.nonce == 9 && decoded.retry_after_ms == 1 &&
              decoded.opcode == 8,
          "member rate: required event metadata and fractional seconds ceil to one millisecond");
    check(decode(utf8_literal(R"({"opcode":8,"retry_after":1.234e1,"meta":{"guild_id":"7"}})")).ok() &&
              decoded.retry_after_ms == 12340 && !decoded.nonce &&
              decode(utf8_literal(
                         R"({"opcode":8,"retry_after":1e-100,"meta":{"guild_id":"7","nonce":"foreign"}})"))
                  .ok() &&
              decoded.retry_after_ms == 1 && !decoded.nonce,
          "member rate: exponent seconds and absent or foreign nonces preserve the guild floor");
    const Utf8View invalid[] = {
        utf8_literal(R"({"opcode":8,"retry_after":-1,"meta":{"guild_id":"7"}})"),
        utf8_literal(R"({"opcode":8,"retry_after":1e100,"meta":{"guild_id":"7"}})"),
        utf8_literal(R"({"opcode":8,"retry_after":1,"retry_after":2,"meta":{"guild_id":"7"}})"),
        utf8_literal(R"({"opcode":8,"retry_after":1,"meta":{"guild_id":"7","guild_id":"8"}})"),
        utf8_literal(R"({"opcode":8,"retry_after":1,"meta":{"guild_id":"0"}})"),
        utf8_literal(R"({"opcode":8,"meta":{"guild_id":"7"}})"),
        utf8_literal(R"({"opcode":8,"retry_after":1,"meta":{"guild_id":"7","nonce":9}})"),
    };
    bool atomic = true;
    for (const auto text : invalid) {
        decoded = {91, 92, 93, 94};
        atomic &= !decode(text).ok() && decoded.guild_id == 91 && decoded.retry_after_ms == 92 &&
                  decoded.nonce == 93 && decoded.opcode == 94;
    }
    check(atomic, "member rate: invalid, duplicate, missing and overflowing values leave output unchanged");

    GatewayGuildMemberRequest batch{};
    batch.guild_id = 7;
    batch.channel_id = 80;
    batch.nonce = 9;
    batch.user_count = 1;
    batch.user_ids[0] = 20;
    GatewayMemberRequestOwner owner{};
    check(gateway_member_request_admit(&owner, batch, 100), "member rate: initial copied request admitted");
    gateway_member_request_sent(&owner);
    gateway_member_request_rate_limited(&owner, {7, 250, 9, 8}, 100);
    gateway_member_request_rate_limited(&owner, {7, 20, 9, 8}, 110);
    check(owner.occupied && owner.batch_retries == 1 && !gateway_member_request_next(owner, 349) &&
              gateway_member_request_next(owner, 350) == &owner.pending &&
              gateway_member_request_deadline(owner, 110) == 350,
          "member rate: matching sent nonce requeues once and repeated events never shorten the server "
          "floor");
    gateway_member_request_sent(&owner, gateway_member_request_next(owner, 350));
    gateway_member_request_rate_limited(&owner, {7, 10, 9, 8}, 350);
    gateway_member_request_sent(&owner, gateway_member_request_next(owner, 360));
    gateway_member_request_rate_limited(&owner, {7, 10, 9, 8}, 360);
    check(!owner.occupied && owner.batch_retries == 2,
          "member rate: a rejected request has at most two automatic replays");

    batch.guild_id = 8;
    batch.channel_id = 81;
    batch.nonce = 10;
    check(gateway_member_request_admit(&owner, batch, 360) && gateway_member_request_next(owner, 360),
          "member rate: another guild is eligible without waiting for the rejected guild");
    gateway_member_request_sent(&owner, gateway_member_request_next(owner, 360));
    gateway_member_request_rate_limited(&owner, {8, 100, 0, 8}, 360);
    gateway_member_request_rate_limited(&owner, {8, 100, 99, 8}, 360);
    check(!owner.occupied && gateway_member_request_guild_deadline(owner, 8) == 460,
          "member rate: absent or unknown nonce gates a guild without inventing replay ownership");
    gateway_member_request_rate_limited(&owner, {8, 900, 10, 9}, 360);
    check(gateway_member_request_guild_deadline(owner, 8) == 460,
          "member rate: other Gateway opcodes do not change the member gate");

    GatewayMemberChunkReceipt receipt{};
    const auto chunk =
        utf8_literal(R"({"guild_id":"8","nonce":"10","chunk_index":0,"chunk_count":1,"members":[]})");
    check(gateway_decode_member_chunk_receipt({chunk, 0, JsonValueKind::object}, {structural, 128}, 16,
                                              &receipt)
              .ok(),
          "member rate: final member chunk retains exact guild and nonce metadata");
    gateway_member_request_complete(&owner, {8, 10, 0, 2});
    check(owner.batch_current, "member rate: an intermediate chunk retains request completion authority");
    gateway_member_request_complete(&owner, receipt);
    gateway_member_request_rate_limited(&owner, {8, 100, 10, 8}, 460);
    check(!owner.occupied && !owner.batch_current,
          "member rate: a late rejection cannot revive an exactly completed member request");

    owner = {};
    for (u64 guild = 1; guild <= 8; ++guild)
        gateway_member_request_rate_limited(&owner, {guild, 100 + guild, 0, 8}, 100);
    gateway_member_request_rate_limited(&owner, {9, 200, 0, 8}, 100);
    gateway_member_request_rate_limited(&owner, {10, 400, 0, 8}, 100);
    batch.guild_id = 11;
    check(!gateway_member_request_admit(&owner, batch, 499) && owner.overflow_until_ms == 500,
          "member rate: full table defers only untracked guilds to the greatest actual overflow deadline");
    batch.guild_id = 1;
    check(gateway_member_request_admit(&owner, batch, 210) && gateway_member_request_next(owner, 210),
          "member rate: tracked guild remains independent of capacity overflow");
    gateway_member_request_cancel_batch(&owner);
    batch.guild_id = 11;
    check(gateway_member_request_admit(&owner, batch, 500) && gateway_member_request_next(owner, 500),
          "member rate: expired overflow and guild slots are reclaimable without a permanent throttle");
    gateway_member_request_rate_limited(&owner, {11, 100, batch.nonce, 8}, 500);
    gateway_member_request_reset_connection(&owner, false);
    check(!owner.batch_current && gateway_member_request_guild_deadline(owner, 11) == 600,
          "member rate: resumed session retains server floors and retires old UI request authority");
    gateway_member_request_reset_connection(&owner, true);
    check(!owner.overflow_until_ms && !gateway_member_request_guild_deadline(owner, 11),
          "member rate: fresh identified session discards old session guild floors");
}
} // namespace

void test_visible_author_hydration(void (*check)(bool, const char*) noexcept) noexcept {
    test_member_rate_limits(check);
    GatewayGuildMemberRequest request{};
    request.guild_id = 7;
    request.channel_id = 80;
    request.nonce = 9;
    request.user_count = 2;
    request.user_ids[0] = 20;
    request.user_ids[1] = 201;
    u8 wire[4096]{};
    const auto written = gateway_write_guild_member_request(request, {wire, sizeof(wire)});
    const auto expected = utf8_literal(
        R"({"op":8,"d":{"guild_id":"7","presences":false,"user_ids":["20","201"],"nonce":"9"}})");
    check(
        written.ok() && utf8_equal({reinterpret_cast<const char*>(wire), written.bytes}, expected),
        "author hydration: opcode8 sends only bounded exact IDs, no whole-guild query or presence request");
    u8 exact[256]{};
    const auto fitted = gateway_write_guild_member_request(request, {exact, written.bytes});
    exact[0] = 0x5A;
    const auto short_output = gateway_write_guild_member_request(request, {exact, written.bytes - 1});
    request.user_ids[1] = 20;
    const auto duplicate = gateway_write_guild_member_request(request, {exact, sizeof(exact)});
    check(fitted.ok() && !short_output.ok() && !duplicate.ok() && exact[0] == 0x5A,
          "author hydration: exact-size command and duplicate rejection preserve failed output");
    request.user_ids[1] = 201;
    GatewayMemberRequestOwner queue{};
    check(gateway_member_request_admit(&queue, request),
          "author hydration: existing-worker queue copies a typed batch");
    request.user_ids[0] = 21;
    check(!gateway_member_request_admit(&queue, request) && queue.pending.user_ids[0] == 20,
          "author hydration: nonce reuse cannot acknowledge a changed payload");
    request.nonce = 10;
    check(!gateway_member_request_admit(&queue, request),
          "author hydration: occupied same-channel queue has bounded backpressure");
    request.channel_id = 81;
    check(gateway_member_request_admit(&queue, request) && queue.pending.channel_id == 81,
          "author hydration: new channel supersedes only unsent old interest");

    VisibleAuthorHydration owner{};
    const u64 ids[] = {1, 201, 1};
    check(visible_author_hydration_plan(&owner, 7, 80, {ids, 3}, 1000, &request) && request.user_count == 2,
          "author hydration: clipped IDs deduplicate without modulo collisions");
    visible_author_hydration_complete(&owner, request, 1000, true);
    bool quiet = true;
    for (u32 i = 0; i < 64; ++i)
        quiet &= !visible_author_hydration_plan(&owner, 7, 81, {ids, 3}, 2000, &request);
    check(quiet && owner.next_at_ms == 31000, "author hydration: repeated passes and same-guild channel "
                                              "changes retain both colliding-ID cooldowns");
    const u64 other[] = {300};
    check(!visible_author_hydration_plan(&owner, 8, 90, {other, 1}, 1500, &request) &&
              owner.next_at_ms == 2000,
          "author hydration: rapid cross-guild scrolling retains account-wide batch admission bound");
    check(visible_author_hydration_plan(&owner, 8, 90, {other, 1}, 2000, &request),
          "author hydration: finite deadline permits next batch");
    visible_author_hydration_complete(&owner, request, 2000, false);
    check(!visible_author_hydration_plan(&owner, 8, 90, {other, 1}, 2249, &request) &&
              owner.next_at_ms == 2250 &&
              visible_author_hydration_plan(&owner, 8, 90, {other, 1}, 2250, &request),
          "author hydration: queue refusal backs off without marking IDs successfully fetched");

    const bool ready = initialize();
    check(ready, "author hydration: original real message scene and member pump initialize");
    if (!ready)
        return;
    auto& f = fixture;
    VisibleAuthorHydration phase_owner{};
    phase_owner.interest = visible_author_interest_key(f.scene, 7, 80, true);
    phase_owner.interest_current = true;
    phase_owner.member_pin_revision = f.members.revision;
    phase_owner.user_pin_revision = f.users.revision;
    phase_owner.next_at_ms = 5000;
    u32 retained_passes = 0;
    for (u32 i = 0; i < 64; ++i)
        if (visible_author_hydration_interest_current(phase_owner,
                                                      visible_author_interest_key(f.scene, 7, 80, true),
                                                      f.members.revision, f.users.revision, 1000 + i))
            ++retained_passes;
    check(retained_passes == 64 &&
              !visible_author_hydration_interest_current(phase_owner,
                                                         visible_author_interest_key(f.scene, 7, 80, false),
                                                         f.members.revision, f.users.revision, 1000) &&
              !visible_author_hydration_interest_current(phase_owner, phase_owner.interest,
                                                         f.members.revision + 1, f.users.revision, 1000) &&
              !visible_author_hydration_interest_current(phase_owner, phase_owner.interest,
                                                         f.members.revision, f.users.revision, 5000),
          "author hydration: 64 unchanged actual scene interests retain pins; cover, role arrival and due "
          "retry invalidate");
    u64 visible[100]{};
    const u32 visible_count = visible_author_ids(f.scene, 80, {visible, 100});
    check(contains({visible, visible_count}, 20) && contains({visible, visible_count}, 201) &&
              contains({visible, visible_count}, 21) && !visible_author_ids(f.scene, 81, {visible, 100}),
          "author hydration: actual viewport collects normal and referenced authors only in selected "
          "channel");
    const auto* last = message_scene_layout_node(f.scene, f.scene.rows[2].author);
    if (last)
        f.scene.viewport.bottom = last->rectangle.top;
    const u32 clipped_count = visible_author_ids(f.scene, 80, {visible, 100});
    check(last && !contains({visible, clipped_count}, 30) && contains({visible, clipped_count}, 20),
          "author hydration: exact clipped author rectangles exclude offscreen users");
    check(!visible_author_hydration_interest_current(phase_owner,
                                                     visible_author_interest_key(f.scene, 7, 80, true),
                                                     f.members.revision, f.users.revision, 1000),
          "author hydration: exact viewport clipping change retires retained interest before pin reuse");

    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    const auto chunk = utf8_literal(
        R"({"op":0,"s":1,"t":"GUILD_MEMBERS_CHUNK","d":{"guild_id":"7","members":[{"user":{"id":"20","username":"Original author"},"roles":["17"],"nick":"Original nickname"},{"user":{"id":"21","username":"Original reply author"},"roles":[]}],"chunk_index":0,"chunk_count":1,"nonce":"9","not_found":["201"]}})");
    const auto before = f.members.revision;
    const bool decoded = route(chunk, &event, &payload);
    const auto applied =
        decoded ? message_event_pump_consume(&f.pump, event, payload) : MessageEventPumpConsumeResult{};
    const auto* author = guild_member_store_find(f.members, 7, 20);
    const auto* reply_author = guild_member_store_find(f.members, 7, 21);
    check(decoded && applied.ok() && applied.repaint && author && reply_author && author->roles_known &&
              author->role_color == 0x9922CC && reply_author->roles_known && !reply_author->role_color &&
              f.members.revision != before && !author->list_id && author->list_index == ~0u &&
              !f.members.visible.guild_id,
          "author hydration: actual opcode8 receipt converges role authority and repaint without any "
          "sidebar subscription");
    check(!guild_member_store_find(f.members, 7, 201),
          "author hydration: not_found does not fabricate member or empty-role authority");
    const auto sparse = utf8_literal(
        R"({"op":0,"s":2,"t":"GUILD_MEMBER_UPDATE","d":{"guild_id":"7","user":{"id":"20","username":"Original sparse name"}}})");
    const bool sparse_applied =
        route(sparse, &event, &payload) && message_event_pump_consume(&f.pump, event, payload).ok();
    author = guild_member_store_find(f.members, 7, 20);
    check(sparse_applied && author && author->roles_known && author->role_color == 0x9922CC &&
              utf8_equal(guild_member_store_nickname(*author), utf8_literal("Original nickname")),
          "author hydration: actual non-self sparse update preserves omitted role and nickname authority");
    const auto live = utf8_literal(
        R"({"op":0,"s":2,"t":"MESSAGE_CREATE","d":{"id":"990","channel_id":"80","author":{"id":"20","username":"Original author"},"member":{"nick":"Live nickname","roles":["17"]},"content":"hi","timestamp":"2024-08-30T12:34:56Z"}})");
    const bool live_applied =
        route(live, &event, &payload) && message_event_pump_consume(&f.pump, event, payload).ok();
    author = guild_member_store_find(f.members, 7, 20);
    check(live_applied && author && author->roles_known && author->role_color == 0x9922CC &&
              utf8_equal(guild_member_store_nickname(*author), utf8_literal("Live nickname")),
          "author hydration: a live message's member object carries the author's server nickname");
    const auto update = utf8_literal(
        R"({"op":0,"s":3,"t":"GUILD_MEMBER_UPDATE","d":{"guild_id":"7","user":{"id":"20","username":"Original renamed author"},"roles":[]}})");
    const bool updated =
        route(update, &event, &payload) && message_event_pump_consume(&f.pump, event, payload).ok();
    author = guild_member_store_find(f.members, 7, 20);
    check(updated && author && author->roles_known && !author->role_color,
          "author hydration: actual non-self member update clears assignments without member-pane toggle");
    guild_member_store_retire_role_authority(&f.members);
    author = guild_member_store_find(f.members, 7, 20);
    check(author && !author->roles_known && !author->role_color && user_store_find(f.users, 20),
          "author hydration: fresh-session role retirement preserves cached identity but requires new "
          "authority");
    const auto malformed = utf8_literal(
        R"({"op":0,"s":3,"t":"GUILD_MEMBERS_CHUNK","d":{"guild_id":"7","members":[{"user":{"id":"20","username":"Must not publish"},"roles":[]},{"user":{"id":"30","username":"No authority"}}]}})");
    const u32 revision = f.members.revision;
    check(!route(malformed, &event, &payload) && f.members.revision == revision,
          "author hydration: malformed trailing chunk member rejects whole decode before model mutation");
    // Voice participants of the selected server join the visible authors: a
    // user in a voice channel without a cached profile or member is requested.
    static VoiceState voice_records[4]{{7, 90, 20, 0, 0}, {7, 90, 31, 0, 0}, {8, 91, 32, 0, 0}, {7, 0, 33, 0, 0}};
    VoiceStateStore voice{};
    voice.records = {voice_records, 4};
    voice.count = 4;
    u64 voice_ids[8]{20};
    const u32 voice_count = visible_voice_user_ids(voice, 7, {voice_ids, 8}, 1);
    check(voice_count == 2 && voice_ids[0] == 20 && voice_ids[1] == 31 &&
              visible_voice_user_ids(voice, 0, {voice_ids, 8}, 1) == 1 &&
              visible_voice_user_ids(voice, 7, {voice_ids, 1}, 1) == 1,
          "voice hydration: the selected guild's connected voice users follow the authors without duplicates");
    check(!visible_voice_user_missing(f.users, f.members, 7, 20) &&
              visible_voice_user_missing(f.users, f.members, 7, 31) &&
              visible_voice_user_missing(f.users, f.members, 8, 20),
          "voice hydration: a voice user lacking a cached profile or server member is requested");
}
} // namespace maicord
