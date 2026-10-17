#include "maicord/app/private_conversation.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/ui/message_context_menu.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/private_channel_store.hpp"

namespace maicord {
void test_private_conversation(void (*check)(bool, const char*) noexcept) noexcept {
    u8 target[96]{}, body[64]{};
    const auto written = private_conversation_write(PrivateConversationOperation::open, 2, {target, 96}, {body, 64});
    check(written.ok && written.target_bytes == 26 && written.body_bytes == 20 &&
        bytes_equal(target, "/api/v9/users/@me/channels", 26) && bytes_equal(body, "{\"recipients\":[\"2\"]}", 20),
        "normal-user DM request writes a recipients array without message content");
    const auto closed = private_conversation_write(PrivateConversationOperation::close, 10, {target, 96}, {});
    check(closed.ok && !closed.body_bytes && closed.target_bytes == 32 &&
        bytes_equal(target, "/api/v9/channels/10?silent=false", 32), "one-to-one close writes the user channel DELETE target");
    check(!private_conversation_write(PrivateConversationOperation::open, 0, {target, 96}, {body, 64}).ok &&
        !private_conversation_write(PrivateConversationOperation::open, 2, {target, 2}, {body, 64}).ok &&
        !private_conversation_write(PrivateConversationOperation::open, 2, {target, 96}, {body, 2}).ok,
        "DM request writers reject invalid identity and bounded-output exhaustion");
    check(private_conversation_route(PrivateConversationOperation::open, 2).major == RestMajorParameter::none &&
        private_conversation_route(PrivateConversationOperation::close, 10).major_key == 10 &&
        private_conversation_route(PrivateConversationOperation::close, 10).method == RestMethod::delete_method,
        "open and close preserve distinct rate-limit identities");
    ChatStateStore state{};
    GuildRecord guilds[2]{};
    ChannelRecord channels[8]{};
    u16 guild_order[2]{}, channel_order[8]{};
    u8 names[2048]{};
    PrivateChannelStore metadata{};
    UserStore users{};
    UserRecord records[8]{};
    u32 user_order[8]{};
    u8 user_names[2048]{};
    check(chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 8}, {channel_order, 8}, {names, 2048}) &&
        private_channel_store_initialize_owned(&metadata, 8, 32) &&
        user_store_initialize(&users, {records, 8}, {user_order, 8}, {user_names, 2048}), "private REST state initializes");
    state.private_channels = &metadata;
    state.self_user_id = 1;
    u32 scratch[2048]{};
    char packet[4096]{};
    u64 ids[8]{};
    const auto dm = utf8_literal(R"({"id":"10","type":1,"recipients":[{"id":"2","username":"Peer"}]})");
    PrivateConversationRequest request{};
    RestClientResponse response{};
    response.request_id = 47;
    response.status_code = 200;
    const auto accept = [&](Utf8View json) noexcept {
        return private_conversation_accept(&request, &state, &users, response, json, {scratch, 2048}, {packet, 4096}, {ids, 8});
    };
    const auto submit = [&]() noexcept { request.phase = PrivateConversationPhase::awaiting; request.request_id = response.request_id; };
    check(!private_conversation_begin(&request, state, PrivateConversationOperation::open, 1) &&
        private_conversation_begin(&request, state, PrivateConversationOperation::open, 2) && private_conversation_due(request, 0),
        "open queues a peer request and rejects self");
    submit();
    check(accept(dm) && state.selected_channel_id == 10 && request.phase == PrivateConversationPhase::idle &&
        private_conversation_cached(state, 2) == 10 && user_store_find(users, 2),
        "validated REST channel response hydrates users and selects the requested DM");
    check(private_conversation_begin(&request, state, PrivateConversationOperation::open, 3), "another open can queue");
    submit();
    check(chat_state_store_select_home(&state).ok() &&
        accept(utf8_literal(R"({"id":"11","type":1,"recipients":[{"id":"3","username":"Other"}]})")) &&
        state.selected_channel_id == 10 && private_conversation_cached(state, 3) == 11,
        "later explicit navigation cancels focus stealing while retaining the returned DM");
    check(private_conversation_begin(&request, state, PrivateConversationOperation::open, 4), "invalid-response fixture queues");
    submit();
    check(accept(dm) && request.phase == PrivateConversationPhase::failed && state.channel_count == 2,
        "wrong recipient response cannot select or create a conversation");
    check(private_conversation_begin(&request, state, PrivateConversationOperation::open, 2), "rate-limit fixture queues");
    submit();
    response.status_code = 429;
    response.rate_limited = true;
    response.retry_at_ms = 1000;
    check(accept({}) && request.phase == PrivateConversationPhase::retry_wait && !private_conversation_due(request, 999) &&
        private_conversation_due(request, 1000), "DM retries wait for the server rate deadline");
    submit();
    response.status_code = 403;
    check(accept({}) && request.phase == PrivateConversationPhase::failed && request.error == PrivateConversationError::http &&
        state.channel_count == 2, "denied requests preserve visible conversations and retain feedback state");
    check(chat_state_store_upsert_channel(&state, {12, 0, 0, {}, 0, 0, 3}).ok() &&
        !private_conversation_begin(&request, state, PrivateConversationOperation::close, 12),
        "Close DM cannot accidentally leave a group");
    check(private_conversation_begin(&request, state, PrivateConversationOperation::close, 10), "close captures a DM generation");
    submit();
    check(chat_state_store_remove_channel(&state, 10), "gateway deletion may precede REST completion");
    response.status_code = 204;
    check(accept({}) && !chat_state_store_find_channel(state, 10) && request.phase == PrivateConversationPhase::idle,
        "already-deleted DM close completes idempotently");
    response.status_code = 200;
    check(private_conversation_begin(&request, state, PrivateConversationOperation::open, 2), "reopen fixture queues");
    submit();
    check(accept(dm) && private_conversation_begin(&request, state, PrivateConversationOperation::close, 10), "close race captures reopened identity");
    submit();
    check(chat_state_store_remove_channel(&state, 10) && chat_state_store_upsert_channel(&state, {10, 0, 0, {}, 0, 0, 1}).ok(),
        "a new channel generation may arrive while old close is outstanding");
    response.status_code = 204;
    check(accept({}) && chat_state_store_find_channel(state, 10), "late close response preserves a replacement generation");
    check(private_conversation_begin(&request, state, PrivateConversationOperation::close, 10) &&
        chat_state_store_upsert_channel(&state, {10, 0, 0, {}, 0, 0, 3}).ok() &&
        !private_conversation_validate_target(&request, state) && request.phase == PrivateConversationPhase::idle,
        "queued and rate-limited close revalidates type before sending so it cannot become a group leave");
    GatewayClientEvent deletion{};
    deletion.kind = GatewayClientEventKind::dispatch;
    deletion.dispatch_kind = GatewayClientDispatchKind::channel_delete;
    deletion.state.id = 999;
    deletion.state.type = 1;
    deletion.state.fields = gateway_client_state_field_id | gateway_client_state_field_type;
    check(chat_state_apply_gateway_event(&state, deletion, {}, {ids, 8}).ok(),
        "a duplicate private gateway delete does not trigger recovery after REST closure");
    MessageRecord message{};
    message.id = 1; message.channel_id = 10; message.author_id = 2;
    MessageContextMenuModel menu{};
    const auto built = message_context_menu_build(&menu, message, false, false, false, nullptr, true);
    bool author_action = false;
    for (u32 i = 0; i < menu.count; ++i) author_action = author_action || menu.items[i].action == static_cast<u32>(MessageContextAction::message_author);
    check(built.ok() && author_action, "Message author composes through the shared context menu");
    private_channel_store_release(&metadata);
}
}
