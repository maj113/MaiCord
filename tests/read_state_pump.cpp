#include "maicord/app/message_event_pump.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/read_ack_controller.hpp"

namespace maicord {
namespace read_pump_probe {
struct Fixture {
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channels[4];
    u16 guild_order[1], channel_order[4];
    u8 names[1024];
    ReadStateStore reads;
    ReadStateEntry pending[4];
    u32 reserve_calls, cancel_calls, reset_calls;
    u64 cancelled_channel;
    bool reserve_fail;
    u32 observed_calls;
    u64 observed_channel, observed_cursor;
    bool observed_manual, observed_model_committed;
    MessageStore messages;
    MessageRecord message_records[8];
    u32 message_order[8], heights[8];
    u64 fenwick[8];
    u8 text[256];
    UiVariableList list;
    MessageTimeline timeline;
    MessageEventPump pump;
    u64 guild_scratch[1];
    GatewayClientReadStateMutation records[3];
    u8 payload[96];
};
Fixture fixture{};
bool reserve(void* context, u32) noexcept {
    auto& f = *static_cast<Fixture*>(context); ++f.reserve_calls; return !f.reserve_fail;
}
void cancel(void* context, u64 channel_id) noexcept {
    auto& f = *static_cast<Fixture*>(context); ++f.cancel_calls; f.cancelled_channel = channel_id;
}
void reset(void* context) noexcept { ++static_cast<Fixture*>(context)->reset_calls; }
void observe_ack(void* context, u64 channel_id, u64 cursor, bool manual) noexcept {
    auto& f = *static_cast<Fixture*>(context); ++f.observed_calls;
    f.observed_channel = channel_id; f.observed_cursor = cursor; f.observed_manual = manual;
    const auto* channel = chat_state_store_find_channel(f.state, channel_id);
    f.observed_model_committed = channel && channel->last_read_message_id == cursor;
}
bool channel(u64 id, u64 last) noexcept {
    ChannelInput input{};
    input.id = id; input.guild_id = 7; input.type = 0; input.last_message_id = last; input.last_message_known = true;
    return chat_state_store_upsert_channel(&fixture.state, input).ok();
}
bool initialize() noexcept {
    auto& f = fixture;
    f.reserve_calls = f.cancel_calls = f.reset_calls = 0; f.cancelled_channel = 0; f.reserve_fail = false;
    f.observed_calls = 0; f.observed_channel = f.observed_cursor = 0;
    f.observed_manual = f.observed_model_committed = false;
    if (!chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1}, {f.channels, 4},
            {f.channel_order, 4}, {f.names, sizeof(f.names)}) ||
        !chat_state_store_upsert_guild(&f.state, {7, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() ||
        !read_state_initialize(&f.reads, {f.pending, 4}) ||
        !read_state_set_reserve(&f.reads, reserve, &f) || !read_state_bind(&f.state, &f.reads) ||
        !message_store_initialize(&f.messages, {f.message_records, 8}, {f.message_order, 8}, {f.text, sizeof(f.text)}) ||
        !ui_variable_list_initialize(&f.list, {f.heights, 8}, {f.fenwick, 8}, 0, 100) ||
        !message_timeline_initialize(&f.timeline, &f.messages, &f.list, 100, 200, true) ||
        !channel(80, 150) || !channel(81, 200)) return false;
    f.state.self_user_id = 1;
    f.pump = {}; f.pump.timeline = &f.timeline;
    message_event_pump_set_read_ack_cancel(&f.pump, cancel, &f);
    message_event_pump_set_read_token_reset(&f.pump, reset);
    return message_event_pump_set_state_store(&f.pump, &f.state, {f.guild_scratch, 1}) &&
        chat_state_store_select_channel(&f.state, 80).ok();
}
GatewayClientReadStateMutation entry(u64 id, u64 cursor, u32 mentions) noexcept {
    return {id, cursor, mentions, 70'000, 0x80000001u,
        gateway_read_state_cursor | gateway_read_state_mentions | gateway_read_state_last_viewed | gateway_read_state_flags, {}};
}
MessageEventPumpConsumeResult snapshot(u32 count, u64 version = 10) noexcept {
    auto& f = fixture;
    copy_bytes(f.payload, f.records, count * sizeof(f.records[0]));
    GatewayClientEvent event{}; event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::read_state_snapshot;
    event.read_state.record_count = count; event.read_state.version = version; event.read_state.version_known = true;
    event.dispatch_payload_bytes = count * 32;
    return message_event_pump_consume(&f.pump, event, {f.payload, count * 32});
}
GatewayClientEvent ack(u64 id, u64 cursor, bool manual, bool count_known, u32 count = 0) noexcept {
    GatewayClientEvent event{}; event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::message_ack;
    event.read_state.entry.channel_id = id; event.read_state.entry.message_id = cursor;
    event.read_state.entry.mention_count = count;
    event.read_state.entry.fields = static_cast<u8>(gateway_read_state_cursor |
        (manual ? gateway_read_state_manual : 0) | (count_known ? gateway_read_state_mentions : 0));
    return event;
}
const ChannelRecord* find(u64 id) noexcept { return chat_state_store_find_channel(fixture.state, id); }
GatewayClientEvent ready(bool partial) noexcept {
    GatewayClientEvent event{}; event.kind = GatewayClientEventKind::ready;
    event.state.id = 1; event.state.record_count = 1; event.state.read_state_partial = partial;
    event.dispatch_payload_bytes = 8; event.timestamp_ms = 1'000;
    return event;
}
GatewayClientEvent message(u64 id, u64 channel_id, u64 author) noexcept {
    GatewayClientEvent event{}; event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::message_create;
    event.message.id = id; event.message.channel_id = channel_id; event.message.author_id = author;
    event.message.timestamp_ms = 1'000;
    event.message.fields = gateway_message_field_id | gateway_message_field_channel_id |
        gateway_message_field_author_id | gateway_message_field_content | gateway_message_field_timestamp;
    return event;
}
struct ManualBacking { u32 calls; bool fail; };
bool manual_reserve(void* context, u32) noexcept {
    auto& probe = *static_cast<ManualBacking*>(context); ++probe.calls; return !probe.fail;
}
void manual_controller(void (*check)(bool, const char*) noexcept) noexcept {
    static_assert(sizeof(ReadAckIntent) == 40);
    ReadAckIntent slots[2]{}; ReadAckController controller{}; ManualBacking backing{};
    ReadAckIntent peek{}; ReadAckRequestTicket ticket{}; u64 due = 0;
    const ReadAckIntent ordinary{80, 100, 70'000, 0x80000005u, 0};
    auto manual = ordinary; manual.kind = ReadAckKind::manual;
    manual.mention_count_known = true; manual.mention_count = 3;
    check(read_ack_controller_initialize(&controller, {slots, 2}) &&
        read_ack_controller_set_reserve(&controller, manual_reserve, &backing) && !backing.calls &&
        read_ack_controller_queue(&controller, ordinary, 1000, true) &&
        read_ack_controller_peek_due(controller, 1000, &peek) &&
        read_ack_controller_admit(&controller, peek, 71, &ticket),
        "manual controller: explicit kind retains lazy backing and existing one-flight transport admission");
    const auto before_failure = controller;
    auto other = manual; other.channel_id = 81; backing.fail = true;
    check(!read_ack_controller_queue(&controller, other, 1001, true) &&
        bytes_equal(&controller, &before_failure, sizeof(controller)) && !controller.inflight_superseded,
        "manual controller: backing failure cannot alter pending state or supersede an admitted request");
    backing.fail = false;
    auto invalid = manual; invalid.kind = static_cast<ReadAckKind>(255);
    check(!read_ack_controller_queue(&controller, invalid, 1001, true) &&
        bytes_equal(&controller, &before_failure, sizeof(controller)),
        "manual controller: invalid intent kind is rejected before desired or inflight mutation");
    invalid = ordinary; invalid.mention_count_known = true; invalid.mention_count = 3;
    check(!read_ack_controller_queue(&controller, invalid, 1001, true) &&
        bytes_equal(&controller, &before_failure, sizeof(controller)),
        "manual controller: ordinary intent cannot accidentally carry manual-only mention metadata");
    check(read_ack_controller_queue(&controller, manual, 1002, false) && controller.inflight_superseded &&
        controller.pending[0].kind == ReadAckKind::manual && controller.pending[0].message_id == 100 &&
        controller.pending[0].flags == 0x80000005u && controller.pending[0].last_viewed == 70'000 &&
        controller.pending[0].mention_count_known && controller.pending[0].mention_count == 3 &&
        !read_ack_controller_next_due(controller, &due),
        "manual controller: same cursor with new kind preserves full metadata and waits for old transport drain");
    constexpr u64 long_retry = 1000ull + 6ull * 60 * 60 * 1000;
    check(read_ack_controller_complete(&controller, ticket, ReadAckCompletion::retry, long_retry) == ReadAckCompletionResult::superseded &&
        controller.count == 1 && controller.pending[0].kind == ReadAckKind::manual &&
        read_ack_controller_next_due(controller, &due) && due == long_retry &&
        !read_ack_controller_peek_due(controller, long_retry - 1, &peek) &&
        read_ack_controller_peek_due(controller, long_retry, &peek) &&
        read_ack_controller_admit(&controller, peek, 72, &ticket),
        "manual controller: superseded 429 preserves the full long retry floor before immediate manual admission");
    check(read_ack_controller_queue(&controller, manual, long_retry + 1, true) && controller.inflight_superseded &&
        read_ack_controller_complete(&controller, ticket, ReadAckCompletion::success) == ReadAckCompletionResult::superseded &&
        controller.count == 1 && controller.pending[0].kind == ReadAckKind::manual,
        "manual controller: fresh identical manual action survives old successful response");
    check(read_ack_controller_peek_due(controller, long_retry + 1, &peek) &&
        read_ack_controller_admit(&controller, peek, 73, &ticket) &&
        read_ack_controller_queue(&controller, ordinary, long_retry + 2, true) && !controller.inflight_superseded &&
        read_ack_controller_complete(&controller, ticket, ReadAckCompletion::success) == ReadAckCompletionResult::completed &&
        controller.count == 1 && controller.pending[0].kind == ReadAckKind::automatic,
        "manual controller: exact cursor ordinary mark-read remains a distinct desired kind after manual response");
    read_ack_controller_reset(&controller);
    check(read_ack_controller_queue(&controller, manual, 1900, true) &&
        read_ack_controller_peek_due(controller, 1900, &peek),
        "manual controller: manual transport captures exact immutable mention payload before admission");
    auto changed_count = manual; changed_count.mention_count = 4;
    check(read_ack_controller_queue(&controller, changed_count, 1901, true) &&
        !read_ack_controller_admit(&controller, peek, 70, &ticket) &&
        read_ack_controller_peek_due(controller, 1901, &peek) && peek.mention_count == 4 &&
        read_ack_controller_admit(&controller, peek, 70, &ticket) &&
        read_ack_controller_complete(&controller, ticket, ReadAckCompletion::success) == ReadAckCompletionResult::completed,
        "manual controller: stale same-cursor peek cannot admit a replaced manual mention payload");
    read_ack_controller_reset(&controller);
    auto beginning = manual; beginning.message_id = 0;
    check(read_ack_controller_queue(&controller, beginning, 2000, false) &&
        read_ack_controller_next_due(controller, &due) && due == 2000 &&
        read_ack_controller_peek_due(controller, 2000, &peek) &&
        read_ack_controller_admit(&controller, peek, 74, &ticket),
        "manual controller: beginning boundary zero is valid and immediate only for manual kind");
    check(read_ack_controller_cancel(&controller, 80) &&
        read_ack_controller_queue(&controller, beginning, 2001, true) &&
        read_ack_controller_complete(&controller, ticket, ReadAckCompletion::terminal) == ReadAckCompletionResult::superseded && controller.count == 1,
        "manual controller: cancelled old terminal response cannot remove a freshly queued identical manual boundary");
    check(read_ack_controller_peek_due(controller, 2001, &peek) && read_ack_controller_admit(&controller, peek, 75, &ticket),
        "manual controller: new manual action admits after cancelled request drains");
    const auto stale = ticket; read_ack_controller_reset(&controller);
    check(!controller.count && !controller.inflight_active && !controller.retry_not_before_ms &&
        read_ack_controller_queue(&controller, beginning, 2002, true) &&
        read_ack_controller_peek_due(controller, 2002, &peek) && read_ack_controller_admit(&controller, peek, stale.request_id, &ticket) &&
        read_ack_controller_complete(&controller, stale, ReadAckCompletion::success) == ReadAckCompletionResult::ignored && controller.count == 1,
        "manual controller: fresh READY generation rejects an old manual response with reused request and cursor IDs");
}
}

void test_read_state_pump(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace read_pump_probe;
    auto& f = fixture;
    const bool initialized = initialize();
    check(initialized, "read pump: bounded isolated read and cached-message fixture initializes");
    if (!initialized) return;
    f.records[0] = entry(80, 100, 2); f.records[1] = entry(82, 180, 1);
    f.records[1].reserved[2] = 1;
    const u32 before_invalid = f.state.channel_mutations;
    check(!snapshot(2).ok() && find(80)->last_read_message_id == 80 && f.state.channel_mutations == before_invalid &&
        !f.reads.count && !f.reserve_calls, "read pump: invalid later record rejects full snapshot before resident mutation or backing");
    f.records[1] = entry(82, 180, 1); f.reserve_fail = true;
    check(!snapshot(2).ok() && find(80)->last_read_message_id == 80 && !f.reads.count && !f.cancel_calls,
        "read pump: unmatched backing failure preserves an otherwise valid earlier resident read");
    f.reserve_fail = false;
    check(snapshot(2).ok() && find(80)->last_read_message_id == 100 && find(80)->mention_count == 2 &&
        f.reads.count == 1 && f.reads.version == 10 && read_state_pending(f.reads, 82),
        "read pump: complete snapshot applies resident and unmatched entries after preflight");
    f.records[0] = entry(80, 140, 0); f.records[1] = entry(80, 145, 0);
    check(!snapshot(2, 11).ok() && find(80)->last_read_message_id == 100 && f.reads.version == 10,
        "read pump: duplicate channel IDs cannot partially replace a read cursor");
    f.records[0] = entry(82, 190, 0); f.records[1] = entry(80, 145, 0);
    check(!snapshot(2).ok() && find(80)->last_read_message_id == 100,
        "read pump: unsorted packed read records reject before model changes");

    const auto add = [&](u64 id, bool mentioned) noexcept {
        MessageInput input{}; input.id = id; input.channel_id = 80; input.author_id = 2;
        input.presentation_flags = mentioned ? message_presentation_mentions_current_user : 0;
        return message_store_upsert(&f.messages, input).ok();
    };
    check(add(120, true) && add(130, false) && add(140, true), "read pump: cached selected mention reconstruction fixture admits messages");
    auto event = ack(80, 125, true, false);
    check(message_event_pump_consume(&f.pump, event, {}).ok() && find(80)->last_read_message_id == 125 &&
        find(80)->mention_count == 1 && find(80)->read_mentions_incomplete && find(80)->read_auto_ack_suppressed &&
        f.cancel_calls == 1 && f.cancelled_channel == 80,
        "read pump: manual missing server count rebuilds bounded cached mentions and cancels outgoing ACK");
    event = ack(80, 145, true, true, 0); event.read_state.entry.reserved[0] = 1;
    check(!message_event_pump_consume(&f.pump, event, {}).ok() && f.cancel_calls == 1 && find(80)->last_read_message_id == 125,
        "read pump: malformed manual ACK neither mutates reads nor invokes cancellation");
    event = ack(80, 125, false, false);
    check(message_event_pump_consume(&f.pump, event, {}).ok() && find(80)->mention_count == 1 &&
        find(80)->read_mentions_incomplete && f.cancel_calls == 1,
        "read pump: automatic missing server count also reconstructs bounded cached mentions as an estimate");
    event = ack(80, 90, false, true, 3); event.read_state.version_known = true; event.read_state.version = 1;
    check(message_event_pump_consume(&f.pump, event, {}).ok() && find(80)->last_read_message_id == 90 &&
        !find(80)->read_mentions_incomplete && find(80)->mention_count == 3 && f.reads.version == 1 && f.cancel_calls == 1,
        "read pump: automatic older ACK/version is accepted and does not cancel queued local work");

    event = message(210, 81, 1);
    event.message.content_bytes = 1;
    check(!message_event_pump_consume(&f.pump, event, {}).ok() && f.cancel_calls == 1,
        "read pump: malformed own-author create cannot cancel outgoing work");
    event.message.content_bytes = 0;
    check(message_event_pump_consume(&f.pump, event, {}).ok() && find(81)->last_read_message_id == 210 &&
        find(81)->read_state_known && !find(81)->read_flags_known && !find(81)->read_last_viewed_known &&
        f.cancel_calls == 2 && f.cancelled_channel == 81,
        "read pump: accepted unselected own-author create advances read and cancels older outbound ACK");
    check(message_event_pump_consume(&f.pump, event, {}).ok() && f.cancel_calls == 2,
        "read pump: duplicate own-author activity cannot cancel newer desired ACK");
    check(ui_variable_list_initialize(&f.list, {f.heights, 8}, {f.fenwick, 8}, f.messages.count, 100) &&
        message_timeline_initialize(&f.timeline, &f.messages, &f.list, 100, 200, true),
        "read pump: seeded cached messages keep timeline geometry in sync");
    event = message(160, 80, 2); f.pump.view_active = true; f.timeline.follow_bottom = true;
    const auto incoming = message_event_pump_consume(&f.pump, event, {});
    check(incoming.ok() && find(80)->last_read_message_id == 90 && find(80)->last_message_id == 160 && f.cancel_calls == 2,
        "read pump: ordinary foreground bottom create leaves automatic ACK to shared observer");
    GatewayClientUserMutation user{}; user.id = 1;
    copy_bytes(f.payload, &user, sizeof(user));
    event = {}; event.kind = GatewayClientEventKind::dispatch; event.dispatch_kind = GatewayClientDispatchKind::user_snapshot;
    event.state.record_count = 1; event.dispatch_payload_bytes = sizeof(user);
    constexpr auto user_update = utf8_literal("USER_UPDATE");
    copy_bytes(event.event_name, user_update.data, user_update.bytes);
    event.event_name_bytes = static_cast<u8>(user_update.bytes);
    event.state.record_count = 2;
    check(!message_event_pump_consume(&f.pump, event, {f.payload, sizeof(user)}).ok() && !f.reset_calls,
        "read pump: malformed current-user update cannot reset rotated ACK token");
    event.state.record_count = 1;
    user.id = 2; copy_bytes(f.payload, &user, sizeof(user));
    check(message_event_pump_consume(&f.pump, event, {f.payload, sizeof(user)}).ok() && !f.reset_calls,
        "read pump: other-user snapshot does not reset current-user ACK token");
    user.id = 1; copy_bytes(f.payload, &user, sizeof(user));
    event.event_name_bytes = 0;
    check(message_event_pump_consume(&f.pump, event, {f.payload, sizeof(user)}).ok() && !f.reset_calls,
        "read pump: untagged READY hydration cannot impersonate USER_UPDATE token reset");
    event.event_name_bytes = static_cast<u8>(user_update.bytes);
    check(message_event_pump_consume(&f.pump, event, {f.payload, sizeof(user)}).ok() && f.reset_calls == 1 && f.cancel_calls == 2,
        "read pump: accepted self USER_UPDATE resets token without cancelling desired ACK queue");

    u8 guild_ids[8] = {7}; event = ready(true); event.state.id = 2;
    const u32 calls_before_ready = f.reserve_calls, count_before_ready = f.reads.count;
    check(!message_event_pump_consume(&f.pump, event, {guild_ids, 8}).ok() &&
        f.reserve_calls == calls_before_ready && f.reads.count == count_before_ready && f.state.channel_count == 2,
        "read pump: invalid READY identity fails before read carry or channel removals");
    event = ready(true); f.reserve_fail = true;
    check(!message_event_pump_consume(&f.pump, event, {guild_ids, 8}).ok() && f.state.channel_count == 2 &&
        f.reads.count == count_before_ready && !f.reads.preparing_ready,
        "read pump: failed partial READY reserve preserves resident channels and unmatched reads");
    f.reserve_fail = false;
    check(message_event_pump_consume(&f.pump, event, {guild_ids, 8}).ok() && !f.state.channel_count &&
        f.reads.count == 3 && !f.reads.preparing_ready && read_state_pending(f.reads, 80)->message_id == 90 &&
        read_state_pending(f.reads, 81)->message_id == 210,
        "read pump: partial READY carries omitted resident reads then immediately ends removal capture");
    GatewayClientEvent resumed{}; resumed.kind = GatewayClientEventKind::resumed;
    const u32 before_resume = f.reads.revision;
    check(message_event_pump_consume(&f.pump, resumed, {}).ok() && f.reads.count == 3 && f.reads.revision == before_resume,
        "read pump: RESUMED retains authoritative read state without fresh-session reset");
    check(chat_state_store_upsert_guild(&f.state, {7, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() &&
        channel(80, 160) && find(80)->last_read_message_id == 90 && find(80)->mention_count == 3 && f.reads.count == 2,
        "read pump: partial READY channel rehydration consumes only its unmatched read entry");
    event = ready(false);
    check(message_event_pump_consume(&f.pump, event, {guild_ids, 8}).ok() && !f.reads.count && !f.state.channel_count &&
        !f.reads.preparing_ready, "read pump: full READY replaces old read state without carrying omitted records");

    check(initialize(), "read pump: local manual unread fixture resets session and timeline");
    f.records[0] = entry(80, 150, 0);
    check(snapshot(1, 42).ok() && read_state_mark_unread_local(&f.state, 80, 100, 2, false) &&
        find(80)->read_auto_ack_suppressed && find(80)->read_mentions_incomplete &&
        f.guild.unread_channels == 2 && f.guild.mention_count == 2,
        "read pump: selected optimistic manual unread publishes guild unread and incomplete mention state");
    f.pump.view_active = true; f.timeline.follow_bottom = true;
    event = message(180, 80, 2);
    check(message_event_pump_consume(&f.pump, event, {}).ok() && find(80)->last_read_message_id == 100 &&
        find(80)->last_message_id == 180 && find(80)->read_auto_ack_suppressed &&
        find(80)->read_mentions_incomplete && find(80)->mention_count == 2 && !f.cancel_calls,
        "read pump: foreground bottom incoming message preserves local manual backward cursor for the shared observer gate");
    event = ack(80, 100, true, true, 1);
    check(message_event_pump_consume(&f.pump, event, {}).ok() && find(80)->last_read_message_id == 100 &&
        find(80)->read_auto_ack_suppressed && !find(80)->read_mentions_incomplete &&
        find(80)->mention_count == 1 && f.guild.mention_count == 1 && f.cancel_calls == 1,
        "read pump: authoritative manual echo replaces local mention estimate and cancels outgoing work through the existing owner");
    check(chat_state_store_select_channel(&f.state, 81).ok() && !find(80)->read_auto_ack_suppressed &&
        find(80)->last_read_message_id == 100 && find(80)->mention_count == 1,
        "read pump: leaving manually unread channel releases only automatic suppression while retaining its backward cursor");
    message_event_pump_set_read_ack_observer(&f.pump, observe_ack, &f);
    event = ack(80, 110, true, true, 2);
    check(message_event_pump_consume(&f.pump, event, {}).ok() && f.observed_calls == 1 &&
        f.observed_channel == 80 && f.observed_cursor == 110 && f.observed_manual &&
        f.observed_model_committed && f.cancel_calls == 1,
        "read pump: typed manual observer receives committed identity without unconditional legacy cancellation");
    event = ack(80, 130, false, true, 1);
    check(message_event_pump_consume(&f.pump, event, {}).ok() && f.observed_calls == 2 &&
        f.observed_channel == 80 && f.observed_cursor == 130 && !f.observed_manual &&
        f.observed_model_committed && f.cancel_calls == 1,
        "read pump: typed ordinary observer receives committed cursor for narrow host-owned manual projection");
    event = ack(80, 0, true, true, 0); event.read_state.entry.reserved[0] = 1;
    check(!message_event_pump_consume(&f.pump, event, {}).ok() && f.observed_calls == 2 && f.cancel_calls == 1 &&
        find(80)->last_read_message_id == 130,
        "read pump: rejected ACK cannot invoke typed observer or cancel hook");
    event = message(190, 80, 1);
    check(message_event_pump_consume(&f.pump, event, {}).ok() && f.observed_calls == 2 &&
        f.cancel_calls == 2 && f.cancelled_channel == 80,
        "read pump: typed ACK observer leaves own-author cancellation ownership unchanged");
    message_event_pump_set_read_ack_observer(&f.pump, nullptr, nullptr);
    event = ack(80, 0, true, true, 0);
    check(message_event_pump_consume(&f.pump, event, {}).ok() && f.observed_calls == 2 && f.cancel_calls == 3 &&
        f.cancelled_channel == 80,
        "read pump: removing typed observer restores legacy remote manual cancellation");
    manual_controller(check);
}
} // namespace maicord
