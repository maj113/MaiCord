#include "maicord/model/user_activity.hpp"
#include "maicord/app/message_event_pump.hpp"
#include "maicord/app/user_store_dispatch.hpp"
#include "maicord/app/member_list_channel_binding.hpp"
#include "maicord/app/gateway_thread_members.hpp"
#include "maicord/model/read_state.hpp"
#include "gateway_member_list_validation.hpp"

namespace maicord {
namespace {

static_assert(sizeof(MessageEventPump) == 328 + sizeof(ReservedBytes));

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
    if (!count)
        return false;
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
            (message.presentation_flags & message_presentation_mentions_current_user))
            saturating_increment(*count);
    }
    return true;
}

bool synchronize_thread_scope(MessageEventPump* pump) noexcept {
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

constexpr bool valid_content_access(MessageContentAvailability access) noexcept {
    return access == MessageContentAvailability::unknown ||
           access == MessageContentAvailability::available ||
           access == MessageContentAvailability::restricted;
}

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

u64 packed_u64(ConstSlice<u8> bytes, u32 offset) noexcept {
    u64 value = 0;
    for (u32 index = 0; index < 8; ++index) {
        value |= static_cast<u64>(bytes[offset + index]) << (index * 8);
    }
    return value;
}

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
             (mutation.hydration_flags & ~gateway_member_timeout_known)))
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
            if (affects_visible_members(*pump->members, list.guild_id, list_id, mutation.index,
                                        mutation.range_end))
                guild_member_store_refresh_visible_users(*pump->members, pump->users);
            continue;
        }
        if (mutation.operation == GatewayClientMemberListOperation::remove) {
            if (!guild_member_store_invalidate_list_range(
                    pump->members, list.guild_id, mutation.index, mutation.index, list_id) ||
                !guild_member_store_shift_list(pump->members, list.guild_id, list_id,
                                                       mutation.index, false)) {
                return false;
            }
            if (affects_visible_members(*pump->members, list.guild_id, list_id, mutation.index, ~0u))
                guild_member_store_refresh_visible_users(*pump->members, pump->users);
            continue;
        }
        if (mutation.operation == GatewayClientMemberListOperation::insert) {
            if (!guild_member_store_shift_list(pump->members, list.guild_id, list_id,
                                                       mutation.index, true))
                return false;
            if (affects_visible_members(*pump->members, list.guild_id, list_id, mutation.index, ~0u))
                guild_member_store_refresh_visible_users(*pump->members, pump->users);
        }
        if (mutation.kind == GatewayClientMemberListItemKind::group) {
            if (!guild_member_store_set_group(
                    pump->members, list.guild_id, list_id, mutation.index, mutation.user_id,
                    mutation.range_end,
                    (mutation.hydration_flags & gateway_member_group_count_omitted) == 0))
                return false;
            if (affects_visible_members(*pump->members, list.guild_id, list_id, mutation.index,
                                        mutation.index))
                guild_member_store_refresh_visible_users(*pump->members, pump->users);
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
            if (affects_visible_members(*pump->members, list.guild_id, list_id, mutation.index,
                                        mutation.index))
                guild_member_store_refresh_visible_users(*pump->members, pump->users);
            return true;
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
        UserAppearance appearance{};
        const u32 appearance_revision = pump->users->appearance.revision;
        if (!user_appearance_read(global_extra, mutation.user_id, 0, &appearance) ||
            !user_appearance_upsert(&pump->users->appearance, appearance) ||
            !user_appearance_read(guild_extra, mutation.user_id, event.member_list.guild_id,
                                     &appearance) ||
            !user_appearance_upsert(&pump->users->appearance, appearance))
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
        if (appearance_revision != pump->users->appearance.revision || platforms_changed) {
            if (!in_place || !pump->presence_visible)
                ++pump->users->revision;
            else if (row_visible)
                pump->presence_visible_dirty = true;
            else
                saturating_increment(pump->presence_offscreen);
        }
        u64 role_ids[256]{};
        for (u32 index = 0; index < mutation.role_count; ++index) {
            role_ids[index] = packed_u64(roles, index * 8u);
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
                if (mutation.hydration_flags & gateway_member_timeout_known)
                    guild_member_store_set_timeout(pump->members, list.guild_id, mutation.user_id,
                                                   mutation.communication_disabled_until);
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
        if (mutation.hydration_flags & gateway_member_timeout_known)
            guild_member_store_set_timeout(pump->members, list.guild_id, mutation.user_id,
                                                   mutation.communication_disabled_until);
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
        if (affects_visible_members(*pump->members, list.guild_id, list_id, 0, ~0u))
            guild_member_store_refresh_visible_users(*pump->members, pump->users);
    }
    return cursor == payload.count && member_records == event.member_list.member_record_count;
}

