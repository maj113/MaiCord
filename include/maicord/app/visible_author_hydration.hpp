#pragma once
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/gateway_commands.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/voice_state_store.hpp"
#include "maicord/ui/message_scene.hpp"

namespace maicord {
inline constexpr u32 kVisibleAuthorCooldownCapacity = 200;
inline constexpr u32 kVisibleAuthorMaximum = 200;
inline constexpr u64 kVisibleAuthorCooldownMs = 30'000;
struct VisibleAuthorCooldown {
    u64 user_id, retry_at_ms;
};
struct VisibleAuthorInterestKey {
    u64 prepared, scroll, guild, channel;
    UiRect viewport;
    u32 rebuilds, first, count, reset, created, updated, deleted, rich_updated;
    bool eligible;
};
inline VisibleAuthorInterestKey visible_author_interest_key(const MessageScene& scene, u64 guild,
                                                            u64 channel, bool eligible) noexcept {
    const auto* timeline = scene.timeline;
    const auto* store = timeline ? timeline->store : nullptr;
    return {scene.prepared_signature,
            scene.observed_scroll_units,
            guild,
            channel,
            scene.viewport,
            scene.rebuilds,
            scene.first,
            scene.count,
            timeline ? timeline->reset_generation : 0,
            store ? store->created : 0,
            store ? store->updated : 0,
            store ? store->deleted : 0,
            scene.observed_rich_updated,
            eligible};
}
inline bool visible_author_interest_equal(const VisibleAuthorInterestKey& a,
                                          const VisibleAuthorInterestKey& b) noexcept {
    return a.prepared == b.prepared && a.scroll == b.scroll && a.guild == b.guild &&
           a.channel == b.channel && a.viewport.left == b.viewport.left &&
           a.viewport.top == b.viewport.top && a.viewport.right == b.viewport.right &&
           a.viewport.bottom == b.viewport.bottom && a.rebuilds == b.rebuilds && a.first == b.first &&
           a.count == b.count && a.reset == b.reset && a.created == b.created && a.updated == b.updated &&
           a.deleted == b.deleted && a.rich_updated == b.rich_updated && a.eligible == b.eligible;
}
// Account-local, UI-thread-only interest. Cooldowns survive same-guild channel
// switches; teardown discards this lazy account owner, never a global timer.
struct VisibleAuthorHydration {
    VisibleAuthorCooldown cooldowns[kVisibleAuthorCooldownCapacity];
    u64 guild_id, channel_id, serial, retry_at_ms, next_at_ms, send_not_before_ms;
    u32 retry_delay_ms, eviction_cursor;
    bool pending_interest;
    // Voice states seen with the interest (the selected guild's voice users
    // join the requested set); fits the padding before the key.
    u32 voice_revision;
    VisibleAuthorInterestKey interest;
    u32 member_pin_revision, user_pin_revision;
    bool interest_current;
};
inline bool visible_author_hydration_interest_current(const VisibleAuthorHydration& owner,
                                                      const VisibleAuthorInterestKey& key, u32 members,
                                                      u32 users, u64 now_ms) noexcept {
    return owner.interest_current && visible_author_interest_equal(owner.interest, key) &&
           owner.member_pin_revision == members && owner.user_pin_revision == users &&
           (!owner.next_at_ms || owner.next_at_ms > now_ms);
}

inline u32 visible_author_ids(const MessageScene& scene, u64 channel_id, Slice<u64> output) noexcept {
    if (!output.data || !output.count || !scene.active || !scene.timeline || !scene.timeline->store ||
        !scene.rows.data || !ui_rect_has_area(scene.viewport))
        return 0;
    u32 count = 0;
    const auto append = [&](u64 id) noexcept {
        if (!id || count == output.count)
            return;
        for (u32 i = 0; i < count; ++i)
            if (output[i] == id)
                return;
        output[count++] = id;
    };
    const auto visible = [&](UiWidgetHandle handle) noexcept {
        const auto* node = ui_widget_manager_layout_node(scene.widgets, handle);
        return node && node->visible &&
               ui_rect_has_area(ui_rect_intersection(
                   scene.viewport, ui_rect_intersection(node->rectangle, node->clip_rectangle)));
    };
    for (u32 i = 0; i < minimum(scene.count, scene.rows.count); ++i) {
        const auto& row = scene.rows[i];
        const auto* message = message_store_resolve(*scene.timeline->store, row.message);
        if (!message || message->channel_id != channel_id || message->id != row.message_id)
            continue;
        if (!row.grouped && message->delivery == MessageDeliveryState::authoritative && visible(row.author))
            append(message->author_id);
        if (scene.timeline->rich_store && visible(row.reply))
            if (const auto* rich = message_rich_store_find(*scene.timeline->rich_store, message->id))
                append(message_rich_header_user(*rich));
    }
    return count;
}

// Appends the users in a guild's voice channels after the visible authors.
// Voice states for users outside the member cache arrive without a member
// object (READY_SUPPLEMENTAL) and the official client requests them by ID; they
// are shown, so they are pinned and requested like authors. Returns the count.
inline u32 visible_voice_user_ids(const VoiceStateStore& voice, u64 guild_id, Slice<u64> output,
                                  u32 count) noexcept {
    if (!guild_id || !output.data || !voice.records.data)
        return count;
    for (u32 i = 0; i < voice.count && i < voice.records.count && count < output.count; ++i) {
        const auto& state = voice.records[i];
        if (state.guild_id != guild_id || !state.channel_id || !state.user_id)
            continue;
        bool duplicate = false;
        for (u32 j = 0; j < count && !duplicate; ++j)
            duplicate = output[j] == state.user_id;
        if (!duplicate)
            output[count++] = state.user_id;
    }
    return count;
}
// A voice participant needs a request when its profile or server member
// record (the nickname shown in the sidebar) is not cached.
inline bool visible_voice_user_missing(const UserStore& users, const GuildMemberStore& members, u64 guild_id,
                                       u64 user_id) noexcept {
    return !user_store_find(users, user_id) || !guild_member_store_find(members, guild_id, user_id);
}

inline bool visible_author_hydration_plan(VisibleAuthorHydration* owner, u64 guild, u64 channel,
                                          ConstSlice<u64> missing, u64 now_ms,
                                          GatewayGuildMemberRequest* request) noexcept {
    if (!guild || !channel || (!missing.data && missing.count))
        return false;
    if (owner->guild_id != guild) {
        const u64 not_before = owner->send_not_before_ms, serial = owner->serial;
        reset_value(owner);
        owner->guild_id = guild;
        owner->send_not_before_ms = not_before;
        owner->serial = serial;
    }
    owner->channel_id = channel;
    owner->next_at_ms = 0;
    reset_value(request);
    request->guild_id = guild;
    request->channel_id = channel;
    for (u32 i = 0; i < missing.count; ++i) {
        const u64 id = missing[i];
        if (!id)
            continue;
        u64 due = 0;
        for (const auto& entry : owner->cooldowns)
            if (entry.user_id == id) {
                due = entry.retry_at_ms;
                break;
            }
        due = maximum(due, maximum(owner->retry_at_ms, owner->send_not_before_ms));
        if (due > now_ms) {
            owner->next_at_ms = earliest_deadline(owner->next_at_ms, due);
            continue;
        }
        bool duplicate = false;
        for (u32 j = 0; j < request->user_count; ++j)
            duplicate |= request->user_ids[j] == id;
        if (!duplicate && request->user_count < kGatewayGuildMemberRequestMaximum)
            request->user_ids[request->user_count++] = id;
        else if (!duplicate)
            owner->next_at_ms = earliest_deadline(owner->next_at_ms, now_ms + 1'000);
    }
    if (!request->user_count)
        return false;
    request->nonce = owner->serial + 1;
    if (!request->nonce)
        request->nonce = 1;
    return true;
}

inline void visible_author_hydration_complete(VisibleAuthorHydration* owner,
                                              const GatewayGuildMemberRequest& request, u64 now_ms,
                                              bool admitted) noexcept {
    if (owner->guild_id != request.guild_id || owner->channel_id != request.channel_id)
        return;
    if (!admitted) {
        owner->retry_delay_ms = owner->retry_delay_ms ? minimum(owner->retry_delay_ms * 2u, 8'000u) : 250u;
        owner->retry_at_ms = now_ms + owner->retry_delay_ms;
        owner->next_at_ms = owner->retry_at_ms;
        return;
    }
    owner->retry_delay_ms = 0;
    owner->retry_at_ms = 0;
    owner->serial = request.nonce;
    owner->send_not_before_ms = now_ms + 1'000;
    owner->pending_interest = true;
    const u64 due = now_ms + kVisibleAuthorCooldownMs;
    owner->next_at_ms = earliest_deadline(owner->next_at_ms, due);
    for (u32 i = 0; i < request.user_count; ++i) {
        auto* slot = &owner->cooldowns[0];
        bool found = false;
        for (auto& entry : owner->cooldowns) {
            if (entry.user_id == request.user_ids[i] || !entry.user_id) {
                slot = &entry;
                found = true;
                break;
            }
        }
        if (!found)
            slot = &owner->cooldowns[owner->eviction_cursor++ % kVisibleAuthorCooldownCapacity];
        *slot = {request.user_ids[i], due};
    }
}
static_assert(sizeof(VisibleAuthorHydration) <= 4'096);
} // namespace maicord
