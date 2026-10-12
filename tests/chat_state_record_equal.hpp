#pragma once
#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
// Field-wise chat-state record comparisons for mutation tests. Explicit fields
// deliberately exclude padding.
inline bool role_equal(const RoleRecord& a, const RoleRecord& b) noexcept {
    return a.id == b.id && a.guild_id == b.guild_id && a.permissions == b.permissions &&
        a.name_offset == b.name_offset && a.name_bytes == b.name_bytes && a.position == b.position &&
        a.color == b.color && a.self_assigned == b.self_assigned && a.hoist == b.hoist &&
        a.managed == b.managed && a.reserved == b.reserved;
}
inline bool guild_equal(const GuildRecord& a, const GuildRecord& b) noexcept {
    return a.id == b.id && a.owner_id == b.owner_id && a.base_permissions == b.base_permissions &&
        a.name_offset == b.name_offset && a.name_bytes == b.name_bytes && a.generation == b.generation &&
        a.revision == b.revision && a.unread_channels == b.unread_channels && a.mention_count == b.mention_count &&
        a.icon_variant == b.icon_variant && bytes_equal(a.icon_hash, b.icon_hash, sizeof(a.icon_hash)) &&
        a.icon_hash_bytes == b.icon_hash_bytes && a.occupied == b.occupied && a.unavailable == b.unavailable &&
        a.permissions_resolved == b.permissions_resolved && a.member_roles_known == b.member_roles_known &&
        a.upload_features == b.upload_features;
}
inline bool channel_equal(const ChannelRecord& a, const ChannelRecord& b) noexcept {
    return a.id == b.id && a.guild_id == b.guild_id && a.parent_id == b.parent_id &&
        a.effective_permissions == b.effective_permissions && a.last_message_id == b.last_message_id &&
        a.last_read_message_id == b.last_read_message_id && a.member_list_id == b.member_list_id &&
        a.name_offset == b.name_offset && a.overwrite_offset == b.overwrite_offset && a.position == b.position &&
        a.flags == b.flags && a.actions == b.actions && a.mention_count == b.mention_count &&
        a.name_bytes == b.name_bytes && a.generation == b.generation && a.overwrite_count == b.overwrite_count &&
        a.thread_archived == b.thread_archived && a.thread_archived_known == b.thread_archived_known &&
        a.revision == b.revision && a.type == b.type && a.occupied == b.occupied &&
        a.permissions_resolved == b.permissions_resolved && a.collapsed == b.collapsed &&
        a.read_state_known == b.read_state_known && a.read_auto_ack_suppressed == b.read_auto_ack_suppressed &&
        a.read_mentions_incomplete == b.read_mentions_incomplete && a.read_flags_known == b.read_flags_known &&
        a.read_last_viewed_known == b.read_last_viewed_known && a.read_last_viewed == b.read_last_viewed &&
        a.read_flags == b.read_flags;
}
} // namespace maicord