constexpr bool lifecycle_repaints(GatewayClientEventKind kind) noexcept {
    return kind == GatewayClientEventKind::ready || kind == GatewayClientEventKind::resumed ||
           kind == GatewayClientEventKind::reconnecting || kind == GatewayClientEventKind::closed ||
           kind == GatewayClientEventKind::failed || kind == GatewayClientEventKind::stopped;
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
    if (!mentioned) {
        return false;
    }
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

bool observe_message_member(MessageEventPump* pump, const GatewayClientEvent& event,
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
    u64 roles[kGatewayMessageMemberRoleMaximumIds]{};
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
                                            MessageEventPumpRejectReason reason = {}) noexcept {
    MessageEventPumpConsumeResult result{};
    result.error = error;
    result.reason = reason;
    return result;
}

// Marks a consume result rejected for one apply check.
void reject(MessageEventPumpConsumeResult* result, MessageEventPumpRejectReason reason, u8 detail = 0) noexcept {
    result->kind = MessageEventPumpConsumeKind::rejected;
    result->error = MessageEventPumpError::apply;
    result->reason = reason;
    result->detail = detail;
}

u8 state_detail(const ChatStateApplyResult& applied) noexcept {
    return static_cast<u8>((static_cast<u32>(applied.error) << 4) | (static_cast<u32>(applied.store_error) & 15u));
}

MessageEventPumpConsumeResult consume_voice_states(MessageEventPump* pump, const GatewayClientEvent& event,
                                                   ConstSlice<u8> payload) noexcept {
    MessageEventPumpConsumeResult result{};
    const auto& snapshot = event.voice_states;
    if (!pump->voice_states) {
        ++pump->ignored;
        result.kind = MessageEventPumpConsumeKind::ignored_dispatch;
        return result;
    }
    if (payload.count != snapshot.record_count * sizeof(GatewayClientVoiceStateRecord)) {
        ++pump->rejected;
        reject(&result, MessageEventPumpRejectReason::voice_states);
        return result;
    }
    const u32 revision = pump->voice_states->revision;
    if (snapshot.replace)
        voice_state_store_clear_guild(pump->voice_states, snapshot.guild_id);
    for (u32 i = 0; i < snapshot.record_count; ++i) {
        GatewayClientVoiceStateRecord record;
        copy_bytes(&record, payload.data + i * sizeof(record), sizeof(record));
        // Capacity overflow drops the row; the store counts it.
        voice_state_store_apply(pump->voice_states, record.guild_id, record.channel_id,
                                        record.user_id, record.flags, record.request_to_speak_s);
    }
    ++pump->applied;
    result.kind = MessageEventPumpConsumeKind::state_applied;
    result.repaint = revision != pump->voice_states->revision;
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

template <class Consume>
__declspec(noinline) MessageEventPumpConsumeResult
consume_with_message_source(MessageEventPump* pump, const GatewayClientEvent& event, ConstSlice<u8> payload,
                            Consume consume) noexcept {
    // The observer and canonical mutation borrow one decoded body. This
    // bounded stack scratch is entered only while the host has retained intents.
    GatewayMessageReaction reactions[kGatewayMessageReactionMaximum];
    GatewayMessageAttachment attachments[kGatewayMessageAttachmentMaximum];
    GatewayMessageEmbed embeds[kGatewayMessageEmbedMaximum];
    GatewayMessageEmbedField fields[kGatewayMessageEmbedFieldMaximum];
    GatewayMessageSticker stickers[kGatewayMessageStickerMaximum];
    GatewayMessageComponent components[kGatewayMessageComponentMaximum];
    GatewayMessageAttachment child_attachments[kGatewayMessageAttachmentMaximum];
    GatewayMessageEmbed child_embeds[kGatewayMessageEmbedMaximum];
    GatewayMessageEmbedField child_fields[kGatewayMessageEmbedFieldMaximum];
    GatewayMessageSticker child_stickers[kGatewayMessageStickerMaximum];
    GatewayMessageComponent child_components[kGatewayMessageComponentMaximum];
    GatewayMessageNamedUser child_users[kGatewayMessageMentionUserMaximum];
    GatewayMessagePollAnswer poll_answers[kGatewayMessagePollMaximumAnswers];
    GatewayMessagePollAnswerCount poll_counts[kGatewayMessagePollMaximumAnswers];
    const GatewayMessageRichScratch scratch{{reactions, kGatewayMessageReactionMaximum},
                                            {attachments, kGatewayMessageAttachmentMaximum},
                                            {embeds, kGatewayMessageEmbedMaximum},
                                            {fields, kGatewayMessageEmbedFieldMaximum},
                                            {stickers, kGatewayMessageStickerMaximum},
                                            {components, kGatewayMessageComponentMaximum},
                                            {{child_attachments, kGatewayMessageAttachmentMaximum},
                                             {child_embeds, kGatewayMessageEmbedMaximum},
                                             {child_fields, kGatewayMessageEmbedFieldMaximum},
                                             {child_stickers, kGatewayMessageStickerMaximum},
                                             {child_components, kGatewayMessageComponentMaximum},
                                             {child_users, kGatewayMessageMentionUserMaximum}},
                                            {{poll_answers, kGatewayMessagePollMaximumAnswers},
                                             {poll_counts, kGatewayMessagePollMaximumAnswers}}};
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

} // namespace

bool message_event_pump_initialize(MessageEventPump* pump, MessageTimeline* timeline,
                                   Slice<u8> payload_scratch, Slice<u64> bulk_id_scratch,
                                   MessageContentAvailability content_access) noexcept {
    if (!pump || !timeline || !timeline->store || !timeline->list || !payload_scratch.data ||
        payload_scratch.count < kMessageEventPumpPayloadCapacity || !bulk_id_scratch.data ||
        bulk_id_scratch.count < kGatewayBulkDeleteMaximumIds || !valid_content_access(content_access)) {
        return false;
    }
    reserved_bytes_release(&pump->large_payload);
    *pump = MessageEventPump{};
    pump->timeline = timeline;
    pump->payload_scratch = payload_scratch;
    pump->bulk_id_scratch = bulk_id_scratch;
    pump->content_access = content_access;
    return true;
}

void message_event_pump_reset(MessageEventPump* pump) noexcept {
    if (pump) {
        reserved_bytes_release(&pump->large_payload);
        *pump = MessageEventPump{};
    }
}

void message_event_pump_set_view_active(MessageEventPump* pump, bool view_active) noexcept {
    if (pump) {
        pump->view_active = view_active;
    }
}

bool message_event_pump_set_state_store(MessageEventPump* pump, ChatStateStore* state,
                                        Slice<u64> guild_id_scratch) noexcept {
    if (!pump || !state || !guild_id_scratch.data || guild_id_scratch.count < state->guild_capacity) {
        return false;
    }
    pump->state = state;
    pump->guild_id_scratch = guild_id_scratch;
    return synchronize_thread_scope(pump);
}

void message_event_pump_set_guild_detail_store(MessageEventPump* pump, GuildDetailStore* details) noexcept {
    if (pump)
        pump->guild_details = details;
}

bool message_event_pump_set_user_store(MessageEventPump* pump, UserStore* users) noexcept {
    if (!pump || !users || !users->records || !users->order || !users->names) {
        return false;
    }
    pump->users = users;
    return true;
}

bool message_event_pump_set_member_store(MessageEventPump* pump,
                                         GuildMemberStore* members) noexcept {
    if (!pump || !members || !members->records || !members->order) {
        return false;
    }
    pump->members = members;
    return true;
}

bool message_event_pump_set_voice_state_store(MessageEventPump* pump,
                                              VoiceStateStore* voice_states) noexcept {
    if (!pump || (voice_states && (!voice_states->records.data ||
                                   voice_states->count > voice_states->records.count)))
        return false;
    pump->voice_states = voice_states;
    return true;
}

bool message_event_pump_set_typing_store(MessageEventPump* pump, TypingStore* typing) noexcept {
    if (!pump || (typing && (!typing->records.data || typing->records.count == 0 ||
                             typing->count > typing->records.count))) {
        return false;
    }
    pump->typing = typing;
    return true;
}
void message_event_pump_set_create_observer(MessageEventPump* pump, MessageCreateObserveFunction observe,
                                            void* context) noexcept {
    if (pump) {
        pump->message_create_observe = observe;
        pump->message_create_context = context;
    }
}
void message_event_pump_set_member_roles_refresh(MessageEventPump* pump,
                                                 MessageMemberRolesRefreshFunction refresh,
                                                 void* context) noexcept {
    if (pump) {
        pump->member_roles_refresh = refresh;
        pump->member_roles_refresh_context = context;
    }
}
void message_event_pump_set_read_ack_cancel(MessageEventPump* pump, MessageReadAckCancelFunction cancel,
                                            void* context) noexcept {
    if (pump) {
        pump->read_ack_cancel = cancel;
        pump->read_ack_cancel_context = context;
    }
}
void message_event_pump_set_read_token_reset(MessageEventPump* pump,
                                             MessageReadTokenResetFunction reset) noexcept {
    if (pump)
        pump->read_token_reset = reset;
}

void message_event_pump_set_poll_observer(MessageEventPump* pump, MessagePollVoteObserveFunction observe,
                                          void* context) noexcept {
    if (pump) {
        pump->poll_vote_observe = observe;
        pump->poll_vote_context = context;
    }
}
void message_event_pump_set_inactive_observer(MessageEventPump* pump, MessageInactiveObserveFunction observe,
                                              void* context) noexcept {
    if (pump) {
        pump->inactive_observe = observe;
        pump->inactive_observe_context = context;
    }
}
void message_event_pump_set_presence_visibility(MessageEventPump* pump, MessagePresenceVisibleFunction visible,
                                                void* context) noexcept {
    if (pump) {
        pump->presence_visible = visible;
        pump->presence_visible_context = context;
    }
}
void message_event_pump_set_reaction_observers(MessageEventPump* pump,
                                               MessageReactionObserveFunction reaction,
                                               MessageSourceObserveFunction source,
                                               MessageDeleteObserveFunction erased,
                                               void* context) noexcept {
    if (pump) {
        pump->reaction_observe = reaction;
        pump->reaction_source_observe = source;
        pump->reaction_delete_observe = erased;
        pump->reaction_observe_context = context;
    }
}
void message_event_pump_set_read_ack_observer(MessageEventPump* pump, MessageReadAckObserveFunction observe,
                                              void* context) noexcept {
    if (pump) {
        pump->read_ack_observe = observe;
        pump->read_ack_observe_context = context;
    }
}

bool message_event_pump_set_thread_member_store(MessageEventPump* pump,
                                                ThreadMemberStore* members) noexcept {
    if (!pump ||
        (members && (!members->banks[0].data || !members->banks[1].data ||
                     members->banks[0].data == members->banks[1].data || !members->banks[0].count ||
                     members->banks[0].count != members->banks[1].count ||
                     members->banks[0].count > kThreadMemberMaximumIds || members->active_bank > 1 ||
                     members->count > members->banks[members->active_bank].count)))
        return false;
    pump->thread_members = members;
    return synchronize_thread_scope(pump);
}

bool message_event_pump_synchronize_thread_scope(MessageEventPump* pump) noexcept {
    return pump && synchronize_thread_scope(pump);
}

bool message_event_pump_set_send_tracker(MessageEventPump* pump, MessageSendTracker* sends) noexcept {
    if (!pump || (sends && !sends->initialized)) {
        return false;
    }
    pump->sends = sends;
    return true;
}

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
        ++pump->rejected;
        pump->resync_required = true;
        reject(&result, MessageEventPumpRejectReason::descriptor_payload_mismatch);
        return result;
    }
    if (event.kind == GatewayClientEventKind::ready && pump->state) {
        const bool had_channel =
            pump->state->selected_channel_id != 0 || pump->state->restore_channel_id != 0;
        const ChatStateApplyResult applied = chat_state_apply_gateway_event(
            pump->state, event, payload, pump->guild_id_scratch, pump->users);
        if (!applied.ok()) {
            if (event.dispatch_kind == GatewayClientDispatchKind::thread_self_update ||
                event.dispatch_kind == GatewayClientDispatchKind::thread_list_sync ||
                event.state.type == 10 || event.state.type == 11 || event.state.type == 12) {
                const u64 guild_id = event.dispatch_kind == GatewayClientDispatchKind::thread_self_update
                                         ? event.thread_self.guild_id
                                     : event.dispatch_kind == GatewayClientDispatchKind::thread_list_sync
                                         ? event.thread_sync.guild_id
                                         : event.state.guild_id;
                chat_state_store_invalidate_thread_authority(pump->state, guild_id);
                result.repaint = true;
            }
            ++pump->rejected;
            pump->resync_required = true;
            reject(&result, MessageEventPumpRejectReason::ready_state, state_detail(applied));
            result.state_incomplete = true;
            return result;
        }
        if (pump->guild_details)
            guild_detail_store_apply_gateway_event(pump->guild_details, *pump->state, event, payload);
        if (pump->members) {
            guild_member_store_reset_lists(pump->members);
            guild_member_store_retire_role_authority(pump->members);
            guild_member_store_refresh_visible_users(*pump->members, pump->users);
        }
        if (pump->thread_members) {
            thread_member_store_reset_snapshot(pump->thread_members);
            if (!synchronize_thread_scope(pump))
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
        if (!typing_store_reset(pump->typing)) {
            ++pump->rejected;
            reject(&result, MessageEventPumpRejectReason::ready_typing_reset);
            return result;
        }
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
        pump->last_lifecycle = event.kind;
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
        if (!synchronize_thread_scope(pump))
            return consume_error(MessageEventPumpError::apply, MessageEventPumpRejectReason::thread_scope);
        result.kind = MessageEventPumpConsumeKind::state_applied;
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::user_snapshot) {
        if (!user_store_apply_gateway_snapshots(pump->users, payload, event.state.record_count)) {
            ++pump->rejected;
            pump->resync_required = true;
            reject(&result, MessageEventPumpRejectReason::user_snapshot);
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
        if (!synchronize_thread_scope(pump))
            return consume_error(MessageEventPumpError::apply, MessageEventPumpRejectReason::thread_scope);
        constexpr auto update = utf8_literal("USER_UPDATE");
        bool original_user_update = event.event_name_bytes == update.bytes;
        for (u32 i = 0; original_user_update && i < update.bytes; ++i)
            original_user_update = event.event_name[i] == update.data[i];
        if (current_user_observed && original_user_update && pump->read_token_reset)
            pump->read_token_reset(pump->read_ack_cancel_context);
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::read_state_snapshot ||
        event.dispatch_kind == GatewayClientDispatchKind::message_ack) {
        if (!pump->state || !pump->state->read_state) {
            ++pump->ignored;
            result.kind = MessageEventPumpConsumeKind::ignored_dispatch;
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
            ++pump->rejected;
            pump->resync_required = true;
            reject(&result, MessageEventPumpRejectReason::read_state,
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
            ++pump->ignored;
            result.kind = MessageEventPumpConsumeKind::ignored_dispatch;
            return result;
        }
        const u32 channel_revision = pump->state->channel_mutations;
        const auto authority = chat_state_apply_thread_authority(event, payload, pump->state);
        if (!authority.ok()) {
            chat_state_store_invalidate_thread_authority(pump->state, event.thread_members.guild_id);
            result.repaint = true;
            ++pump->rejected;
            pump->resync_required = true;
            reject(&result, MessageEventPumpRejectReason::thread_authority, state_detail(authority));
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
        if (!synchronize_thread_scope(pump) ||
            !gateway_client_apply_thread_members(event, payload, pump->thread_members, pump->members,
                                                 pump->users, *pump->state)) {
            chat_state_store_invalidate_thread_authority(pump->state, event.thread_members.guild_id);
            result.repaint = true;
            ++pump->rejected;
            pump->resync_required = true;
            reject(&result, MessageEventPumpRejectReason::thread_members);
            return result;
        }
        if (!thread_member_store_owns(*pump->thread_members, event.thread_members.guild_id,
                                              event.thread_members.thread_id)) {
            if (authority.channels)
                result.kind = MessageEventPumpConsumeKind::state_applied;
            else {
                ++pump->ignored;
                result.kind = MessageEventPumpConsumeKind::ignored_dispatch;
            }
        } else
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
            ++pump->rejected;
            reject(&result, MessageEventPumpRejectReason::guild_membership);
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
            ++pump->ignored;
            result.kind = MessageEventPumpConsumeKind::ignored_dispatch;
            return result;
        }
        const bool role_event = event.dispatch_kind == GatewayClientDispatchKind::guild_role_create ||
                                event.dispatch_kind == GatewayClientDispatchKind::guild_role_update ||
                                event.dispatch_kind == GatewayClientDispatchKind::guild_role_delete;
        RoleRecord previous_role{};
        bool previous_known = false;
        if (role_event)
            if (const auto* role =
                    chat_state_store_find_role(*pump->state, event.state.guild_id, event.state.id)) {
                previous_role = *role;
                previous_known = true;
            }
        const ChatStateApplyResult applied = chat_state_apply_gateway_event(
            pump->state, event, payload, pump->guild_id_scratch, pump->users);
        if (!applied.ok()) {
            ++pump->rejected;
            if (event.dispatch_kind == GatewayClientDispatchKind::thread_self_update ||
                event.dispatch_kind == GatewayClientDispatchKind::thread_list_sync ||
                event.state.type == 10 || event.state.type == 11 || event.state.type == 12) {
                const u64 guild_id = event.dispatch_kind == GatewayClientDispatchKind::thread_self_update
                                         ? event.thread_self.guild_id
                                     : event.dispatch_kind == GatewayClientDispatchKind::thread_list_sync
                                         ? event.thread_sync.guild_id
                                         : event.state.guild_id;
                chat_state_store_invalidate_thread_authority(pump->state, guild_id);
                result.repaint = true;
            }
            const bool private_event =
                event.dispatch_kind == GatewayClientDispatchKind::channel_recipient_add ||
                event.dispatch_kind == GatewayClientDispatchKind::channel_recipient_remove ||
                channel_is_private(event.state.type);
            if (private_event)
                pump->private_state_incomplete = true;
            else
                pump->resync_required = true;
            reject(&result, MessageEventPumpRejectReason::guild_state, state_detail(applied));
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
        if (pump->guild_details && (event.dispatch_kind == GatewayClientDispatchKind::guild_create ||
                                    event.dispatch_kind == GatewayClientDispatchKind::guild_update ||
                                    event.dispatch_kind == GatewayClientDispatchKind::guild_delete))
            guild_detail_store_apply_gateway_event(pump->guild_details, *pump->state, event, payload);
        result.kind = MessageEventPumpConsumeKind::state_applied;
        result.repaint = true;
        if (!synchronize_thread_scope(pump))
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
            ++pump->rejected;
            pump->resync_required = true;
            reject(&result, MessageEventPumpRejectReason::member_list);
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
    if (event.dispatch_kind == GatewayClientDispatchKind::presence_update) {
        const auto& presence = event.presence;
        UserAppearance appearance{};
        UserActivitySet activities{};
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
            ++pump->rejected;
            reject(&result, MessageEventPumpRejectReason::presence_payload,
                   static_cast<u8>((payload.count ? 1 : 0) | (has_activities ? 2 : 0)));
            return result;
        }
        // Large guilds stream presence for members nobody can see. The model
        // always updates; only an on-screen row or author invalidates text,
        // and the owner coalesces that repaint (see presence_visible_dirty).
        const bool filtered = pump->presence_visible && presence.guild_id != 0;
        // Activities without a trailer (or an offline presence) carry no rich
        // presence: the user's activities clear.
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
                if (!has_activities)
                    activities = {presence.user_id};
                user_activity_upsert(&pump->users->activities, activities);
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
            ++pump->ignored;
            result.kind = MessageEventPumpConsumeKind::ignored_dispatch;
            return result;
        }
        const u32 presence_revision = pump->members->presence_revision;
        if (!guild_member_store_set_status(pump->members, presence.guild_id, presence.user_id,
                                                   member_status(presence.status))) {
            ++pump->rejected;
            reject(&result, MessageEventPumpRejectReason::presence_status);
            return result;
        }
        ++pump->applied;
        result.kind = MessageEventPumpConsumeKind::state_applied;
        result.repaint = result.repaint || (!filtered && presence_revision != pump->members->presence_revision);
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::message_catchup_required) {
        pump->catchup_required = true;
        if (pump->timeline) {
            pump->timeline->newer_refetch_required = true;
        }
        ++pump->ignored;
        result.kind = MessageEventPumpConsumeKind::ignored_dispatch;
        return result;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::typing_start) {
        if (!pump->typing) {
            ++pump->ignored;
            result.kind = MessageEventPumpConsumeKind::ignored_dispatch;
            return result;
        }
        GatewayTypingStartEvent typing{};
        if (!gateway_client_event_as_typing_start(event, payload, &typing)) {
            ++pump->rejected;
            reject(&result, MessageEventPumpRejectReason::typing, 1);
            return result;
        }
        const TypingStoreResult observed = typing_store_start(
            pump->typing, typing.channel_id, typing.guild_id, typing.user_id, event.timestamp_ms);
        if (observed.error == TypingStoreError::capacity) {
            ++pump->ignored;
            result.kind = MessageEventPumpConsumeKind::ignored_dispatch;
            return result;
        }
        if (!observed.ok()) {
            ++pump->rejected;
            reject(&result, MessageEventPumpRejectReason::typing, 2);
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
            ++pump->rejected;
            reject(&result, MessageEventPumpRejectReason::poll_vote);
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
            ++pump->rejected;
            pump->resync_required = true;
            reject(&result, MessageEventPumpRejectReason::reaction_decode);
            return result;
        }
        if (pump->state && (pump->state->selected_channel_id == 0 ||
                            reaction.channel_id != pump->state->selected_channel_id)) {
            if (pump->reaction_observe)
                pump->reaction_observe(pump->reaction_observe_context, reaction, MessageHandle{~u32{0}, 0},
                                       0, false);
            if (pump->inactive_observe)
                pump->inactive_observe(pump->inactive_observe_context, event, payload, pump->content_access);
            ++pump->ignored;
            result.kind = MessageEventPumpConsumeKind::ignored_dispatch;
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
        MessageRichReactionMutationKind kind{};
        if (reaction.kind == GatewayMessageReactionEventKind::add) {
            kind = MessageRichReactionMutationKind::add;
        } else if (reaction.kind == GatewayMessageReactionEventKind::remove) {
            kind = MessageRichReactionMutationKind::remove;
        } else if (reaction.kind == GatewayMessageReactionEventKind::remove_all) {
            kind = MessageRichReactionMutationKind::remove_all;
        } else {
            kind = MessageRichReactionMutationKind::remove_emoji;
        }
        const MessageRichReactionMutation mutation{reaction.emoji_id,
                                                   reaction.name,
                                                   reaction.type,
                                                   kind,
                                                   pump->state && reaction.user_id != 0 &&
                                                       reaction.user_id == pump->state->self_user_id,
                                                   reaction.animated};
        const MessageRichStoreResult applied = message_rich_store_mutate_reaction(
            pump->timeline->rich_store, reaction.message_id, record->revision, mutation);
        if (!applied.ok()) {
            ++pump->rejected;
            pump->resync_required = true;
            reject(&result, MessageEventPumpRejectReason::reaction_apply, static_cast<u8>(applied.error));
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
        ++pump->ignored;
        result.kind = MessageEventPumpConsumeKind::ignored_dispatch;
        return result;
    }
    const auto consume_message =
        [&](const GatewayMessageEvent* decoded_message) noexcept -> MessageEventPumpConsumeResult {
        if (!observe_message_users(pump, event, payload) || !observe_message_member(pump, event, payload)) {
            ++pump->rejected;
            pump->resync_required = true;
            reject(&result, MessageEventPumpRejectReason::message_users);
            return result;
        }
        if (event.dispatch_kind == GatewayClientDispatchKind::message_create && pump->typing) {
            const TypingStoreResult stopped = typing_store_note_message(
                pump->typing, event.message.channel_id, event.message.author_id, false);
            if (!stopped.ok()) {
                ++pump->rejected;
                reject(&result, MessageEventPumpRejectReason::message_typing);
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
                ++pump->rejected;
                pump->resync_required = true;
                reject(&result, MessageEventPumpRejectReason::message_send, static_cast<u8>(observed.error));
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
                ++pump->rejected;
                pump->resync_required = true;
                reject(&result, MessageEventPumpRejectReason::message_mentions);
                return result;
            }
            presentation_mention_known = true;
        }
        bool message_validated = false;
        if (event.dispatch_kind == GatewayClientDispatchKind::message_create && pump->state) {
            GatewayMessageEvent message{};
            if (!gateway_client_event_as_message(event, payload, &message)) {
                ++pump->rejected;
                pump->resync_required = true;
                reject(&result, MessageEventPumpRejectReason::message_decode);
                return result;
            }
            const u32 channel_mutations = pump->state->channel_mutations;
            const bool noted =
                chat_state_store_note_message(pump->state, message.channel_id, message.id,
                                                 message.author_id, presentation_mentioned, false);
            message_validated = true;
            if (noted && message.author_id == pump->state->self_user_id && pump->read_ack_cancel)
                pump->read_ack_cancel(pump->read_ack_cancel_context, message.channel_id);
            result.repaint = noted && channel_mutations != pump->state->channel_mutations;
        }
        if (pump->state && (pump->state->selected_channel_id == 0 ||
                            event.message.channel_id != pump->state->selected_channel_id)) {
            if (decoded_message)
                observe_message_source(pump, *decoded_message, false);
            if (!observe_message_deletions(pump, event, payload)) {
                ++pump->rejected;
                pump->resync_required = true;
                reject(&result, MessageEventPumpRejectReason::message_deletions);
                return result;
            }
            if (message_validated && pump->message_create_observe)
                pump->message_create_observe(pump->message_create_context, event, payload);
            if (pump->inactive_observe)
                pump->inactive_observe(pump->inactive_observe_context, event, payload,
                                       resolve_content_access(*pump, event));
            ++pump->ignored;
            result.kind = MessageEventPumpConsumeKind::ignored_dispatch;
            return result;
        }

        result.apply = decoded_message
                           ? (event.dispatch_kind == GatewayClientDispatchKind::message_create
                                  ? message_timeline_apply_created(pump->timeline, *decoded_message,
                                                                   resolve_content_access(*pump, event))
                                  : message_timeline_apply_updated(pump->timeline, *decoded_message,
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
                if (!presentation ||
                    !message_store_set_presentation_flags(
                        pump->timeline->store, event.message.id,
                        (presentation->presentation_flags & ~message_presentation_mentions_current_user) |
                            (presentation_mentioned ? message_presentation_mentions_current_user : 0u))) {
                    ++pump->rejected;
                    pump->resync_required = true;
                    reject(&result, MessageEventPumpRejectReason::message_presentation);
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
                ++pump->rejected;
                pump->resync_required = true;
                reject(&result, MessageEventPumpRejectReason::message_deletions);
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
                ++pump->rejected;
                pump->resync_required = true;
                reject(&result, MessageEventPumpRejectReason::message_deletions);
                return result;
            }
            ++pump->not_resident;
            result.kind = MessageEventPumpConsumeKind::not_resident;
            return result;
        }

        ++pump->rejected;
        pump->resync_required = true;
        reject(&result, MessageEventPumpRejectReason::message_apply, static_cast<u8>(result.apply.error));
        return result;
    };
    if (pump->reaction_source_observe &&
        (!pump->source_observe_poll_only || (event.message.fields & gateway_message_field_poll)) &&
        (event.dispatch_kind == GatewayClientDispatchKind::message_create ||
         event.dispatch_kind == GatewayClientDispatchKind::message_update))
        return consume_with_message_source(pump, event, payload, consume_message);
    return consume_message(nullptr);
}

const char* message_event_pump_reject_reason_name(MessageEventPumpRejectReason reason) noexcept {
    using R = MessageEventPumpRejectReason;
    switch (reason) {
    case R::none: return "none";
    case R::descriptor_payload_mismatch: return "descriptor_payload_mismatch";
    case R::notification_sidecar: return "notification_sidecar";
    case R::ready_state: return "ready_state";
    case R::ready_typing_reset: return "ready_typing_reset";
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
    case R::message_users: return "message_users";
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
    if (!pump || !pump->timeline || !source || !pop || maximum_events == 0) {
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
            event = {};
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
