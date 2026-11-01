#pragma once
// Shared by event_pump.cpp and its parts (event_pump_*.cpp); private to them.
#include "maicord/model/user/activity.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/chat/messages/event_pump.hpp"
#include "maicord/app/gateway/user_store_dispatch.hpp"
#include "maicord/app/guild/member_list_channel_binding.hpp"
#include "maicord/app/gateway/thread_members.hpp"
#include "maicord/model/chat/read_state.hpp"
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/app/gateway/client_dispatch.hpp"
#include "../../gateway/member_list_validation.hpp"
#include "maicord/model/chat/message_rich_store.hpp"
#include "maicord/model/voice/state_store.hpp"
#include "maicord/protocol/gateway/reaction_events.hpp"
#include "maicord/app/chat/messages/send_tracker.hpp"
#include "maicord/app/chat/messages/event_scratch.hpp"

namespace maicord {
namespace event_pump_detail {}
using namespace event_pump_detail;
namespace event_pump_detail {

extern MessageEventScratch g_message_scratch;
ReadStateEntry read_model_entry(const GatewayClientReadStateMutation& record) noexcept;
bool preflight_read_snapshot(const MessageEventPump& pump, const GatewayClientEvent& event,
                             ConstSlice<u8> payload, u32* required) noexcept;
bool cached_mentions(const MessageEventPump& pump, u64 channel_id, u64 cursor, u32* count) noexcept;
bool synchronize_thread_scope(MessageEventPump* pump) noexcept;

constexpr bool is_message_dispatch(GatewayClientDispatchKind kind) noexcept {
    return kind == GatewayClientDispatchKind::message_create ||
           kind == GatewayClientDispatchKind::message_update ||
           kind == GatewayClientDispatchKind::message_delete ||
           kind == GatewayClientDispatchKind::message_bulk_delete;
}

constexpr bool is_reaction_dispatch(GatewayClientDispatchKind kind) noexcept {
    return kind == GatewayClientDispatchKind::message_reaction_add ||
           kind == GatewayClientDispatchKind::message_reaction_remove ||
           kind == GatewayClientDispatchKind::message_reaction_remove_all ||
           kind == GatewayClientDispatchKind::message_reaction_remove_emoji;
}

bool observe_member_list(MessageEventPump* pump, const GatewayClientEvent& event,
                         ConstSlice<u8> payload) noexcept;

constexpr bool lifecycle_repaints(GatewayClientEventKind kind) noexcept {
    return kind == GatewayClientEventKind::ready || kind == GatewayClientEventKind::resumed ||
           kind == GatewayClientEventKind::reconnecting || kind == GatewayClientEventKind::closed ||
           kind == GatewayClientEventKind::failed || kind == GatewayClientEventKind::stopped;
}

MessageContentAvailability resolve_content_access(const MessageEventPump& pump,
                                                  const GatewayClientEvent& event) noexcept;
bool is_not_resident(const MessageTimelineApplyResult& result) noexcept;
bool message_mentions_current_member(const ChatStateStore& state, const GatewayClientEvent& event,
                                     ConstSlice<u8> payload, bool* mentioned) noexcept;
bool observe_message_users(MessageEventPump* pump, const GatewayClientEvent& event,
                           ConstSlice<u8> payload) noexcept;
__declspec(noinline) bool observe_message_member(MessageEventPump* pump, const GatewayClientEvent& event,
                                                 ConstSlice<u8> payload) noexcept;
void observe_sequence(MessageEventPump* pump, const GatewayClientEvent& event) noexcept;
void merge_invalidation(MessageTimelineInvalidation* destination,
                        const MessageTimelineInvalidation& source) noexcept;
MessageEventPumpConsumeResult consume_error(MessageEventPumpError error,
                                            MessageEventPumpRejectReason reason = {}) noexcept;
bool invalidate_rejected_thread_authority(MessageEventPump* pump, const GatewayClientEvent& event) noexcept;
void ignore(MessageEventPump* pump, MessageEventPumpConsumeResult* result) noexcept;
void reject(MessageEventPump* pump, MessageEventPumpConsumeResult* result, MessageEventPumpRejectReason reason,
            u8 detail = 0) noexcept;
void reject_resync(MessageEventPump* pump, MessageEventPumpConsumeResult* result,
                   MessageEventPumpRejectReason reason, u8 detail = 0) noexcept;
u8 state_detail(const ChatStateApplyResult& applied) noexcept;
MessageEventPumpConsumeResult consume_voice_states(MessageEventPump* pump, const GatewayClientEvent& event,
                                                   ConstSlice<u8> payload) noexcept;
__declspec(noinline) MessageEventPumpConsumeResult consume_presence(MessageEventPump* pump,
                                                                    const GatewayClientEvent& event,
                                                                    ConstSlice<u8> payload) noexcept;
void observe_message_source(MessageEventPump* pump, const GatewayMessageEvent& message,
                            bool model_applied) noexcept;
bool observe_message_deletions(MessageEventPump* pump, const GatewayClientEvent& event,
                               ConstSlice<u8> payload) noexcept;

template <class Consume>
__declspec(noinline) MessageEventPumpConsumeResult
consume_with_message_source(MessageEventPump* pump, const GatewayClientEvent& event, ConstSlice<u8> payload,
                            Consume consume) noexcept {
    // The observer and canonical mutation borrow one decoded body, which lives
    // in the pump's static scratch until the consumer returns.
    const GatewayMessageRichScratch scratch = g_message_scratch.view(true);
    GatewayMessageEvent message{};
    GatewayMessageNamedUser system_target{};
    bool valid = gateway_client_event_as_message(event, payload, scratch, &message) &&
                 gateway_client_event_message_system_target(event, payload, &message, &system_target);
    if (valid) {
        const auto access = resolve_content_access(*pump, event);
        const auto checked =
            event.dispatch_kind == GatewayClientDispatchKind::message_create
                ? message_reconcile_validate_create(pump->timeline->store, message, access)
                : message_reconcile_validate_update(pump->timeline->store, message, access);
        valid = message.id && message.channel_id &&
                (!(message.fields & gateway_message_field_author_id) || message.author_id) &&
                (checked.ok() || checked.error == MessageReconcileError::not_found);
    }
    if (!valid) {
        ++pump->rejected;
        pump->resync_required = true;
        auto result = consume_error(MessageEventPumpError::apply, MessageEventPumpRejectReason::message_decode);
        result.kind = MessageEventPumpConsumeKind::rejected;
        return result;
    }
    return consume(&message);
}

} // namespace event_pump_detail
} // namespace maicord
