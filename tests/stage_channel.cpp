#include "maicord/app/gateway_client.hpp"
#include "maicord/app/gateway_voice_control.hpp"
#include "maicord/app/voice_connection.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/stage_instance_store.hpp"
#include "maicord/model/stage_policy.hpp"
#include "maicord/protocol/stage_channel.hpp"
#include "maicord/ui/call_scene.hpp"
#include "maicord/core/win32_kernel.hpp"

// Stage channels: gateway fixtures (stage instances, suppress and
// request_to_speak), moderator permission gating, the call view's stage
// layout and DAVE passthrough for stages only.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

u32 scratch[8192];
char storage[kGatewayClientDispatchPayloadCapacity];

struct Published {
    GatewayClientEvent events[4];
    u8 payloads[4][4096];
    u32 bytes[4];
    u32 count;
};
bool capture(void* opaque, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    auto& published = *static_cast<Published*>(opaque);
    if (published.count == 4 || payload.count > sizeof(published.payloads[0]))
        return false;
    published.events[published.count] = event;
    if (payload.count)
        copy_bytes(published.payloads[published.count], payload.data, payload.count);
    published.bytes[published.count++] = payload.count;
    return true;
}
JsonValue object(Utf8View text) noexcept {
    return {text, 0, JsonValueKind::object};
}
GatewayEnvelope envelope(Utf8View name, Utf8View data) noexcept {
    GatewayEnvelope value{};
    value.opcode = 0;
    value.has_event_name = true;
    value.event_name = {name, 0, false};
    value.data = object(data);
    return value;
}
bool text_is(const char* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({data, bytes}, expected);
}
GatewayClientStageInstanceRecord stage_record(const u8* payload, u32 index) noexcept {
    GatewayClientStageInstanceRecord record{};
    copy_bytes(&record, payload + index * sizeof(record), sizeof(record));
    return record;
}

