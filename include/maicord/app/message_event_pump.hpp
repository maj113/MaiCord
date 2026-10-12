#pragma once

#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/app/message_timeline.hpp"
#include "maicord/app/message_send_tracker.hpp"
#include "maicord/core/reserved_bytes.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/typing_store.hpp"
#include "maicord/model/voice_state_store.hpp"
#include "maicord/model/thread_member_store.hpp"
#include "maicord/model/user_store.hpp"

namespace maicord {

inline constexpr u32 kMessageEventPumpPayloadCapacity = 32 * 1024;

enum class MessageEventPumpError : u8 {
    none,
    invalid_input,
    source,
    payload_capacity,
    apply,
};

// Why one event was rejected with MessageEventPumpError::apply. Diagnostics
// only: it names the check that failed, never payload contents.
enum class MessageEventPumpRejectReason : u8 {
    none,
    descriptor_payload_mismatch, // dispatch_payload_bytes disagrees with the payload
    notification_sidecar,
    ready_state,       // READY header did not apply to the chat state
    ready_typing_reset,
    ready_voice_reset,
    thread_scope,
    completion_payload, // READY/supplemental completion carried bytes
    user_snapshot,
    read_state,
    thread_authority,
    thread_members,
    voice_states,
    guild_membership,
    guild_state,       // guild/channel/role/member state did not apply
    member_list,
    presence_payload,
    presence_status,
    typing,
    poll_vote,
    reaction_decode,
    reaction_apply,
    message_decode,
    message_users,
    message_typing,
    message_send,
    message_mentions,
    message_deletions,
    message_presentation,
    message_apply,
};

enum class MessageEventPumpConsumeKind : u8 {
    none,
    lifecycle,
    state_applied,
    ignored_dispatch,
    applied,
    deferred,
    not_resident,
    rejected,
};

struct MessageEventPumpConsumeResult {
    MessageTimelineApplyResult apply;
    MessageTimelineInvalidation invalidation;
    MessageEventPumpConsumeKind kind;
    MessageEventPumpError error;
    bool repaint;
    MessageEventPumpRejectReason reason;
    // Reason-specific code (for example the ChatStateApplyError and store
    // error of a state rejection); zero when there is none.
    u8 detail;
    // The rejected event may have left account-wide state (guilds, channels,
    // read state) stale; only a fresh session state repairs it.
    bool state_incomplete;

    constexpr bool ok() const noexcept {
        return error == MessageEventPumpError::none;
    }
};

// The first event a drain skipped after an apply rejection.
struct MessageEventPumpFailure {
    GatewayClientEventKind kind;
    GatewayClientDispatchKind dispatch_kind;
    MessageEventPumpRejectReason reason;
    u8 detail;
    u32 payload_bytes;
    u64 sequence;
};

// Apply rejections are isolated: the event is counted in apply_failures, the
// first one is described in failure, and the drain continues with the next
// event. `error` reports only transport failures (source, capacity, input)
// that stop the drain.
struct MessageEventPumpDrainResult {
    MessageTimelineInvalidation invalidation;
    u32 popped;
    u32 applied;
    u32 deferred;
    u32 ignored;
    u32 not_resident;
    u32 rejected;
    u32 apply_failures;
    MessageEventPumpFailure failure;
    MessageEventPumpError error;
    bool repaint;
    bool resync_required;
    bool catchup_required;
    bool private_state_incomplete;
    // A skipped event may have left account-wide state stale.
    bool state_incomplete;
    // A READY applied in this drain (a fresh account-wide state).
    bool ready_applied;

