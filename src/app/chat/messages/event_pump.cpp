#include "event_pump_internal.hpp"
#include "maicord/app/gateway/user_store_dispatch.hpp"
#include "maicord/model/chat/message_rich_store.hpp"
#include "maicord/protocol/gateway/reaction_events.hpp"

namespace maicord {

MessageEventPumpConsumeResult message_event_pump_consume(MessageEventPump* pump,
                                                         const GatewayClientEvent& input_event,
                                                         ConstSlice<u8> input_payload) noexcept {
    if (!pump || !pump->timeline || (!input_payload.data && input_payload.count != 0)) {
        return consume_error(MessageEventPumpError::invalid_input);
    }
    if (input_event.dispatch_kind == GatewayClientDispatchKind::unknown &&
        input_event.kind == GatewayClientEventKind::dispatch &&
        input_event.notification_payload_bytes == 0 &&
        input_event.dispatch_payload_bytes == input_payload.count) {
        observe_sequence(pump, input_event);
        ++pump->ignored;
        MessageEventPumpConsumeResult ignored{};
        ignored.kind = MessageEventPumpConsumeKind::ignored_dispatch;
        return ignored;
    }
    GatewayClientEvent event;
    ConstSlice<u8> payload{};
    if (!gateway_client_notification_trim(input_event, input_payload, &event, &payload)) {
        ++pump->rejected;
        pump->resync_required = true;
        // A descriptor whose byte count disagrees with the mailbox payload is
        // a producer defect, reported apart from a malformed sidecar.
        auto rejected = consume_error(MessageEventPumpError::apply,
                                      input_event.dispatch_payload_bytes != input_payload.count
                                          ? MessageEventPumpRejectReason::descriptor_payload_mismatch
                                          : MessageEventPumpRejectReason::notification_sidecar);
        rejected.kind = MessageEventPumpConsumeKind::rejected;
        return rejected;
    }
    observe_sequence(pump, event);

    MessageEventPumpConsumeResult result{};
    if (event.dispatch_payload_bytes != payload.count) {
        reject_resync(pump, &result, MessageEventPumpRejectReason::descriptor_payload_mismatch);
        return result;
    }
    if (event.kind == GatewayClientEventKind::ready && pump->state) {
        const bool had_channel =
            pump->state->selected_channel_id != 0 || pump->state->restore_channel_id != 0;
        const ChatStateApplyResult applied = chat_state_apply_gateway_event(
            pump->state, event, payload, pump->guild_id_scratch, pump->users);
        if (!applied.ok()) {
            if (invalidate_rejected_thread_authority(pump, event))
                result.repaint = true;
            reject_resync(pump, &result, MessageEventPumpRejectReason::ready_state, state_detail(applied));
            result.state_incomplete = true;
            return result;
        }
        guild_detail_store_apply_gateway_event(pump->guild_details, *pump->state, event, payload);
        if (pump->members) {
            guild_member_store_reset_lists(pump->members);
            guild_member_store_retire_role_authority(pump->members);
            guild_member_store_refresh_visible_users(*pump->members, pump->users);
        }
        // The session's presences follow READY; anything offline meanwhile
        // is never re-sent, so no earlier platform status may survive.
        if (pump->users && user_store_clear_client_status(pump->users))
            ++pump->users->revision;
        if (pump->thread_members) {
            thread_member_store_reset_snapshot(pump->thread_members);
            if (!message_event_pump_synchronize_thread_scope(pump))
                return consume_error(MessageEventPumpError::apply, MessageEventPumpRejectReason::thread_scope);
        }
        // A fresh READY begins a new full state. RESUMED never
        // enters this branch and cannot erase a prior incomplete-state warning.
        pump->private_state_incomplete = false;
        pump->private_recovery_requested = false;
        // A fresh session may omit edits/deletes from the old one. Even when
        // restoration returns to the same channel in one mailbox drain, its
        // retained message history must pass through the normal refetch path.
        if (had_channel)
            pump->resync_required = true;
        result.repaint = applied.selection_changed || applied.channels != 0;
    }
    if (event.kind == GatewayClientEventKind::ready && pump->typing) {
        const bool had_typing = pump->typing->count != 0;
        typing_store_reset(pump->typing);
        result.repaint = result.repaint || had_typing;
    }
    if (event.kind == GatewayClientEventKind::ready && pump->voice_states) {
        const bool had_voice = pump->voice_states->count != 0;
        if (!voice_state_store_reset(pump->voice_states))
            return consume_error(MessageEventPumpError::apply, MessageEventPumpRejectReason::ready_voice_reset);
        result.repaint = result.repaint || had_voice;
    }
    if (event.kind == GatewayClientEventKind::ready && !pump->state && pump->thread_members)
        thread_member_store_reset_snapshot(pump->thread_members);
    if (event.kind != GatewayClientEventKind::dispatch) {
        ++pump->lifecycle;
        result.kind = MessageEventPumpConsumeKind::lifecycle;
        result.repaint = result.repaint || lifecycle_repaints(event.kind);
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::private_state_incomplete) {
        pump->private_state_incomplete = true;
        if (pump->state) {
            chat_state_store_invalidate_thread_authority(pump->state);
            result.repaint = true;
            pump->resync_required = true;
        }
        ++pump->rejected;
        result.kind = MessageEventPumpConsumeKind::rejected;
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::ready_state_complete ||
        event.dispatch_kind == GatewayClientDispatchKind::supplemental_state_complete) {
        if (payload.count)
            return consume_error(MessageEventPumpError::apply, MessageEventPumpRejectReason::completion_payload);
        if (pump->state && (event.dispatch_kind == GatewayClientDispatchKind::supplemental_state_complete ||
                            pump->state->restore_guild_id))
            result.repaint = chat_state_store_finish_restore(pump->state);
        if (!message_event_pump_synchronize_thread_scope(pump))
            return consume_error(MessageEventPumpError::apply, MessageEventPumpRejectReason::thread_scope);
        result.kind = MessageEventPumpConsumeKind::state_applied;
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::user_snapshot) {
        if (!user_store_apply_gateway_snapshots(pump->users, payload, event.state.record_count)) {
            reject_resync(pump, &result, MessageEventPumpRejectReason::user_snapshot);
            return result;
        }
        bool current_user_observed = false;
        if (pump->state) {
            if (event.state.upload_policy_known)
                pump->state->upload_policy = event.state.upload_policy;
            u32 cursor = 0;
            for (u32 i = 0; i < event.state.record_count; ++i) {
                GatewayClientUserMutation user{};
                Utf8View name{}, avatar{};
                if (gateway_client_user_record(payload, &cursor, &user, &name, &avatar) &&
                    user.id == pump->state->self_user_id) {
                    current_user_observed = true;
                    if (user.flags & 4)
                        pump->state->self_premium_type = static_cast<u8>(user.premium_type);
                    if (user.flags & 32)
                        pump->state->content_policy.eligibility = !(user.flags & 64)
                            ? AccountContentEligibility::unknown : (user.flags & 128)
                            ? AccountContentEligibility::allowed : AccountContentEligibility::restricted;
                    if (user.content_fields & 1)
                        pump->state->content_policy.age_verification =
                            static_cast<AccountAgeVerification>(user.age_verification_status);
                    if (user.content_fields & 2) {
                        auto& flags = pump->state->content_policy.flags;
                        flags = static_cast<u8>((flags & ~account_content_parent_managed) |
                            ((user.content_fields & 4) ? account_content_parent_managed : 0));
                    }
                    const auto mfa = (user.flags & 16) ? SelfMfaState::enabled : SelfMfaState::disabled;
                    if ((user.flags & 8) && pump->state->self_mfa != mfa) {
                        pump->state->self_mfa = mfa;
                        // The guild two-factor gate depends on this account state.
                        for (u32 g = 0; g < pump->state->guild_count; ++g) {
                            const auto& guild = pump->state->guilds[pump->state->guild_order[g]];
                            if ((guild.settings.known & guild_settings_mfa_level) && guild.settings.mfa_level)
                                chat_state_store_recompute_guild_permissions(pump->state, guild.id);
                        }
                    }
                }
            }
        }
        result.kind = MessageEventPumpConsumeKind::state_applied;
        result.repaint = true;
        if (!message_event_pump_synchronize_thread_scope(pump))
            return consume_error(MessageEventPumpError::apply, MessageEventPumpRejectReason::thread_scope);
        constexpr auto update = utf8_literal("USER_UPDATE");
        bool original_user_update = event.event_name_bytes == update.bytes;
        for (u32 i = 0; original_user_update && i < update.bytes; ++i)
            original_user_update = event.event_name[i] == update.data[i];
        if (current_user_observed && original_user_update && pump->read_token_reset)
            pump->read_token_reset(pump->read_ack_cancel_context);
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::channel_cursor_update) {
        // Validate the whole batch before updating even its first resident channel.
        bool valid = event.state.guild_id && event.state.fields == gateway_client_state_field_guild_id &&
            event.state.record_count <= kGatewayChannelCursorMaximumEntries &&
            payload.count == event.state.record_count * u32{sizeof(GatewayClientChannelCursor)};
        for (u32 index = 0; valid && index < event.state.record_count; ++index) {
            GatewayClientChannelCursor record{};
            valid = gateway_client_channel_cursor_record(payload, index, &record);
            const auto* channel = valid && pump->state
                ? chat_state_store_find_channel(*pump->state, record.channel_id) : nullptr;
            if (channel && channel->guild_id != event.state.guild_id)
                valid = false;
        }
        if (!valid) {
            reject(pump, &result, MessageEventPumpRejectReason::guild_state);
            return result;
        }
        const auto* guild = pump->state
            ? chat_state_store_find_guild(*pump->state, event.state.guild_id) : nullptr;
        if (!guild || guild->unavailable) {
            ignore(pump, &result);
            return result;
        }
        const u32 revision = pump->state->channel_mutations;
        for (u32 index = 0; index < event.state.record_count; ++index) {
            GatewayClientChannelCursor record{};
            gateway_client_channel_cursor_record(payload, index, &record);
            const auto* channel = chat_state_store_find_channel(*pump->state, record.channel_id);
            constexpr u32 readable = channel_action_view | channel_action_read_history;
            if (!channel || !channel->permissions_resolved || (channel->actions & readable) != readable ||
                (channel->flags & kChannelObfuscatedFlag) ||
                !chat_state_store_channel_visible(*pump->state, *channel) ||
                !record.last_message_id || record.last_message_id <= channel->last_message_id)
                continue;
            if (chat_state_store_note_history_latest(pump->state, channel->id, record.last_message_id) &&
                channel->id == pump->state->selected_channel_id && channel_is_message_capable(channel->type)) {
                // A cursor has no message body. Fetch only the selected view and
                // withhold automatic ACKs until its new tail can actually be seen.
                pump->catchup_required = true;
                pump->timeline->newer_refetch_required = true;
            }
        }
        result.kind = MessageEventPumpConsumeKind::state_applied;
        result.repaint = revision != pump->state->channel_mutations;
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::read_state_snapshot ||
        event.dispatch_kind == GatewayClientDispatchKind::message_ack) {
        if (!pump->state || !pump->state->read_state) {
            ignore(pump, &result);
            return result;
        }
        auto& reads = *pump->state->read_state;
        const u32 channel_revision = pump->state->channel_mutations;
        const u64 version = event.read_state.version_known ? event.read_state.version : reads.version;
        bool accepted = false;
        bool manual = false;
        if (event.dispatch_kind == GatewayClientDispatchKind::read_state_snapshot) {
            u32 required = 0;
            if (preflight_read_snapshot(*pump, event, payload, &required) &&
                read_state_reserve_pending(&reads, required)) {
                accepted = true;
                for (u32 i = 0; i < event.read_state.record_count; ++i) {
                    GatewayClientReadStateMutation record{};
                    if (!gateway_client_read_state_record(payload, i, &record) ||
                        !read_state_upsert(pump->state, read_model_entry(record), version)) {
                        accepted = false;
                        break;
                    }
                }
                if (accepted && reads.version != version) {
                    reads.version = version;
                    saturating_increment(reads.revision);
                }
            }
        } else {
            GatewayClientReadStateMutation record{};
            constexpr u8 ack_fields =
                gateway_read_state_cursor | gateway_read_state_mentions | gateway_read_state_manual;
            const auto bytes = ConstSlice<u8>{reinterpret_cast<const u8*>(&event.read_state.entry),
                                              sizeof(event.read_state.entry)};
            if (!payload.count && !event.read_state.record_count && !event.read_state.partial &&
                gateway_client_read_state_record(bytes, 0, &record) && !(record.fields & ~ack_fields)) {
                manual = (record.fields & gateway_read_state_manual) != 0;
                const bool mentions_known = (record.fields & gateway_read_state_mentions) != 0;
                u32 count = record.mention_count;
                if (mentions_known || cached_mentions(*pump, record.channel_id, record.message_id, &count))
                    accepted =
                        read_state_apply_ack(pump->state, record.channel_id, record.message_id,
                                                     manual, count, mentions_known, version);
            }
        }
        if (!accepted) {
            reject_resync(pump, &result, MessageEventPumpRejectReason::read_state,
                   event.dispatch_kind == GatewayClientDispatchKind::read_state_snapshot ? 1 : 2);
            // A partial snapshot leaves unread state stale; a lone ACK does not.
            result.state_incomplete = event.dispatch_kind == GatewayClientDispatchKind::read_state_snapshot;
            return result;
        }
        if (event.dispatch_kind == GatewayClientDispatchKind::message_ack && pump->read_ack_observe)
            pump->read_ack_observe(pump->read_ack_observe_context, event.read_state.entry.channel_id,
                                   event.read_state.entry.message_id, manual);
        else if (manual && pump->read_ack_cancel)
            pump->read_ack_cancel(pump->read_ack_cancel_context, event.read_state.entry.channel_id);
        result.kind = MessageEventPumpConsumeKind::state_applied;
        result.repaint = channel_revision != pump->state->channel_mutations;
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::thread_member_list_update ||
        event.dispatch_kind == GatewayClientDispatchKind::thread_members_update) {
        if (!pump->state) {
            ignore(pump, &result);
            return result;
        }
        const u32 channel_revision = pump->state->channel_mutations;
        const auto authority = chat_state_apply_thread_authority(event, payload, pump->state);
        if (!authority.ok()) {
            chat_state_store_invalidate_thread_authority(pump->state, event.thread_members.guild_id);
            result.repaint = true;
            reject_resync(pump, &result, MessageEventPumpRejectReason::thread_authority, state_detail(authority));
            return result;
        }
        if (!pump->thread_members || !pump->members || !pump->users) {
            result.kind = MessageEventPumpConsumeKind::state_applied;
            result.repaint = channel_revision != pump->state->channel_mutations;
            return result;
        }
        const u32 revision = pump->thread_members->revision;
        const u32 member_revision = pump->members->revision;
        const u32 user_revision = pump->users->revision;
        if (!message_event_pump_synchronize_thread_scope(pump) ||
            !gateway_client_apply_thread_members(event, payload, pump->thread_members, pump->members,
                                                 pump->users, *pump->state)) {
            chat_state_store_invalidate_thread_authority(pump->state, event.thread_members.guild_id);
            result.repaint = true;
            reject_resync(pump, &result, MessageEventPumpRejectReason::thread_members);
            return result;
        }
        if (!thread_member_store_owns(*pump->thread_members, event.thread_members.guild_id,
                                      event.thread_members.thread_id) &&
            !authority.channels)
            ignore(pump, &result);
        else
            result.kind = MessageEventPumpConsumeKind::state_applied;
        result.repaint =
            revision != pump->thread_members->revision || member_revision != pump->members->revision ||
            user_revision != pump->users->revision || channel_revision != pump->state->channel_mutations;
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::voice_state_snapshot)
        return consume_voice_states(pump, event, payload);
    if (event.dispatch_kind == GatewayClientDispatchKind::guild_member_remove ||
        event.dispatch_kind == GatewayClientDispatchKind::guild_ban_add ||
        event.dispatch_kind == GatewayClientDispatchKind::guild_ban_remove) {
        u64 guild_id = 0, user_id = 0;
        if (!gateway_client_event_guild_membership(event, payload, &guild_id, &user_id, nullptr)) {
            reject(pump, &result, MessageEventPumpRejectReason::guild_membership);
            return result;
        }
        // A positioned row belongs to the lazy member list, which removes it
        // with its own operation; only unpositioned cache entries go here.
        const auto* cached = pump->members && event.dispatch_kind == GatewayClientDispatchKind::guild_member_remove
                                 ? guild_member_store_find(*pump->members, guild_id, user_id)
                                 : nullptr;
        if (cached && cached->list_index == ~0u)
            guild_member_store_remove(pump->members, guild_id, user_id);
        ++pump->applied;
        result.kind = MessageEventPumpConsumeKind::state_applied;
        result.repaint = cached != nullptr;
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::guild_delete && pump->voice_states)
        voice_state_store_clear_guild(pump->voice_states, event.state.id);
    if (gateway_client_dispatch_is_state(event.dispatch_kind)) {
        if (!pump->state) {
            ignore(pump, &result);
            return result;
        }
        const bool role_event = event.dispatch_kind == GatewayClientDispatchKind::guild_role_create ||
                                event.dispatch_kind == GatewayClientDispatchKind::guild_role_update ||
                                event.dispatch_kind == GatewayClientDispatchKind::guild_role_delete;
        RoleRecord previous_role{};
        bool previous_known = false;
        const bool own_thread_creation = event.dispatch_kind == GatewayClientDispatchKind::channel_create &&
            (event.state.channel_event_flags & gateway_client_channel_new_thread) &&
            event.state.owner_id != 0 && event.state.owner_id == pump->state->self_user_id;
        u64 previous_parent_cursor = 0;
        bool previous_parent_known = false;
        if (own_thread_creation) {
            const auto* thread = chat_state_store_find_channel(*pump->state, event.state.id);
            const u64 parent_id = (event.state.fields & gateway_client_state_field_parent_id)
                ? event.state.parent_id : thread ? thread->parent_id : 0;
            if (const auto* parent = chat_state_store_find_channel(*pump->state, parent_id)) {
                previous_parent_cursor = parent->last_read_message_id;
                previous_parent_known = true;
            }
        }
        if (role_event)
            if (const auto* role =
                    chat_state_store_find_role(*pump->state, event.state.guild_id, event.state.id)) {
                previous_role = *role;
                previous_known = true;
            }
        const ChatStateApplyResult applied = chat_state_apply_gateway_event(
            pump->state, event, payload, pump->guild_id_scratch, pump->users);
        if (!applied.ok()) {
            if (invalidate_rejected_thread_authority(pump, event))
                result.repaint = true;
            const bool private_event =
                event.dispatch_kind == GatewayClientDispatchKind::channel_recipient_add ||
                event.dispatch_kind == GatewayClientDispatchKind::channel_recipient_remove ||
                channel_is_private(event.state.type);
            if (private_event)
                pump->private_state_incomplete = true;
            else
                pump->resync_required = true;
            reject(pump, &result, MessageEventPumpRejectReason::guild_state, state_detail(applied));
            // Private failures have their own bounded recovery (private_state_incomplete).
            if (private_event)
                result.error = MessageEventPumpError::none;
            else
                result.state_incomplete = true;
            return result;
        }
        if (role_event &&
            guild_member_store_observe_role(pump->members, *pump->state, event.state.guild_id,
                                                    event.state.id,
                                                    previous_known ? &previous_role : nullptr) &&
            pump->member_roles_refresh) {
            pump->member_roles_refresh(pump->member_roles_refresh_context, event.state.guild_id);
        }
        if (own_thread_creation && previous_parent_known && pump->read_ack_cancel) {
            const auto* thread = chat_state_store_find_channel(*pump->state, event.state.id);
            const auto* parent = thread ? chat_state_store_find_channel(*pump->state, thread->parent_id) : nullptr;
            if (thread && thread->thread_parent_creation_noted && parent &&
                parent->last_read_message_id > previous_parent_cursor && (parent->type == 15 || parent->type == 16))
                pump->read_ack_cancel(pump->read_ack_cancel_context, parent->id);
        }
        if (pump->guild_details && (event.dispatch_kind == GatewayClientDispatchKind::guild_create ||
                                    event.dispatch_kind == GatewayClientDispatchKind::guild_update ||
                                    event.dispatch_kind == GatewayClientDispatchKind::guild_delete))
            guild_detail_store_apply_gateway_event(pump->guild_details, *pump->state, event, payload);
        result.kind = MessageEventPumpConsumeKind::state_applied;
        result.repaint = true;
        if (!message_event_pump_synchronize_thread_scope(pump))
            return consume_error(MessageEventPumpError::apply, MessageEventPumpRejectReason::thread_scope);
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::guild_member_list_update ||
        event.dispatch_kind == GatewayClientDispatchKind::guild_members_chunk ||
        event.dispatch_kind == GatewayClientDispatchKind::guild_member_profile_update) {
        const u32 member_revision = pump->members ? pump->members->revision : 0;
        const u32 user_revision = pump->users ? pump->users->revision : 0;
        const u32 presence_revision = pump->members ? pump->members->presence_revision : 0;
        if (!observe_member_list(pump, event, payload)) {
            reject_resync(pump, &result, MessageEventPumpRejectReason::member_list);
            return result;
        }
        result.kind = MessageEventPumpConsumeKind::state_applied;
        // Structural changes repaint now. In-place row updates advance
        // presence_revision (or presence_visible_dirty), which the owner
        // coalesces like PRESENCE_UPDATE; without a visibility predicate they
        // repaint directly.
        result.repaint = member_revision != pump->members->revision || user_revision != pump->users->revision ||
                         (!pump->presence_visible && presence_revision != pump->members->presence_revision);
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::presence_update)
        return consume_presence(pump, event, payload);
    if (event.dispatch_kind == GatewayClientDispatchKind::message_catchup_required) {
        pump->catchup_required = true;
        pump->timeline->newer_refetch_required = true;
        ignore(pump, &result);
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::typing_start) {
        if (!pump->typing) {
            ignore(pump, &result);
            return result;
        }
        GatewayTypingStartEvent typing{};
        if (!gateway_client_event_as_typing_start(event, payload, &typing)) {
            reject(pump, &result, MessageEventPumpRejectReason::typing, 1);
            return result;
        }
        const TypingStoreResult observed = typing_store_start(
            pump->typing, typing.channel_id, typing.guild_id, typing.user_id, event.timestamp_ms);
        if (observed.error == TypingStoreError::capacity) {
            ignore(pump, &result);
            return result;
        }
        if (!observed.ok()) {
            reject(pump, &result, MessageEventPumpRejectReason::typing, 2);
            return result;
        }
        ++pump->applied;
        result.kind = MessageEventPumpConsumeKind::state_applied;
        result.repaint = observed.changed;
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::message_poll_vote_add ||
        event.dispatch_kind == GatewayClientDispatchKind::message_poll_vote_remove) {
        GatewayMessagePollVoteEvent vote{};
        if (!gateway_client_event_as_message_poll_vote(event, payload, &vote)) {
            reject(pump, &result, MessageEventPumpRejectReason::poll_vote);
            return result;
        }
        result.repaint = pump->poll_vote_observe && pump->poll_vote_observe(pump->poll_vote_context, vote);
        ++pump->applied;
        result.kind = MessageEventPumpConsumeKind::state_applied;
        return result;
    }
    if (is_reaction_dispatch(event.dispatch_kind)) {
        GatewayMessageReactionEvent reaction{};
        if (!gateway_client_event_as_message_reaction(event, payload, &reaction)) {
            reject_resync(pump, &result, MessageEventPumpRejectReason::reaction_decode);
            return result;
        }
        if (pump->state && (pump->state->selected_channel_id == 0 ||
                            reaction.channel_id != pump->state->selected_channel_id)) {
            if (pump->reaction_observe)
                pump->reaction_observe(pump->reaction_observe_context, reaction, MessageHandle{~u32{0}, 0},
                                       0, false);
            if (pump->inactive_observe)
                pump->inactive_observe(pump->inactive_observe_context, event, payload, pump->content_access);
            ignore(pump, &result);
            return result;
        }
        const MessageStoreResult found = message_store_find(*pump->timeline->store, reaction.message_id);
        const MessageRecord* record =
            found.ok() ? message_store_resolve(*pump->timeline->store, found.handle) : nullptr;
        if (!record || record->channel_id != reaction.channel_id || !pump->timeline->rich_store) {
            if (pump->reaction_observe)
                pump->reaction_observe(pump->reaction_observe_context, reaction, MessageHandle{~u32{0}, 0},
                                       0, false);
            ++pump->not_resident;
            result.kind = MessageEventPumpConsumeKind::not_resident;
            return result;
        }
        const MessageRichStoreResult applied = message_rich_store_mutate_reaction(
            pump->timeline->rich_store, reaction.message_id, record->revision,
            message_reaction_mutation(reaction, pump->state ? pump->state->self_user_id : 0));
        if (!applied.ok()) {
            reject_resync(pump, &result, MessageEventPumpRejectReason::reaction_apply, static_cast<u8>(applied.error));
            return result;
        }
        ++pump->applied;
        result.kind = MessageEventPumpConsumeKind::applied;
        result.repaint = true;
        if (pump->reaction_observe)
            pump->reaction_observe(pump->reaction_observe_context, reaction, found.handle, record->revision,
                                   true);
        return result;
    }
    if (!is_message_dispatch(event.dispatch_kind)) {
        ignore(pump, &result);
        return result;
    }
    const auto consume_message =
        [&](const GatewayMessageEvent* decoded_message) noexcept -> MessageEventPumpConsumeResult {
        // The caches are best effort: an author they cannot take must not
        // cost the message its read state, badge or notification.
        const bool users = observe_message_users(pump, event, payload);
        const bool member = observe_message_member(pump, event, payload);
        if (!users || !member)
            saturating_increment(pump->hydration_skipped);
        if (event.dispatch_kind == GatewayClientDispatchKind::message_create && pump->typing) {
            const TypingStoreResult stopped = typing_store_note_message(
                pump->typing, event.message.channel_id, event.message.author_id, false);
            if (!stopped.ok()) {
                reject(pump, &result, MessageEventPumpRejectReason::message_typing);
                return result;
            }
            result.repaint = result.repaint || stopped.changed;
        }
        if (event.dispatch_kind == GatewayClientDispatchKind::message_create && event.message.nonce != 0 &&
            pump->sends) {
            const MessageSendObserveResult observed = message_send_tracker_observe_gateway(
                pump->sends, pump->timeline, event.message.nonce, event.message.channel_id,
                event.message.author_id, event.message.id, event.timestamp_ms);
            if (!observed.ok() && observed.error != MessageSendTrackerError::channel_mismatch &&
                observed.error != MessageSendTrackerError::author_mismatch &&
                observed.error != MessageSendTrackerError::message_mismatch) {
                reject_resync(pump, &result, MessageEventPumpRejectReason::message_send, static_cast<u8>(observed.error));
                return result;
            }
            merge_invalidation(&result.invalidation, observed.invalidation);
            result.repaint = result.repaint || observed.changed;
        }
        bool presentation_mention_known = false;
        bool presentation_mentioned = false;
        const u32 mention_fields = gateway_message_field_mention_everyone | gateway_message_field_mentions |
                                   gateway_message_field_mention_roles;
        if (pump->state && (event.dispatch_kind == GatewayClientDispatchKind::message_create ||
                            (event.dispatch_kind == GatewayClientDispatchKind::message_update &&
                             (event.message.fields & mention_fields) != 0))) {
            if (!message_mentions_current_member(*pump->state, event, payload, &presentation_mentioned)) {
                reject_resync(pump, &result, MessageEventPumpRejectReason::message_mentions);
                return result;
            }
            presentation_mention_known = true;
        }
        bool message_validated = false;
        MessageReadBadge read_badge = MessageReadBadge::none;
        // A create the loaded timeline will apply is decoded once, the way message_timeline_apply decodes
        // it, and that body serves the read state and the timeline. When that decode fails the plain
        // decode below and message_timeline_apply reject it exactly as before.
        GatewayMessageEvent full_message;
        GatewayMessageNamedUser system_target;
        const GatewayMessageEvent* full = decoded_message;
        if (!full && event.dispatch_kind == GatewayClientDispatchKind::message_create && pump->state &&
            pump->state->selected_channel_id != 0 &&
            event.message.channel_id == pump->state->selected_channel_id &&
            (pump->timeline->rich_store
                 ? gateway_client_event_as_message(event, payload, g_message_event_scratch.view(false), &full_message)
                 : gateway_client_event_as_message(event, payload, &full_message)) &&
            gateway_client_event_message_system_target(event, payload, &full_message, &system_target))
            full = &full_message;
        if (event.dispatch_kind == GatewayClientDispatchKind::message_create && pump->state) {
            // The fallback decode reuses the buffer: the timeline then decodes on its own.
            if (!full && !gateway_client_event_as_message(event, payload, &full_message)) {
                reject_resync(pump, &result, MessageEventPumpRejectReason::message_decode);
                return result;
            }
            const GatewayMessageEvent& message = full ? *full : full_message;
            const u32 channel_mutations = pump->state->channel_mutations;
            const auto* channel = chat_state_store_find_channel(*pump->state, message.channel_id);
            read_badge = pump->read_badge
                             ? pump->read_badge(pump->read_badge_context, event, payload)
                             : ((presentation_mentioned ||
                                 (channel && channel_is_private(channel->type) &&
                                  event.message.type != 2 && !channel->notification_muted))
                                    ? MessageReadBadge::mention : MessageReadBadge::none);
            const bool acknowledge_self = event.message.type != 46;
            const bool noted =
                chat_state_store_note_message(pump->state, message.channel_id, message.id,
                                                 message.author_id, read_badge != MessageReadBadge::none,
                                                 false, acknowledge_self,
                                                 read_badge == MessageReadBadge::low_importance);
            message_validated = true;
            if (noted && acknowledge_self && message.author_id == pump->state->self_user_id && pump->read_ack_cancel)
                pump->read_ack_cancel(pump->read_ack_cancel_context, message.channel_id);
            result.repaint = noted && channel_mutations != pump->state->channel_mutations;
        }
        if (pump->state && (pump->state->selected_channel_id == 0 ||
                            event.message.channel_id != pump->state->selected_channel_id)) {
            if (decoded_message)
                observe_message_source(pump, *decoded_message, false);
            if (!observe_message_deletions(pump, event, payload)) {
                reject_resync(pump, &result, MessageEventPumpRejectReason::message_deletions);
                return result;
            }
            if (message_validated && pump->message_create_observe)
                pump->message_create_observe(pump->message_create_context, event, payload);
            if (pump->inactive_observe)
                pump->inactive_observe(pump->inactive_observe_context, event, payload,
                                       resolve_content_access(*pump, event));
            ignore(pump, &result);
            return result;
        }

        result.apply = full ? (event.dispatch_kind == GatewayClientDispatchKind::message_create
                                   ? message_timeline_apply_created(pump->timeline, *full,
                                                                    resolve_content_access(*pump, event))
                                   : message_timeline_apply_updated(pump->timeline, *full,
                                                                    resolve_content_access(*pump, event)))
                            : message_timeline_apply(pump->timeline, event, payload, pump->bulk_id_scratch,
                                                     resolve_content_access(*pump, event));
        if (result.apply.ok()) {
            if (pump->users && pump->timeline->rich_store) {
                const auto& rich_store = *pump->timeline->rich_store;
                const auto* rich = message_rich_store_find(rich_store, event.message.id);
                if (rich && rich->reference.state == GatewayMessageReferenceState::available) {
                    const auto& reference = rich->reference;
                    const auto name = message_rich_store_string(rich_store, reference.author_name);
                    if (name.bytes &&
                        user_store_upsert(pump->users, reference.author_id, name).ok()) {
                        const auto avatar =
                            message_rich_store_string(rich_store, reference.author_avatar_hash);
                        if (avatar.bytes)
                            user_store_set_avatar(pump->users, reference.author_id, avatar);
                    }
                }
                if (rich && message_rich_has_interaction(*rich)) {
                    const auto& interaction = rich->interaction;
                    const Utf8View name = message_rich_store_string(rich_store, interaction.user_name);
                    if (name.bytes != 0 &&
                        user_store_upsert(pump->users, interaction.user_id, name).ok()) {
                        const Utf8View avatar =
                            message_rich_store_string(rich_store, interaction.user_avatar_hash);
                        if (avatar.bytes != 0)
                            user_store_set_avatar(pump->users, interaction.user_id, avatar);
                    }
                }
            }
            if (presentation_mention_known && (result.apply.kind == GatewayMessageApplyKind::created ||
                                               result.apply.kind == GatewayMessageApplyKind::updated)) {
                const auto presentation_record =
                    message_store_find(*pump->timeline->store, event.message.id);
                const auto* presentation =
                    presentation_record.ok()
                        ? message_store_resolve(*pump->timeline->store, presentation_record.handle)
                        : nullptr;
                // Replayed creates can arrive after notification settings or
                // relationships change; preserve the first receipt's decision.
                const bool cache_badge = message_validated && presentation &&
                    !(presentation->presentation_flags & message_presentation_read_badge_known);
                if (!presentation ||
                    !message_store_set_presentation_flags(
                        pump->timeline->store, event.message.id,
                        (presentation->presentation_flags & ~message_presentation_mentions_current_user) |
                            (presentation_mentioned ? message_presentation_mentions_current_user : 0u) |
                            (cache_badge ? message_presentation_read_badge_known : 0u) |
                            (cache_badge && read_badge != MessageReadBadge::none
                                 ? message_presentation_read_badge : 0u))) {
                    reject_resync(pump, &result, MessageEventPumpRejectReason::message_presentation);
                    return result;
                }
            }
            if (result.apply.kind == GatewayMessageApplyKind::deferred) {
                ++pump->deferred;
                pump->catchup_required = true;
                result.kind = MessageEventPumpConsumeKind::deferred;
                return result;
            }
            if (message_validated && pump->message_create_observe)
                pump->message_create_observe(pump->message_create_context, event, payload);
            if (decoded_message)
                observe_message_source(pump, *decoded_message, true);
            if (!observe_message_deletions(pump, event, payload)) {
                reject_resync(pump, &result, MessageEventPumpRejectReason::message_deletions);
                return result;
            }
            ++pump->applied;
            result.kind = MessageEventPumpConsumeKind::applied;
            merge_invalidation(&result.invalidation, result.apply.invalidation);
            result.repaint = result.repaint || result.invalidation.count != 0;
            return result;
        }
        if (is_not_resident(result.apply)) {
            if (decoded_message)
                observe_message_source(pump, *decoded_message, false);
            if (!observe_message_deletions(pump, event, payload)) {
                reject_resync(pump, &result, MessageEventPumpRejectReason::message_deletions);
                return result;
            }
            ++pump->not_resident;
            result.kind = MessageEventPumpConsumeKind::not_resident;
            return result;
        }

        reject_resync(pump, &result, MessageEventPumpRejectReason::message_apply, static_cast<u8>(result.apply.error));
        return result;
    };
    if (pump->reaction_source_observe &&
        (!pump->source_observe_poll_only || (event.message.fields & gateway_message_field_poll)) &&
        (event.dispatch_kind == GatewayClientDispatchKind::message_create ||
         event.dispatch_kind == GatewayClientDispatchKind::message_update))
        return consume_with_message_source(pump, event, payload, consume_message);
    return consume_message(nullptr);
}

MessageRichReactionMutation message_reaction_mutation(const GatewayMessageReactionEvent& event,
                                                      u64 self_user_id) noexcept {
    using Kind = MessageRichReactionMutationKind;
    const Kind kind = event.kind == GatewayMessageReactionEventKind::add         ? Kind::add
                      : event.kind == GatewayMessageReactionEventKind::remove     ? Kind::remove
                      : event.kind == GatewayMessageReactionEventKind::remove_all ? Kind::remove_all
                                                                                  : Kind::remove_emoji;
    return {event.emoji_id, event.name, event.type, kind, event.user_id != 0 && event.user_id == self_user_id,
            event.animated};
}

const char* message_event_pump_reject_reason_name(MessageEventPumpRejectReason reason) noexcept {
    using R = MessageEventPumpRejectReason;
    switch (reason) {
    case R::none: return "none";
    case R::descriptor_payload_mismatch: return "descriptor_payload_mismatch";
    case R::notification_sidecar: return "notification_sidecar";
    case R::ready_state: return "ready_state";
    case R::ready_voice_reset: return "ready_voice_reset";
    case R::thread_scope: return "thread_scope";
    case R::completion_payload: return "completion_payload";
    case R::user_snapshot: return "user_snapshot";
    case R::read_state: return "read_state";
    case R::thread_authority: return "thread_authority";
    case R::thread_members: return "thread_members";
    case R::voice_states: return "voice_states";
    case R::guild_membership: return "guild_membership";
    case R::guild_state: return "guild_state";
    case R::member_list: return "member_list";
    case R::presence_payload: return "presence_payload";
    case R::presence_status: return "presence_status";
    case R::typing: return "typing";
    case R::poll_vote: return "poll_vote";
    case R::reaction_decode: return "reaction_decode";
    case R::reaction_apply: return "reaction_apply";
    case R::message_decode: return "message_decode";
    case R::message_typing: return "message_typing";
    case R::message_send: return "message_send";
    case R::message_mentions: return "message_mentions";
    case R::message_deletions: return "message_deletions";
    case R::message_presentation: return "message_presentation";
    case R::message_apply: return "message_apply";
    }
    return "unknown";
}

MessageEventPumpDrainResult message_event_pump_drain(MessageEventPump* pump, void* source,
                                                     MessageEventPopFunction pop,
                                                     u32 maximum_events) noexcept {
    MessageEventPumpDrainResult result{};
    if (!pump->timeline || !source || !pop || maximum_events == 0) {
        result.error = MessageEventPumpError::invalid_input;
        return result;
    }
    while (result.popped < maximum_events) {
        GatewayClientEvent event{};
        Slice<u8> payload = pump->payload_scratch;
        GatewayClientPopResult popped = pop(source, &event, payload);
        if (popped.error == GatewayClientPopError::empty) {
            break;
        }
        if (popped.error == GatewayClientPopError::output_too_small &&
            popped.payload_bytes <= kGatewayClientGuildRecordCapacity &&
            (pump->large_payload.base ||
             reserved_bytes_initialize(&pump->large_payload, kGatewayClientGuildRecordCapacity)) &&
            reserved_bytes_commit(&pump->large_payload, popped.payload_bytes)) {
            payload = {pump->large_payload.base, pump->large_payload.committed};
            reset_value(&event);
            popped = pop(source, &event, payload);
        }
        // Only the record that needed it keeps the large pages committed.
        const auto settle = [pump]() noexcept { reserved_bytes_decommit(&pump->large_payload); };
        if (popped.error == GatewayClientPopError::output_too_small) {
            settle();
            pump->resync_required = true;
            result.error = MessageEventPumpError::payload_capacity;
            break;
        }
        if (!popped.ok() || popped.payload_bytes > payload.count) {
            settle();
            pump->resync_required = true;
            result.error = MessageEventPumpError::source;
            break;
        }
        ++result.popped;
        ++pump->drained;
        const MessageEventPumpConsumeResult consumed =
            message_event_pump_consume(pump, event, ConstSlice<u8>{payload.data, popped.payload_bytes});
        if (payload.data != pump->payload_scratch.data)
            settle();
        if (consumed.kind == MessageEventPumpConsumeKind::applied) {
            ++result.applied;
        } else if (consumed.kind == MessageEventPumpConsumeKind::deferred) {
            ++result.deferred;
        } else if (consumed.kind == MessageEventPumpConsumeKind::not_resident) {
            ++result.not_resident;
        } else if (consumed.kind == MessageEventPumpConsumeKind::rejected) {
            ++result.rejected;
        } else {
            ++result.ignored;
        }
        result.repaint = result.repaint || consumed.repaint;
        merge_invalidation(&result.invalidation, consumed.invalidation);
        if (consumed.ok() && event.kind == GatewayClientEventKind::ready)
            result.ready_applied = true;
        if (consumed.error == MessageEventPumpError::apply) {
            // One event that cannot apply is skipped, never a stop for the
            // session; consume has already requested the matching resync.
            if (!result.apply_failures++)
                result.failure = {event.kind,      event.dispatch_kind, consumed.reason,
                                  consumed.detail, popped.payload_bytes, event.sequence};
            result.state_incomplete = result.state_incomplete || consumed.state_incomplete;
            continue;
        }
        if (!consumed.ok()) {
            result.error = consumed.error;
            break;
        }
    }
    result.resync_required = pump->resync_required;
    result.private_state_incomplete = pump->private_state_incomplete;
    result.catchup_required = pump->catchup_required;
    return result;
}

} // namespace maicord