void gateway_cases(Check check) noexcept {
    GatewayStageInstance instance{};
    check(gateway_decode_stage_instance(
              object(utf8_literal(R"({"id":"900","guild_id":"5","channel_id":"7","topic":"Town hall",)"
                                  R"("privacy_level":2,"discoverable_disabled":false,"guild_scheduled_event_id":null})")),
              {scratch, 8192}, 32, &instance)
                  .ok() &&
              instance.id == 900 && instance.guild_id == 5 && instance.channel_id == 7 && instance.privacy_level == 2,
          "stage gateway: an instance decodes its IDs, topic and privacy");
    check(gateway_decode_stage_instance(object(utf8_literal(R"({"id":"900","topic":"x"})")), {scratch, 8192}, 32,
                                        &instance)
                      .error == GatewayEventDecodeError::missing_required_field &&
              gateway_decode_stage_instance(object(utf8_literal(R"({"id":"1","channel_id":"7","channel_id":"8"})")),
                                            {scratch, 8192}, 32, &instance)
                      .error == GatewayEventDecodeError::duplicate_field,
          "stage gateway: instances without a channel or with duplicate fields are refused");

    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    const auto route = [&](Utf8View name, Utf8View data) noexcept {
        event = {};
        payload = {};
        return gateway_client_route_dispatch(envelope(name, data), {scratch, 8192}, 64, {storage, sizeof(storage)}, {},
                                             1, &event, &payload);
    };
    check(route(utf8_literal("STAGE_INSTANCE_CREATE"),
                utf8_literal(R"({"id":"900","guild_id":"5","channel_id":"7","topic":"Café \"AMA\"","privacy_level":2})"))
                  .ok() &&
              event.dispatch_kind == GatewayClientDispatchKind::stage_instance_snapshot &&
              event.voice_states.guild_id == 5 && event.voice_states.record_count == 1 && !event.voice_states.replace &&
              payload.count == sizeof(GatewayClientStageInstanceRecord) &&
              event.dispatch_payload_bytes == payload.count,
          "stage gateway: STAGE_INSTANCE_CREATE routes as a one-record stage snapshot");
    auto record = stage_record(payload.data, 0);
    check(record.id == 900 && record.channel_id == 7 && record.privacy_level == 2 &&
              text_is(record.topic, record.topic_bytes, utf8_literal("Caf\xC3\xA9 \"AMA\"")),
          "stage gateway: the topic is unescaped into the record");
    check(route(utf8_literal("STAGE_INSTANCE_DELETE"),
                utf8_literal(R"({"id":"900","guild_id":"5","channel_id":"7","topic":"Town hall","privacy_level":2})"))
                  .ok() &&
              event.dispatch_kind == GatewayClientDispatchKind::stage_instance_snapshot &&
              stage_record(payload.data, 0).id == 0 && stage_record(payload.data, 0).channel_id == 7,
          "stage gateway: STAGE_INSTANCE_DELETE ends the channel's stage");
    check(route(utf8_literal("STAGE_INSTANCE_UPDATE"), utf8_literal(R"({"id":"900","guild_id":"x"})")).ok() &&
              event.dispatch_kind == GatewayClientDispatchKind::unknown && !payload.count,
          "stage gateway: a malformed instance stays an unknown dispatch, never a session failure");

    static Published published;
    published = {};
    constexpr auto guild = utf8_literal(
        R"({"id":"5","name":"g","stage_instances":[{"id":"900","guild_id":"5","channel_id":"7","topic":"A",)"
        R"("privacy_level":2},{"id":"901","channel_id":"8","topic":"B","privacy_level":1},{"id":"x"}]})");
    check(gateway_client_publish_stage_instances(object(guild), 5, {scratch, 8192}, {storage, sizeof(storage)}, 3, 4,
                                                 capture, &published) &&
              published.count == 1 &&
              published.events[0].dispatch_kind == GatewayClientDispatchKind::stage_instance_snapshot &&
              published.events[0].voice_states.replace && published.events[0].voice_states.guild_id == 5 &&
              published.events[0].voice_states.record_count == 2 &&
              stage_record(published.payloads[0], 1).guild_id == 5 &&
              stage_record(published.payloads[0], 1).channel_id == 8,
          "stage gateway: GUILD_CREATE stage_instances replace the guild's stages, skipping malformed rows");
    published = {};
    check(gateway_client_publish_stage_instances(object(utf8_literal(R"({"id":"5","name":"g"})")), 5, {scratch, 8192},
                                                 {storage, sizeof(storage)}, 3, 4, capture, &published) &&
              !published.count,
          "stage gateway: a guild without stage_instances publishes nothing");

    // Voice states: suppress and request_to_speak_timestamp.
    GatewayVoiceState state{};
    check(gateway_decode_voice_state_row(
              object(utf8_literal(R"({"channel_id":"7","user_id":"9","suppress":true,)"
                                  R"("request_to_speak_timestamp":"2026-10-07T00:00:05.250000+00:00"})")),
              {scratch, 8192}, 32, &state)
                  .ok() &&
              (state.flags & gateway_voice_suppress) && (state.flags & gateway_voice_request_to_speak) &&
              (state.present_flags & gateway_voice_request_to_speak) && state.request_to_speak_ms == 1'791'331'205'250ull,
          "stage gateway: a raised hand sets the flag and its time");
    check(gateway_decode_voice_state_row(
              object(utf8_literal(R"({"channel_id":"7","user_id":"9","suppress":false,"request_to_speak_timestamp":null})")),
              {scratch, 8192}, 32, &state)
                  .ok() &&
              !(state.flags & (gateway_voice_suppress | gateway_voice_request_to_speak)) &&
              (state.present_flags & gateway_voice_request_to_speak) &&
              !gateway_decode_voice_state_row(
                   object(utf8_literal(R"({"channel_id":"7","user_id":"9","request_to_speak_timestamp":"soon"})")),
                   {scratch, 8192}, 32, &state)
                   .ok(),
          "stage gateway: a null timestamp lowers the hand; a malformed one is refused");
    published = {};
    constexpr auto update = utf8_literal(R"({"guild_id":"5","channel_id":"7","user_id":"9","session_id":"s",)"
                                         R"("suppress":true,"request_to_speak_timestamp":"2026-10-07T00:00:05+00:00"})");
    check(gateway_client_publish_voice_states(object(update), GatewayVoiceStateSource::update, 0, {scratch, 8192},
                                              {storage, sizeof(storage)}, 3, 4, capture, &published) &&
              published.count == 1,
          "stage gateway: a stage voice state publishes one row");
    GatewayClientVoiceStateRecord row{};
    copy_bytes(&row, published.payloads[0], sizeof(row));
    check(row.flags == (gateway_voice_suppress | gateway_voice_request_to_speak) &&
              row.request_to_speak_s == 1'791'331'205u,
          "stage gateway: the row carries the hand's time in seconds");
}