    constexpr bool ok() const noexcept {
        return error == MessageEventPumpError::none;
    }
};

using MessageEventPopFunction = GatewayClientPopResult (*)(void* source, GatewayClientEvent* event,
                                                           Slice<u8> payload_output) noexcept;
using MessageReadAckCancelFunction = void (*)(void* context, u64 channel_id) noexcept;
using MessageReadAckObserveFunction = void (*)(void* context, u64 channel_id, u64 cursor,
                                               bool manual) noexcept;
using MessageReadTokenResetFunction = void (*)(void* context) noexcept;
// Called synchronously only after a validated live create has committed to
// channel activity (and, for the selected channel, its loaded timeline).
using MessageCreateObserveFunction = void (*)(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept;
using MessageMemberRolesRefreshFunction = void (*)(void*, u64 guild_id) noexcept;
// Borrowed typed data are valid only during this synchronous post-validation
// callback. Model-applied=false describes a valid event outside the loaded
// timeline, never permission to change an unrelated message store.
using MessageReactionObserveFunction = void (*)(void*, const GatewayMessageReactionEvent&, MessageHandle,
                                                u32 revision, bool model_applied) noexcept;
using MessageSourceObserveFunction = void (*)(void*, const GatewayMessageEvent&, MessageHandle,
                                              u32 revision, bool model_applied) noexcept;
using MessageDeleteObserveFunction = void (*)(void*, u64 channel_id, u64 message_id) noexcept;
using MessagePollVoteObserveFunction = bool (*)(void*, const GatewayMessagePollVoteEvent&) noexcept;
// Message and reaction dispatches outside the selected channel, after the
// pump validated them; retained channel snapshots follow these.
using MessageInactiveObserveFunction = void (*)(void*, const GatewayClientEvent&, ConstSlice<u8>,
                                                MessageContentAvailability) noexcept;
// True when a member row or message author for (guild, user) is on screen.
using MessagePresenceVisibleFunction = bool (*)(void*, u64 guild_id, u64 user_id) noexcept;

struct MessageEventPump {
    MessageTimeline* timeline;
    ChatStateStore* state;
    UserStore* users;
    GuildMemberStore* members;
    ThreadMemberStore* thread_members;
    TypingStore* typing;
    VoiceStateStore* voice_states;
    MessageSendTracker* sends;
    MessageReadAckCancelFunction read_ack_cancel;
    MessageReadTokenResetFunction read_token_reset;
    void* read_ack_cancel_context;
    MessageReadAckObserveFunction read_ack_observe;
    void* read_ack_observe_context;
    MessageCreateObserveFunction message_create_observe;
    void* message_create_context;
    MessageMemberRolesRefreshFunction member_roles_refresh;
    void* member_roles_refresh_context;
    MessageReactionObserveFunction reaction_observe;
    MessageSourceObserveFunction reaction_source_observe;
    MessageDeleteObserveFunction reaction_delete_observe;
    void* reaction_observe_context;
    MessagePollVoteObserveFunction poll_vote_observe;
    void* poll_vote_context;
    MessageInactiveObserveFunction inactive_observe;
    void* inactive_observe_context;
    MessagePresenceVisibleFunction presence_visible;
    void* presence_visible_context;
    Slice<u8> payload_scratch;
    // A record larger than payload_scratch (an oversized guild snapshot) is
    // popped into this reservation, committed for that one record only.
    ReservedBytes large_payload;
    Slice<u64> bulk_id_scratch;
    Slice<u64> guild_id_scratch;
    u64 last_sequence;
    u32 drained;
    u32 applied;
    u32 deferred;
    u32 ignored;
    u32 not_resident;
    u32 rejected;
    u32 lifecycle;
    // Guild presence appearance changes applied without any repaint.
    u32 presence_offscreen;
    MessageContentAvailability content_access;
    GatewayClientEventKind last_lifecycle;
    bool has_sequence;
    bool resync_required;
    bool catchup_required;
    bool view_active;
    bool private_state_incomplete;
    bool private_recovery_requested;
    bool source_observe_poll_only;
    // A visible guild presence appearance changed; the owner coalesces the repaint.
    bool presence_visible_dirty;
};

bool message_event_pump_initialize(MessageEventPump* pump, MessageTimeline* timeline,
                                   Slice<u8> payload_scratch, Slice<u64> bulk_id_scratch,
                                   MessageContentAvailability content_access) noexcept;
void message_event_pump_reset(MessageEventPump* pump) noexcept;
void message_event_pump_set_create_observer(MessageEventPump*, MessageCreateObserveFunction,
                                            void*) noexcept;
void message_event_pump_set_member_roles_refresh(MessageEventPump*, MessageMemberRolesRefreshFunction,
                                                 void*) noexcept;
void message_event_pump_set_reaction_observers(MessageEventPump*, MessageReactionObserveFunction,
                                               MessageSourceObserveFunction, MessageDeleteObserveFunction,
                                               void*) noexcept;
void message_event_pump_set_poll_observer(MessageEventPump*, MessagePollVoteObserveFunction,
                                          void*) noexcept;
void message_event_pump_set_inactive_observer(MessageEventPump*, MessageInactiveObserveFunction,
                                              void*) noexcept;
// With a predicate, guild presence never repaints directly: off-screen
// appearance changes are absorbed, visible ones set presence_visible_dirty, and
// member status advances GuildMemberStore::presence_revision.
void message_event_pump_set_presence_visibility(MessageEventPump*, MessagePresenceVisibleFunction,
                                                void*) noexcept;
bool message_event_pump_set_state_store(MessageEventPump* pump, ChatStateStore* state,
                                        Slice<u64> guild_id_scratch) noexcept;
bool message_event_pump_set_user_store(MessageEventPump* pump, UserStore* users) noexcept;
bool message_event_pump_set_member_store(MessageEventPump* pump, GuildMemberStore* members) noexcept;
bool message_event_pump_set_thread_member_store(MessageEventPump* pump,
                                                ThreadMemberStore* members) noexcept;
// Refresh after a local selection before projecting the member sidebar.
bool message_event_pump_synchronize_thread_scope(MessageEventPump* pump) noexcept;
bool message_event_pump_set_typing_store(MessageEventPump* pump, TypingStore* typing) noexcept;
bool message_event_pump_set_voice_state_store(MessageEventPump* pump, VoiceStateStore* voice_states) noexcept;
bool message_event_pump_set_send_tracker(MessageEventPump* pump, MessageSendTracker* sends) noexcept;
void message_event_pump_set_read_ack_cancel(MessageEventPump* pump, MessageReadAckCancelFunction cancel,
                                            void* context) noexcept;
// Called after a validated Gateway ACK commits its model state, for ordinary
// and manual ACKs. The observer handles manual cancellation/reconciliation; when
// absent, the manual cancel hook is used. Other cancellation hooks are
// independent (including accepted own-author messages).
void message_event_pump_set_read_ack_observer(MessageEventPump* pump, MessageReadAckObserveFunction observe,
                                              void* context) noexcept;
// Token reset shares the ACK controller's context and does not cancel queued ACKs.
void message_event_pump_set_read_token_reset(MessageEventPump* pump,
                                             MessageReadTokenResetFunction reset) noexcept;
void message_event_pump_set_view_active(MessageEventPump* pump, bool view_active) noexcept;
MessageEventPumpConsumeResult message_event_pump_consume(MessageEventPump* pump,
                                                         const GatewayClientEvent& event,
                                                         ConstSlice<u8> payload) noexcept;
// Static ASCII label for diagnostics ("unknown" for out-of-range values).
const char* message_event_pump_reject_reason_name(MessageEventPumpRejectReason reason) noexcept;
MessageEventPumpDrainResult message_event_pump_drain(MessageEventPump* pump, void* source,
                                                     MessageEventPopFunction pop,
                                                     u32 maximum_events) noexcept;

} // namespace maicord
