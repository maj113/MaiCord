#include "maicord/app/private_channel_actions.hpp"
#include "maicord/app/message_event_pump.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/private_channel_store.hpp"

namespace maicord {
void test_private_channel_actions(void (*check)(bool, const char*) noexcept) noexcept {
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
    u32 scratch[4096]{};
    char packet[8192]{};
    u64 ids[256]{};
    check(chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 16}, {channel_order, 16},
                                         {names, 4096}) &&
              private_channel_store_initialize_owned(&metadata, 16, 256) &&
              user_store_initialize(&users, {user_records, 32}, {user_order, 32}, {user_names, 4096}),
          "group DM action stores initialize");
    state.private_channels = &metadata;
    state.self_user_id = 1;
    const auto gateway = [&](Utf8View json) noexcept {
        GatewayEnvelope envelope{};
        GatewayClientEvent event{};
        ConstSlice<u8> payload{};
        return gateway_decode_envelope(json, {scratch, 4096}, 64, &envelope).ok() &&
               gateway_client_route_dispatch(envelope, {scratch, 4096}, 64, {packet, 8192}, {ids, 256}, 1, &event,
                                             &payload)
                   .ok() &&
               chat_state_apply_gateway_event(&state, event, payload, {ids, 2}, &users).ok();
    };
    check(gateway(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","s":1,"d":{"id":"10","type":3,"owner_id":"1","recipients":[{"id":"2","username":"B"},{"id":"3","username":"C"}]}})")) &&
              gateway(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","s":2,"d":{"id":"11","type":3,"owner_id":"2","recipients":[{"id":"2","username":"B"},{"id":"3","username":"C"}]}})")) &&
              gateway(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","s":3,"d":{"id":"12","type":1,"recipients":[{"id":"4","username":"D"}]}})")) &&
              gateway(utf8_literal(R"({"op":0,"t":"CHANNEL_CREATE","s":4,"d":{"id":"13","type":3,"owner_id":"1","recipients":[{"id":"2"},{"id":"3"},{"id":"4"},{"id":"5"},{"id":"6"},{"id":"7"},{"id":"8"},{"id":"9"},{"id":"14"}]}})")),
          "group DM fixtures arrive through the gateway");
    check(private_channel_member_count(state, 10) == 3 && private_channel_capacity_left(state, 10) == 7 &&
              private_channel_member_count(state, 12) == 2 && private_channel_capacity_left(state, 12) == 8 &&
              private_channel_member_count(state, 13) == 10 && !private_channel_capacity_left(state, 13) &&
              !private_channel_member_count(state, 99) && private_channel_owner(state, 10) == 1 &&
              !private_channel_owner(state, 12) && private_channel_is_group(state, 10) &&
              !private_channel_is_group(state, 12),
          "member counts include the current user and groups stop at ten");
    check(private_channel_can_remove(state, 10, 2) && !private_channel_can_remove(state, 10, 1) &&
              !private_channel_can_remove(state, 10, 5) && !private_channel_can_remove(state, 11, 3) &&
              !private_channel_can_remove(state, 12, 4),
          "only the owner removes other members; self removal and DMs are refused");

    PrivateChannelActionController actions{};
    RestClientResponse response{};
    response.status_code = 200;
    const auto send = [&](u64 id) noexcept {
        response.request_id = id;
        return private_channel_action_due(actions, 0) && private_channel_action_submitted(&actions, id);
    };
    const auto accept = [&](Utf8View body) noexcept {
        return private_channel_action_accept(&actions, &state, &users, response, body, {scratch, 4096},
                                             {packet, 8192}, {ids, 256});
    };
    const u64 self_only[]{1}, duplicate[]{2, 2}, zero[]{0}, ten[]{2, 3, 4, 5, 6, 7, 8, 9, 14, 15};
    check(!private_channel_begin_create(&actions, state, {}) &&
              !private_channel_begin_create(&actions, state, {self_only, 1}) &&
              !private_channel_begin_create(&actions, state, {duplicate, 2}) &&
              !private_channel_begin_create(&actions, state, {zero, 1}) &&
              !private_channel_begin_create(&actions, state, {ten, 10}) &&
              actions.phase == PrivateChannelActionPhase::idle,
          "the picker limit is nine distinct people other than the current user");
    check(private_channel_begin_create(&actions, state, {ten, 9}) && actions.request.recipient_count == 9 &&
              !private_channel_begin_create(&actions, state, {ten, 2}),
          "nine people create a full group and one request runs at a time");
    actions = {};
    const u64 pair[]{2, 4};
    check(private_channel_begin_create(&actions, state, {pair, 2}) && send(41) &&
              accept(utf8_literal(R"({"id":"20","type":3,"owner_id":"1","recipients":[{"id":"2","username":"B"},{"id":"4","username":"D"}]})")) &&
              actions.phase == PrivateChannelActionPhase::idle && actions.result_channel_id == 20 &&
              state.selected_channel_id == 20 && private_channel_member_count(state, 20) == 3,
          "a created group is applied from the response and opened");
    check(private_channel_begin_create(&actions, state, {pair, 2}) && send(42) &&
              accept(utf8_literal(R"({"id":"21","type":3,"owner_id":"1","recipients":[{"id":"2","username":"B"}]})")) &&
              actions.phase == PrivateChannelActionPhase::failed &&
              actions.error == PrivateChannelActionError::response && !chat_state_store_find_channel(state, 21),
          "a response missing requested people is refused");

    const u64 existing[]{3}, five_six[]{5, 6}, too_many[]{5, 6, 7, 8, 9, 14, 15, 16};
    check(!private_channel_begin_add(&actions, state, 10, {existing, 1}) &&
              !private_channel_begin_add(&actions, state, 13, {five_six, 1}) &&
              !private_channel_begin_add(&actions, state, 10, {too_many, 8}) &&
              !private_channel_begin_add(&actions, state, 99, {five_six, 1}),
          "adding members refuses current members, full groups and unknown channels");
    check(private_channel_begin_add(&actions, state, 12, {five_six, 1}) &&
              actions.request.kind == PrivateChannelActionKind::create && actions.request.recipient_count == 2 &&
              actions.request.recipients[0] == 4 && actions.request.recipients[1] == 5,
          "adding to a DM creates a new group with the peer");
    actions = {};
    response.status_code = 204;
    check(private_channel_begin_add(&actions, state, 10, {five_six, 2}) &&
              actions.request.kind == PrivateChannelActionKind::add_recipient && actions.request.user_id == 5 &&
              send(43) && accept({}) && actions.phase == PrivateChannelActionPhase::queued &&
              actions.request.user_id == 6 && send(44) && accept({}) &&
              actions.phase == PrivateChannelActionPhase::idle,
          "adding several people sends one PUT per person in order");

    response.status_code = 200;
    check(!private_channel_begin_rename(&actions, state, 12, utf8_literal("Nope")) &&
              private_channel_begin_rename(&actions, state, 10, utf8_literal("Trip")) && send(45) &&
              accept(utf8_literal(R"({"id":"10","type":3,"name":"Trip","owner_id":"1","recipients":[{"id":"2"},{"id":"3"}]})")) &&
              utf8_equal(chat_state_store_channel_name(state, *chat_state_store_find_channel(state, 10)),
                         utf8_literal("Trip")),
          "renaming applies the returned name to groups only");

    check(!private_channel_begin_remove(&actions, state, 11, 3) && private_channel_begin_remove(&actions, state, 10, 2) &&
              actions.request.kind == PrivateChannelActionKind::remove_recipient && send(46),
          "remove is owner-only before reaching transport");
    response.status_code = 429;
    response.rate_limited = true;
    response.retry_at_ms = 900;
    check(accept({}) && actions.phase == PrivateChannelActionPhase::retry_wait &&
              !private_channel_action_due(actions, 899) && private_channel_action_due(actions, 900),
          "rate-limited actions wait for the server deadline");
    response = {};
    response.status_code = 403;
    response.request_id = 47;
    check(private_channel_action_submitted(&actions, 47) && accept({}) &&
              actions.phase == PrivateChannelActionPhase::failed && actions.error == PrivateChannelActionError::http &&
              actions.status_code == 403 && private_channel_has_member(state, 10, 2),
          "denied actions keep membership and report the status");

    response.status_code = 204;
    check(!private_channel_begin_leave(&actions, state, 12, false) &&
              private_channel_begin_leave(&actions, state, 11, true) && actions.request.silent && send(48) &&
              accept({}) && !chat_state_store_find_channel(state, 11),
          "leaving removes the group locally; DMs close instead");
    check(private_channel_begin_ring(&actions, state, 12, false) &&
              actions.request.kind == PrivateChannelActionKind::ring && send(49) && accept({}) &&
              private_channel_begin_ring(&actions, state, 12, true) &&
              actions.request.kind == PrivateChannelActionKind::stop_ringing,
          "calls ring and decline through the same lane");

    check(gateway(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","s":5,"d":{"id":"10","type":3,"name":"Renamed","owner_id":"2","recipients":[{"id":"2"},{"id":"3"},{"id":"5"},{"id":"6"}]}})")) &&
              private_channel_owner(state, 10) == 2 && !private_channel_can_remove(state, 10, 3) &&
              private_channel_member_count(state, 10) == 5 &&
              utf8_equal(chat_state_store_channel_name(state, *chat_state_store_find_channel(state, 10)),
                         utf8_literal("Renamed")),
          "CHANNEL_UPDATE transfers ownership and renames the group");
    check(gateway(utf8_literal(R"({"op":0,"t":"CHANNEL_RECIPIENT_REMOVE","s":6,"d":{"channel_id":"10","user":{"id":"6"}}})")) &&
              private_channel_member_count(state, 10) == 4 && private_channel_capacity_left(state, 10) == 6,
          "recipient removal frees a group slot");
    PrivateCallPrompt prompt{};
    const auto call = [&](Utf8View json, u64 voice_channel) noexcept {
        GatewayEnvelope envelope{};
        GatewayClientEvent event{};
        ConstSlice<u8> payload{};
        if (!gateway_decode_envelope(json, {scratch, 4096}, 64, &envelope).ok() ||
            !gateway_client_route_dispatch(envelope, {scratch, 4096}, 64, {packet, 8192}, {ids, 256}, 1, &event,
                                           &payload)
                 .ok())
            return false;
        private_call_observe(&prompt, state, event, voice_channel);
        return true;
    };
    check(call(utf8_literal(R"({"op":0,"t":"CALL_CREATE","s":7,"d":{"channel_id":"12","message_id":"50","region":"rotterdam","ringing":["1"],"voice_states":[]}})"), 0) &&
              prompt.channel_id == 12,
          "CALL_CREATE ringing the current user prompts for the DM call");
    check(call(utf8_literal(R"({"op":0,"t":"CALL_UPDATE","s":8,"d":{"channel_id":"12","message_id":"50","region":"rotterdam","ringing":["4"]}})"), 0) &&
              !prompt.channel_id,
          "CALL_UPDATE without the current user in ringing clears the prompt");
    check(call(utf8_literal(R"({"op":0,"t":"CALL_CREATE","s":9,"d":{"channel_id":"10","ringing":["2","1"]}})"), 10) &&
              !prompt.channel_id,
          "a call already joined never prompts");
    check(call(utf8_literal(R"({"op":0,"t":"CALL_UPDATE","s":10,"d":{"channel_id":"10","ringing":["1"]}})"), 0) &&
              prompt.channel_id == 10 &&
              call(utf8_literal(R"({"op":0,"t":"CALL_DELETE","s":11,"d":{"channel_id":"99"}})"), 0) &&
              prompt.channel_id == 10 &&
              call(utf8_literal(R"({"op":0,"t":"CALL_DELETE","s":12,"d":{"channel_id":"10","unavailable":false}})"), 0) &&
              !prompt.channel_id,
          "CALL_DELETE clears only its own call prompt");
    check(call(utf8_literal(R"({"op":0,"t":"CALL_CREATE","s":13,"d":{"channel_id":"55","ringing":["1"]}})"), 0) &&
              !prompt.channel_id,
          "calls in unknown channels are ignored");
    private_channel_store_release(&metadata);
}
} // namespace maicord