void store_cases(Check check) noexcept {
    static StageInstanceStore store;
    stage_instance_store_reset(&store);
    check(stage_instance_store_apply(&store, 900, 5, 7, 2, utf8_literal("Town hall")) &&
              stage_instance_store_apply(&store, 901, 6, 8, 2, utf8_literal("Other")) && store.count == 2,
          "stage store: instances are added per channel");
    const u32 revision = store.revision;
    check(stage_instance_store_apply(&store, 900, 5, 7, 2, utf8_literal("Town hall")) && store.revision == revision &&
              stage_instance_store_apply(&store, 900, 5, 7, 2, utf8_literal("New topic")) &&
              store.revision == revision + 1 &&
              text_is(stage_instance_store_find(store, 7)->topic, stage_instance_store_find(store, 7)->topic_bytes,
                      utf8_literal("New topic")),
          "stage store: an update replaces the topic; a repeat is unchanged");
    check(stage_instance_store_apply(&store, 0, 5, 7, 0, {}) && !stage_instance_store_find(store, 7) &&
              stage_instance_store_clear_guild(&store, 6) && !store.count &&
              !stage_instance_store_apply(&store, 1, 0, 7, 0, {}) && !stage_instance_store_apply(&store, 1, 5, 0, 0, {}),
          "stage store: a zero id ends a stage, guilds clear, IDs are required");
}

void policy_cases(Check check) noexcept {
    constexpr PermissionSet base = kPermissionViewChannel | kPermissionConnect;
    check(stage_moderator(base | kStageModeratorPermissions) && stage_moderator(kPermissionAdministrator) &&
              !stage_moderator(base | kPermissionManageChannels | kPermissionMuteMembers) &&
              !stage_moderator(base | kPermissionMuteMembers | kPermissionMoveMembers) &&
              !stage_moderator(base | kPermissionManageChannels | kPermissionMoveMembers),
          "stage policy: moderators need Manage Channels, Mute Members and Move Members together");
    check(stage_role_flags(voice_suppress) == StageRole::audience &&
              stage_role_flags(voice_suppress | voice_request_to_speak) == StageRole::requested &&
              stage_role_flags(voice_request_to_speak) == StageRole::invited && stage_role_flags(0) == StageRole::speaker &&
              stage_role(nullptr) == StageRole::none,
          "stage policy: suppress and the hand classify like the official client");
    const PermissionSet member = base | kPermissionRequestToSpeak, moderator = base | kStageModeratorPermissions;
    check(stage_self_actions(StageRole::audience, member, true) == stage_action_raise_hand &&
              !stage_self_actions(StageRole::audience, base, true) &&
              !stage_self_actions(StageRole::audience, member, false) &&
              stage_self_actions(StageRole::requested, member, true) == stage_action_lower_hand &&
              stage_self_actions(StageRole::invited, member, true) ==
                  (stage_action_accept_invite | stage_action_decline_invite) &&
              stage_self_actions(StageRole::speaker, member, true) == stage_action_move_to_audience &&
              !stage_self_actions(StageRole::none, moderator, true),
          "stage policy: members raise a hand only with Request to Speak on a live stage");
    const u32 audience = stage_self_actions(StageRole::audience, moderator, false);
    const u32 live = stage_self_actions(StageRole::speaker, moderator, true);
    check((audience & stage_action_become_speaker) && (audience & stage_action_start) &&
              !(audience & (stage_action_end | stage_action_edit_topic | stage_action_raise_hand)) &&
              (live & stage_action_end) && (live & stage_action_edit_topic) && !(live & stage_action_start) &&
              (live & stage_action_invite_others) && (live & stage_action_move_others),
          "stage policy: moderators start, edit and end stages and manage speakers");

    static VoiceState records[8];
    VoiceStateStore store{};
    voice_state_store_initialize_sparse(&store, {records, 8});
    voice_state_store_apply(&store, 5, 7, 1, 0);                                            // speaker
    voice_state_store_apply(&store, 5, 7, 2, voice_suppress | voice_request_to_speak, 300); // hand at 300
    voice_state_store_apply(&store, 5, 7, 3, voice_suppress);                               // audience
    voice_state_store_apply(&store, 5, 7, 4, voice_suppress | voice_request_to_speak, 100); // hand at 100
    voice_state_store_apply(&store, 5, 7, 5, voice_request_to_speak, 200);                  // invited
    voice_state_store_apply(&store, 5, 8, 6, 0);                                            // other channel
    const VoiceState *speakers[4]{}, *crowd[4]{}, *hands[1]{};
    const auto roster = stage_roster(store, 7, {speakers, 4}, {crowd, 4}, {hands, 1});
    check(roster.speakers == 1 && roster.audience == 4 && roster.requests == 2 && speakers[0]->user_id == 1 &&
              crowd[0]->user_id == 2 && crowd[3]->user_id == 5 && hands[0]->user_id == 4,
          "stage policy: speakers, audience (invited included) and the oldest hand first");
    check(voice_state_store_find(store, 5, 2)->request_to_speak_s == 300 &&
              voice_state_store_apply(&store, 5, 7, 2, voice_suppress, 300) == VoiceStateResult::changed &&
              voice_state_store_find(store, 5, 2)->request_to_speak_s == 0,
          "stage policy: lowering a hand clears its time");
}

