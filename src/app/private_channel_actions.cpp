#include "maicord/app/private_channel_actions.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/private_channel_store.hpp"

namespace maicord {
namespace {
const PrivateChannelRecord* metadata(const ChatStateStore& state, u64 channel_id,
                                            const ChannelRecord** channel_out = nullptr) noexcept {
    const auto* channel = chat_state_store_find_channel(state, channel_id);
    if (channel_out) *channel_out = channel;
    if (!channel || !channel_is_private(channel->type) || !state.private_channels) return nullptr;
    return private_channel_store_find(*state.private_channels, channel_id);
}
bool startable(const PrivateChannelActionController* c, const ChatStateStore& state) noexcept {
    return c && state.self_user_id &&
           (c->phase == PrivateChannelActionPhase::idle || c->phase == PrivateChannelActionPhase::failed);
}
void start(PrivateChannelActionController* c, const ChatStateStore& state,
           const PrivateChannelActionRequest& request) noexcept {
    *c = {};
    c->request = request;
    c->phase = PrivateChannelActionPhase::queued;
    c->intent_revision = state.navigation_intent_revision;
}
bool begin_request_decision(PrivateChannelActionController* c, const ChatStateStore& state, u64 channel_id,
                            PrivateChannelActionKind kind) noexcept {
    const auto* record = metadata(state, channel_id);
    if (!startable(c, state) || !record || !private_channel_is_request(*record)) return false;
    PrivateChannelActionRequest request{};
    request.kind = kind;
    request.channel_id = channel_id;
    start(c, state, request);
    return true;
}
// Distinct, non-self users that fit in one request.
bool valid_people(const ChatStateStore& state, ConstSlice<u64> users) noexcept {
    if (!users.count || !users.data || users.count > kPrivateChannelRecipientMaximum) return false;
    for (u32 i = 0; i < users.count; ++i) {
        if (!users[i] || users[i] == state.self_user_id) return false;
        for (u32 j = 0; j < i; ++j) if (users[j] == users[i]) return false;
    }
    return true;
}
bool apply_channel(PrivateChannelActionController* c, ChatStateStore* state, UserStore* users,
                   Utf8View body, Slice<u32> scratch, Slice<char> packet, Slice<u64> ids, bool select) noexcept {
    JsonValue data{};
    data.kind = JsonValueKind::object;
    data.source = body;
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    const auto routed = gateway_client_route_private_channel(data, scratch, 64, packet, &event, &payload);
    if (!routed.ok() || !event.state.id || !event.state.recipients_known) return false;
    const auto& request = c->request;
    if (request.kind == PrivateChannelActionKind::rename && event.state.id != request.channel_id) return false;
    if (request.kind == PrivateChannelActionKind::create) {
        if (request.recipient_count == 1 ? event.state.type != 1 && event.state.type != 3 : event.state.type != 3)
            return false;
        u32 cursor = event.state.name_bytes + event.state.icon_bytes, found = 0;
        for (u32 i = 0; i < event.state.recipient_count; ++i) {
            GatewayClientUserMutation user{};
            Utf8View name{}, avatar{};
            if (!gateway_client_user_record(payload, &cursor, &user, &name, &avatar)) return false;
            for (u32 j = 0; j < request.recipient_count; ++j) found += request.recipients[j] == user.id;
        }
        if (found != request.recipient_count) return false;
    }
    if (!chat_state_apply_gateway_event(state, event, payload, ids, users).ok()) return false;
    c->result_channel_id = event.state.id;
    return !select || state->navigation_intent_revision != c->intent_revision ||
           chat_state_store_select_channel(state, event.state.id).ok();
}
} // namespace

u32 private_channel_member_count(const ChatStateStore& state, u64 channel_id) noexcept {
    const auto* record = metadata(state, channel_id);
    if (!record) return 0;
    const auto recipients = private_channel_store_recipients(*state.private_channels, *record);
    u32 count = 1;
    for (u32 i = 0; i < recipients.count; ++i) count += recipients[i] != state.self_user_id;
    return count;
}
bool private_channel_has_member(const ChatStateStore& state, u64 channel_id, u64 user_id) noexcept {
    const auto* record = metadata(state, channel_id);
    if (!record || !user_id) return false;
    if (user_id == state.self_user_id) return true;
    const auto recipients = private_channel_store_recipients(*state.private_channels, *record);
    for (u32 i = 0; i < recipients.count; ++i)
        if (recipients[i] == user_id) return true;
    return false;
}
u64 private_channel_owner(const ChatStateStore& state, u64 channel_id) noexcept {
    const ChannelRecord* channel = nullptr;
    const auto* record = metadata(state, channel_id, &channel);
    return record && channel->type == 3 ? record->owner_id : 0;
}
bool private_channel_is_group(const ChatStateStore& state, u64 channel_id) noexcept {
    const ChannelRecord* channel = nullptr;
    return metadata(state, channel_id, &channel) && channel->type == 3;
}
u32 private_channel_capacity_left(const ChatStateStore& state, u64 channel_id) noexcept {
    const u32 members = private_channel_member_count(state, channel_id);
    return members && members < kPrivateChannelGroupMaximum ? kPrivateChannelGroupMaximum - members : 0;
}
bool private_channel_can_remove(const ChatStateStore& state, u64 channel_id, u64 user_id) noexcept {
    const u64 owner = private_channel_owner(state, channel_id);
    return owner && owner == state.self_user_id && user_id != state.self_user_id &&
           private_channel_has_member(state, channel_id, user_id);
}

bool private_channel_begin_create(PrivateChannelActionController* c, const ChatStateStore& state,
                                  ConstSlice<u64> users) noexcept {
    if (!startable(c, state) || !valid_people(state, users)) return false;
    PrivateChannelActionRequest request{};
    request.kind = PrivateChannelActionKind::create;
    copy_bytes(request.recipients, users.data, users.count * sizeof(u64));
    request.recipient_count = static_cast<u8>(users.count);
    start(c, state, request);
    return true;
}
bool private_channel_begin_add(PrivateChannelActionController* c, const ChatStateStore& state, u64 channel_id,
                               ConstSlice<u64> users) noexcept {
    const ChannelRecord* channel = nullptr;
    const auto* record = metadata(state, channel_id, &channel);
    if (!startable(c, state) || !record || !valid_people(state, users) ||
        users.count > private_channel_capacity_left(state, channel_id))
        return false;
    for (u32 i = 0; i < users.count; ++i)
        if (private_channel_has_member(state, channel_id, users[i])) return false;
    if (channel->type == 1) {
        // Adding to a DM starts a new group with the peer instead of
        // converting the conversation, as the current client does.
        u64 people[kPrivateChannelRecipientMaximum]{};
        const auto recipients = private_channel_store_recipients(*state.private_channels, *record);
        u32 count = 0;
        for (u32 i = 0; i < recipients.count && count < kPrivateChannelRecipientMaximum; ++i)
            if (recipients[i] != state.self_user_id) people[count++] = recipients[i];
        if (count + users.count > kPrivateChannelRecipientMaximum) return false;
        copy_bytes(people + count, users.data, users.count * sizeof(u64));
        return private_channel_begin_create(c, state, {people, count + users.count});
    }
    PrivateChannelActionRequest request{};
    request.kind = PrivateChannelActionKind::add_recipient;
    request.channel_id = channel_id;
    request.user_id = users[0];
    start(c, state, request);
    copy_bytes(c->queued, users.data + 1, (users.count - 1) * sizeof(u64));
    c->queued_count = static_cast<u8>(users.count - 1);
    return true;
}
bool private_channel_begin_remove(PrivateChannelActionController* c, const ChatStateStore& state,
                                  u64 channel_id, u64 user_id) noexcept {
    if (!startable(c, state) || !private_channel_can_remove(state, channel_id, user_id)) return false;
    PrivateChannelActionRequest request{};
    request.kind = PrivateChannelActionKind::remove_recipient;
    request.channel_id = channel_id;
    request.user_id = user_id;
    start(c, state, request);
    return true;
}
bool private_channel_begin_rename(PrivateChannelActionController* c, const ChatStateStore& state,
                                  u64 channel_id, Utf8View name) noexcept {
    if (!startable(c, state) || !private_channel_is_group(state, channel_id) ||
        name.bytes > kPrivateChannelNameMaximumBytes || (name.bytes && !name.data))
        return false;
    PrivateChannelActionRequest request{};
    request.kind = PrivateChannelActionKind::rename;
    request.channel_id = channel_id;
    if (name.bytes) copy_bytes(request.name, name.data, name.bytes);
    request.name_bytes = static_cast<u16>(name.bytes);
    if (!private_channel_action_valid(request)) return false;
    start(c, state, request);
    return true;
}
bool private_channel_begin_leave(PrivateChannelActionController* c, const ChatStateStore& state,
                                 u64 channel_id, bool silent) noexcept {
    if (!startable(c, state) || !private_channel_is_group(state, channel_id)) return false;
    PrivateChannelActionRequest request{};
    request.kind = PrivateChannelActionKind::leave;
    request.channel_id = channel_id;
    request.silent = silent;
    start(c, state, request);
    return true;
}
bool private_channel_begin_ring(PrivateChannelActionController* c, const ChatStateStore& state,
                                u64 channel_id, bool stop) noexcept {
    if (!startable(c, state) || !private_channel_member_count(state, channel_id)) return false;
    PrivateChannelActionRequest request{};
    request.kind = stop ? PrivateChannelActionKind::stop_ringing : PrivateChannelActionKind::ring;
    request.channel_id = channel_id;
    start(c, state, request);
    return true;
}
bool private_channel_begin_accept_request(PrivateChannelActionController* c, const ChatStateStore& state,
                                          u64 channel_id) noexcept {
    return begin_request_decision(c, state, channel_id, PrivateChannelActionKind::accept_request);
}
bool private_channel_begin_ignore_request(PrivateChannelActionController* c, const ChatStateStore& state,
                                          u64 channel_id) noexcept {
    return begin_request_decision(c, state, channel_id, PrivateChannelActionKind::ignore_request);
}
bool private_channel_begin_dismiss_safety(PrivateChannelActionController* c, const ChatStateStore& state,
                                          u64 channel_id) noexcept {
    const auto* record = metadata(state, channel_id);
    if (!startable(c, state) || !record || !record->safety_warning_id) return false;
    PrivateChannelActionRequest request{};
    request.kind = PrivateChannelActionKind::acknowledge_safety;
    request.channel_id = channel_id;
    request.recipients[0] = record->safety_warning_id;
    request.recipient_count = 1;
    start(c, state, request);
    return true;
}
bool private_channel_action_due(const PrivateChannelActionController& c, u64 now_ms) noexcept {
    return c.phase == PrivateChannelActionPhase::queued ||
           (c.phase == PrivateChannelActionPhase::retry_wait && now_ms >= c.retry_at_ms);
}
bool private_channel_action_submitted(PrivateChannelActionController* c, u64 request_id) noexcept {
    if (!c || !request_id || (c->phase != PrivateChannelActionPhase::queued &&
                              c->phase != PrivateChannelActionPhase::retry_wait))
        return false;
    c->request_id = request_id;
    c->phase = PrivateChannelActionPhase::awaiting;
    return true;
}
bool private_channel_action_accept(PrivateChannelActionController* c, ChatStateStore* state,
                                   UserStore* users, const RestClientResponse& response,
                                   Utf8View body, Slice<u32> scratch, Slice<char> packet, Slice<u64> ids) noexcept {
    if (!c || !state || c->phase != PrivateChannelActionPhase::awaiting || response.request_id != c->request_id)
        return false;
    const auto fail = [&](PrivateChannelActionError error) noexcept {
        c->phase = PrivateChannelActionPhase::failed;
        c->error = error;
        c->status_code = response.status_code;
        c->queued_count = 0;
        c->feedback_shown = false;
        return true;
    };
    if (!response.transport_ok()) return fail(PrivateChannelActionError::transport);
    if (response.status_code == 429 && response.rate_limited && response.retry_at_ms) {
        c->phase = PrivateChannelActionPhase::retry_wait;
        c->retry_at_ms = response.retry_at_ms;
        return true;
    }
    const u16 status = response.status_code;
    const bool success = status == 200 || status == 201 || status == 204;
    switch (c->request.kind) {
    case PrivateChannelActionKind::create:
        if (status != 200 && status != 201) return fail(PrivateChannelActionError::http);
        if (!apply_channel(c, state, users, body, scratch, packet, ids, true))
            return fail(PrivateChannelActionError::response);
        break;
    case PrivateChannelActionKind::rename:
        if (!success) return fail(PrivateChannelActionError::http);
        // CHANNEL_UPDATE follows; an unreadable body only delays the label.
        if (status != 204) apply_channel(c, state, users, body, scratch, packet, ids, false);
        break;
    case PrivateChannelActionKind::leave:
        if (!success && status != 404) return fail(PrivateChannelActionError::http);
        if (const auto* channel = chat_state_store_find_channel(*state, c->request.channel_id);
            channel && channel->type == 3)
            chat_state_store_remove_channel(state, channel->id);
        break;
    case PrivateChannelActionKind::accept_request:
        if (!success) return fail(PrivateChannelActionError::http);
        // The official client applies acceptance optimistically; CHANNEL_UPDATE confirms it.
        if (state->private_channels)
            private_channel_store_set_request(state->private_channels, c->request.channel_id, 0);
        chat_state_store_touch_private_channel(state, c->request.channel_id);
        break;
    case PrivateChannelActionKind::ignore_request:
    case PrivateChannelActionKind::ignore_requests: {
        if (!success && status != 404) return fail(PrivateChannelActionError::http);
        const bool batch = c->request.kind == PrivateChannelActionKind::ignore_requests;
        const u32 count = batch ? c->request.recipient_count : 1u;
        for (u32 i = 0; i < count; ++i) {
            const u64 id = batch ? c->request.recipients[i] : c->request.channel_id;
            if (const auto* channel = chat_state_store_find_channel(*state, id); channel && channel->type == 1)
                chat_state_store_remove_channel(state, id);
        }
        break;
    }
    case PrivateChannelActionKind::acknowledge_safety:
        if (!success) return fail(PrivateChannelActionError::http);
        if (state->private_channels)
            private_channel_store_clear_safety_warning(state->private_channels, c->request.channel_id,
                                                       c->request.recipients[0]);
        chat_state_store_touch_private_channel(state, c->request.channel_id);
        break;
    case PrivateChannelActionKind::add_recipient:
        if (!success) return fail(PrivateChannelActionError::http);
        if (c->queued_count) {
            c->request.user_id = c->queued[0];
            copy_bytes(c->queued, c->queued + 1, (c->queued_count - 1) * sizeof(u64));
            c->queued[--c->queued_count] = 0;
            c->request_id = 0;
            c->phase = PrivateChannelActionPhase::queued;
            return true;
        }
        break;
    default:
        if (!success) return fail(PrivateChannelActionError::http);
        break;
    }
    const u64 result = c->result_channel_id;
    *c = {};
    c->result_channel_id = result;
    return true;
}
bool private_call_observe(PrivateCallPrompt* prompt, const ChatStateStore& state, const GatewayClientEvent& event,
                          u64 voice_channel_id) noexcept {
    if (!prompt || event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::call_update &&
         event.dispatch_kind != GatewayClientDispatchKind::call_delete))
        return false;
    const u64 channel = event.state.id;
    const bool ringing = event.dispatch_kind == GatewayClientDispatchKind::call_update && event.state.record_count &&
                         channel != voice_channel_id && private_channel_member_count(state, channel);
    const u64 next = ringing ? channel : prompt->channel_id == channel ? 0 : prompt->channel_id;
    if (next == prompt->channel_id) return false;
    prompt->channel_id = next;
    return true;
}
} // namespace maicord
