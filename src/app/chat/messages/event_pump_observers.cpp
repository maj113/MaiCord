#include "event_pump_internal.hpp"
#include "maicord/app/guild/member_list_channel_binding.hpp"
#include "../../gateway/member_list_validation.hpp"

namespace maicord {
namespace {

static_assert(sizeof(MessageEventPump) == 344 + sizeof(ReservedBytes));

} // namespace
namespace {

// One presence record's activities (consume_presence and observe_member_extras: single-threaded, never
// nested). A UserActivitySet is 3.5 KB, which would otherwise cost every presence a stack probe.
UserActivitySet g_activity_scratch;

} // namespace
namespace event_pump_detail {

ReadStateEntry read_model_entry(const GatewayClientReadStateMutation& record) noexcept {
    return {record.channel_id,
            record.message_id,
            record.mention_count,
            record.last_viewed,
            record.flags,
            record.fields,
            {}};
}
bool preflight_read_snapshot(const MessageEventPump& pump, const GatewayClientEvent& event,
                             ConstSlice<u8> payload, u32* required) noexcept {
    const auto& reads = *pump.state->read_state;
    if (!required || !reads.pending.data || reads.count > reads.backed_count ||
        reads.backed_count > reads.pending.count || reads.preparing_ready ||
        event.read_state.record_count > kGatewayReadStateMaximumEntries ||
        payload.count != event.read_state.record_count * sizeof(GatewayClientReadStateMutation))
        return false;
    *required = reads.count;
    u64 previous = 0;
    for (u32 i = 0; i < event.read_state.record_count; ++i) {
        GatewayClientReadStateMutation record{};
        if (!gateway_client_read_state_record(payload, i, &record) || record.channel_id <= previous ||
            (record.fields & gateway_read_state_manual))
            return false;
        previous = record.channel_id;
        const auto* channel = chat_state_store_find_channel(*pump.state, record.channel_id);
        if (channel) {
            if (!channel_is_private(channel->type) &&
                !chat_state_store_find_guild(*pump.state, channel->guild_id))
                return false;
        } else if (!read_state_pending(reads, record.channel_id))
            ++*required;
    }
    return true;
}
bool cached_mentions(const MessageEventPump& pump, u64 channel_id, u64 cursor, u32* count) noexcept {
    *count = 0;
    const auto* store = pump.timeline ? pump.timeline->store : nullptr;
    if (pump.state->selected_channel_id != channel_id || !store)
        return true;
    if (!store->records || !store->order || store->count > store->capacity)
        return false;
    for (u32 i = 0; i < store->count; ++i) {
        const u32 slot = store->order[i];
        if (slot >= store->capacity || !store->records[slot].occupied)
            return false;
        const auto& message = store->records[slot];
        if (message.channel_id == channel_id && message.id > cursor &&
            message.delivery == MessageDeliveryState::authoritative &&
            (message.presentation_flags &
                ((message.presentation_flags & message_presentation_read_badge_known)
                     ? message_presentation_read_badge : message_presentation_mentions_current_user)))
            saturating_increment(*count);
    }
    return true;
}
} // namespace event_pump_detail

bool message_event_pump_synchronize_thread_scope(MessageEventPump* pump) noexcept {
    if (!pump->thread_members)
        return true;
    const auto* channel =
        pump->state ? chat_state_store_find_channel(*pump->state, pump->state->selected_channel_id)
                    : nullptr;
    const auto* parent = channel && channel->parent_id
                             ? chat_state_store_find_channel(*pump->state, channel->parent_id)
                             : nullptr;
    const auto* guild = channel ? chat_state_store_find_guild(*pump->state, channel->guild_id) : nullptr;
    const bool active = channel && guild && !guild->unavailable && parent &&
                        parent->guild_id == channel->guild_id &&
                        (channel->type == 11 || channel->type == 12) && channel->thread_archived_known &&
                        !channel->thread_archived;
    return thread_member_store_select(pump->thread_members, active ? channel->guild_id : 0,
                                              active ? channel->id : 0);
}

namespace {

// The official client keeps no per-platform presence for the current user
// (its own avatar never shows the mobile indicator).
u8 platform_presence(const MessageEventPump* pump, u64 user_id, u8 client_status) noexcept {
    return pump->state && user_id == pump->state->self_user_id ? u8{0} : client_status;
}

GuildMemberStatus member_status(GatewayClientMemberListStatus status) noexcept {
    switch (status) {
    case GatewayClientMemberListStatus::online:
        return GuildMemberStatus::online;
    case GatewayClientMemberListStatus::idle:
        return GuildMemberStatus::idle;
    case GatewayClientMemberListStatus::dnd:
        return GuildMemberStatus::dnd;
    case GatewayClientMemberListStatus::offline:
        return GuildMemberStatus::offline;
    default:
        return GuildMemberStatus::unknown;
    }
}

bool validate_member_list(const MessageEventPump& pump, const GatewayClientEvent& event,
                          ConstSlice<u8> payload) noexcept {
    const auto& list = event.member_list;
    if ((!payload.data && payload.count) || list.record_count > 2'048 ||
        list.member_record_count > list.record_count || !pump.users->records || !pump.users->order ||
        !pump.users->names || !pump.users->capacity || !pump.members->records || !pump.members->order ||
        !pump.members->capacity || pump.members->count > pump.members->capacity)
        return false;
    // Decode the complete packed body before modifying positions, counts or
    // identity caches. Malformed trailing records must not partially apply an
    // otherwise valid SYNC/INSERT. This adds no packet-sized snapshot/buffer.
    u32 cursor = 0, index = 0, members = 0, groups = 0, logical = 0;
    bool summaries = false;
    while (index < list.record_count) {
        GatewayClientMemberListMutation mutation{};
        ConstSlice<u8> roles{}, global_extra{}, guild_extra{};
        Utf8View nickname{}, name{}, avatar{}, username{};
        if (!gateway_client_event_member_list_record(event, payload, &cursor, &index, &mutation, &roles,
                                                     &nickname, &name, &avatar, &username, &global_extra,
                                                     &guild_extra))
            return false;
        if (event.dispatch_kind == GatewayClientDispatchKind::guild_members_chunk &&
            (mutation.kind != GatewayClientMemberListItemKind::member ||
             mutation.operation != GatewayClientMemberListOperation::sync ||
             (mutation.hydration_flags & ~(gateway_member_timeout_known |
                                          gateway_member_quarantine_known | gateway_member_quarantined))))
            return false;
        if (event.dispatch_kind == GatewayClientDispatchKind::guild_member_profile_update &&
            (mutation.kind != GatewayClientMemberListItemKind::member ||
             mutation.operation != GatewayClientMemberListOperation::sync))
            return false;
        if (mutation.kind == GatewayClientMemberListItemKind::summary) {
            summaries = true;
            if (!list.groups_known || mutation.index != logical || mutation.range_end > ~0u - logical - 1u)
                return false;
            logical += mutation.range_end + 1u;
            ++groups;
        } else {
            if (summaries)
                return false;
            if (mutation.operation == GatewayClientMemberListOperation::sync ||
                mutation.operation == GatewayClientMemberListOperation::insert ||
                mutation.operation == GatewayClientMemberListOperation::update) {
                if (mutation.kind == GatewayClientMemberListItemKind::member) {
                    if (name.bytes + username.bytes > pump.users->name_bank_bytes)
                        return false;
                    ++members;
                }
            }
        }
    }
    return cursor == payload.count && members == list.member_record_count &&
           (list.groups_known ? groups == list.group_count && logical == list.logical_count
                              : groups == 0 && list.group_count == 0 && list.logical_count == 0) &&
           gateway_member_list_summary_ids_unique(payload, groups);
}

bool affects_visible_members(const GuildMemberStore& members, u64 guild_id, u64 list_id, u32 first,
                             u32 last) noexcept {
    const auto& visible = members.visible;
    return visible.guild_id == guild_id && visible.list_id == list_id && guild_id &&
           first <= visible.last && last >= visible.first;
}

// Out of line: the activity set would otherwise join observe_member_list's stack frame. Applies a
// member record's appearance and presence activities; *changed reports a store revision change.
__declspec(noinline) bool observe_member_extras(MessageEventPump* pump, u64 guild_id,
                                                const GatewayClientMemberListMutation& mutation,
                                                ConstSlice<u8> global_extra, ConstSlice<u8> guild_extra,
                                                bool* changed) noexcept {
    UserAppearance appearance;
    // Only these two fields are read when the record carries no trailer
    // (an empty set clears); a trailer read fills the rest.
    UserActivitySet& activities = g_activity_scratch;
    activities.user_id = mutation.user_id;
    activities.count = 0;
    u32 appearance_bytes = 0;
    const u32 appearance_revision = pump->users->appearance.revision;
    const u32 activity_revision = pump->users->activities.revision;
    if (!user_appearance_read(global_extra, mutation.user_id, 0, &appearance) ||
        !user_appearance_upsert(&pump->users->appearance, appearance) ||
        !user_appearance_read_prefix(guild_extra, mutation.user_id, guild_id, &appearance, &appearance_bytes) ||
        !user_appearance_upsert(&pump->users->appearance, appearance) ||
        (appearance_bytes < guild_extra.count &&
         !user_activities_read({guild_extra.data + appearance_bytes, guild_extra.count - appearance_bytes},
                               mutation.user_id, &activities)))
        return false;
    // The member's presence activities, scoped like a guild PRESENCE_UPDATE.
    if (mutation.hydration_flags & gateway_member_activities_known)
        user_activity_apply(&pump->users->activities, activities, guild_id,
                            mutation.status == GatewayClientMemberListStatus::offline);
    *changed = appearance_revision != pump->users->appearance.revision ||
               activity_revision != pump->users->activities.revision;
    return true;
}

} // namespace
namespace event_pump_detail {

bool observe_member_list(MessageEventPump* pump, const GatewayClientEvent& event,
                         ConstSlice<u8> payload) noexcept {
    if (!pump->members || !pump->users || !pump->state || event.member_list.guild_id == 0) {
        return false;
    }
    // Preserve the server's visibility-set ID across channel switches. Legacy
    // packets lacking it can only use the selected channel's derived list ID.
    u64 list_id = event.member_list.list_id;
    const bool targeted = event.dispatch_kind == GatewayClientDispatchKind::guild_members_chunk ||
                          event.dispatch_kind == GatewayClientDispatchKind::guild_member_profile_update;
    if (targeted && !chat_state_store_find_guild(*pump->state, event.member_list.guild_id))
        return false;
    if (!targeted && list_id == 0) {
        const ChannelRecord* selected =
            chat_state_store_find_channel(*pump->state, pump->state->selected_channel_id);
        if (!selected || selected->guild_id != event.member_list.guild_id) {
            return false;
        }
        list_id = member_list_channel_id(*pump->state, selected->id);
        if (!list_id)
            return false;
    }
    if (!validate_member_list(*pump, event, payload))
        return false;
    if (!targeted)
        guild_member_store_refresh_visible_users(*pump->members, pump->users);
    const auto& list = event.member_list;
    const auto refresh_visible = [&](u32 first, u32 last) noexcept {
        if (affects_visible_members(*pump->members, list.guild_id, list_id, first, last))
            guild_member_store_refresh_visible_users(*pump->members, pump->users);
    };
    if (!targeted &&
        !guild_member_store_observe_list(pump->members, list.guild_id, list_id, list.member_count,
                                                 list.online_count, list.members_known, list.online_known,
                                                 list.groups_known, list.group_count, list.logical_count))
        return false;
    u32 cursor = 0;
    u32 record_index = 0;
    u32 member_records = 0;
    while (record_index < event.member_list.record_count) {
        GatewayClientMemberListMutation mutation{};
        ConstSlice<u8> roles{};
        Utf8View nickname{};
        Utf8View display_name{};
        Utf8View avatar_hash{};
        Utf8View username{};
        ConstSlice<u8> global_extra{}, guild_extra{};
        if (!gateway_client_event_member_list_record(event, payload, &cursor, &record_index, &mutation,
                                                     &roles, &nickname, &display_name, &avatar_hash,
                                                     &username, &global_extra, &guild_extra)) {
            return false;
        }
        if (mutation.kind == GatewayClientMemberListItemKind::summary) {
            if (!guild_member_store_group_summary(pump->members, list.guild_id, list_id,
                                                          mutation.index, mutation.user_id,
                                                          mutation.range_end))
                return false;
            continue;
        }
        if (mutation.operation == GatewayClientMemberListOperation::invalidate) {
            if (!guild_member_store_invalidate_list_range(
                    pump->members, list.guild_id, mutation.index, mutation.range_end, list_id)) {
                return false;
            }
            refresh_visible(mutation.index, mutation.range_end);
            continue;
        }
        if (mutation.operation == GatewayClientMemberListOperation::remove) {
            if (!guild_member_store_invalidate_list_range(
                    pump->members, list.guild_id, mutation.index, mutation.index, list_id) ||
                !guild_member_store_shift_list(pump->members, list.guild_id, list_id,
                                                       mutation.index, false)) {
                return false;
            }
            refresh_visible(mutation.index, ~0u);
            continue;
        }
        if (mutation.operation == GatewayClientMemberListOperation::insert) {
            if (!guild_member_store_shift_list(pump->members, list.guild_id, list_id,
                                                       mutation.index, true))
                return false;
            refresh_visible(mutation.index, ~0u);
        }
        if (mutation.kind == GatewayClientMemberListItemKind::group) {
            if (!guild_member_store_set_group(
                    pump->members, list.guild_id, list_id, mutation.index, mutation.user_id,
                    mutation.range_end,
                    (mutation.hydration_flags & gateway_member_group_count_omitted) == 0))
                return false;
            refresh_visible(mutation.index, mutation.index);
            continue;
        }
        // An UPDATE for the member already at this index is applied in place
        // when its sorted group is unchanged (see the store call below);
        // anything else replaces the positional row.
        bool in_place = false;
        if (mutation.operation == GatewayClientMemberListOperation::update && !targeted) {
            const auto* resident = guild_member_store_find(*pump->members, list.guild_id, mutation.user_id);
            in_place = resident && resident->list_id == list_id && resident->list_index == mutation.index;
        }
        const auto withdraw_update_row = [&]() noexcept {
            if (!guild_member_store_invalidate_list_range(pump->members, list.guild_id, mutation.index,
                                                          mutation.index, list_id))
                return false;
            refresh_visible(mutation.index, mutation.index);
            return true;
        };
        // Timeout and quarantine state ride the member record's hydration flags.
        const auto apply_member_flags = [&]() noexcept {
            if (mutation.hydration_flags & gateway_member_timeout_known)
                guild_member_store_set_timeout(pump->members, list.guild_id, mutation.user_id,
                                               mutation.communication_disabled_until);
            if (mutation.user_id == pump->state->self_user_id &&
                (mutation.hydration_flags & gateway_member_quarantine_known)) {
                GuildSettings settings{};
                settings.known = guild_settings_self_quarantine;
                settings.self_member =
                    (mutation.hydration_flags & gateway_member_quarantined) ? guild_self_member_quarantined : 0;
                chat_state_store_set_guild_settings(pump->state, list.guild_id, settings);
            }
        };
        if (mutation.operation == GatewayClientMemberListOperation::update && !in_place &&
            !withdraw_update_row())
            return false;
        if (!targeted && (mutation.operation == GatewayClientMemberListOperation::sync ||
                          mutation.operation == GatewayClientMemberListOperation::update)) {
            guild_member_store_extend_list(pump->members, list.guild_id, list_id, mutation.index);
        }
        if (roles.count != static_cast<u32>(mutation.role_count) * 8u ||
            !user_store_upsert(pump->users, mutation.user_id, display_name, username).ok() ||
            (!(mutation.hydration_flags & gateway_member_avatar_omitted) &&
             !user_store_set_avatar(pump->users, mutation.user_id, avatar_hash))) {
            return false;
        }
        bool extras_changed = false;
        if (!observe_member_extras(pump, list.guild_id, mutation, global_extra, guild_extra, &extras_changed))
            return false;
        const bool row_visible =
            !targeted && guild_member_store_visible_index(*pump->members, list.guild_id, list_id, mutation.index);
        // Repeated identical appearance is not a change. An in-place UPDATE's
        // appearance follows the presence policy: visible rows coalesce, the
        // rest only update the model.
        // Per-platform presence arrives with a known status and repaints like
        // the appearance above (the mobile indicator replaces the online dot).
        const bool platforms_changed = mutation.status != GatewayClientMemberListStatus::unknown &&
                                       user_store_set_client_status(
                                           pump->users, mutation.user_id,
                                           platform_presence(pump, mutation.user_id, mutation.client_status));
        if (extras_changed || platforms_changed) {
            if (!in_place || !pump->presence_visible)
                ++pump->users->revision;
            else if (row_visible)
                pump->presence_visible_dirty = true;
            else
                saturating_increment(pump->presence_offscreen);
        }
        u64 role_ids[256];
        for (u32 index = 0; index < mutation.role_count; ++index) {
            role_ids[index] = byte_load_le64(roles.data + index * 8u);
            if (role_ids[index] == 0) {
                return false;
            }
        }
        if (mutation.hydration_flags & gateway_member_nickname_omitted)
            if (const auto* previous =
                    guild_member_store_find(*pump->members, list.guild_id, mutation.user_id))
                nickname = guild_member_store_nickname(*previous);
        if (in_place) {
            const bool roles_known = (mutation.hydration_flags & gateway_member_roles_omitted) == 0;
            const auto placed = guild_member_store_update_in_place(
                pump->members, *pump->state, list.guild_id, list_id, mutation.index, mutation.user_id, nickname,
                {role_ids, mutation.role_count}, roles_known, member_status(mutation.status));
            if (placed.applied) {
                // Status and role color refresh resident rows through
                // presence_revision; a visible label change asks the owner
                // for one coalesced text refresh.
                if (placed.label_changed && row_visible) {
                    if (pump->presence_visible)
                        pump->presence_visible_dirty = true;
                    else
                        ++pump->users->revision;
                }
                apply_member_flags();
                if (row_visible)
                    user_store_pin(pump->users, mutation.user_id);
                ++member_records;
                continue;
            }
            // Its sorted group changed: replace the positional row instead.
            if (!withdraw_update_row())
                return false;
        }
        const auto admitted = guild_member_store_upsert(
            pump->members, *pump->state, list.guild_id, mutation.user_id, nickname,
            {role_ids, mutation.role_count}, (mutation.hydration_flags & gateway_member_roles_omitted) == 0,
            member_status(mutation.status), targeted ? ~0u : mutation.index, list_id);
        if (!admitted.ok() || !admitted.record)
            return false;
        apply_member_flags();
        const auto& cached = *admitted.record;
        if (targeted) {
            // The visible-author planner owns pins; a non-positional response
            // must never manufacture a sidebar index or visibility-set extent.
        } else if (guild_member_store_visible_index(*pump->members, list.guild_id, list_id, mutation.index)) {
            user_store_pin(pump->users, cached.user_id);
        } else {
            const auto* profile = user_store_find(*pump->users, cached.user_id);
            // Most page arrivals only pin their just-accepted visible identity.
            // A previously visible user moving out needs a bounded refresh so
            // its old profile pin does not accumulate beside replacements.
            if (profile && profile->pinned)
                guild_member_store_refresh_visible_users(*pump->members, pump->users);
        }
        ++member_records;
    }
    if (list.groups_known) {
        guild_member_store_finish_groups(pump->members, list.guild_id, list_id);
        refresh_visible(0, ~0u);
    }
    return cursor == payload.count && member_records == event.member_list.member_record_count;
}

MessageContentAvailability resolve_content_access(const MessageEventPump& pump,
                                                  const GatewayClientEvent& event) noexcept {
    // Field presence and string length are different facts. The server sends an
    // explicit empty content string for embed-, sticker-, and component-only
    // messages. Those messages are fully loaded and must not inherit the
    // pump's unknown-content fallback.
    if ((event.message.fields & gateway_message_field_content) != 0) {
        return MessageContentAvailability::available;
    }
    return pump.content_access;
}

bool is_not_resident(const MessageTimelineApplyResult& result) noexcept {
    return result.error == MessageTimelineError::reconcile &&
           result.mutation.error == MessageReconcileError::not_found;
}

bool message_mentions_current_member(const ChatStateStore& state, const GatewayClientEvent& event,
                                     ConstSlice<u8> payload, bool* mentioned) noexcept {
    *mentioned = (event.message.mention_flags & (gateway_client_message_mention_everyone |
                                                 gateway_client_message_mention_current_user)) != 0;
    const ChannelRecord* channel = chat_state_store_find_channel(state, event.message.channel_id);
    for (u32 index = 0; index < event.message.mention_role_count; ++index) {
        u64 role_id = 0;
        if (!gateway_client_event_message_mention_role_at(event, payload, index, &role_id)) {
            return false;
        }
        if (channel && chat_state_store_self_has_role(state, channel->guild_id, role_id)) {
            *mentioned = true;
        }
    }
    return true;
}

bool observe_message_users(MessageEventPump* pump, const GatewayClientEvent& event,
                           ConstSlice<u8> payload) noexcept {
    if (!pump->users || (event.dispatch_kind != GatewayClientDispatchKind::message_create &&
                         event.dispatch_kind != GatewayClientDispatchKind::message_update)) {
        return true;
    }
    if (event.message.author_name_bytes != 0) {
        Utf8View name{};
        if (event.message.author_id == 0 ||
            !gateway_client_event_message_author_name(event, payload, &name) ||
            !user_store_upsert(pump->users, event.message.author_id, name).ok()) {
            return false;
        }
    }
    if (event.message.author_avatar_hash_bytes != 0) {
        Utf8View avatar_hash{};
        if (event.message.author_id == 0 ||
            !gateway_client_event_message_author_avatar_hash(event, payload, &avatar_hash) ||
            !user_store_find(*pump->users, event.message.author_id) ||
            !user_store_set_avatar(pump->users, event.message.author_id, avatar_hash)) {
            return false;
        }
    }
    for (u32 index = 0; index < event.message.mention_user_count; ++index) {
        u64 user_id = 0;
        Utf8View name{};
        if (!gateway_client_event_message_mention_user_at(event, payload, index, &user_id, &name) ||
            !user_store_upsert(pump->users, user_id, name).ok()) {
            return false;
        }
    }
    return gateway_client_event_visit_message_snapshot_mention_users(
        event, payload, pump->users, [](void* context, u64 user_id, Utf8View name) noexcept {
            return user_store_upsert(static_cast<UserStore*>(context), user_id, name).ok();
        });
}

// Out of line: its role-id array would otherwise join message_event_pump_consume's stack frame.
__declspec(noinline) bool observe_message_member(MessageEventPump* pump, const GatewayClientEvent& event,
                                                 ConstSlice<u8> payload) noexcept {
    if (!pump->members || !pump->state ||
        (event.dispatch_kind != GatewayClientDispatchKind::message_create &&
         event.dispatch_kind != GatewayClientDispatchKind::message_update) ||
        (event.message.fields & gateway_message_field_member) == 0) {
        return true;
    }
    const ChannelRecord* channel =
        chat_state_store_find_channel(*pump->state, event.message.channel_id);
    if (!channel || channel->guild_id == 0 || event.message.author_id == 0 ||
        event.message.member_role_count > kGatewayMessageMemberRoleMaximumIds) {
        return false;
    }
    u64 roles[kGatewayMessageMemberRoleMaximumIds];
    for (u32 index = 0; index < event.message.member_role_count; ++index) {
        if (!gateway_client_event_message_member_role_at(event, payload, index, &roles[index])) {
            return false;
        }
    }
    // The member object is authoritative: an empty nickname means none.
    Utf8View nick{};
    if (!gateway_client_event_message_member_nick(event, payload, &nick))
        return false;
    return guild_member_store_upsert(pump->members, *pump->state, channel->guild_id,
                                             event.message.author_id, nick,
                                             ConstSlice<u64>{roles, event.message.member_role_count}, true,
                                             GuildMemberStatus::unknown)
        .ok();
}

void observe_sequence(MessageEventPump* pump, const GatewayClientEvent& event) noexcept {
    if (event.has_sequence) {
        pump->last_sequence = event.sequence;
        pump->has_sequence = true;
    }
}

void merge_invalidation(MessageTimelineInvalidation* destination,
                        const MessageTimelineInvalidation& source) noexcept {
    if (source.count == 0) {
        return;
    }
    if (destination->count == 0) {
        *destination = source;
        return;
    }
    const u32 destination_end = destination->first + destination->count;
    const u32 source_end = source.first + source.count;
    const u32 first = minimum(destination->first, source.first);
    const u32 end = maximum(destination_end, source_end);
    destination->first = first;
    destination->count = end - first;
    destination->structural = destination->structural || source.structural;
    destination->scroll_changed = destination->scroll_changed || source.scroll_changed;
}

MessageEventPumpConsumeResult consume_error(MessageEventPumpError error,
                                            MessageEventPumpRejectReason reason) noexcept {
    MessageEventPumpConsumeResult result{};
    result.error = error;
    result.reason = reason;
    return result;
}

// A rejected thread or channel-thread event may have left the guild's thread authority stale.
bool invalidate_rejected_thread_authority(MessageEventPump* pump, const GatewayClientEvent& event) noexcept {
    if (event.dispatch_kind != GatewayClientDispatchKind::thread_self_update &&
        event.dispatch_kind != GatewayClientDispatchKind::thread_list_sync && event.state.type != 10 &&
        event.state.type != 11 && event.state.type != 12)
        return false;
    chat_state_store_invalidate_thread_authority(
        pump->state, event.dispatch_kind == GatewayClientDispatchKind::thread_self_update ? event.thread_self.guild_id
                     : event.dispatch_kind == GatewayClientDispatchKind::thread_list_sync ? event.thread_sync.guild_id
                                                                                          : event.state.guild_id);
    return true;
}

// An event this view has no use for.
void ignore(MessageEventPump* pump, MessageEventPumpConsumeResult* result) noexcept {
    ++pump->ignored;
    result->kind = MessageEventPumpConsumeKind::ignored_dispatch;
}

// Counts one event rejected for one apply check.
void reject(MessageEventPump* pump, MessageEventPumpConsumeResult* result, MessageEventPumpRejectReason reason,
            u8 detail) noexcept {
    ++pump->rejected;
    result->kind = MessageEventPumpConsumeKind::rejected;
    result->error = MessageEventPumpError::apply;
    result->reason = reason;
    result->detail = detail;
}

// A rejection that may have dropped state also forces a resync.
void reject_resync(MessageEventPump* pump, MessageEventPumpConsumeResult* result,
                   MessageEventPumpRejectReason reason, u8 detail) noexcept {
    pump->resync_required = true;
    reject(pump, result, reason, detail);
}

u8 state_detail(const ChatStateApplyResult& applied) noexcept {
    return static_cast<u8>((static_cast<u32>(applied.error) << 4) | (static_cast<u32>(applied.store_error) & 15u));
}

MessageEventPumpConsumeResult consume_voice_states(MessageEventPump* pump, const GatewayClientEvent& event,
                                                   ConstSlice<u8> payload) noexcept {
    MessageEventPumpConsumeResult result{};
    const auto& snapshot = event.voice_states;
    if (!pump->voice_states) {
        ignore(pump, &result);
        return result;
    }
    if (payload.count != snapshot.record_count * sizeof(GatewayClientVoiceStateRecord)) {
        reject(pump, &result, MessageEventPumpRejectReason::voice_states);
        return result;
    }
    // Gateway rows share VoiceState's field order.
    static_assert(sizeof(GatewayClientVoiceStateRecord) == sizeof(VoiceState));
    const u32 revision = pump->voice_states->revision;
    if (snapshot.replace)
        voice_state_store_clear_guild(pump->voice_states, snapshot.guild_id);
    if (pump->state)
        pump->voice_states->retained_guild = pump->state->selected_guild_id;
    // Overflow at the bound drops rows or evicts other guilds; the store counts them.
    voice_state_store_apply_batch(pump->voice_states, payload);
    ++pump->applied;
    result.kind = MessageEventPumpConsumeKind::state_applied;
    result.repaint = revision != pump->voice_states->revision;
    return result;
}

// Out of line: its activity set would otherwise join message_event_pump_consume's stack frame.
__declspec(noinline) MessageEventPumpConsumeResult consume_presence(MessageEventPump* pump,
                                                                    const GatewayClientEvent& event,
                                                                    ConstSlice<u8> payload) noexcept {
    MessageEventPumpConsumeResult result{};
    const auto& presence = event.presence;
    UserAppearance appearance;
    appearance.fields = 0;
    UserActivitySet& activities = g_activity_scratch;
    u32 appearance_bytes = 0;
    bool valid = presence.user_id != 0 && presence.status <= GatewayClientMemberListStatus::offline;
    bool has_activities = false;
    if (valid && payload.count) {
        // Appearance record, then the optional activities trailer.
        valid = user_appearance_read_prefix(payload, presence.user_id, presence.guild_id, &appearance,
                                            &appearance_bytes);
        has_activities = valid && appearance_bytes < payload.count;
        if (has_activities)
            valid = user_activities_read({payload.data + appearance_bytes, payload.count - appearance_bytes},
                                         presence.user_id, &activities);
    } else if (valid)
        valid = user_appearance_read(payload, presence.user_id, presence.guild_id, &appearance);
    if (!valid) {
        reject(pump, &result, MessageEventPumpRejectReason::presence_payload,
               static_cast<u8>((payload.count ? 1 : 0) | (has_activities ? 2 : 0)));
        return result;
    }
    // Large guilds stream presence for members nobody can see. The model
    // always updates; only an on-screen row or author invalidates text,
    // and the owner coalesces that repaint (see presence_visible_dirty).
    const bool filtered = pump->presence_visible && presence.guild_id != 0;
    // Activities without a trailer (or an offline presence) carry no rich
    // presence; user_activity_apply decides which scope may clear them.
    const bool activity_update =
        pump->users && (has_activities || (appearance.fields & appearance_activity) ||
                        presence.status == GatewayClientMemberListStatus::offline);
    // client_status is per user (the server keeps one per session set), so a
    // friend presence and a guild presence update the same record.
    const bool platforms_changed =
        pump->users && presence.status != GatewayClientMemberListStatus::unknown &&
        user_store_set_client_status(pump->users, presence.user_id,
                                     presence.status == GatewayClientMemberListStatus::offline
                                         ? u8{0}
                                         : platform_presence(pump, presence.user_id, presence.client_status));
    if (pump->users && (payload.count || activity_update || platforms_changed)) {
        const u32 appearance_revision = pump->users->appearance.revision;
        const u32 activity_revision = pump->users->activities.revision;
        if (payload.count && !user_appearance_upsert(&pump->users->appearance, appearance))
            return result;
        if (activity_update) {
            if (!has_activities) {
                activities.user_id = presence.user_id;
                activities.count = 0;
            }
            user_activity_apply(&pump->users->activities, activities, presence.guild_id,
                                presence.status == GatewayClientMemberListStatus::offline);
        }
        if (appearance_revision != pump->users->appearance.revision ||
            activity_revision != pump->users->activities.revision || platforms_changed) {
            if (!filtered) {
                ++pump->users->revision;
                result.repaint = true;
            } else if (pump->presence_visible(pump->presence_visible_context, presence.guild_id,
                                              presence.user_id)) {
                pump->presence_visible_dirty = true;
            } else {
                saturating_increment(pump->presence_offscreen);
            }
        }
    }
    // A guild-less friend presence is a different scope, not permission to
    // overwrite every cached guild status for that user.
    if (!pump->members || presence.guild_id == 0) {
        ignore(pump, &result);
        return result;
    }
    const u32 presence_revision = pump->members->presence_revision;
    if (!guild_member_store_set_status(pump->members, presence.guild_id, presence.user_id,
                                       member_status(presence.status))) {
        reject(pump, &result, MessageEventPumpRejectReason::presence_status);
        return result;
    }
    ++pump->applied;
    result.kind = MessageEventPumpConsumeKind::state_applied;
    result.repaint = result.repaint || (!filtered && presence_revision != pump->members->presence_revision);
    return result;
}

void observe_message_source(MessageEventPump* pump, const GatewayMessageEvent& message,
                            bool model_applied) noexcept {
    if (!pump->reaction_source_observe)
        return;
    const auto found = message_store_find(*pump->timeline->store, message.id);
    const auto* record = found.ok() ? message_store_resolve(*pump->timeline->store, found.handle) : nullptr;
    const bool resident = record && record->channel_id == message.channel_id;
    pump->reaction_source_observe(pump->reaction_observe_context, message,
                                  resident ? found.handle : MessageHandle{~u32{0}, 0},
                                  resident ? record->revision : 0, model_applied);
}

bool observe_message_deletions(MessageEventPump* pump, const GatewayClientEvent& event,
                               ConstSlice<u8> payload) noexcept {
    if (!pump->reaction_delete_observe)
        return true;
    if (event.dispatch_kind == GatewayClientDispatchKind::message_delete) {
        GatewayMessageDeleteEvent message{};
        if (!gateway_client_event_as_message_delete(event, payload, &message))
            return false;
        pump->reaction_delete_observe(pump->reaction_observe_context, message.channel_id, message.id);
    } else if (event.dispatch_kind == GatewayClientDispatchKind::message_bulk_delete) {
        GatewayMessageBulkDeleteEvent message{};
        if (!gateway_client_event_as_message_bulk_delete(event, payload, pump->bulk_id_scratch, &message))
            return false;
        for (u32 i = 0; i < message.id_count; ++i)
            pump->reaction_delete_observe(pump->reaction_observe_context, message.channel_id,
                                          pump->bulk_id_scratch[i]);
    }
    return true;
}

} // namespace event_pump_detail

bool message_event_pump_initialize(MessageEventPump* pump, MessageTimeline* timeline,
                                   Slice<u8> payload_scratch, Slice<u64> bulk_id_scratch,
                                   MessageContentAvailability content_access) noexcept {
    if (!pump || !timeline || !timeline->store || !timeline->list || !payload_scratch.data ||
        payload_scratch.count < kMessageEventPumpPayloadCapacity || !bulk_id_scratch.data ||
        bulk_id_scratch.count < kGatewayBulkDeleteMaximumIds) {
        return false;
    }
    reserved_bytes_release(&pump->large_payload);
    reset_value<MessageEventPump>(pump);
    pump->timeline = timeline;
    pump->payload_scratch = payload_scratch;
    pump->bulk_id_scratch = bulk_id_scratch;
    pump->content_access = content_access;
    return true;
}

void message_event_pump_reset(MessageEventPump* pump) noexcept {
    reserved_bytes_release(&pump->large_payload);
    reset_value<MessageEventPump>(pump);
}

void message_event_pump_set_view_active(MessageEventPump* pump, bool view_active) noexcept {
    pump->view_active = view_active;
}

bool message_event_pump_set_state_store(MessageEventPump* pump, ChatStateStore* state,
                                        Slice<u64> guild_id_scratch) noexcept {
    if (!state || !guild_id_scratch.data || guild_id_scratch.count < state->guild_capacity) {
        return false;
    }
    pump->state = state;
    pump->guild_id_scratch = guild_id_scratch;
    return message_event_pump_synchronize_thread_scope(pump);
}

void message_event_pump_set_guild_detail_store(MessageEventPump* pump, GuildDetailStore* details) noexcept {
    pump->guild_details = details;
}

bool message_event_pump_set_user_store(MessageEventPump* pump, UserStore* users) noexcept {
    if (!users || !users->records || !users->order || !users->names) {
        return false;
    }
    pump->users = users;
    return true;
}

bool message_event_pump_set_member_store(MessageEventPump* pump,
                                         GuildMemberStore* members) noexcept {
    if (!members || !members->records || !members->order) {
        return false;
    }
    pump->members = members;
    return true;
}

bool message_event_pump_set_voice_state_store(MessageEventPump* pump,
                                              VoiceStateStore* voice_states) noexcept {
    if (voice_states && (!voice_states->records.data || voice_states->count > voice_states->records.count))
        return false;
    pump->voice_states = voice_states;
    return true;
}

bool message_event_pump_set_typing_store(MessageEventPump* pump, TypingStore* typing) noexcept {
    if (typing && (!typing->records.data || typing->records.count == 0 || typing->count > typing->records.count)) {
        return false;
    }
    pump->typing = typing;
    return true;
}
void message_event_pump_set_create_observer(MessageEventPump* pump, MessageCreateObserveFunction observe,
                                            void* context) noexcept {
    pump->message_create_observe = observe;
    pump->message_create_context = context;
}
void message_event_pump_set_read_badge(MessageEventPump* pump, MessageReadBadgeFunction policy,
                                       void* context) noexcept {
    pump->read_badge = policy;
    pump->read_badge_context = context;
}
void message_event_pump_set_member_roles_refresh(MessageEventPump* pump,
                                                 MessageMemberRolesRefreshFunction refresh,
                                                 void* context) noexcept {
    pump->member_roles_refresh = refresh;
    pump->member_roles_refresh_context = context;
}
void message_event_pump_set_read_ack_cancel(MessageEventPump* pump, MessageReadAckCancelFunction cancel,
                                            void* context) noexcept {
    pump->read_ack_cancel = cancel;
    pump->read_ack_cancel_context = context;
}
void message_event_pump_set_read_token_reset(MessageEventPump* pump,
                                             MessageReadTokenResetFunction reset) noexcept {
    pump->read_token_reset = reset;
}

void message_event_pump_set_poll_observer(MessageEventPump* pump, MessagePollVoteObserveFunction observe,
                                          void* context) noexcept {
    pump->poll_vote_observe = observe;
    pump->poll_vote_context = context;
}
void message_event_pump_set_inactive_observer(MessageEventPump* pump, MessageInactiveObserveFunction observe,
                                              void* context) noexcept {
    pump->inactive_observe = observe;
    pump->inactive_observe_context = context;
}
void message_event_pump_set_presence_visibility(MessageEventPump* pump, MessagePresenceVisibleFunction visible,
                                                void* context) noexcept {
    pump->presence_visible = visible;
    pump->presence_visible_context = context;
}
void message_event_pump_set_reaction_observers(MessageEventPump* pump,
                                               MessageReactionObserveFunction reaction,
                                               MessageSourceObserveFunction source,
                                               MessageDeleteObserveFunction erased,
                                               void* context) noexcept {
    pump->reaction_observe = reaction;
    pump->reaction_source_observe = source;
    pump->reaction_delete_observe = erased;
    pump->reaction_observe_context = context;
}
void message_event_pump_set_read_ack_observer(MessageEventPump* pump, MessageReadAckObserveFunction observe,
                                              void* context) noexcept {
    pump->read_ack_observe = observe;
    pump->read_ack_observe_context = context;
}

bool message_event_pump_set_thread_member_store(MessageEventPump* pump,
                                                ThreadMemberStore* members) noexcept {
    if (members && (!members->banks[0].data || !members->banks[1].data ||
                    members->banks[0].data == members->banks[1].data || !members->banks[0].count ||
                    members->banks[0].count != members->banks[1].count ||
                    members->banks[0].count > kThreadMemberMaximumIds || members->active_bank > 1 ||
                    members->count > members->banks[members->active_bank].count))
        return false;
    pump->thread_members = members;
    return message_event_pump_synchronize_thread_scope(pump);
}

bool message_event_pump_set_send_tracker(MessageEventPump* pump, MessageSendTracker* sends) noexcept {
    if (sends && !sends->initialized) {
        return false;
    }
    pump->sends = sends;
    return true;
}

} // namespace maicord