UiCallStagePerson people[6]{{11, utf8_literal("Ann")}, {12, utf8_literal("Bo")},   {13, utf8_literal("Cy")},
                            {14, utf8_literal("Di")},  {15, utf8_literal("Ed")},   {16, utf8_literal("Flo")}};
bool rect_inside(UiRect inner, UiRect outer) noexcept {
    return inner.left >= outer.left - .01f && inner.right <= outer.right + .01f && inner.top >= outer.top - .01f &&
           inner.bottom <= outer.bottom + .01f;
}
UiPoint middle(UiRect r) noexcept {
    return {(r.left + r.right) * .5f, (r.top + r.bottom) * .5f};
}

void layout_cases(Check check) noexcept {
    static UiCallScene scene;
    UiCallParticipant speakers[2]{};
    speakers[0].user_id = 1;
    speakers[0].name = utf8_literal("Host");
    speakers[1].user_id = 2;
    speakers[1].name = utf8_literal("Guest");
    UiCallContent content{};
    content.title = utf8_literal("stage-room");
    content.participants = {speakers, 2};
    content.stage.active = true;
    content.stage.live = true;
    content.stage.topic = utf8_literal("Weekly AMA");
    content.stage.actions = stage_self_actions(StageRole::audience, kPermissionRequestToSpeak, true);
    content.stage.audience_count = 40;
    content.stage.audience = {people, 6};
    const UiRect viewport{0, 0, 1200, 800};
    check(ui_call_scene_update(&scene, content, viewport, 0, true) && scene.stage_active && scene.stage_live &&
              text_is(scene.stage_topic, scene.stage_topic_bytes, utf8_literal("Weekly AMA")),
          "stage view: a live stage copies its topic for the header");
    check(ui_rect_has_area(scene.stage_audience) && !ui_rect_has_area(scene.stage_requests) &&
              !ui_rect_has_area(scene.stage_banner) && scene.tile_count == 2 &&
              rect_inside(scene.tiles[0].target, scene.stage_speakers) &&
              scene.tiles[1].target.bottom <= scene.stage_audience.top &&
              scene.stage_audience.bottom <= scene.bar.top + .01f,
          "stage view: speakers are tiles above the audience band");
    check(text_is(scene.stage_text[0], scene.stage_text_bytes[0], utf8_literal("Audience \xC2\xB7 40")) &&
              text_is(scene.stage_text[1], scene.stage_text_bytes[1], utf8_literal("+34")),
          "stage view: the band counts everyone and summarizes the rest");
    UiCallTileMedia avatar{};
    u32 avatars = 0;
    while (ui_call_scene_stage_avatar(scene, avatars, &avatar))
        ++avatars;
    check(avatars == 6 && ui_call_scene_stage_avatar(scene, 0, &avatar) && avatar.user_id == 11 &&
              rect_inside(avatar.avatar, scene.stage_audience),
          "stage view: audience avatars sit in the band");
    const auto hand = scene.controls[static_cast<u32>(UiCallControl::stage_hand)];
    check(ui_rect_has_area(hand) && !ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::camera)]) &&
              !ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::stage_start)]) &&
              ui_call_scene_hit_test(scene, middle(hand).x, middle(hand).y).control == UiCallControl::stage_hand &&
              utf8_equal(ui_call_control_label(scene, UiCallControl::stage_hand), utf8_literal("Request to speak")),
          "stage view: the audience bar offers Request to speak instead of camera and screen share");
    content.stage.hand_raised = true;
    content.stage.actions = stage_action_lower_hand;
    check(ui_call_scene_update(&scene, content, viewport, 0, true) &&
              ui_call_control_active(scene, UiCallControl::stage_hand) &&
              utf8_equal(ui_call_control_label(scene, UiCallControl::stage_hand), utf8_literal("Lower hand")),
          "stage view: a raised hand reads as Lower hand");
    // Invited: accept/decline banner above the speakers.
    content.stage.hand_raised = false;
    content.stage.actions = stage_action_accept_invite | stage_action_decline_invite;
    check(ui_call_scene_update(&scene, content, viewport, 0, true) && ui_rect_has_area(scene.stage_banner) &&
              scene.tiles[0].target.top >= scene.stage_banner.bottom &&
              ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::stage_accept)]) &&
              ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::stage_decline)]) &&
              !ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::stage_hand)]),
          "stage view: an invite shows Accept and Decline above the speakers");
    // Moderator on stage: hand raises beside the speakers, header actions.
    content.stage.actions = stage_self_actions(StageRole::speaker, kStageModeratorPermissions, true);
    content.stage.request_count = 2;
    content.stage.requests = {people + 4, 2};
    check(ui_call_scene_update(&scene, content, viewport, 0, true) && ui_rect_has_area(scene.stage_requests) &&
              scene.tiles[0].target.right <= scene.stage_requests.left &&
              ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::stage_end)]) &&
              ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::stage_topic)]) &&
              ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::stage_audience)]) &&
              text_is(scene.stage_text[2], scene.stage_text_bytes[2], utf8_literal("Requests to speak \xC2\xB7 2")),
          "stage view: moderators see hand raises, Edit topic, End stage and Move to audience");
    // The second request's Invite button names that user.
    UiCallTileMedia request_avatar{};
    check(ui_call_scene_stage_avatar(scene, 7, &request_avatar) && request_avatar.user_id == 16 &&
              rect_inside(request_avatar.avatar, scene.stage_requests),
          "stage view: hand-raise avatars follow the audience avatars");
    bool invite_hit = false;
    for (float y = scene.stage_requests.top; y < scene.stage_requests.bottom && !invite_hit; y += 2) {
        const auto hit = ui_call_scene_hit_test(scene, scene.stage_requests.right - 40, y);
        invite_hit = hit.control == UiCallControl::stage_invite && hit.user_id == 16;
    }
    check(invite_hit, "stage view: Invite on a hand-raise row hits with its user");
    // Hovering another speaker offers Move to audience to moderators.
    const auto& guest = scene.tiles[1];
    const float hx = guest.display.right - 22, hy = guest.display.top + 22;
    ui_call_scene_set_hover(&scene, hx, hy);
    const auto move = ui_call_scene_tile_control(scene, 2, UiCallControl::stage_move);
    check(ui_rect_has_area(move) &&
              ui_call_scene_hit_test(scene, middle(move).x, middle(move).y).control == UiCallControl::stage_move &&
              ui_call_scene_hit_test(scene, middle(move).x, middle(move).y).user_id == 2,
          "stage view: a hovered speaker tile offers Move to audience");
    // Not live: moderators start it; nobody on stage reads as not started.
    content.participants = {};
    content.stage.live = false;
    content.stage.topic = {};
    content.stage.request_count = 0;
    content.stage.requests = {};
    content.stage.actions = stage_self_actions(StageRole::audience, kStageModeratorPermissions, false);
    check(ui_call_scene_update(&scene, content, viewport, 0, true) && !scene.stage_topic_bytes &&
              ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::stage_start)]) &&
              ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::stage_become)]) &&
              !ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::stage_end)]) && !scene.tile_count,
          "stage view: before it starts, moderators get Start stage and Become a speaker");
    content.stage = {};
    content.participants = {speakers, 2};
    check(ui_call_scene_update(&scene, content, viewport, 0, true) && !scene.stage_active &&
              !ui_rect_has_area(scene.stage_audience) &&
              ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::camera)]) &&
              !ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::stage_hand)]),
          "stage view: a voice channel keeps the regular call layout");
    ui_call_scene_release(&scene);
}

