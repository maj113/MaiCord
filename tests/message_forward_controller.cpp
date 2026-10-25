#include "maicord/app/message_forward_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/model/relationship_store.hpp"
#include "maicord/model/message_composer.hpp"

namespace maicord {
namespace {
struct ForwardFixture {
    ChatStateStore state{}; GuildRecord guilds[2]{}; u16 guild_order[2]{};
    ChannelRecord channels[128]{}; u16 channel_order[128]{}; u8 names[4096]{};
    MessageRecord records[16]{}; u32 order[16]{}, heights[16]{}; u64 fenwick[16]{}; u8 text[4096]{};
    MessageStore store{}; UiVariableList list{}; MessageTimeline timeline{};
    MessageComposer composer{}; char composer_text[128]{};
    MessageDraftStore drafts{}; MessageDraftRecord draft_records[4]{}; u8 draft_text[512]{};
    MessageComposerController outbox{}; u8 request_text[kMessageComposerMaximumBytes]{};
    MessageForwardController forward{}; QuickSwitcherScope scope{1, 2, 3}; MessageHandle source{};
    QuickSwitcherSources sources() noexcept { return {&state, nullptr, nullptr}; }
    bool channel(u64 id, Utf8View name, u8 type = 0) noexcept {
        ChannelInput input{}; input.id = id; input.guild_id = 10; input.name = name; input.type = type;
        if (!chat_state_store_upsert_channel(&state, input).ok()) return false;
        auto* c = const_cast<ChannelRecord*>(chat_state_store_find_channel(state, id));
        c->permissions_resolved = true; c->actions = channel_action_view | channel_action_read_history | channel_action_send_messages;
        ++state.permission_mutations; return true;
    }
    bool initialize() noexcept {
        if (!chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 128}, {channel_order, 128}, {names, sizeof(names)}) ||
            !message_store_initialize(&store, {records, 16}, {order, 16}, {text, sizeof(text)}) ||
            !ui_variable_list_initialize(&list, {heights, 16}, {fenwick, 16}, 0, 100) ||
            !message_timeline_initialize(&timeline, &store, &list, 101, 100, true) ||
            !message_composer_initialize(&composer, {composer_text, sizeof(composer_text)}) ||
            !message_draft_store_initialize(&drafts, {draft_records, 4}, {draft_text, sizeof(draft_text)}) ||
            !message_composer_controller_initialize(&outbox, &composer, &drafts, &timeline, {request_text, sizeof(request_text)})) return false;
        state.self_user_id = 3; state.selected_channel_id = 101; state.selected_guild_id = 10;
        if (!chat_state_store_upsert_guild(&state, {10, utf8_literal("Workbench"), GuildMutationKind::create, 3}).ok()) return false;
        for (u64 id = 101; id <= 108; ++id) if (!channel(id, utf8_literal("destination"))) return false;
        GatewayMessageEvent source_event{}; source_event.id = 500; source_event.channel_id = 101; source_event.author_id = 4;
        source_event.timestamp_ms = 1; source_event.content = utf8_literal("Source message");
        source_event.fields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_author_id |
            gateway_message_field_content | gateway_message_field_timestamp;
        if (!message_timeline_apply_created(&timeline, source_event).ok()) return false;
        const auto added = message_store_find(store, 500);
        if (!added.ok()) return false; source = added.handle;
        message_composer_reset(&composer, 101, true);
        return message_composer_insert_utf8(&composer, utf8_literal("Keep draft")) == MessageComposerResult::changed;
    }
    bool open() noexcept { return message_forward_open(&forward, scope, sources(), &store, source, utf8_literal("Alice")); }
    u32 find(u64 id) noexcept {
        for (u32 i = 0; i < forward.candidates.count; ++i) if (forward.candidates.rows[i].target.channel_id == id) return i;
        return ~0u;
    }
    bool toggle(u64 id) noexcept { return message_forward_toggle(&forward, scope, sources(), find(id), forward.candidates.revision); }
};
} // namespace
void test_message_forward_controller(void (*check)(bool, const char*) noexcept) noexcept {
    ForwardFixture f{};
    check(f.initialize() && f.open(), "forward workflow captures an authoritative source in fixed storage");
    check(f.forward.candidates.count == 8 && f.find(101) != ~0u && f.forward.source.preview_bytes == 14,
        "forward destinations include the active channel and retain a source preview");
    const auto rebuilds = f.forward.candidates.rebuild_count;
    check(!message_forward_synchronize(&f.forward, f.scope, f.sources(), {}, &f.outbox) &&
        f.forward.candidates.rebuild_count == rebuilds, "unchanged forward destination sources avoid scans");
    ++f.state.activity_mutations; ++f.state.selection_revision;
    check(!message_forward_synchronize(&f.forward, f.scope, f.sources(), {}, &f.outbox) &&
        f.forward.candidates.rebuild_count == rebuilds, "incoming activity and navigation do not rescan forwarding destinations");
    auto* readonly = const_cast<ChannelRecord*>(chat_state_store_find_channel(f.state, 108));
    readonly->actions &= ~channel_action_send_messages; ++f.state.permission_mutations;
    auto* archived = const_cast<ChannelRecord*>(chat_state_store_find_channel(f.state, 107));
    archived->thread_archived = true; ++f.state.channel_mutations;
    const auto old_revision = f.forward.candidates.revision;
    check(!message_forward_toggle(&f.forward, f.scope, f.sources(), 0, old_revision) &&
        message_forward_synchronize(&f.forward, f.scope, f.sources(), {}, &f.outbox) &&
        f.find(108) == ~0u && f.find(107) == ~0u, "source versions fence stale results and filter unavailable send destinations");
    check(f.toggle(101) && f.toggle(102) && f.toggle(103) && f.toggle(104) && f.toggle(105) && !f.toggle(106) &&
        f.forward.destination_count == 5 && f.forward.error == MessageForwardError::selection_limit,
        "forward selection admits exactly five unique destinations");
    check(f.toggle(105) && f.forward.destination_count == 4 && f.toggle(106) &&
        message_forward_set_caption(&f.forward, utf8_literal("Caption")), "forward selection toggles and caption are independent of the composer");
    const auto record_count = f.store.count;
    check(message_forward_confirm(&f.forward, f.scope, f.sources(), &f.outbox, 1725012296789ull, 100) &&
        f.outbox.request_count == 5 && f.store.count == record_count && f.composer.channel_id == 101 &&
        message_composer_text(f.composer).bytes == 10 && bytes_equal(message_composer_text(f.composer).data, "Keep draft", 10),
        "forward confirmation reuses detached outbox sends without touching the timeline or draft");
    const auto request_count = f.outbox.request_count;
    check(!message_forward_confirm(&f.forward, f.scope, f.sources(), &f.outbox, 1725012296790ull, 101) &&
        f.outbox.request_count == request_count && !message_forward_set_caption(&f.forward, utf8_literal("Changed")),
        "double confirmation and committed-caption mutation cannot create another forward");
    MessageComposerSubmit submit{};
    check(message_composer_controller_prepare_submit(&f.outbox, 102, &submit).ok() && submit.content.bytes == 7 &&
        submit.forward_reference.message_id == 500 && submit.forward_reference.channel_id == 101 && submit.forward_reference.guild_id == 10 &&
        message_composer_controller_mark_submitted(&f.outbox, submit.nonce).ok(), "forward submit carries caption and immutable origin reference");
    const auto first_nonce = submit.nonce;
    check(message_composer_controller_observe_http(&f.outbox, 200, MessageSendHttpDisposition::success, 103).ok(),
        "forward HTTP receipt follows the existing tracker");
    GatewayMessageEvent response{}; response.id = 600; response.channel_id = 101; response.author_id = 3;
    response.nonce = first_nonce; response.content = utf8_literal("Caption"); response.timestamp_ms = 1725012296790ull;
    response.fields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_author_id |
        gateway_message_field_content | gateway_message_field_timestamp | gateway_message_field_nonce;
    check(message_composer_controller_observe_response(&f.outbox, first_nonce, response, 101).ok() &&
        message_forward_refresh(&f.forward, f.outbox) && f.forward.destinations[0].state == MessageForwardDestinationState::sent,
        "authoritative response marks only the matching destination as forwarded");
    MessageSendCompletion completed{};
    check(message_composer_controller_take_completion(&f.outbox, &completed) && completed.nonce == first_nonce &&
        completed.kind == MessageSendCompletionKind::accepted && !message_forward_observe_completion(&f.forward, completed),
        "shared completion drain releases the forwarded outbox entry without changing its successful receipt");
    check(message_composer_controller_prepare_submit(&f.outbox, 104, &submit).ok() &&
        message_composer_controller_mark_submitted(&f.outbox, submit.nonce).ok() &&
        message_composer_controller_observe_http(&f.outbox, 403, MessageSendHttpDisposition::terminal_failure, 105).ok() &&
        message_forward_refresh(&f.forward, f.outbox) && f.forward.destinations[1].state == MessageForwardDestinationState::failed,
        "failed forward shows per-destination permission feedback");
    const auto failed_nonce = f.forward.destinations[1].nonce;
    check(message_forward_retry_failed(&f.forward, f.scope, f.sources(), &f.outbox, 1725012296791ull, 106, 102) &&
        f.forward.destinations[0].state == MessageForwardDestinationState::sent && f.forward.destinations[0].nonce == first_nonce &&
        f.forward.destinations[1].nonce == failed_nonce && f.outbox.request_count == 4,
        "retry retains failed nonce and never duplicates a successful destination");
    auto* revoked = const_cast<ChannelRecord*>(chat_state_store_find_channel(f.state, 103));
    revoked->actions &= ~channel_action_send_messages; ++f.state.permission_mutations;
    check(message_forward_synchronize(&f.forward, f.scope, f.sources(), {}, &f.outbox) &&
        f.forward.destinations[2].state == MessageForwardDestinationState::unavailable && !f.forward.destinations[2].nonce &&
        f.outbox.request_count == 3, "permission revocation cancels queued work before it reaches transport");
    f.state.selected_channel_id = 108; ++f.state.selection_revision;
    check(message_timeline_reset(&f.timeline) && message_forward_current(f.forward, f.scope, f.sources()),
        "forward source remains valid after navigation evicts the source timeline");
    check(message_composer_controller_prepare_submit(&f.outbox, 107, &submit).ok() &&
        message_composer_controller_mark_submitted(&f.outbox, submit.nonce).ok() &&
        message_forward_cancel_pending(&f.forward, &f.outbox) && f.outbox.request_count == 1 && f.outbox.inflight_nonce == submit.nonce,
        "explicit cancellation removes unsent destinations while preserving an in-flight receipt");
    const MessageSendCompletion success{submit.nonce, submit.channel_id, 700, 0, 0, 200, 0, MessageSendCompletionKind::accepted};
    check(message_forward_observe_completion(&f.forward, success) &&
        !message_forward_observe_completion(&f.forward, success), "completion observation is idempotent without draining shared completions");
    auto scope = f.scope; ++scope.account_serial;
    check(!message_forward_synchronize(&f.forward, scope, f.sources(), {}, &f.outbox) && !f.forward.initialized &&
        !f.forward.source.store && !f.forward.candidates.count,
        "account scope retirement removes borrowed sources and selectable destinations");

    ForwardFixture invalid{};
    check(invalid.initialize() && invalid.open(), "forward identity fence fixture initializes");
    auto* original = const_cast<MessageRecord*>(message_store_resolve(invalid.store, invalid.source));
    ++original->generation;
    check(!message_forward_current(invalid.forward, invalid.scope, invalid.sources()), "resident source generation reuse invalidates forwarding");
    --original->generation;
    char long_caption[2001]; for (auto& c : long_caption) c = 'a';
    check(!message_forward_set_caption(&invalid.forward, {long_caption, sizeof(long_caption)}) && !invalid.forward.caption_bytes,
        "caption character limit rejects input before any destination send");
    auto* origin = const_cast<ChannelRecord*>(chat_state_store_find_channel(invalid.state, 101));
    ++origin->generation;
    check(!message_forward_current(invalid.forward, invalid.scope, invalid.sources()), "origin channel generation fences a captured source");
    --origin->generation;
    origin->actions &= ~channel_action_read_history;
    original->type = 20;
    check(message_forward_source_available(invalid.sources(), invalid.store, invalid.source),
        "viewable resident chat command messages may be forwarded without an extra history permission gate");
    original->type = 23;
    check(message_forward_source_available(invalid.sources(), invalid.store, invalid.source), "context command messages are valid forward sources");
    original->flags |= 1u << 6;
    check(!message_forward_source_available(invalid.sources(), invalid.store, invalid.source), "ephemeral messages cannot be forwarded");
    original->flags = 0; original->presentation_flags = message_presentation_has_poll;
    check(!message_forward_current(invalid.forward, invalid.scope, invalid.sources()),
        "a poll added to a resident source invalidates its captured forwarding affordance");
    original->presentation_flags = message_presentation_has_call;
    check(!message_forward_source_available(invalid.sources(), invalid.store, invalid.source), "call messages cannot be forwarded");
    original->presentation_flags = message_presentation_has_activity;
    check(!message_forward_source_available(invalid.sources(), invalid.store, invalid.source), "activity messages cannot be forwarded");

    ForwardFixture many{};
    check(many.initialize(), "bounded forwarding search fixture initializes");
    for (u64 i = 200; i < 305; ++i) many.channel(i, utf8_literal("destination"));
    check(many.open() && many.forward.candidates.count == kQuickSwitcherMaximumResults && many.forward.candidates.overflow,
        "forwarding search scans authoritative channels but publishes at most one hundred ranked rows");
    const auto committed = many.forward.revision;
    check(!message_forward_set_caption(&many.forward, {nullptr, 1}) && many.forward.revision != committed && !many.forward.caption_bytes,
        "malformed caption input cannot enter the retained forwarding payload");
    check(many.toggle(101) && !message_forward_confirm(&many.forward, many.scope, many.sources(), &many.outbox, 1725012296792ull, 108) &&
        !many.outbox.request_count && !many.forward.committed && message_forward_set_caption(&many.forward, {}),
        "invalid caption input blocks confirmation until corrected even when a previous caption was retained");

    PrivateChannelStore private_channels{}; PrivateChannelRecord private_records[2]{}; u64 recipients[4]{};
    RelationshipStore relationships{}; RelationshipRecord relations[2]{};
    check(private_channel_store_initialize(&private_channels, {private_records, 2}, {recipients, 4}) &&
        relationship_store_initialize(&relationships, {relations, 2}), "forwarding private eligibility fixture initializes");
    ChannelInput dm{}; dm.id = 400; dm.type = 1; dm.name = utf8_literal("Private contact");
    const u64 recipient = 4;
    check(chat_state_store_upsert_channel(&many.state, dm).ok() &&
        private_channel_store_replace(&private_channels, {400, 0, {&recipient, 1}, {}, true, false, false}),
        "forwarding private eligibility retains existing conversation recipients");
    many.state.private_channels = &private_channels;
    const QuickSwitcherSources private_sources{&many.state, nullptr, &relationships};
    const auto* dm_record = chat_state_store_find_channel(many.state, 400);
    check(dm_record && quick_switcher_channel_forwardable(private_sources, *dm_record),
        "an existing nonfriend DM is a valid forwarding destination");
    RelationshipRecord blocked{}; blocked.user_id = 4; blocked.type = RelationshipType::blocked;
    check(relationship_store_upsert(&relationships, blocked).ok() &&
        !quick_switcher_channel_forwardable(private_sources, *dm_record), "a blocked private recipient cannot be selected for forwarding");
    // Unrestricted matches are filtered before the bounded ranked insertion.
    for (u32 i = 0; i < many.state.channel_count; ++i) {
        auto& channel = many.state.channels[many.state.channel_order[i]];
        channel.content_policy = channel.id == 101 || channel.id >= 200 && channel.id < 305 ? kChannelContentAgeRestricted : 0;
        channel.content_policy_known = true;
    }
    ++many.state.channel_mutations;
    check(message_forward_synchronize(&many.forward, many.scope, many.sources(), {}, &many.outbox) &&
        many.forward.source.age_restricted && many.forward.candidates.count == 100 && many.forward.candidates.overflow,
        "a source age update filters before the result cap and retains one hundred eligible destinations");
    bool only_restricted = true;
    for (u32 i = 0; i < many.forward.candidates.count; ++i) {
        const auto* channel = chat_state_store_find_channel(many.state, many.forward.candidates.rows[i].target.channel_id);
        only_restricted = only_restricted && channel && chat_state_store_channel_age_restricted(many.state, *channel);
    }
    check(only_restricted && !quick_switcher_channel_forwardable(private_sources, *dm_record, true),
        "age-restricted sources cannot be forwarded to unrestricted channels or private conversations");
    ForwardFixture changed_age{};
    check(changed_age.initialize() && changed_age.open() && changed_age.toggle(102), "forward age revalidation fixture initializes");
    auto* restricted_origin = const_cast<ChannelRecord*>(chat_state_store_find_channel(changed_age.state, 101));
    restricted_origin->content_policy = kChannelContentAgeRestricted; restricted_origin->content_policy_known = true;
    ++changed_age.state.channel_mutations;
    check(!message_forward_confirm(&changed_age.forward, changed_age.scope, changed_age.sources(), &changed_age.outbox, 1725012296793ull, 109) &&
        !changed_age.outbox.request_count, "confirmation rechecks changed source age policy before admitting an unsafe destination");

    ForwardFixture retired{};
    check(retired.initialize() && retired.open() && retired.toggle(102) &&
        message_forward_confirm(&retired.forward, retired.scope, retired.sources(), &retired.outbox, 1725012296794ull, 110) &&
        message_composer_controller_prepare_submit(&retired.outbox, 111, &submit).ok() &&
        message_composer_controller_mark_submitted(&retired.outbox, submit.nonce).ok(),
        "in-flight source retirement fixture submits through the shared outbox");
    auto* retired_origin = const_cast<ChannelRecord*>(chat_state_store_find_channel(retired.state, 101));
    retired_origin->actions &= ~channel_action_view; ++retired.state.permission_mutations;
    check(!message_forward_synchronize(&retired.forward, retired.scope, retired.sources(), {}, &retired.outbox) &&
        !retired.forward.initialized && retired.outbox.inflight_nonce == submit.nonce,
        "source retirement removes candidate authority while retaining an in-flight receipt");
    response.id = 800; response.channel_id = 102; response.nonce = submit.nonce;
    check(message_composer_controller_observe_http(&retired.outbox, 200, MessageSendHttpDisposition::success, 112).ok() &&
        message_composer_controller_observe_response(&retired.outbox, submit.nonce, response, 101).ok() &&
        message_composer_controller_take_completion(&retired.outbox, &completed) &&
        message_forward_observe_completion(&retired.forward, completed) &&
        retired.forward.destinations[0].state == MessageForwardDestinationState::sent,
        "accepted authoritative response settles a forward even after source eligibility retires");
    retired_origin->actions |= channel_action_view; ++retired.state.permission_mutations;
    check(retired.open() && !retired.forward.committed && !retired.forward.destination_count,
        "a settled retired receipt no longer blocks a new valid forward");
}
} // namespace maicord
