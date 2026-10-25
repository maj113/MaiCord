#include "maicord/app/private_conversation.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/model/private_channel_store.hpp"

namespace maicord {
static_assert(sizeof(PrivateConversationRequest) == 40);
u64 private_conversation_cached(const ChatStateStore& state, u64 user_id) noexcept {
    if (!state.private_channels || !state.self_user_id || !user_id || user_id == state.self_user_id) return 0;
    for (u32 i = 0; i < state.private_channels->count; ++i) {
        const auto& metadata = state.private_channels->records[i];
        const auto* channel = chat_state_store_find_channel(state, metadata.id);
        if (!channel || channel->type != 1) continue;
        const auto recipients = private_channel_store_recipients(*state.private_channels, metadata);
        bool found = false, compatible = true;
        for (u32 j = 0; j < recipients.count; ++j) {
            if (recipients[j] == state.self_user_id) continue;
            found = found || recipients[j] == user_id;
            compatible = compatible && recipients[j] == user_id;
        }
        if (found && compatible) return channel->id;
    }
    return 0;
}
bool private_conversation_begin(PrivateConversationRequest* request, const ChatStateStore& state,
    PrivateConversationOperation operation, u64 target_id) noexcept {
    if (!request || !target_id || !state.self_user_id ||
        (request->phase != PrivateConversationPhase::idle && request->phase != PrivateConversationPhase::failed)) return false;
    const auto* channel = chat_state_store_find_channel(state, target_id);
    if (operation == PrivateConversationOperation::open) { if (target_id == state.self_user_id) return false; }
    else if (operation != PrivateConversationOperation::close || !channel || channel->type != 1) return false;
    reset_value(request);
    request->target_id = target_id;
    request->operation = operation;
    request->phase = PrivateConversationPhase::queued;
    request->intent_revision = state.navigation_intent_revision;
    if (operation == PrivateConversationOperation::close) request->channel_generation = channel->generation;
    return true;
}
bool private_conversation_due(const PrivateConversationRequest& request, u64 now_ms) noexcept {
    return request.phase == PrivateConversationPhase::queued ||
        (request.phase == PrivateConversationPhase::retry_wait && now_ms >= request.retry_at_ms);
}
bool private_conversation_validate_target(PrivateConversationRequest* request, const ChatStateStore& state) noexcept {
    if (!request || !request->target_id) return false;
    if (request->operation == PrivateConversationOperation::open) return state.self_user_id && request->target_id != state.self_user_id;
    const auto* channel = chat_state_store_find_channel(state, request->target_id);
    if (channel && channel->type == 1 && channel->generation == request->channel_generation) return true;
    reset_value(request);
    return false;
}
bool private_conversation_accept(PrivateConversationRequest* request, ChatStateStore* state, UserStore* users,
    const RestClientResponse& response, Utf8View body, Slice<u32> scratch, Slice<char> packet, Slice<u64> ids) noexcept {
    if (!request || !state || request->phase != PrivateConversationPhase::awaiting || response.request_id != request->request_id) return false;
    const auto fail = [&](PrivateConversationError error) noexcept {
        request->phase = PrivateConversationPhase::failed;
        request->error = error;
        request->status_code = response.status_code;
        return true;
    };
    if (!response.transport_ok()) return fail(PrivateConversationError::transport);
    if (response.status_code == 429 && response.rate_limited && response.retry_at_ms) {
        request->phase = PrivateConversationPhase::retry_wait;
        request->retry_at_ms = response.retry_at_ms;
        return true;
    }
    if (request->operation == PrivateConversationOperation::close) {
        if (response.status_code != 200 && response.status_code != 204 && response.status_code != 404)
            return fail(PrivateConversationError::http);
        const auto* channel = chat_state_store_find_channel(*state, request->target_id);
        if (channel && channel->type == 1 && channel->generation == request->channel_generation)
            chat_state_store_remove_channel(state, channel->id);
    } else {
        if (response.status_code != 200) return fail(PrivateConversationError::http);
        JsonValue data{};
        data.kind = JsonValueKind::object;
        data.source = body;
        GatewayClientEvent event{};
        ConstSlice<u8> payload{};
        const auto routed = gateway_client_route_private_channel(data, scratch, 64, packet, &event, &payload);
        if (!routed.ok() || event.state.type != 1 || !event.state.recipients_known || !event.state.recipient_count ||
            event.state.recipient_count > 2) return fail(PrivateConversationError::response);
        u32 cursor = event.state.name_bytes + event.state.icon_bytes;
        bool target_found = false;
        for (u32 i = 0; i < event.state.recipient_count; ++i) {
            GatewayClientUserMutation user{};
            Utf8View name{}, avatar{};
            if (!gateway_client_user_record(payload, &cursor, &user, &name, &avatar) ||
                (user.id != request->target_id && user.id != state->self_user_id)) return fail(PrivateConversationError::response);
            target_found = target_found || user.id == request->target_id;
        }
        if (!target_found) return fail(PrivateConversationError::response);
        if (!chat_state_apply_gateway_event(state, event, payload, ids, users).ok()) return fail(PrivateConversationError::capacity);
        if (state->navigation_intent_revision == request->intent_revision) {
            if (!chat_state_store_select_channel(state, event.state.id).ok()) return fail(PrivateConversationError::response);
        }
    }
    reset_value(request);
    return true;
}
}