VoiceConnection* make_connection() noexcept {
    auto* c = static_cast<VoiceConnection*>(
        VirtualAlloc(nullptr, sizeof(VoiceConnection), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!c)
        return nullptr;
    c->dave_group_id = 7;
    c->dave_user_id = 3;
    c->socket.setup_generation = 9;
    return c;
}

void dave_cases(Check check) noexcept {
    auto* stage = make_connection();
    auto* voice = make_connection();
    if (!stage || !voice) {
        check(false, "stage DAVE: fixture allocation");
        return;
    }
    stage->dave_optional = true;
    stage->media.dave_protocol_version = 0;
    check(voice_connection_dave_description(stage, 100) && stage->dave_session.initialized &&
              !stage->dave_session.protocol_version && !stage->dave_outbox_count && voice_connection_dave_ready(*stage),
          "stage DAVE: a stage may negotiate version 0 and sends media at once (no key package)");
    u8 opus[24]{1, 2, 3}, sealed[96], work[96], opened[96];
    const auto frame = voice_connection_dave_encrypt(stage, {opus, sizeof(opus)}, {sealed, sizeof(sealed)});
    check(frame.ok() && frame.bytes == sizeof(opus) && bytes_equal(sealed, opus, sizeof(opus)),
          "stage DAVE: passthrough sends the Opus frame unchanged");
    u32 json_scratch[64];
    const auto json = [&](VoiceConnection* c, VoiceDaveOpcode opcode, Utf8View text) noexcept {
        return voice_connection_dave_json(c, static_cast<u8>(opcode), object(text), {json_scratch, 64}, 100);
    };
    check(json(stage, VoiceDaveOpcode::prepare_transition, utf8_literal(R"({"transition_id":4,"protocol_version":0})")) &&
              stage->dave_close_code == kVoiceCloseDaveDowngradeRefused && !stage->dave_outbox_count,
          "stage DAVE: a later transition to version 0 is refused (close 4804) even on a stage");
    stage->dave_close_code = 0;
    stage->dave_reconnect_requested = false;
    check(json(stage, VoiceDaveOpcode::prepare_epoch, utf8_literal(R"({"epoch":2,"protocol_version":0})")) &&
              stage->dave_close_code == kVoiceCloseDaveDowngradeRefused,
          "stage DAVE: an epoch with version 0 is refused (close 4804)");
    stage->dave_close_code = 0;
    stage->dave_reconnect_requested = false;
    // An announced upgrade from version 0: libdave keeps accepting unencrypted
    // frames for the transition expiry, then refuses them.
    check(json(stage, VoiceDaveOpcode::prepare_epoch, utf8_literal(R"({"epoch":1,"protocol_version":1})")) &&
              stage->dave_outbox_count == 1 && voice_connection_dave_ready(*stage) &&
              json(stage, VoiceDaveOpcode::prepare_transition,
                   utf8_literal(R"({"transition_id":9,"protocol_version":1})")) &&
              json(stage, VoiceDaveOpcode::execute_transition, utf8_literal(R"({"transition_id":9})")) &&
              !voice_connection_dave_ready(*stage),
          "stage DAVE: an upgrade to version 1 executes and stops passthrough sending");
    const auto during = dave_session_decrypt(&stage->dave_session, 5, {opus, sizeof(opus)}, 101, {work, 96},
                                             {opened, 96});
    const auto after = dave_session_decrypt(&stage->dave_session, 5, {opus, sizeof(opus)},
                                            100 + kDaveEpochGraceMs, {work, 96}, {opened, 96});
    check(during.ok() && during.passthrough && !after.ok(),
          "stage DAVE: unencrypted frames pass only within the upgrade's transition expiry");
    voice->media.dave_protocol_version = 0;
    check(voice_connection_dave_description(voice, 100) && !voice->dave_session.initialized &&
              voice->dave_close_code == kVoiceCloseDaveDowngradeRefused && voice->dave_reconnect_requested,
          "stage DAVE: a voice channel refuses version 0 at Session Description (close 4804, reconnect)");
    voice->dave_close_code = 0;
    voice->dave_reconnect_requested = false;
    voice->media.dave_protocol_version = 1;
    check(voice_connection_dave_description(voice, 100) && voice->dave_session.protocol_version == 1 &&
              voice->dave_outbox_count == 1 && !voice_connection_dave_ready(*voice) &&
              json(voice, VoiceDaveOpcode::prepare_transition,
                   utf8_literal(R"({"transition_id":5,"protocol_version":0})")) &&
              voice->dave_close_code == kVoiceCloseDaveDowngradeRefused && voice->dave_outbox_count == 1,
          "stage DAVE: voice channels require DAVE and refuse a downgrade");
    voice_connection_dave_reset(stage);
    voice_connection_dave_reset(voice);
    VirtualFree(stage, 0, MEM_RELEASE);
    VirtualFree(voice, 0, MEM_RELEASE);

    // The stage flag reaches the connection's credentials through the request.
    static GatewayVoiceControl control;
    control = {};
    const JsonString session{utf8_literal("stage-session"), 0, false};
    check(voice_setup_bind_utf8(&control.setup, 3, session.encoded) == VoiceSetupResult::updated,
          "stage DAVE: voice control binds the session");
    gateway_voice_request(&control, {5, 7, false, false, false, true});
    GatewayVoiceStateUpdate command{};
    check(gateway_voice_next(control, &command) && command.stage && gateway_voice_submitted(&control, command, 1000) &&
              control.setup.stage,
          "stage DAVE: a stage join marks the voice setup");
    gateway_voice_state(&control, {5, 7, 3, session, gateway_voice_suppress, gateway_voice_suppress, 0}, 1001);
    gateway_voice_server(&control, {5, 0, {utf8_literal("token"), 0, false}, {utf8_literal("host:443"), 0, false},
                                    false});
    VoiceSetupCredentials credentials{};
    check(voice_setup_credentials(control.setup, &credentials) && credentials.stage && !credentials.stream,
          "stage DAVE: the stage flag is in the connection credentials");
    gateway_voice_request(&control, {5, 7, true, false, false, false});
    check(!control.setup.stage, "stage DAVE: a same-channel request updates the channel kind");
    gateway_voice_request(&control, {5, 8, false, false, false, false});
    check(gateway_voice_next(control, &command) && gateway_voice_submitted(&control, command, 2000) &&
              !control.setup.stage && !(control.setup.flags & gateway_voice_suppress),
          "stage DAVE: moving to a voice channel requires DAVE again");
    gateway_voice_release(&control);
}
} // namespace

void test_stage_channel(Check check) noexcept {
    gateway_cases(check);
    store_cases(check);
    policy_cases(check);
    layout_cases(check);
    dave_cases(check);
}
} // namespace maicord
